/**
 * shot_probe.c — 「把屏幕渲染结果 dump 到串口，本地还原成 PNG」的排查探针
 *
 * 为什么要有它（2026-09-29 第三轮）：
 *   连续三轮 UI 改版，验收方式一直是"烧进去 → 让用户看屏幕 → 用户描述问题"。
 *   这条路有两个硬伤：
 *     ① 布局有没有压字/错位，我**无法自证**，只能请用户当人眼检测器；
 *     ② 用户在等，我在猜 —— 迭代成本被推给用户。
 *   本次改成：让固件把屏幕**真的渲染一遍**（lv_snapshot），把像素逐字节吐到串口，
 *   本地拼回 PNG，我自己用视觉能力看。⇒ 布局问题当场可见，不用等回话。
 *
 * 实现要点（三个都是踩过的坑，写死在这里）：
 *   ① **必须在 LVGL 任务上下文里抓图**。LVGL 非线程安全，从 app_main 直接调会与
 *      刷新任务并发遍历对象树。做法：本探针在 lcd_hw_start() **之前**（此刻 LVGL 独占）
 *      只注册一个一次性 lv_timer，真正的抓图发生在 lvgl 任务调用 lv_timer_handler 时。
 *   ② **抓图期间必须周期性让出 CPU**（vTaskDelay(1)）。整屏 320x240 RGB565 ≈ 150KB，
 *      一路阻塞写串口会饿死 IDLE 任务 → CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPUx=y
 *      下的 TG1WDT 复位（这会伪装成"加了图标就崩了"，是经典的假故障）。
 *   ③ **维度以文本形式先发**（w/h/stride/cf），不靠 host 端解析位域头。
 *      位域布局随编译器/配置可变，用名字读（dsc->header.w）才是可靠路径。
 *
 * 输出协议（纯 ASCII，host 端按此解析）：
 *   ##SHOT n MODE\n        每页开始；MODE = base(关键帧) | diff(与前一帧的差异掩码)
 *   ##HDR w h stride cf \n  随后是二进制载荷
 *   <二进制 stride*h 字节>
 *   ##END n\n               本页结束
 *   10 页全部发完后：##DONE\n
 *
 * ⚠️ 这是**排查专用**代码，产品构建下 SHOT_PROBE=0 → 整个文件编译为空，零开销。
 */
#include "sdkconfig.h"

#if SHOT_PROBE

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "lvgl.h"

#include "ui.h"          /* ui_goto()：由探针确定性地切页，而不是依赖 AUTO 轮播 */

#define TAG_SHOT "shot"

#define SHOT_W      320
#define SHOT_H      240
#define SHOT_PAGES  9

/* 只抓 [FIRST,LAST] 区间（默认 0..9 全抓）。
 * 加这个是因为：自检/其它探针的文本输出会与逐页二进制载荷在串口上交错，
 * 互相污染解析；能只抓关心的一页，就把交错的窗口压到一个上。 */
#ifndef SHOT_PAGE_FIRST
#define SHOT_PAGE_FIRST 0
#endif
#ifndef SHOT_PAGE_LAST
#define SHOT_PAGE_LAST  (SHOT_PAGES - 1)
#endif

/* 首帧延迟：抓单页时要等其它探针（如 PROG_SELFTEST）把文本打完，避免交错 */
#ifndef SHOT_FIRST_DELAY_MS
#define SHOT_FIRST_DELAY_MS 1500
#endif

/* 自动轮播周期，与 ui.c 的 s_auto_ms 一致（抓图期间临时打开轮播） */
#define SHOT_DWELL_MS  3000

static uint8_t *s_snap_raw;      /* 上一帧 RGB565（用于差异掩码） */
static int      s_page;          /* 当前抓到第几页 */
static int      s_stage;         /* 0/1/2 … 每页的子步骤 */

/* 一次性吐出整块二进制：小块 + 周期性 vTaskDelay 让 IDLE 有机会跑（防 TG1WDT） */
static void dump_block(const uint8_t *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        size_t chunk = n - off;
        if (chunk > 4096) chunk = 4096;
        fwrite(p + off, 1, chunk, stdout);
        fflush(stdout);
        off += chunk;
        vTaskDelay(1);           /* ← 关键：别饿死 IDLE */
    }
}

/* 产出差异掩码：m[i] = 与上一帧同一像素是否不同（1 bit/pixel，按行打包）
 * 目的：host 端可以断言"这一页真的换了内容"，而不是"没崩就算过"。
 */
static uint8_t *diff_mask(const uint8_t *cur, const uint8_t *prev, int w, int rows, size_t *out_len)
{
    const int row_bytes = (w + 7) / 8;
    const size_t len = (size_t)row_bytes * rows + 8;
    uint8_t *m = malloc(len);
    if (!m) return NULL;
    memset(m, 0, len);
    /* 前 8 字节放 diff 像素计数（小端），后面是位掩码 */
    uint32_t cnt = 0;
    for (int y = 0; y < rows; y++) {
        const uint8_t *a = cur  + (size_t)y * w * 2;
        const uint8_t *b = prev + (size_t)y * w * 2;
        for (int x = 0; x < w; x++) {
            if (a[x * 2] != b[x * 2] || a[x * 2 + 1] != b[x * 2 + 1]) {
                m[8 + (size_t)y * row_bytes + (x >> 3)] |= (uint8_t)(1u << (x & 7));
                cnt++;
            }
        }
    }
    memcpy(m, &cnt, 4);
    *out_len = len;
    return m;
}

/* 抓一页：snapshot → 发 base 帧 → 等一帧 → 发 diff 掩码 */
static void shot_one_page(lv_obj_t *scr)
{
    lv_draw_buf_t *dsc = lv_snapshot_take(scr, LV_COLOR_FORMAT_RGB565);
    if (!dsc) {
        printf("##SHOT %d FAIL take\n", s_page);
        fflush(stdout);
        return;
    }

    const int w      = dsc->header.w;
    const int h      = dsc->header.h;
    const int stride = dsc->header.stride;
    const size_t sz  = (size_t)stride * h;

    /* 维度以文本发出，host 端不必解析位域头 */
    printf("##SHOT %d MODE=base\n", s_page);
    printf("##HDR %d %d %d %d\n", w, h, stride, (int)dsc->header.cf);
    fflush(stdout);

    /* 先备好用于 diff 的整洁缓冲（stride 可能与 w*2 不同，按 w 逐行拷） */
    uint8_t *clean = malloc((size_t)w * h * 2);
    if (clean) {
        for (int y = 0; y < h; y++)
            memcpy(clean + (size_t)y * w * 2, dsc->data + (size_t)y * stride, (size_t)w * 2);
    }

    dump_block(dsc->data, sz);
    printf("\n##END %d\n", s_page);
    fflush(stdout);

    if (clean) {
        if (s_snap_raw && s_page > 0) {
            size_t mlen = 0;
            uint8_t *m = diff_mask(clean, s_snap_raw, w, h, &mlen);
            if (m) {
                printf("##SHOT %d MODE=diff\n", s_page);
                printf("##MASK %zu\n", mlen);      /* 非图像载荷：直接给字节数，避免 stride/h 语义歧义 */
                fflush(stdout);
                dump_block(m, mlen);
                printf("\n##END %d\n", s_page);
                fflush(stdout);
                free(m);
            }
        }
        free(s_snap_raw);
        s_snap_raw = clean;
    }

    lv_draw_buf_destroy(dsc);
}

/* 一次性定时器回调：跑在 lvgl 任务里（安全），逐页抓图 */
static void shot_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_page > SHOT_PAGE_LAST) {
        printf("##DONE\n");
        fflush(stdout);
        ESP_LOGI(TAG_SHOT, "抓图完成（页 %d..%d）", SHOT_PAGE_FIRST, SHOT_PAGE_LAST);
        free(s_snap_raw);
        s_snap_raw = NULL;
        return;
    }

    if (s_stage == 0) {
        ESP_LOGI(TAG_SHOT, "抓图 第 %d 页 ...", s_page);
        shot_one_page(lv_screen_active());
        s_stage = 1;
        return;                         /* 等下一 tick 再切页 */
    }

    /* 切到下一页（AUTO 轮播由 ui.c 自己的 tick 驱动；这里直接调 ui_goto 更确定） */
    s_page++;
    s_stage = 0;
    if (s_page <= SHOT_PAGE_LAST) ui_goto(s_page);
}

/* 页号哨兵：表示「抓当前屏，不切页」。
 * 用途 = 抓 ui_goto 管辖之外的自建屏（例如 cjk_probe 的中文字体冒烟页）。
 * ⚠️ 必须比所有真实页号大，否则会被上面的 ui_goto(s_page) 当成合法页号调用。
 */
#define SHOT_PAGE_CURRENT 99

void shot_probe_start(void)
{
    s_page = SHOT_PAGE_FIRST;
    s_stage = 0;
    s_snap_raw = NULL;

    /* 🕳️ 必须**先切到首页**再抓（2026-09-30 踩到）：
     *   回调第一次 tick 抓的是「当前屏」。若 SHOT_PAGE_FIRST > 0，此刻屏上还是
     *   ui_init 末尾 ui_goto(0) 留下的第 0 页（MENU），却会被打上 `##SHOT <FIRST>` 的标签
     *   → 拉回来的图是**别的页**，而文件名/编号全对，看起来一切正常（最毒的一类假通过）。
     *   FIRST=0 时碰巧正确（第 0 页本就在屏上），所以这个 bug 一直藏着没暴露。
     *   此刻 LVGL 仍独占（lcd_hw_start 未调），ui_goto 无并发风险。
     * ⚠️ SHOT_PAGE_CURRENT 是「不导航」哨兵，绝不能喂给 ui_goto（越界）。 */
    if (SHOT_PAGE_FIRST > 0 && SHOT_PAGE_FIRST < SHOT_PAGES) {
        ui_goto(SHOT_PAGE_FIRST);
    }

    ESP_LOGW(TAG_SHOT, "!!! SHOT_PROBE 已启用：dump 第 %d..%d 页到串口（仅排查用）!!!",
             SHOT_PAGE_FIRST, SHOT_PAGE_LAST);
    /* 首个 tick 稍晚一点，让 lcd_hw_start 之后的初始化 / 其它探针文本先稳定 */
    lv_timer_t *t = lv_timer_create(shot_timer_cb, SHOT_FIRST_DELAY_MS, NULL);
    lv_timer_set_repeat_count(t, (SHOT_PAGE_LAST - SHOT_PAGE_FIRST + 1) * 2 + 4);
}

#endif /* SHOT_PROBE */
