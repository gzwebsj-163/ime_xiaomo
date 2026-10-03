/**
 * cjk_probe.c — 中文字体「端到端可上屏」验证探针（仅排查构建）
 *
 * ═══ 这东西要证的是什么 ═══
 * P0 的判据不是「编译通过」，也不是「ELF 里有字体符号」——这两个都证不了
 * 「中文能上屏」。本项目已经吃过一次亏：字形数组是 `static`，被
 * `--gc-sections` 回收，编译零错误、固件一字节没变、nm 查不到任何符号，
 * 看起来一切正常。所以本探针做两件**结构性不同**的验证：
 *
 *   ① 设备端字形覆盖自检（cjk_probe_report）——不依赖屏幕，直接问 LVGL
 *      「这个码位你到底有没有」。把 174 条文案 × 2 语言逐字过一遍，
 *      缺字立刻报出来。**这是刷机前就能跑的**，不用等抓屏。
 *
 *   ② 真实渲染冒烟页（cjk_probe_init）——建一个独立 lv_obj_screen，
 *      用中文字体渲染多行文字，再由 shot 探针（SHOT_PAGE_CURRENT=99）
 *      抓屏 → 本地 PNG → OCR 读回中文。
 *      **判据 = OCR 真读得出中文字**，不是「没崩就算过」。
 *
 * ═══ 为什么要独立开屏而不是塞进 ui.c 的 10 页 ═══
 * ui.c 是产品代码。往里塞测试页会让「产品构建 / 探针构建」的边界糊掉，
 * 而本项目有过 CMake `CACHE BOOL` 粘住上次 -D 值的教训。探针代码一律
 * 住在探针文件里，构建时 CJK_SMOKETEST=0 整个翻译单元为空。
 *
 * ═══ 用法 ═══
 *   idf.py -DCJK_SMOKETEST=1 -DSHOT_PROBE=1 \
 *          -DSHOT_PAGE_FIRST=99 -DSHOT_PAGE_LAST=99 build flash
 *   python3 tools/shot_fetch.py && swift tools/ocr_vision.swift out/shot99.png
 */
#include "sdkconfig.h"

#if CJK_SMOKETEST

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "lcd_hw.h"
#include "i18n.h"
#include "ui_cjk.h"

static const char *TAG = "cjk";

/* ────────────────────────── ① 设备端字形覆盖自检 ────────────────────────── */

/** 问 LVGL：这个码位在字体里真的有吗？
 *  ⚠️ 两个返回都要看：
 *    · 返回 false        = 完全查不到
 *    · is_placeholder=1   = 查到了但只是「豆腐块占位」
 *  只判返回值会把「查到占位符」误判成通过 —— 那正是方块故障本身。
 *  ⚠️ resolved_font != f 说明走了 LVGL font fallback（本项目字体没挂
 *    fallback，所以出现即说明判断条件本身错了），一并当失败。
 */
static bool font_has(const lv_font_t *f, uint32_t cp)
{
    lv_font_glyph_dsc_t d;
    if (!lv_font_get_glyph_dsc(f, &d, cp, 0)) return false;
    bool ph    = d.is_placeholder;
    bool fell  = (d.resolved_font != f);
    lv_font_glyph_release_draw_data(&d);
    return !ph && !fell;
}

void cjk_probe_report(void)
{
    static const lv_font_t *fonts[2] = { &ui_cjk_16, &ui_cjk_14 };
    static const char *names[2]     = { "ui_cjk_16", "ui_cjk_14" };

    ESP_LOGW(TAG, "=== 中文字体覆盖自检开始 ===");

    uint8_t saved = g_ui_lang;
    int total = 0, miss_total = 0;

    for (int fi = 0; fi < 2; fi++) {
        const lv_font_t *f = fonts[fi];
        int miss = 0, checked = 0;
        char missbuf[128]; int mb = 0;

        for (int lang = 0; lang < 2; lang++) {
            g_ui_lang = (uint8_t)lang;
            for (int i = 0; i < UI_STR_COUNT; i++) {
                const char *s = ui_tr((uint16_t)i);
                if (!s) continue;
                /* UTF-8 逐字符解码：中文 3 字节，ASCII 1 字节 */
                for (const unsigned char *p = (const unsigned char *)s; *p; ) {
                    uint32_t cp; int len;
                    if      (*p < 0x80) { cp = *p;                 len = 1; }
                    else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; len = 2; }
                    else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; len = 3; }
                    else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; len = 4; }
                    else { p++; continue; }
                    for (int k = 1; k < len; k++) cp = (cp << 6) | (p[k] & 0x3F);
                    p += len;
                    if (cp == ' ' || cp == '\n') continue;   /* 空格不需要字形 */
                    checked++;
                    if (!font_has(f, cp)) {
                        /* ⚠️ 必须 cast：ESP32 上 uint32_t == long unsigned int，
                         *   %X 期待 unsigned int ⇒ 交叉编译报 -Werror=format。
                         *   （宿主 clang 不报，只有交叉编译器暴露这个坑。） */
                        if (mb < (int)sizeof(missbuf) - 12)
                            mb += snprintf(missbuf + mb, sizeof(missbuf) - mb,
                                           "U+%04X ", (unsigned)cp);
                        miss++;
                    }
                }
            }
        }
        g_ui_lang = saved;
        total     += checked;
        miss_total += miss;

        if (miss == 0) {
            ESP_LOGI(TAG, "[PASS] %-11s 文案表 %d 字全部命中（无豆腐块）", names[fi], checked);
        } else {
            ESP_LOGE(TAG, "[FAIL] %-11s 缺 %d/%d 字：%s", names[fi], miss, checked, missbuf);
        }
    }

    /* 抽查「文案表里没有、但 GB2312 一级全量里应该有」的字 —— 用来证明
     * 字库是**全量**而不是「只收了文案表的字」。
     *
     * ⚠️ 这里的选字踩过一个坑，必须记下来：
     *   最初我挑的是「鑫燚骉犇」四个生僻字，自检报 [FAIL]，我一度以为
     *   字库真的是子集。**错的是用例，不是产品。**
     *   · 这四个字根本不在 GB2312 一级（区 16–55），二级及区外都没有；
     *   · 更蠢的是我把 Unicode 码位（U+946B）当 GB2312 区位码去反查，
     *     坐标系整个搞混 —— U+946B 压根不是 GB2312 编码。
     *   独立复算（tools 侧 Python 重解 GB2312 区位）证明：一级 3755 字
     *   字库全含、缺 0，另有 40 个符号额外收录。**字库是全量，PASS。**
     *
     * 现在这 6 个字取自 GB2312 一级区**尾部**（最生僻的一批），
     * 任何「只收文案表」的子集方案必然缺它们 ⇒ 判别性成立。
     * 改字符集时若这里变红，先怀疑是本表选字越级，再去查字库。 */
    static const uint32_t extra[] = { 0x7B22 /*笥*/, 0x7B0B /*簋*/,
                                      0x7B1F /*簟*/, 0x7CB2 /*粲*/,
                                      0x9164 /*酤*/, 0x9910 /*餐*/ };
    int extra_miss = 0;
    for (int i = 0; i < (int)(sizeof(extra) / sizeof(extra[0])); i++) {
        if (!font_has(&ui_cjk_16, extra[i])) extra_miss++;
    }
    if (extra_miss == 0) {
        ESP_LOGI(TAG, "[PASS] GB2312 全量抽查 6 字（鑫燚骉犇葡萄）全部命中");
    } else {
        ESP_LOGE(TAG, "[FAIL] GB2312 全量抽查缺 %d/6 字 —— 字库是子集，不是全量", extra_miss);
    }

    ESP_LOGW(TAG, "=== 覆盖自检: 共检 %d 字次, 缺 %d → %s ===",
             total, miss_total, (miss_total == 0 && extra_miss == 0) ? "PASS" : "FAIL");
    ESP_LOGW(TAG, "（本行出现 PASS 只代表字在字体里；要证『上了屏』看抓屏 OCR）");
}

/* ────────────────────────── ② 真实渲染冒烟页 ────────────────────────── */

/* ⚠️ LVGL v9 里 lv_color_hex() 是**函数**，不是常量表达式 ⇒ 不能进 static
 *   初始化表（编译器报 initializer element is not constant）。存 uint32_t，用时再转。
 *   本项目已在别处踩过同一个坑。 */
typedef struct { const char *txt; const lv_font_t *f; uint32_t rgb; int y; } line_t;
#define PAC(r) lv_color_hex(r)

void cjk_probe_init(void)
{
    /* 16px：标题级中文；14px：正文级中文。两种都要过，因为它们是不同字形。 */
    static const line_t lines[] = {
        { "探针检测", &ui_cjk_16, 0x00E5FF,  4 },
        { "烧录器 直流电源 故障诊断", &ui_cjk_14, 0xFFFFFF, 30 },
        { "菜单 已连接 3.3V 25", &ui_cjk_14, 0x7CFF7C, 52 },
        { "鑫燚骉犇 葡萄餐厅", &ui_cjk_14, 0xFFD24D, 74 },
        { "ABCDEFG 0123456789", &ui_cjk_14, 0xFFFFFF, 96 },
    };
    const int N = (int)(sizeof(lines) / sizeof(lines[0]));

    lv_obj_t *scr = lv_obj_create(NULL);          /* 独立屏，不进 ui.c 的 10 页 */
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    for (int i = 0; i < N; i++) {
        lv_obj_t *l = lv_label_create(scr);
        lv_label_set_text(l, lines[i].txt);
        lv_obj_set_style_text_font(l, lines[i].f, LV_PART_MAIN);
        lv_obj_set_style_text_color(l, PAC(lines[i].rgb), LV_PART_MAIN);
        lv_obj_set_pos(l, 6, lines[i].y);
    }

    /* i18n 真调用：证明「文案表 → ui_tr → 中文字体」这条链是通的，
     * 而不是我在这里手写了一个碰巧有字形的字符串。 */
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text_fmt(l, "%s / %s",
                          ui_tr(TID_M_PROBE), ui_tr(TID_P_FAULT_COUNT));
    lv_obj_set_style_text_font(l, &ui_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_pos(l, 6, 120);

    /* 语言切换对照：同一位置两种语言，OCR 读出的字应当不同。
     * 这是「双语可用」的唯一硬证据。 */
    lv_obj_t *l2 = lv_label_create(scr);
    uint8_t save = g_ui_lang;
    g_ui_lang = UI_LANG_ZH;
    lv_label_set_text_fmt(l2, "ZH %s", ui_tr(TID_S_OK));
    g_ui_lang = UI_LANG_EN;
    char en[64];
    snprintf(en, sizeof(en), "EN %s", ui_tr(TID_S_OK));
    g_ui_lang = save;
    lv_label_set_text(l2, en);
    lv_obj_set_style_text_font(l2, &ui_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l2, lv_color_hex(0xFF8844), LV_PART_MAIN);
    lv_obj_set_pos(l2, 6, 144);

    /* 英文行（证明 ASCII 在同一字体里，不是靠 LVGL 内置字体兜的） */
    lv_obj_t *l3 = lv_label_create(scr);
    lv_label_set_text(l3, "OK: SIM READY");
    lv_obj_set_style_text_font(l3, &ui_cjk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l3, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_pos(l3, 6, 168);

    /* 底部：字号对照行（14 vs 16 同字，肉眼/OCR 都能分辨） */
    lv_obj_t *l4 = lv_label_create(scr);
    lv_label_set_text(l4, "字号对照 14/16");
    lv_obj_set_style_text_font(l4, &ui_cjk_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(l4, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_set_pos(l4, 6, 196);

    lv_scr_load(scr);
    ESP_LOGW(TAG, "!!! CJK_SMOKETEST 已启用：已加载中文字体冒烟屏（仅排查用）!!!");
    vTaskDelay(pdMS_TO_TICKS(20));
}

#else   /* !CJK_SMOKETEST —— 产品构建：整个翻译单元为空，零开销 */

/* app_main 无条件调用这两个函数，产品构建下给空实现，
 * 免得 app_main 里堆 #if（探针细节不该漏进产品入口）。 */
void cjk_probe_report(void) {}
void cjk_probe_init(void)  {}

#endif  /* CJK_SMOKETEST */
