/**
 * ui.c — 「AI 远程调试器」界面 v3（中文产线版）  2026-10-02 全面重设计
 * ═══════════════════════════════════════════════════════════════════════════
 * 为什么要重设计（上一版的根本问题不是"不好看"，是"假"）：
 *   旧 UI 复刻自立创开源 160x80 工程，10 页里有 8 页的数据是**编出来的**：
 *     · SPI 页 TX/RX = (i*7+3)&0xFF        —— 从没碰过任何总线
 *     · PWM 波形 = 定时器每 300ms 推一格    —— 从没配置过 LEDC
 *     · RX 终端 = 10 条 demo 字符串循环     —— 从没打开过串口
 *     · CONFIG / 电池 / RX-TX 计数 = 递增假数
 *   真正连了后端的只有 2 页（STATUS 真 WiFi、PROG 真烧录）。
 *   ⇒ 本版第一原则：**页面上出现的每个数字都必须有真源头**。
 *      没有后端支撑的页面一律删除，不保留"看起来在干活"的装饰。
 *
 * 本版页面（9 页）与各自数据源，逐页可查：
 *   0 BOOT   开机页           静态 + 2.4s 自动进菜单（按返回跳过）
 *   1 WIZ    首启向导          ui_cfg.c（NVS，判据"断电不丢"）
 *   2 HOME   主菜单（列表）    纯导航
 *   3 PROBE  探针检测          prog_api → hw_pin（真读 0x9F JEDEC / ISP 0x7F）
 *   4 FLASH  烧录器             prog_api（5 操作；进度/日志全来自后端）
 *   5 PINS   引脚档案          hw_pin 档案表（9 信号 → 真 GPIO 号）
 *   6 STATUS 状态信息          wifi_sta.c + heap/flash/esp_timer
 *   7 SETUP  系统设置          ui_cfg.c（语言/时间/网络时间/保存/出厂）
 *   8 ABOUT  关于              esp_app_desc / esp_chip_info / esp_flash
 *
 * ── 视觉语言 ─────────────────────────────────────────────────────────────
 *   纯黑底 + 霓虹强调色 + **一套字体**。这是被字体现实逼出来的：
 *   本工程只编进 ui_cjk_14 / ui_cjk_16，它们**不含 FontAwesome**，
 *   所以 LV_SYMBOL_* 会渲染成缺字方块 —— 旧版靠 Montserrat 的 59 个符号撑图标，
 *   改中文后那条路就断了。
 *   ⇒ 图标语言改用「字体里确实存在的字符」：●○■□▲→←↑↓★☆√✕↻ 与汉字。
 *   ⚠️ 换符号前先查 tools/font_charsets.json，别凭想象填（会出方块）。
 *
 * ── 按键语义（与旧版一致，物理不变）─────────────────────────────────────
 *   短按 上/下 ：同层移动光标
 *   长按 上    ：进入 / 执行当前项
 *   长按 下    ：返回上一级
 *   短按 返回  ：直接回主菜单（任何层级）
 *   长按 返回  ：开关 AUTO 轮播（保留旧行为）
 *   · 向导/探针/烧录/设置 各自有 page_key() 钩子，先由本页消费，不收才落回全局。
 *
 * ── 线程模型（硬约束，勿破）─────────────────────────────────────────────
 *   任何 hw_pin / prog_api 的**阻塞**调用都不得从 LVGL 任务发起。
 *   UI 只调 prog_api_status() 读加锁快照；真活由 prog_api 的 worker 干。
 *   WiFi 同理：只调 wifi_st_get() 拿快照。
 * ═══════════════════════════════════════════════════════════════════════════
 */
#include "ui.h"
#include "btn.h"
#include "wifi_sta.h"
#include "prog_api.h"
#include "prog_hw.h"
#include "hw_pin.h"
#include "ui_cfg.h"
#include "i18n.h"
#include "ui_cjk.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_netif_sntp.h"
#include "lvgl.h"

#define TAG_UI "ui"

/* ================================ 色板 ================================ */
#define C_BG      lv_color_hex(0x000000)
#define C_BLACK   lv_color_hex(0x000000)
#define C_LINE    lv_color_hex(0x1B2530)
#define C_DIM     lv_color_hex(0x3A4A57)
#define C_GRAY    lv_color_hex(0x6E8496)
#define C_TEXT    lv_color_hex(0xE6F7FF)
#define C_CYAN    lv_color_hex(0x00E5FF)
#define C_GREEN   lv_color_hex(0x39FF14)
#define C_YELLOW  lv_color_hex(0xFFC400)
#define C_RED     lv_color_hex(0xFF3B57)
#define C_MAGENTA lv_color_hex(0xFF2D95)
#define C_MINT    lv_color_hex(0x00FF9C)

/* 字体：全工程只有这两款（含 ASCII + GB2312 一级常用字） */
#define F16 (&ui_cjk_16)
#define F14 (&ui_cjk_14)

/* ================================ 几何 ================================ */
#define SCR_W   320
#define SCR_H   240
#define TITLE_H 32
#define BODY_Y  34
#define HINT_Y  203

enum {
    PG_BOOT = 0, PG_WIZ, PG_HOME, PG_PROBE, PG_FLASH,
    PG_PINS, PG_STATUS, PG_SETUP, PG_ABOUT, PG_MAX
};

typedef struct { uint16_t tid; uint32_t accent; } pageinfo_t;
static const pageinfo_t PINFO[PG_MAX] = {
    { TID_W_BRAND,         0x00E5FF },
    { TID_W_SETUP_FIRST,   0x00E5FF },
    { TID_M_HOME,          0x00E5FF },
    { TID_P_PROBE_TITLE,   0x00E5FF },
    { TID_M_FLASH,         0x00FF9C },
    { TID_P_PINS_TITLE,    0xFFC400 },
    { TID_P_STA_TITLE,     0x00E5FF },
    { TID_P_SET_TITLE,     0xFF2D95 },
    { TID_P_ABOUT_TITLE,   0x00E5FF },
};
static inline lv_color_t pac(int i) { return lv_color_hex(PINFO[i].accent); }

/* ================================ 运行时状态 ================================ */
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_page[PG_MAX];
static lv_obj_t *s_title[PG_MAX];
static lv_obj_t *s_dot[PG_MAX];

static int      s_cur = PG_BOOT;
static bool     s_in_menu = true;
static int      s_sel = 0;
static uint32_t s_tick_ms = 0;
static int      s_boot_ms = 0;

static bool      s_auto_on = false;
static int       s_auto_ms = 4000;
static lv_obj_t *s_lbl_mode = NULL;
static lv_obj_t *s_pill_mode = NULL;

/* 前向声明 */
static void home_apply_sel(void);

/* ================================ 绘制原语 ================================ */
static lv_obj_t *mk_bar(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, c, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollable(o, false);
    return o;
}

static lv_obj_t *mk_txt(lv_obj_t *parent, const char *txt, const lv_font_t *f,
                        lv_color_t col, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, col, LV_PART_MAIN);
    lv_obj_set_pos(l, x, y);
    return l;
}
static lv_obj_t *mk_tr(lv_obj_t *parent, uint16_t tid, const lv_font_t *f,
                       lv_color_t col, int x, int y)
{
    return mk_txt(parent, TR(tid), f, col, x, y);
}

static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h,
                         lv_color_t accent, lv_opa_t border_opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, accent, LV_PART_MAIN);
    lv_obj_set_style_border_opa(o, border_opa, LV_PART_MAIN);
    lv_obj_set_scrollable(o, false);
    return o;
}

static void mk_section(lv_obj_t *parent, const char *txt, lv_color_t accent, int y)
{
    mk_bar(parent, 6, y + 3, 3, 12, accent);
    mk_txt(parent, txt, F14, C_GRAY, 15, y);
}

static void mk_hint(lv_obj_t *p, const char *txt)
{
    mk_txt(p, txt, F14, C_DIM, 10, HINT_Y);
}

static lv_obj_t *mk_page(int i)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollable(p, false);

    if (i == PG_BOOT) return p;

    mk_bar(p, 8, 8, 3, 17, pac(i));
    s_title[i] = mk_tr(p, PINFO[i].tid, F16, C_TEXT, 18, 5);

    lv_obj_t *glow = lv_obj_create(p);
    lv_obj_remove_style_all(glow);
    lv_obj_set_size(glow, SCR_W, 3);
    lv_obj_set_pos(glow, 0, TITLE_H - 3);
    lv_obj_set_style_bg_color(glow, pac(i), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(glow, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(glow, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(glow, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollable(glow, false);

    mk_bar(p, 0, TITLE_H, SCR_W, 1, C_LINE);
    return p;
}

/* ================================ 页 0：BOOT ================================ */
static lv_obj_t *s_boot_seg[7];

static void build_boot(void)
{
    lv_obj_t *p = s_page[PG_BOOT];
    /* ⚠️ 行距必须留够：F16 行高 20、F14 行高 17。2026-10-02 抓图实测，
     *    原来 92/114/132 的间距只有 2px/1px → 两行 F14 在屏上**视觉粘成一行**
     *    （离线 OCR 把 "AI WIRELESS DEBUGGER" 和 "ESP32-S3 LVGL" 识别成一个字符串）。
     *    这里统一按「上一行行高 + ≥7px 间隙」排，改完再抓图确认没并行。 */
    mk_bar(p, 26, 78, 4, 46, C_CYAN);
    mk_tr(p, TID_W_BRAND, F16, C_TEXT, 44, 82);          /* 82 + 20 = 102 */
    mk_txt(p, "AI WIRELESS DEBUGGER", F14, C_GRAY, 44, 116);   /* 116 + 17 = 133 */
    mk_txt(p, "ESP32-S3  LVGL", F14, C_DIM, 44, 140);          /* 140 + 17 = 157 */
    mk_tr(p, TID_W_BOOTING, F14, C_CYAN, 44, 170);
    for (int k = 0; k < 7; k++)
        s_boot_seg[k] = mk_bar(p, 44 + k * 16, 196, 12, 4, C_LINE);
}

/* ================================ 页 1：WIZARD ================================ */
#define WIZ_TFIELD  5
#define WIZ_ROWS    8

static int  s_wiz_step = 0;
static int  s_wiz_sel  = 0;
static int  s_wiz_t[WIZ_TFIELD];
static lv_obj_t *s_wiz_row[WIZ_ROWS];
static lv_obj_t *s_wiz_cursor = NULL;
static lv_obj_t *s_wiz_foot = NULL;

static const uint16_t WIZ_FIELD_TID[WIZ_TFIELD] = {
    TID_W_TIME_YEAR, TID_W_TIME_MONTH, TID_W_TIME_DAY, TID_W_TIME_HOUR, TID_W_TIME_MIN
};

static void wiz_time_from_now(void)
{
    int64_t e = ui_cfg_now();
    time_t tt = (time_t)(e > UI_CFG_MIN_EPOCH ? e : UI_CFG_MIN_EPOCH);
    struct tm tmv;
    gmtime_r(&tt, &tmv);
    s_wiz_t[0] = tmv.tm_year + 1900;
    s_wiz_t[1] = tmv.tm_mon + 1;
    s_wiz_t[2] = tmv.tm_mday;
    s_wiz_t[3] = tmv.tm_hour;
    s_wiz_t[4] = tmv.tm_min;
}

static int64_t wiz_time_epoch(void)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = s_wiz_t[0] - 1900;
    tmv.tm_mon  = s_wiz_t[1] - 1;
    tmv.tm_mday = s_wiz_t[2];
    tmv.tm_hour = s_wiz_t[3];
    tmv.tm_min  = s_wiz_t[4];
    tmv.tm_isdst = 0;
    time_t t = mktime(&tmv);
    return (t > 0) ? (int64_t)t : UI_CFG_MIN_EPOCH;
}

static void build_wiz(void)
{
    lv_obj_t *p = s_page[PG_WIZ];
    s_wiz_foot = mk_txt(p, "", F14, C_CYAN, 14, HINT_Y);

    /* 0,1 = 语言选项 */
    s_wiz_row[0] = mk_tr(p, TID_W_LANG_ZH, F16, C_TEXT, 120, 84);
    s_wiz_row[1] = mk_tr(p, TID_W_LANG_EN, F16, C_TEXT, 120, 120);
    for (int i = 0; i < 2; i++)
        lv_obj_set_style_pad_all(s_wiz_row[i], 3, LV_PART_MAIN);

    /* 2..6 = 时间字段；7 = 下一步 */
    for (int i = 0; i < WIZ_TFIELD; i++)
        s_wiz_row[2 + i] = mk_txt(p, "--", F16, C_TEXT, 108, 56 + i * 27);
    s_wiz_row[7] = mk_tr(p, TID_W_TIME_NEXT, F16, C_CYAN, 108, 56 + WIZ_TFIELD * 27);

    s_wiz_cursor = mk_bar(p, 104, 0, 150, 22, C_CYAN);
    lv_obj_move_background(s_wiz_cursor);
}

static void wiz_refresh(void)
{
    const bool is_lang = (s_wiz_step == 0);
    const int rows = is_lang ? 2 : (WIZ_TFIELD + 1);

    for (int i = 0; i < WIZ_ROWS; i++) {
        if (!s_wiz_row[i]) continue;
        lv_obj_set_hidden(s_wiz_row[i], is_lang ? (i >= 2) : (i < 2));
    }

    if (is_lang) {
        lv_obj_set_pos(s_wiz_cursor, 112, 82 + s_wiz_sel * 36);
        lv_obj_set_size(s_wiz_cursor, 96, 26);
        for (int i = 0; i < 2; i++) {
            const bool sel = (i == s_wiz_sel);
            lv_obj_set_style_text_color(s_wiz_row[i], sel ? C_BLACK : C_TEXT, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_wiz_row[i], C_CYAN, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_wiz_row[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        }
    } else {
        lv_obj_set_pos(s_wiz_cursor, 104, 54 + s_wiz_sel * 27);
        lv_obj_set_size(s_wiz_cursor, 170, 23);
        for (int i = 0; i < WIZ_TFIELD; i++)
            lv_obj_set_style_text_color(s_wiz_row[2 + i], (i == s_wiz_sel) ? C_BLACK : C_TEXT, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_wiz_row[7], (s_wiz_sel == WIZ_TFIELD) ? C_BLACK : C_CYAN, LV_PART_MAIN);
    }

    char b[40];
    for (int i = 0; i < WIZ_TFIELD; i++) {
        snprintf(b, sizeof(b), (i == 0) ? "%s  %04d" : "%s  %02d",
                 TR(WIZ_FIELD_TID[i]), s_wiz_t[i]);
        lv_label_set_text(s_wiz_row[2 + i], b);
    }

    lv_label_set_text(s_title[PG_WIZ],
        s_wiz_step == 0 ? TR(TID_W_STEP_LANG) :
        s_wiz_step == 1 ? TR(TID_W_STEP_TIME) : TR(TID_W_STEP_DONE));
    if (s_wiz_step == 2)                       lv_label_set_text(s_wiz_foot, TR(TID_W_ENTER_MENU));
    else if (is_lang)                          lv_label_set_text(s_wiz_foot, TR(TID_S_HINT_NAV));
    else if (s_wiz_sel == WIZ_TFIELD)          lv_label_set_text(s_wiz_foot, TR(TID_W_TIME_NEXT));
    else                                       lv_label_set_text(s_wiz_foot, TR(TID_S_HINT_EDIT));
    (void)rows;
}

static int wiz_page_key(btn_id_t id, bool is_long)
{
    if (id == BTN_BACK && !is_long) {
        if (s_wiz_step > 0) { s_wiz_step--; s_wiz_sel = 0; wiz_refresh(); }
        else { s_in_menu = true; ui_goto(PG_HOME); home_apply_sel(); }
        return 0;
    }
    if (id == BTN_BACK) return 1;

    const int rows = (s_wiz_step == 0) ? 2 : (WIZ_TFIELD + 1);

    if (!is_long) {
        s_wiz_sel = (s_wiz_sel + rows + ((id == BTN_UP) ? -1 : 1)) % rows;
        wiz_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;

    if (s_wiz_step == 0) {                       /* 选语言 → 存 + 重建（文案要换）*/
        g_ui_lang = (uint8_t)s_wiz_sel;
        (void)ui_cfg_set_lang((uint8_t)s_wiz_sel);
        wiz_time_from_now();
        s_wiz_step = 1; s_wiz_sel = 0;
        ui_rebuild();
        return 0;
    }
    if (s_wiz_step == 1) {
        if (s_wiz_sel == WIZ_TFIELD) {           /* 下一步 → 完成 */
            s_wiz_step = 2; s_wiz_sel = 0;
            ui_cfg_set_now(wiz_time_epoch());
            wiz_refresh();
        } else {                                 /* 调字段 */
            static const int lo[WIZ_TFIELD] = { 2024, 1, 1, 0, 0 };
            static const int hi[WIZ_TFIELD] = { 2099, 12, 31, 23, 59 };
            int v = s_wiz_t[s_wiz_sel] + 1;
            if (v > hi[s_wiz_sel]) v = lo[s_wiz_sel];
            s_wiz_t[s_wiz_sel] = v;
            wiz_refresh();
        }
        return 0;
    }
    /* 完成：写入 NVS 并进主菜单 */
    ui_cfg_set_now(wiz_time_epoch());
    {
        ui_cfg_t c;
        c.ver = UI_CFG_VER; c.lang = (uint8_t)g_ui_lang; c.sntp = ui_cfg_sntp_get();
        c._pad = 0; c.epoch = ui_cfg_now();
        (void)ui_cfg_save(&c);
    }
    s_in_menu = true; s_sel = 0;
    ui_goto(PG_HOME);
    home_apply_sel();
    return 0;
}

/* ================================ 页 2：HOME ================================ */
#define MENU_N 7
static const uint16_t MENU_TID[MENU_N] = {
    TID_M_PROBE, TID_M_FLASH, TID_M_PINS, TID_M_STATUS, TID_M_SETUP, TID_M_ABOUT, TID_M_WIZ
};
static const int   MENU_PG[MENU_N] = { PG_PROBE, PG_FLASH, PG_PINS, PG_STATUS, PG_SETUP, PG_ABOUT, PG_WIZ };
static const char *MENU_GLYPH[MENU_N] = { "○", "↓", "■", "◎", "☆", "◆", "↻" };

static lv_obj_t *s_menu_row[MENU_N];

static void build_home(void)
{
    lv_obj_t *p = s_page[PG_HOME];
    const int rh = 20, gap = 2;
    int y = BODY_Y + 2;

    for (int k = 0; k < MENU_N; k++) {
        lv_obj_t *row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, SCR_W - 16, rh);
        lv_obj_set_pos(row, 8, y);
        lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_scrollable(row, false);
        s_menu_row[k] = row;

        lv_obj_t *g = lv_label_create(row);
        lv_label_set_text(g, MENU_GLYPH[k]);
        lv_obj_set_style_text_font(g, F16, LV_PART_MAIN);
        lv_obj_align(g, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, TR(MENU_TID[k]));
        lv_obj_set_style_text_font(t, F16, LV_PART_MAIN);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 32, 0);

        lv_obj_t *a = lv_label_create(row);
        lv_label_set_text(a, "→");
        lv_obj_set_style_text_font(a, F14, LV_PART_MAIN);
        lv_obj_align(a, LV_ALIGN_RIGHT_MID, -8, 0);

        y += rh + gap;
    }
    mk_hint(p, TR(TID_S_HINT_NAV));

    /* ⚠️ 必须在这里就上一次色：菜单项的文字是在 home_apply_sel() 里才被赋色的，
     *    而那一层又是用 lv_obj_remove_style_all() 建的（无默认样式）。
     *    如果只等「用户按键时」才调用，首次进入主菜单会看到**一整页隐形的菜单**
     *    （黑字画在黑底上）—— 2026-10-02 抓图探针实测踩到，非黑像素只有 3219。
     *    旧代码本来就在 build_home 末尾调了一次，重写时漏掉，这里补回并写清原因。 */
    home_apply_sel();
}

static void home_apply_sel(void)
{
    for (int k = 0; k < MENU_N; k++) {
        const bool sel = (k == s_sel);
        lv_color_t col = lv_color_hex(PINFO[MENU_PG[k]].accent);
        lv_obj_t *row = s_menu_row[k];
        lv_obj_set_style_border_color(row, col, LV_PART_MAIN);
        lv_obj_set_style_border_opa(row, sel ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
        lv_obj_set_style_bg_color(row, sel ? col : C_BG, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(row, sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(row, sel ? 10 : 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(row, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(row, col, LV_PART_MAIN);

        uint32_t n = lv_obj_get_child_count(row);
        for (uint32_t i = 0; i < n; i++)
            lv_obj_set_style_text_color(lv_obj_get_child(row, i), sel ? C_BLACK : col, LV_PART_MAIN);
    }
}

/* ================================ 页 3：PROBE ================================ */
static const char *PROBE_TGT[3] = { "s3-spi", "w25q", "s3-isp" };
static const char *PROBE_DRV[2] = { "SIM", "REAL" };
static int  s_pr_tgt = 0, s_pr_drv = 0, s_pr_sel = 0;
static lv_obj_t *s_pr_val[2];
static lv_obj_t *s_pr_cursor;
static lv_obj_t *s_pr_res[4];

static const char *jedec_vendor(uint8_t mfr)
{
    switch (mfr) {
    case 0xEF: return "Winbond";
    case 0xC8: return "GigaDevice";
    case 0x1F: return "Atmel/Adesto";
    case 0x20: return "Micron/ST";
    case 0xBF: return "SST";
    case 0x01: return "Spansion";
    case 0xC2: return "Macronix";
    case 0x62: return "SANYO";
    default:   return "Unknown";
    }
}

static void build_probe(void)
{
    lv_obj_t *p = s_page[PG_PROBE];
    lv_obj_t *ca = mk_card(p, 8, BODY_Y, SCR_W - 16, 58, C_CYAN, LV_OPA_40);
    static const uint16_t key[2] = { TID_P_PROBE_PROFILE, TID_P_PROBE_DRIVER };
    for (int k = 0; k < 2; k++) {
        const int y = 6 + k * 22;
        mk_tr(ca, key[k], F14, C_GRAY, 14, y + 2);
        s_pr_val[k] = mk_txt(ca, "-", F16, C_CYAN, 130, y);
    }
    s_pr_cursor = mk_bar(ca, 8, 4, SCR_W - 32, 21, C_CYAN);
    lv_obj_move_background(s_pr_cursor);

    lv_obj_t *cb = mk_card(p, 8, BODY_Y + 62, SCR_W - 16, 104, C_CYAN, LV_OPA_30);
    mk_section(cb, TR(TID_P_PROBE_START), C_CYAN, 2);
    mk_txt(cb, "0x9F  RDID        ISP 0x7F", F14, C_DIM, 12, 20);
    static const uint16_t rtid[4] = { TID_P_PROBE_VENDOR, TID_P_PROBE_DEV,
                                      TID_P_PROBE_CAPACITY, TID_P_PROBE_SERIAL };
    for (int i = 0; i < 4; i++) {
        const int y = 40 + i * 16;
        mk_tr(cb, rtid[i], F14, C_GRAY, 14, y);
        s_pr_res[i] = mk_txt(cb, "--", F14, C_TEXT, 104, y);
    }
    mk_hint(p, TR(TID_S_HINT_ACT));
}

static void probe_apply(void)
{
    if (s_pr_val[0]) lv_label_set_text(s_pr_val[0], PROBE_TGT[s_pr_tgt]);
    if (s_pr_val[1]) {
        lv_label_set_text(s_pr_val[1], PROBE_DRV[s_pr_drv]);
        lv_obj_set_style_text_color(s_pr_val[1], s_pr_drv ? C_YELLOW : C_MINT, LV_PART_MAIN);
    }
    (void)prog_api_cfg(s_pr_tgt, s_pr_drv, 0);
}

static void probe_refresh(void)
{
    if (s_pr_cursor) {
        if (s_pr_sel < 2) {
            lv_obj_set_pos(s_pr_cursor, 8, 4 + s_pr_sel * 22);
            lv_obj_set_style_bg_opa(s_pr_cursor, LV_OPA_30, LV_PART_MAIN);
        } else {
            lv_obj_set_style_bg_opa(s_pr_cursor, LV_OPA_TRANSP, LV_PART_MAIN);
        }
    }
    for (int k = 0; k < 2; k++) {
        if (!s_pr_val[k]) continue;
        lv_obj_set_style_text_color(s_pr_val[k],
            (k == s_pr_sel) ? C_TEXT : (k == 1 && s_pr_drv) ? C_YELLOW : C_CYAN, LV_PART_MAIN);
    }

    prog_status_t st;
    prog_api_status(&st);
    char b[32];
    if (st.state == PROG_ST_RUNNING) {
        for (int i = 0; i < 4; i++) lv_label_set_text(s_pr_res[i], "...");
        return;
    }
    if (s_pr_tgt == 2) {                        /* ISP：版本 + PID */
        if (st.state == PROG_ST_DONE && (st.pid || st.ver)) {
            snprintf(b, sizeof(b), "0x%02X", (unsigned)st.ver);
            lv_label_set_text(s_pr_res[0], b);
            snprintf(b, sizeof(b), "0x%04X", (unsigned)st.pid);
            lv_label_set_text(s_pr_res[1], b);
            lv_label_set_text(s_pr_res[2], TR(TID_P_PROBE_FOUND));
            lv_label_set_text(s_pr_res[3], "STM32");
        } else if (st.state == PROG_ST_FAIL) {
            lv_label_set_text(s_pr_res[0], TR(TID_S_NONE_DETECTED));
            lv_label_set_text(s_pr_res[1], TR(TID_P_PROBE_TIP_CHECK));
            lv_label_set_text(s_pr_res[2], "--");
            lv_label_set_text(s_pr_res[3], "--");
        }
    } else {                                    /* SPI：JEDEC 译码 */
        const uint32_t j = st.jedec;
        const bool ok = (st.state == PROG_ST_DONE) && (j != 0) && ((j & 0xFFu) != 0xFFu);
        if (ok) {
            const uint8_t mfr = (uint8_t)(j & 0xFFu);
            const uint8_t dev = (uint8_t)((j >> 8) & 0xFFu);
            const uint8_t cap = (uint8_t)((j >> 16) & 0xFFu);
            const uint32_t bytes = (cap < 32u) ? (1u << cap) : 0u;
            lv_label_set_text(s_pr_res[0], jedec_vendor(mfr));
            snprintf(b, sizeof(b), "0x%02X 0x%02X", (unsigned)mfr, (unsigned)dev);
            lv_label_set_text(s_pr_res[1], b);
            if (bytes >= (1u << 20)) snprintf(b, sizeof(b), "%u MB", (unsigned)(bytes >> 20));
            else                     snprintf(b, sizeof(b), "%u KB", (unsigned)(bytes >> 10));
            lv_label_set_text(s_pr_res[2], b);
            snprintf(b, sizeof(b), "%06X", (unsigned)(j & 0xFFFFFFu));
            lv_label_set_text(s_pr_res[3], b);
        } else if (st.state == PROG_ST_FAIL) {
            lv_label_set_text(s_pr_res[0], TR(TID_S_NONE_DETECTED));
            lv_label_set_text(s_pr_res[1], TR(TID_P_PROBE_NO_CHIP));
            lv_label_set_text(s_pr_res[2], TR(TID_P_PROBE_TIP_CHECK));
            lv_label_set_text(s_pr_res[3], "--");
        }
    }
}

static int probe_page_key(btn_id_t id, bool is_long)
{
    if (id == BTN_BACK) return 1;
    if (!is_long) {
        s_pr_sel = (s_pr_sel + 3 + ((id == BTN_UP) ? -1 : 1)) % 3;
        probe_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;
    if (s_pr_sel == 0)      { s_pr_tgt = (s_pr_tgt + 1) % 3; probe_apply(); }
    else if (s_pr_sel == 1) { s_pr_drv = (s_pr_drv + 1) % 2; probe_apply(); }
    else {
        prog_status_t st;
        prog_api_status(&st);
        if (st.state != PROG_ST_RUNNING)
            (void)prog_api_start(s_pr_tgt == 2 ? PROG_OP_ISPSYNC : PROG_OP_RDID);
    }
    probe_refresh();
    return 0;
}

/* ================================ 页 4：FLASH（左右双栏）================================ */
#define FLASH_CFG_N 3
#define FLASH_OP_N  5
#define FLASH_ROWS  (FLASH_CFG_N + FLASH_OP_N)
#define FLASH_LOG_N 4

static lv_obj_t *s_fl_cursor[FLASH_ROWS];
static lv_obj_t *s_fl_name[FLASH_ROWS];
static lv_obj_t *s_fl_val[FLASH_CFG_N];
static lv_obj_t *s_fl_bar, *s_fl_pct, *s_fl_msg;
static lv_obj_t *s_fl_log[FLASH_LOG_N];
static int       s_fl_sel = 0;
static int       s_fl_tgt = 0, s_fl_drv = 0, s_fl_img = 0;

static const char *FLASH_TGT[3] = { "s3-spi", "w25q", "s3-isp" };
static const char *FLASH_DRV[2] = { "SIM", "REAL" };
static const char *FLASH_IMG[3] = { "banner-256", "pattern-1k", "rand-2k" };

static const uint16_t FLASH_CFG_TID[FLASH_CFG_N] = {
    TID_P_PROG_TARGET, TID_P_PROG_DRIVER, TID_P_PROG_IMAGE
};
static const uint16_t FLASH_OP_TID[FLASH_OP_N] = {
    TID_P_PROG_READ_ID, TID_P_PROG_ERASE, TID_P_PROG_PROGRAM,
    TID_P_PROG_VERIFY, TID_P_PROG_ISP
};
static const char *FLASH_OP_GLYPH[FLASH_OP_N] = { "○", "■", "↓", "√", "↻" };
static const uint32_t FLASH_OP_HEX[FLASH_OP_N] = { 0x00E5FF, 0xFF3B57, 0x00FF9C, 0x39FF14, 0xFFC400 };

static void flash_cfg_apply(void)
{
    if (s_fl_val[0]) lv_label_set_text(s_fl_val[0], FLASH_TGT[s_fl_tgt]);
    if (s_fl_val[1]) {
        lv_label_set_text(s_fl_val[1], FLASH_DRV[s_fl_drv]);
        lv_obj_set_style_text_color(s_fl_val[1], s_fl_drv ? C_YELLOW : C_MINT, LV_PART_MAIN);
    }
    if (s_fl_val[2]) lv_label_set_text(s_fl_val[2], FLASH_IMG[s_fl_img]);
    (void)prog_api_cfg(s_fl_tgt, s_fl_drv, s_fl_img);
}

static void flash_refresh(void)
{
    prog_status_t st;
    prog_api_status(&st);

    for (int k = 0; k < FLASH_ROWS; k++) {
        const bool sel = (k == s_fl_sel);
        lv_obj_set_style_bg_opa(s_fl_cursor[k], sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_fl_name[k], sel ? C_BLACK : C_TEXT, LV_PART_MAIN);
    }
    for (int k = 0; k < FLASH_CFG_N; k++) {
        if (!s_fl_val[k]) continue;
        lv_obj_set_style_text_color(s_fl_val[k],
            (k == s_fl_sel) ? C_BLACK : (k == 1 && s_fl_drv) ? C_YELLOW : C_MINT, LV_PART_MAIN);
    }

    if (s_fl_bar) lv_obj_set_width(s_fl_bar, (st.pct * 118) / 100);
    if (s_fl_pct) {
        char b[8];
        snprintf(b, sizeof(b), "%3d%%", st.pct);
        lv_label_set_text(s_fl_pct, b);
    }
    if (s_fl_msg) {
        lv_label_set_text(s_fl_msg, st.msg);
        lv_obj_set_style_text_color(s_fl_msg,
            (st.state == PROG_ST_RUNNING) ? C_YELLOW :
            (st.state == PROG_ST_DONE)    ? C_GREEN  :
            (st.state == PROG_ST_FAIL)    ? C_RED    : C_GREEN, LV_PART_MAIN);
    }
    for (int i = 0; i < FLASH_LOG_N; i++) {
        if (!s_fl_log[i]) continue;
        lv_label_set_text(s_fl_log[i], st.log[i]);
        lv_obj_set_style_text_color(s_fl_log[i], (i == 0) ? C_TEXT : C_DIM, LV_PART_MAIN);
    }
}

static void build_flash(void)
{
    lv_obj_t *p = s_page[PG_FLASH];

    /* 左栏 8 行（3 设置 + 5 操作）：x 8..184 */
    int y = BODY_Y + 2;
    for (int k = 0; k < FLASH_ROWS; k++) {
        s_fl_cursor[k] = mk_bar(p, 8, y, 176, 17, (k < FLASH_CFG_N) ? C_MINT
                                 : lv_color_hex(FLASH_OP_HEX[k - FLASH_CFG_N]));
        if (k < FLASH_CFG_N) {
            s_fl_name[k] = mk_tr(p, FLASH_CFG_TID[k], F14, C_TEXT, 14, y + 2);
            s_fl_val[k]  = mk_txt(p, "-", F14, C_MINT, 108, y + 2);
        } else {
            const int i = k - FLASH_CFG_N;
            lv_color_t col = lv_color_hex(FLASH_OP_HEX[i]);
            mk_txt(p, FLASH_OP_GLYPH[i], F14, col, 14, y + 2);
            char b[24];
            snprintf(b, sizeof(b), "%u  %s", (unsigned)(i + 1), TR(FLASH_OP_TID[i]));
            s_fl_name[k] = mk_txt(p, b, F14, C_TEXT, 32, y + 2);
        }
        y += 18;
    }

    /* 分隔竖线 */
    mk_bar(p, 188, BODY_Y, 1, 162, C_LINE);

    /* 右栏：进度 / 结论 / 日志  x 196..312 */
    s_fl_pct = mk_txt(p, "  0%", F16, C_MINT, 196, BODY_Y + 2);
    mk_bar(p, 196, BODY_Y + 26, 118, 8, C_LINE);
    s_fl_bar = mk_bar(p, 196, BODY_Y + 26, 0, 8, C_MINT);
    s_fl_msg = mk_txt(p, "READY", F16, C_GREEN, 196, BODY_Y + 42);
    mk_section(p, "LOG", C_GRAY, BODY_Y + 66);
    for (int i = 0; i < FLASH_LOG_N; i++)
        s_fl_log[i] = mk_txt(p, "", F14, C_DIM, 196, BODY_Y + 84 + i * 16);

    flash_cfg_apply();
    flash_refresh();
}

static int flash_page_key(btn_id_t id, bool is_long)
{
    if (id == BTN_BACK) return 1;
    if (!is_long) {
        s_fl_sel = (s_fl_sel + FLASH_ROWS + ((id == BTN_UP) ? -1 : 1)) % FLASH_ROWS;
        flash_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;

    if (s_fl_sel < FLASH_CFG_N) {
        switch (s_fl_sel) {
        case 0: s_fl_tgt = (s_fl_tgt + 1) % 3; break;
        case 1: s_fl_drv = (s_fl_drv + 1) % 2; break;
        case 2: s_fl_img = (s_fl_img + 1) % 3; break;
        default: break;
        }
        flash_cfg_apply();
    } else {
        prog_status_t st;
        prog_api_status(&st);
        if (st.state != PROG_ST_RUNNING)
            (void)prog_api_start(s_fl_sel - FLASH_CFG_N);
    }
    flash_refresh();
    return 0;
}

/* ================================ 页 5：PINS ================================ */
static lv_obj_t *s_pin_val[HW_PIN_SIG_MAX];
static lv_obj_t *s_pin_profile = NULL;

static void build_pins(void)
{
    lv_obj_t *p = s_page[PG_PINS];
    lv_obj_t *card = mk_card(p, 8, BODY_Y, SCR_W - 16, 164, C_YELLOW, LV_OPA_40);

    mk_tr(card, TID_P_PINS_PROFILE, F14, C_GRAY, 10, 4);
    s_pin_profile = mk_txt(card, "--", F16, C_YELLOW, 110, 1);

    mk_tr(card, TID_P_PINS_SIG, F14, C_GRAY, 16, 26);
    mk_tr(card, TID_P_PINS_GPIO, F14, C_GRAY, 250, 26);
    mk_bar(card, 12, 43, SCR_W - 44, 1, C_LINE);

    for (int i = 0; i < HW_PIN_SIG_MAX; i++) {
        const int y = 48 + i * 12;
        mk_txt(card, hw_pin_sig_name(i), F14, C_TEXT, 20, y);
        s_pin_val[i] = mk_txt(card, "--", F14, C_MINT, 250, y);
    }
    mk_hint(p, TR(TID_S_BACK));
}

static void pins_refresh(void)
{
    const hw_pin_profile_t *pr = hw_pin_active();
    if (s_pin_profile)
        lv_label_set_text(s_pin_profile, pr ? (pr->name ? pr->name : "--") : "--");
    for (int i = 0; i < HW_PIN_SIG_MAX; i++) {
        if (!s_pin_val[i]) continue;
        int gpio = hw_pin_gpio_of(i);
        char b[16];
        if (gpio < 0) snprintf(b, sizeof(b), "%s", TR(TID_P_PINS_UNMAPPED));
        else          snprintf(b, sizeof(b), "IO%d", gpio);
        lv_label_set_text(s_pin_val[i], b);
        lv_obj_set_style_text_color(s_pin_val[i], (gpio < 0) ? C_DIM : C_MINT, LV_PART_MAIN);
    }
}

/* ================================ 页 6：STATUS ================================ */
static lv_obj_t *s_st_val[7];
static const uint16_t ST_KEY[7] = {
    TID_P_STA_WIFI, TID_P_STA_IP, TID_P_STA_RSSI, TID_P_STA_UPTIME,
    TID_P_STA_HEAP, TID_P_STA_FLASH, TID_P_STA_TIME
};

static void build_status(void)
{
    lv_obj_t *p = s_page[PG_STATUS];
    lv_obj_t *card = mk_card(p, 8, BODY_Y, SCR_W - 16, 164, C_CYAN, LV_OPA_40);
    for (int i = 0; i < 7; i++) {
        const int y = 6 + i * 22;
        if (i > 0) mk_bar(card, 12, y - 5, SCR_W - 44, 1, C_LINE);
        mk_tr(card, ST_KEY[i], F14, C_GRAY, 16, y + 3);
        s_st_val[i] = mk_txt(card, "--", F16, C_TEXT, 118, y);
    }
    mk_hint(p, TR(TID_S_BACK));
}

static void status_refresh(void)
{
    wifi_st_status_t w;
    wifi_st_get(&w);
    char b[40];

    const char *ws;
    lv_color_t wc = C_GRAY;
    if (w.state == WIFI_ST_GOT_IP)      { ws = TR(TID_P_STA_LINKED);  wc = C_GREEN; }
    else if (!w.scan_done)              { ws = TR(TID_S_LOADING);     wc = C_CYAN; }
    else if (!w.ap_seen)                { ws = TR(TID_P_STA_NONET);   wc = C_RED; }
    else if (w.state == WIFI_ST_CONNECTING || w.state == WIFI_ST_STARTING)
                                        { ws = TR(TID_P_STA_LINKING); wc = C_YELLOW; }
    else                                { ws = TR(TID_P_STA_NONET);   wc = C_YELLOW; }
    lv_label_set_text(s_st_val[0], ws);
    lv_obj_set_style_text_color(s_st_val[0], wc, LV_PART_MAIN);

    lv_label_set_text(s_st_val[1], (w.state == WIFI_ST_GOT_IP) ? w.ip : TR(TID_S_NONE));

    if (w.state == WIFI_ST_GOT_IP && w.rssi != 0) {
        snprintf(b, sizeof(b), "%d dBm", w.rssi);
        lv_label_set_text(s_st_val[2], b);
        lv_obj_set_style_text_color(s_st_val[2],
            (w.rssi > -60) ? C_GREEN : (w.rssi > -75) ? C_YELLOW : C_RED, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_st_val[2], TR(TID_S_NONE));
        lv_obj_set_style_text_color(s_st_val[2], C_GRAY, LV_PART_MAIN);
    }

    uint32_t s = s_tick_ms / 1000;
    snprintf(b, sizeof(b), "%02u:%02u:%02u", (unsigned)(s / 3600) % 100,
             (unsigned)(s / 60) % 60, (unsigned)(s % 60));
    lv_label_set_text(s_st_val[3], b);

    uint32_t heap = (uint32_t)esp_get_free_heap_size();
    snprintf(b, sizeof(b), "%u KB", (unsigned)(heap >> 10));
    lv_label_set_text(s_st_val[4], b);
    lv_obj_set_style_text_color(s_st_val[4], (heap < 64u * 1024u) ? C_YELLOW : C_TEXT, LV_PART_MAIN);

    uint32_t fsz = 0;
    if (esp_flash_get_size(NULL, &fsz) == ESP_OK) {
        snprintf(b, sizeof(b), "%u MB", (unsigned)(fsz >> 20));
        lv_label_set_text(s_st_val[5], b);
    } else {
        lv_label_set_text(s_st_val[5], TR(TID_S_UNKNOWN));
    }

    int64_t e = ui_cfg_now();
    if (e > UI_CFG_MIN_EPOCH) {
        time_t tt = (time_t)e;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    } else {
        snprintf(b, sizeof(b), "%s", TR(TID_S_UNKNOWN));
    }
    lv_label_set_text(s_st_val[6], b);
}

/* ================================ 页 7：SETUP ================================ */
#define SET_ROWS 9
static const uint16_t SET_KEY[SET_ROWS] = {
    TID_P_SET_LANG, TID_W_TIME_YEAR, TID_W_TIME_MONTH, TID_W_TIME_DAY,
    TID_W_TIME_HOUR, TID_W_TIME_MIN, TID_P_SET_SNTP, TID_P_SET_SAVE, TID_P_SET_FACTORY
};
static lv_obj_t *s_set_cursor[SET_ROWS];
static lv_obj_t *s_set_name[SET_ROWS];
static lv_obj_t *s_set_val[SET_ROWS];
static lv_obj_t *s_set_foot = NULL;
static int       s_set_sel = 0;
static int       s_set_t[5];
static int       s_set_arm = 0;

static void set_time_from_now(void)
{
    int64_t e = ui_cfg_now();
    time_t tt = (time_t)(e > UI_CFG_MIN_EPOCH ? e : UI_CFG_MIN_EPOCH);
    struct tm tmv;
    gmtime_r(&tt, &tmv);
    s_set_t[0] = tmv.tm_year + 1900;
    s_set_t[1] = tmv.tm_mon + 1;
    s_set_t[2] = tmv.tm_mday;
    s_set_t[3] = tmv.tm_hour;
    s_set_t[4] = tmv.tm_min;
}

static void build_setup(void)
{
    lv_obj_t *p = s_page[PG_SETUP];
    set_time_from_now();
    int y = BODY_Y + 1;
    for (int i = 0; i < SET_ROWS; i++) {
        s_set_cursor[i] = mk_bar(p, 8, y, SCR_W - 16, 17, C_MAGENTA);
        s_set_name[i] = mk_tr(p, SET_KEY[i], F14, C_TEXT, 14, y + 2);
        s_set_val[i]  = mk_txt(p, "-", F14, C_MINT, 220, y + 2);
        y += 18;
    }
    s_set_foot = mk_txt(p, TR(TID_S_HINT_NAV), F14, C_DIM, 10, HINT_Y);
}

static void setup_refresh(void)
{
    char b[24];
    lv_label_set_text(s_set_val[0], UI_LANG_NAME[g_ui_lang]);
    for (int i = 0; i < 5; i++) {
        snprintf(b, sizeof(b), (i == 0) ? "%04d" : "%02d", s_set_t[i]);
        lv_label_set_text(s_set_val[1 + i], b);
    }
    lv_label_set_text(s_set_val[6], ui_cfg_sntp_get() ? TR(TID_S_ON) : TR(TID_S_OFF));
    lv_label_set_text(s_set_val[7], "--");
    lv_label_set_text(s_set_val[8], s_set_arm ? TR(TID_S_CONFIRM) : "--");

    for (int i = 0; i < SET_ROWS; i++) {
        const bool sel = (i == s_set_sel);
        lv_obj_set_style_bg_opa(s_set_cursor[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_set_name[i], sel ? C_BLACK : C_TEXT, LV_PART_MAIN);
        if (i != 8)
            lv_obj_set_style_text_color(s_set_val[i], sel ? C_BLACK : C_MINT, LV_PART_MAIN);
        else
            lv_obj_set_style_text_color(s_set_val[i], s_set_arm ? C_RED : C_GRAY, LV_PART_MAIN);
    }
}

static int setup_page_key(btn_id_t id, bool is_long)
{
    if (id == BTN_BACK) return 1;
    if (!is_long) {
        s_set_sel = (s_set_sel + SET_ROWS + ((id == BTN_UP) ? -1 : 1)) % SET_ROWS;
        s_set_arm = 0;
        setup_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;

    if (s_set_sel == 0) {                        /* 语言：切换即存 + 重建整树 */
        uint8_t nl = (uint8_t)((g_ui_lang + 1) % UI_LANG_COUNT);
        g_ui_lang = nl;
        (void)ui_cfg_set_lang(nl);
        ui_rebuild();
        return 0;
    }
    if (s_set_sel >= 1 && s_set_sel <= 5) {      /* 时间字段 */
        static const int lo[5] = { 2024, 1, 1, 0, 0 };
        static const int hi[5] = { 2099, 12, 31, 23, 59 };
        const int f = s_set_sel - 1;
        int v = s_set_t[f] + 1;
        if (v > hi[f]) v = lo[f];
        s_set_t[f] = v;
        setup_refresh();
        return 0;
    }
    if (s_set_sel == 6) {                        /* 网络校时 */
        uint8_t on = (uint8_t)(!ui_cfg_sntp_get());
        (void)ui_cfg_set_sntp(on);
        if (on) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&cfg) != ESP_OK)
                ESP_LOGW(TAG_UI, "SNTP 启动失败（无网络时正常；开关已保存）");
        } else {
            esp_netif_sntp_deinit();
        }
        setup_refresh();
        return 0;
    }
    if (s_set_sel == 7) {                        /* 保存：写 NVS + 重设时间基准 */
        ui_cfg_t c;
        c.ver = UI_CFG_VER; c.lang = (uint8_t)g_ui_lang; c.sntp = ui_cfg_sntp_get();
        c._pad = 0;
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_year = s_set_t[0] - 1900; tmv.tm_mon = s_set_t[1] - 1; tmv.tm_mday = s_set_t[2];
        tmv.tm_hour = s_set_t[3];        tmv.tm_min = s_set_t[4];     tmv.tm_isdst = 0;
        time_t t = mktime(&tmv);
        c.epoch = (t > 0) ? (int64_t)t : ui_cfg_now();
        if (ui_cfg_save(&c) == ESP_OK) {
            if (t > 0) ui_cfg_set_now((int64_t)t);
            lv_label_set_text(s_set_foot, TR(TID_P_SET_SAVED));
        } else {
            lv_label_set_text(s_set_foot, TR(TID_S_ERROR));
        }
        setup_refresh();
        return 0;
    }
    /* 恢复出厂：两段确认（第一下 arm，第二下执行） */
    if (!s_set_arm) {
        s_set_arm = 1;
        lv_label_set_text(s_set_foot, TR(TID_P_SET_FACTORY_WARN));
        setup_refresh();
        return 0;
    }
    ESP_LOGW(TAG_UI, "恢复出厂：擦 ui_cfg + 重启");
    ui_cfg_factory_reset();                      /* 内部 esp_restart()，不返回 */
    return 0;
}

/* ================================ 页 8：ABOUT ================================ */
static lv_obj_t *s_ab_val[7];
static const uint16_t AB_KEY[7] = {
    TID_P_ABOUT_MODEL, TID_P_ABOUT_NAME, TID_P_ABOUT_BUILD, TID_P_ABOUT_FLASH,
    TID_P_ABOUT_SCREEN, TID_P_ABOUT_BUTTONS, TID_P_ABOUT_HW
};

static const char *chip_model_str(esp_chip_model_t m)
{
    switch (m) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32C6: return "ESP32-C6";
    default:           return "ESP32-?";
    }
}

static void build_about(void)
{
    lv_obj_t *p = s_page[PG_ABOUT];
    lv_obj_t *card = mk_card(p, 8, BODY_Y, SCR_W - 16, 164, C_CYAN, LV_OPA_40);
    for (int i = 0; i < 7; i++) {
        const int y = 6 + i * 22;
        if (i > 0) mk_bar(card, 12, y - 5, SCR_W - 44, 1, C_LINE);
        mk_tr(card, AB_KEY[i], F14, C_GRAY, 16, y + 3);
        s_ab_val[i] = mk_txt(card, "--", F14, C_TEXT, 108, y + 2);
    }
    mk_hint(p, TR(TID_S_BACK));
}

static void about_refresh(void)
{
    char b[48];
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    const esp_app_desc_t *d = esp_app_get_description();
    uint32_t fsz = 0;
    (void)esp_flash_get_size(NULL, &fsz);

    lv_label_set_text(s_ab_val[0], chip_model_str(ci.model));      /* 型号 */
    lv_label_set_text(s_ab_val[1], TR(TID_W_BRAND));               /* 产品名 */
    if (d) {
        /* ⚠️ 行序必须与 AB_KEY 严格对齐：0 型号 1 产品名 2 构建 3 固件 4 屏幕 5 按键 6 硬件。
         *    2026-10-02 抓图 OCR 抓到过「标签与数值错开一行」的错版（构建显示成 1、
         *    固件显示成构建时间）—— 多行 KV 页最容易被这种错位骗过，改完务必再抓图核对。 */
        snprintf(b, sizeof(b), "%s %s", d->date, d->time);
        lv_label_set_text(s_ab_val[2], b);                         /* 构建 = 编译时间 */
        /* d->version 默认是 "1"（工程无 version.txt）—— 单字符既没信息量、
         * 也小于 OCR 的识别下限。补上 IDF 版本，让这一行真能表达「跑的哪套固件」。 */
        snprintf(b, sizeof(b), "v%s  IDF %s", d->version, IDF_VER);
        lv_label_set_text(s_ab_val[3], b);                         /* 固件 = 版本 + IDF */
    }
    lv_label_set_text(s_ab_val[4], "2.0 ST7789 320x240");          /* 屏幕 */
    lv_label_set_text(s_ab_val[5], "UP / DOWN / BACK");            /* 按键 */
    snprintf(b, sizeof(b), "%d core @ %u MHz, %u MB", (int)ci.cores,
             (unsigned)(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ), (unsigned)(fsz >> 20));
    lv_label_set_text(s_ab_val[6], b);                             /* 硬件 */
}

/* ================================ 页标 ================================ */
static void build_dots(void)
{
    const int w = 10, gap = 6;
    const int total = PG_MAX * w + (PG_MAX - 1) * gap;
    int x = (SCR_W - total) / 2;
    for (int i = 0; i < PG_MAX; i++) {
        lv_obj_t *d = mk_bar(s_scr, x, 226, w, 3, C_LINE);
        lv_obj_set_style_radius(d, 2, LV_PART_MAIN);
        s_dot[i] = d;
        x += w + gap;
    }
}

/* ================================ 页面切换 ================================ */
void ui_goto(int n)
{
    if (n < 0 || n >= PG_MAX) return;
    for (int i = 0; i < PG_MAX; i++) {
        lv_obj_set_hidden(s_page[i], i != n);
        if (s_dot[i]) {
            const bool on = (i == n);
            lv_obj_set_style_bg_color(s_dot[i], on ? pac(i) : C_LINE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_dot[i], on ? LV_OPA_COVER : LV_OPA_60, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(s_dot[i], on ? 10 : 0, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(s_dot[i], pac(i), LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(s_dot[i], LV_OPA_60, LV_PART_MAIN);
        }
    }
    s_cur = n;
    s_auto_ms = 4000;

    /* 开机页是「品牌闪现」，不该挂 AUTO/MANUAL 胶囊和页标 —— 那两样是功能页的控件。
     * （2026-10-02 抓图实测：开机页右上角挂着「手动」，显得像张没做完的截图。） */
    if (s_pill_mode) lv_obj_set_hidden(s_pill_mode, (n == PG_BOOT));
    for (int i = 0; i < PG_MAX; i++)
        if (s_dot[i]) lv_obj_set_hidden(s_dot[i], (n == PG_BOOT));

    /* 进哪页刷哪页（只刷当前页，省 CPU） */
    switch (n) {
    case PG_PINS:   pins_refresh();   break;
    case PG_STATUS: status_refresh(); break;
    case PG_ABOUT:  about_refresh();  break;
    case PG_SETUP:  setup_refresh();  break;
    case PG_PROBE:  probe_apply(); probe_refresh(); break;
    case PG_FLASH:  flash_refresh();  break;
    case PG_WIZ:    wiz_refresh();    break;
    default: break;
    }
}

/* ================================ 按键导航 ================================ */
static void update_mode_label(void)
{
    if (!s_lbl_mode) return;
    lv_label_set_text(s_lbl_mode, s_auto_on ? TR(TID_S_AUTO) : TR(TID_S_MANUAL));
    lv_obj_set_style_text_color(s_lbl_mode, s_auto_on ? C_GRAY : C_YELLOW, LV_PART_MAIN);
    if (s_pill_mode) {
        lv_obj_set_style_border_color(s_pill_mode, s_auto_on ? C_LINE : C_YELLOW, LV_PART_MAIN);
        lv_obj_set_style_border_opa(s_pill_mode, s_auto_on ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }
}

static void set_auto(bool on)
{
    if (s_auto_on == on) return;
    s_auto_on = on;
    s_auto_ms = 4000;
    update_mode_label();
}

static void auto_step(void)
{
    int n = s_cur + 1;
    if (n >= PG_MAX || n <= PG_HOME) n = PG_PROBE;
    s_in_menu = false;
    for (int k = 0; k < MENU_N; k++) if (MENU_PG[k] == n) s_sel = k;
    ui_goto(n);
}

static void menu_move(int d)
{
    s_sel = (s_sel + MENU_N + d) % MENU_N;
    home_apply_sel();
}

static void menu_enter(void) { s_in_menu = false; ui_goto(MENU_PG[s_sel]); }

static void to_menu(void)
{
    s_in_menu = true;
    ui_goto(PG_HOME);
    home_apply_sel();
}

static void handle_buttons(void)
{
    btn_id_t id;
    bool is_long;

    while (btn_get_event(&id, &is_long)) {
        /* 开机页：任意返回键 = 跳过 */
        if (s_cur == PG_BOOT && id == BTN_BACK) {
            s_in_menu = true;
            ui_goto(PG_HOME);
            home_apply_sel();
            continue;
        }

        const bool turning_on = (id == BTN_BACK && is_long);
        if (!turning_on) set_auto(false);

        /* 页内钩子：返回 0 = 本页已消费 */
        if (!s_in_menu) {
            int handled = 1;
            switch (s_cur) {
            case PG_WIZ:   handled = wiz_page_key(id, is_long);   break;
            case PG_PROBE: handled = probe_page_key(id, is_long); break;
            case PG_FLASH: handled = flash_page_key(id, is_long); break;
            case PG_SETUP: handled = setup_page_key(id, is_long); break;
            default:       handled = 1;                           break;
            }
            if (!handled) continue;
        }

        switch (id) {
        case BTN_UP:
        case BTN_DOWN: {
            const int d = (id == BTN_UP) ? -1 : +1;
            if (!is_long) {
                if (s_in_menu) menu_move(d);
            } else if (id == BTN_UP) {
                if (s_in_menu) menu_enter();
            } else {
                if (!s_in_menu) to_menu();
            }
            break;
        }
        case BTN_BACK:
            if (is_long) set_auto(true);
            else         to_menu();
            break;
        default: break;
        }

        ESP_LOGI(TAG_UI, "key=%s%s page=%d sel=%d mode=%s",
                 (id == BTN_UP) ? "UP" : (id == BTN_DOWN) ? "DOWN" : "BACK",
                 is_long ? "-LONG" : "", s_cur, s_sel, s_auto_on ? "AUTO" : "MANUAL");
    }
}

/* ================================ 定时刷新 ================================ */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    s_tick_ms += 300;

    handle_buttons();

    if (s_cur == PG_BOOT) {
        s_boot_ms += 300;
        for (int k = 0; k < 7; k++)
            lv_obj_set_style_bg_color(s_boot_seg[k],
                (s_boot_ms > (k + 1) * 300) ? C_CYAN : C_LINE, LV_PART_MAIN);
        if (s_boot_ms >= 2400) {
            s_in_menu = true;
            ui_goto(PG_HOME);
            home_apply_sel();
        }
        return;
    }

    if (s_cur == PG_STATUS && (s_tick_ms % 600 == 0))  status_refresh();
    if (s_cur == PG_PINS   && (s_tick_ms % 3000 == 0)) pins_refresh();
    if (s_cur == PG_ABOUT  && (s_tick_ms % 3000 == 0)) about_refresh();
    if (s_cur == PG_PROBE  && (s_tick_ms % 300 == 0))  probe_refresh();
    if (s_cur == PG_FLASH  && (s_tick_ms % 300 == 0))  flash_refresh();

    /* SNTP：已同步则把系统时间并回 ui_cfg 基准 */
    if (ui_cfg_sntp_get() && (s_tick_ms % 3000 == 0)) {
        time_t now = time(NULL);
        if (now > (time_t)UI_CFG_MIN_EPOCH) ui_cfg_set_now((int64_t)now);
    }

    if (s_auto_on) {
        s_auto_ms -= 300;
        if (s_auto_ms <= 0) auto_step();
    }
}

/* ================================ 整树重建（切语言） ================================ */
static void build_all(void)
{
    s_scr = lv_screen_active();
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, C_BLACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollable(s_scr, false);

    memset(s_page, 0, sizeof(s_page));
    memset(s_title, 0, sizeof(s_title));
    memset(s_dot, 0, sizeof(s_dot));

    for (int i = 0; i < PG_MAX; i++) s_page[i] = mk_page(i);

    build_boot();
    build_wiz();
    build_home();
    build_probe();
    build_flash();
    build_pins();
    build_status();
    build_setup();
    build_about();
    build_dots();

    s_pill_mode = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_pill_mode);
    lv_obj_set_size(s_pill_mode, 76, 20);
    lv_obj_align(s_pill_mode, LV_ALIGN_TOP_RIGHT, -6, 7);
    lv_obj_set_style_radius(s_pill_mode, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pill_mode, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pill_mode, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pill_mode, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollable(s_pill_mode, false);

    s_lbl_mode = lv_label_create(s_pill_mode);
    lv_obj_set_style_text_font(s_lbl_mode, F14, LV_PART_MAIN);
    lv_obj_center(s_lbl_mode);

    update_mode_label();
}

void ui_rebuild(void)
{
    const int keep = s_cur;
    const bool was_menu = s_in_menu;
    lv_obj_clean(s_scr);
    build_all();
    s_in_menu = was_menu;
    ui_goto(keep);
    if (s_in_menu) home_apply_sel();
}

void ui_start_wizard(void)
{
    s_boot_ms = 0;
    s_wiz_step = 0;
    s_wiz_sel = 0;
    wiz_time_from_now();
    s_in_menu = false;
    ui_goto(PG_WIZ);
}

/* ================================ 入口 ================================ */
void ui_init(void)
{
    setenv("TZ", "UTC0", 1);     /* 与 ui_cfg 的 epoch 同口径，避免时区歧义 */
    tzset();

    /* 烧录后端必须先起来：build_probe/build_flash 末尾会调 prog_api_cfg()，
     * 那条路要用它的互斥锁。此刻刷新任务还没启动 → 无并发风险。 */
    prog_api_init();

    build_all();

    s_boot_ms = 0;
    s_wiz_step = 0;
    s_wiz_sel = 0;
    wiz_time_from_now();
    ui_goto(PG_BOOT);

    if (btn_init() != ESP_OK)
        ESP_LOGW(TAG_UI, "按键初始化失败 —— 退化为纯自动轮播");

    lv_timer_create(tick_cb, 300, NULL);
    ESP_LOGI(TAG_UI, "UI v3 就绪：9 页（开机/向导/菜单/探针/烧录/引脚/状态/设置/关于）");
}
