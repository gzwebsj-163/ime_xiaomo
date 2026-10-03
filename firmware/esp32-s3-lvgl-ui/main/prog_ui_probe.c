/**
 * prog_ui_probe.c — 「物理按键 → 烧录后端」写路径的真机自证（仅排查用）
 *
 * 为什么要有它（2026-10-01）：
 *   到这一刻，烧录器已有两条链各证了一半，**唯独中间那一段没有独立证据**：
 *     · PROG_SELFTEST（prog_api.c）证的是「后端自己跑得对」——程序**直接调** prog_api_start()；
 *     · SHOT_PROBE          证的是「后端数据能上屏」——**读**路径；
 *     · 而「按一下键 → 后端真的开跑」这条**写**路径，
 *       过去只能靠人肉按键 + 看屏幕/看串口，无法无人值守地自证。
 *
 *   本探针把「按键」变成**可注入的合成事件**，塞进 btn 的**真实环形队列**，
 *   于是走的是与物理按键完全相同的路径：
 *       btn_inject → 环形队列 → handle_buttons → prog_page_key → prog_api_start → 后端 op
 *   整条写路径可在无人值守下跑完并留下串口证据。
 *
 * 判据（看串口日志，不需要看图）：
 *   1) 每一步都应有 `[pui] [注入 i/N] …` 与对应的 `key=… -> …` 两行（注入→被消费）；
 *   2) 执行操作时应有成对的
 *        `[prog] ===== op READ ID start (s3-spi / SIM) =====`
 *        `[prog] ===== op READ ID end rc=0 (OK) =====`
 *      —— 出现即证明「按键真的把后端开跑」；
 *   3) 末尾 REAL 段必须 `rc!=0`（没接芯片时的**诚实失败**），
 *      与 PROG_SELFTEST 的 S7 是同一条断言的**按键版本**。
 *
 * 与 BTN_SELFTEST 的区别：BTN_SELFTEST 注入的是**菜单导航**手势；本探针专打 PROG 页的**烧录操作**手势。
 *
 * ⚠️ 排查专用：产品构建 PROG_UI_TEST=0 → 整个文件编译为空，零开销。
 */
#include "sdkconfig.h"

#if PROG_UI_TEST

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "btn.h"

#define TAG_PUI "pui"

typedef struct {
    btn_id_t    id;
    bool        lg;
    uint32_t    wait_ms;      /* 发出前先等待（给上一步的 UI/后端留出处理时间） */
    const char *desc;         /* 期望到达的状态，用来和串口里的 key=… -> … 行对拍 */
} pui_step_t;

/* FLASH 页行序（与 ui.c 的 build_flash 一致）：
 *   0=TARGET 1=DRIVER 2=IMAGE | 3=READ ID 4=ERASE 5=PROGRAM 6=VERIFY 7=ISP SYNC
 * 主菜单行序（ui.c MENU_TID）：0=探针 1=烧录 2=引脚 3=状态 4=设置 5=关于 6=向导
 *
 * 起始态是确定的（ui.c: s_sel=0, s_in_menu=true, s_fl_sel=0, s_auto_on=false），
 * 且开机页 2.4s 后自动进主菜单 —— 所以每一步的期望状态都能写死、可对拍。 */
static const pui_step_t S_STEPS[] = {
    /* ---------- ① 进 FLASH 页：菜单 0(探针) → 1(烧录)，长按 UP 进入 ---------- */
    { BTN_DOWN, false, 3200, "菜单光标 0->1(烧录)" },
    { BTN_UP,   true,   400, "长按UP进入 FLASH 页 (s_in_menu=false, page=4)" },

    /* ---------- ② SIM READ ID：页内光标 0 -> 3 ---------- */
    { BTN_DOWN, false,  400, "页内光标 0->1" },
    { BTN_DOWN, false,  400, "页内光标 1->2" },
    { BTN_DOWN, false,  400, "页内光标 2->3(READ ID)" },
    { BTN_UP,   true,   400, "长按UP执行 READ ID (SIM) —— 期望 [prog] op READ ID start/end rc=0" },

    /* ---------- ③ SIM PROGRAM（擦→页编程→自校验 一条龙） ---------- */
    { BTN_DOWN, false, 2500, "页内光标 3->4" },
    { BTN_DOWN, false,  400, "页内光标 4->5(PROGRAM)" },
    { BTN_UP,   true,   400, "长按UP执行 PROGRAM (SIM) —— 期望 end rc=0 (OK)" },

    /* ---------- ④ SIM VERIFY ---------- */
    { BTN_DOWN, false, 4000, "页内光标 5->6(VERIFY)" },
    { BTN_UP,   true,   400, "长按UP执行 VERIFY (SIM) —— 期望 end rc=0 (OK)" },

    /* ---------- ⑤ 切 DRIVER：SIM -> REAL（设置行 1） ---------- */
    { BTN_UP,   false, 2500, "页内光标 6->5" },
    { BTN_UP,   false,  400, "页内光标 5->4" },
    { BTN_UP,   false,  400, "页内光标 4->3" },
    { BTN_UP,   false,  400, "页内光标 3->2" },
    { BTN_UP,   false,  400, "页内光标 2->1(DRIVER)" },
    { BTN_UP,   true,   400, "长按UP切换 DRIVER SIM->REAL" },

    /* ---------- ⑥ REAL READ ID：没接芯片 → 必须诚实失败（写路径版 S7） ---------- */
    { BTN_DOWN, false,  400, "页内光标 1->2" },
    { BTN_DOWN, false,  400, "页内光标 2->3(READ ID)" },
    { BTN_UP,   true,   400, "长按UP执行 READ ID (REAL) —— 期望 end rc!=0（诚实 NOFLASH）" },

    /* ---------- ⑦ 收尾：DRIVER 切回 SIM，别把 REAL 留给用户 ---------- */
    { BTN_UP,   false, 3000, "页内光标 3->2" },
    { BTN_UP,   false,  400, "页内光标 2->1(DRIVER)" },
    { BTN_UP,   true,   400, "长按UP切换 DRIVER REAL->SIM（恢复默认）" },
};

static void pui_task(void *arg)
{
    (void)arg;
    const int n = (int)(sizeof(S_STEPS) / sizeof(S_STEPS[0]));

    ESP_LOGW(TAG_PUI, "=== PROG UI 写路径自检开始：注入 %d 个合成按键事件 ===", n);
    ESP_LOGW(TAG_PUI, "（走真实事件队列 → handle_buttons → prog_page_key → prog_api_start，不碰物理引脚）");

    for (int i = 0; i < n; i++) {
        vTaskDelay(pdMS_TO_TICKS(S_STEPS[i].wait_ms));
        ESP_LOGW(TAG_PUI, "[注入 %d/%d] %s  (%s%s)", i + 1, n, S_STEPS[i].desc,
                 S_STEPS[i].id == BTN_UP ? "UP" :
                 S_STEPS[i].id == BTN_DOWN ? "DOWN" : "BACK",
                 S_STEPS[i].lg ? "-LONG" : "");
        btn_inject(S_STEPS[i].id, S_STEPS[i].lg);
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG_PUI, "=== PROG UI 写路径自检结束（对照上面成对的 [prog] op … start/end 行判成败）===");
    vTaskDelete(NULL);
}

void prog_ui_probe_start(void)
{
    /* 任务自己先睡一会儿再开始注入，避免与 boot 日志/后端初始化抢串口 */
    xTaskCreate(pui_task, "puitest", 3072, NULL, 4, NULL);
}

#endif /* PROG_UI_TEST */
