# 逐行代码注解（三）UI 层骨架

> 对应源码：`main/ui.c` 第 1-460 行（文件头 / 色板 / 绘制原语 / 页 0-2）
> `ui.c` 全文件 1357 行，本篇讲骨架与前三页，下篇（ANNOT_04）讲页 3-8 与导航/定时器。

---

# 一、文件头：设计哲学（第 1-46 行）

```c
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
 */
```

> **这段注释是整个 v3 版本的立项理由。**
>
> 值得强调的是判断标准：不是"UI 好不好看"，而是"**数据是不是真的**"。
> 上一页显示的 TX/RX 计数如果是 `(i*7+3)&0xFF` 算出来的，那它**在骗用户**——
> 比"这一页不存在"更糟。
>
> **⇒ 判断一个新页面该不该加的三个问题**：
> 1. 它的每个数字从哪来？（说不出来 → 不做）
> 2. 那个数据源是真的吗？（假数据 → 不做）
> 3. 失败了怎么显示？（说不出来 → 做不到"诚实失败" → 不做）

## 1.1 页面清单与数据源（第 14-23 行）

```c
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
```

> **这张表是"每个数字都有源头"的直接证据。** 每一页都能回答"数据从哪来"。
> 加页时必须在这张表里加一行。

## 1.2 视觉语言的由来（第 25-31 行）

```c
 * ── 视觉语言 ─────────────────────────────────────────────────────────────
 *   纯黑底 + 霓虹强调色 + **一套字体**。这是被字体现实逼出来的：
 *   本工程只编进 ui_cjk_14 / ui_cjk_16，它们**不含 FontAwesome**，
 *   所以 LV_SYMBOL_* 会渲染成缺字方块 —— 旧版靠 Montserrat 的 59 个符号撑图标，
 *   改中文后那条路就断了。
 *   ⇒ 图标语言改用「字体里确实存在的字符」：●○■□▲→←↑↓★☆√✕↻ 与汉字。
 *   ⚠️ 换符号前先查 tools/font_charsets.json，别凭想象填（会出方块）。
 */
```

> **"被字体现实逼出来的"** —— 这解释了为什么图标全是几何符号。
>
> LVGL 默认的 `LV_SYMBOL_*`（FontAwesome）来自 Montserrat 14/16/18/20/24/28。
> v3 为了显示中文，把字体换成了自建的 CJK 子集（`ui_cjk_14/16`），
> 而这个子集**没有 FontAwesome 那 500+ 个图标码位**。
>
> ⇒ 后果：`lv_obj_add_state(obj, LV_STATE_CHECKED)` 自带的勾选图标会变方块。
>
> **⇒ 纪律：换任何显示字符前，先查 `tools/font_charsets.json` 确认字体里有这个码位。**

## 1.3 按键语义与线程模型（第 33-45 行）

```c
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
 */
```

> **线程模型那三行是硬约束**：
> - ❌ 不能在 `tick_cb` 里调 `hw_pin_flash_rdid()`（阻塞几毫秒~几百毫秒 → LVGL 卡帧）
> - ❌ 不能在 `tick_cb` 里调 `esp_wifi_*`（阻塞扫描几秒）
> - ✅ 只能调 `prog_api_status()`（纯 memcpy + 锁）
>
> **验证手段**：`grep 'hw_pin_\|esp_wifi_' main/ui.c` 应该**只命中非阻塞的
> `hw_pin_gpio_of` / `hw_pin_sig_name` / `hw_pin_active` 这几个查询函数**。

---

# 二、include 与常量（第 47-118 行）

## 2.1 include 段（第 47-72 行）

```c
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
```

> **注意这里 include 了 `hw_pin.h` 和 `prog_hw.h`** —— 但只用了查询函数
> （`hw_pin_active` / `hw_pin_gpio_of` / `hw_pin_sig_name`），**没调任何阻塞操作**。
> 符合 1.3 的线程约束。
>
> `esp_netif_sntp.h` 是设置页的"网络校时"开关用的。

## 2.2 色板（第 76-88 行）

```c
#define C_BG      lv_color_hex(0x000000)   /* 页面底色 */
#define C_BLACK   lv_color_hex(0x000000)   /* 选中项上的文字色（反白用） */
#define C_LINE    lv_color_hex(0x1B2530)   /* 分隔线/暗条 */
#define C_DIM     lv_color_hex(0x3A4A57)   /* 最弱文字（提示行） */
#define C_GRAY    lv_color_hex(0x6E8496)   /* 次要文字（标签） */
#define C_TEXT    lv_color_hex(0xE6F7FF)   /* 正文/标题 */
#define C_CYAN    lv_color_hex(0x00E5FF)   /* 强调色·青 */
#define C_GREEN   lv_color_hex(0x39FF14)   /* 霓虹绿 */
#define C_YELLOW  lv_color_hex(0xFFC400)   /* 警示黄 */
#define C_RED     lv_color_hex(0xFF3B57)   /* 错误红 */
#define C_MAGENTA lv_color_hex(0xFF2D95)   /* 品红（设置页强调） */
#define C_MINT    lv_color_hex(0x00FF9C)   /* 薄荷绿（烧录页强调） */
```

> 🔴 **`lv_color_hex()` 在 LVGL v9 是函数不是常量**，所以**不能**写进静态初始化表。
> 这里用 `#define` 而不是 `static const lv_color_t` 就是这个原因。
> （v8 时代 `lv_color_hex` 是宏，可以静态初始化；v9 改成函数了。）
>
> **`C_BG` 和 `C_BLACK` 值相同但语义不同**：
> - `C_BG` = "页面背景"（我想要黑）
> - `C_BLACK` = "文字颜色"（选中项反白时我要黑字）
> 区分开是为了将来改主题时不会误改。

## 2.3 字体与几何（第 90-99 行）

```c
/* 字体：全工程只有这两款（含 ASCII + GB2312 一级常用字） */
#define F16 (&ui_cjk_16)
#define F14 (&ui_cjk_14)

/* ================================ 几何 ================================ */
#define SCR_W   320
#define SCR_H   240
#define TITLE_H 32
#define BODY_Y  34
#define HINT_Y  203
```

> **只有两款字体**，这是 v3 的硬约束（见 1.2）。
>
> **行高（重要）**：`ui_cjk_16` 行高 **20px**，`ui_cjk_14` 行高 **17px**。
> 排版时下一行 y 至少是 `上一行 y + 上一行行高 + 7`。
>
> **几何常量**：
> - `TITLE_H 32` = 标题栏高度（标题 y=5，渐变线 y=29，分隔线 y=32）
> - `BODY_Y 34` = 内容区起始 y（在分隔线下方）
> - `HINT_Y 203` = 底部操作提示行 y（下面留 37px 给页标）

## 2.4 页面枚举（第 101-104 行）

```c
enum {
    PG_BOOT = 0, PG_WIZ, PG_HOME, PG_PROBE, PG_FLASH,
    PG_PINS, PG_STATUS, PG_SETUP, PG_ABOUT, PG_MAX
};
```

> **`PG_MAX` = 9**，是数组长度的哨兵。`s_page[PG_MAX]` 等数组都用它。
>
> ⚠️ **页号是"魔法数字"**：`prog_ui_probe.c` 和 `shot_probe.c` 里写死了
> "FLASH 页 = 4"、"菜单行 1 = 烧录"。**插入新页会让这些探针失效**。
> 改页面顺序时必须同步改那两个文件。

## 2.5 页面信息表（第 106-118 行）

```c
typedef struct { uint16_t tid; uint32_t accent; } pageinfo_t;
static const pageinfo_t PINFO[PG_MAX] = {
    { TID_W_BRAND,         0x00E5FF },   /* BOOT   青 */
    { TID_W_SETUP_FIRST,   0x00E5FF },   /* WIZ    青 */
    { TID_M_HOME,          0x00E5FF },   /* HOME   青 */
    { TID_P_PROBE_TITLE,   0x00E5FF },   /* PROBE  青 */
    { TID_M_FLASH,         0x00FF9C },   /* FLASH  薄荷绿 ★ */
    { TID_P_PINS_TITLE,    0xFFC400 },   /* PINS   黄 */
    { TID_P_STA_TITLE,     0x00E5FF },   /* STATUS 青 */
    { TID_P_SET_TITLE,     0xFF2D95 },   /* SETUP  品红 ★ */
    { TID_P_ABOUT_TITLE,   0x00E5FF },   /* ABOUT  青 */
};
static inline lv_color_t pac(int i) { return lv_color_hex(PINFO[i].accent); }
```

> **`tid` = 标题文案 ID，`accent` = 该页强调色。**
> 这是"一页一色"视觉系统的唯一数据源——`mk_page()`、`ui_goto()`、主菜单边框
> 都从这里取色。
>
> **`accent` 存 `uint32_t` 而不是 `lv_color_t`**，然后用 `pac(i)` 现场转换 ——
> **因为 `lv_color_hex()` 是函数，不能静态初始化**（见 2.2）。
>
> **`pac(i)` 是 `static inline`**，调用点零开销。

---

# 三、运行时状态与绘制原语（第 120-224 行）

## 3.1 全局状态变量（第 121-138 行）

```c
static lv_obj_t *s_scr = NULL;            /* 当前活动屏幕 */
static lv_obj_t *s_page[PG_MAX];          /* 9 个页面容器 */
static lv_obj_t *s_title[PG_MAX];         /* 9 个标题 label */
static lv_obj_t *s_dot[PG_MAX];           /* 9 个页标（底部小条） */

static int      s_cur = PG_BOOT;          /* 当前页号 */
static bool     s_in_menu = true;         /* 在主菜单层还是在功能页层 */
static int      s_sel = 0;                /* 主菜单光标 */
static uint32_t s_tick_ms = 0;            /* LVGL tick 累计毫秒 */
static int      s_boot_ms = 0;            /* 开机页计时 */

static bool      s_auto_on = false;       /* AUTO 轮播开关（默认关） */
static int       s_auto_ms = 4000;        /* AUTO 剩余时间 */
static lv_obj_t *s_lbl_mode = NULL;       /* "自动/手动" 文字 */
static lv_obj_t *s_pill_mode = NULL;      /* "自动/手动" 胶囊底框 */

static void home_apply_sel(void);         /* 前向声明 */
```

> **`s_in_menu` 是导航的核心状态**，它决定按键怎么解释：
> - `true` = 在主菜单（光标 `s_sel` 有效）
> - `false` = 在功能页（光标由该页自己管）
>
> **`s_auto_on` 默认 `false`** —— 2026-09-30 决定关掉开机自动轮播，
> 避免用户操作被自动切页打断。
>
> **前向声明 `home_apply_sel`**：因为 `build_home()` 里要调它，
> 而它的定义在 `build_home` 之后。

## 3.2 `mk_bar()` — 纯色块（第 141-151 行）

```c
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
```

> **`mk_*` 系列是本工程的绘制原语。** 改 UI 时**只用这几个**，别直接写
> `lv_obj_create` —— 统一风格、统一去除默认样式。
>
> **`lv_obj_remove_style_all(o)`**：清空 LVGL 默认主题的所有样式
> （边框、圆角、内边距、滚动条…）。**必须第一个调**，
> 否则后面 `set_style` 会被默认样式的一部分影响。
>
> **`lv_obj_set_scrollable(o, false)`**：关掉滚动。这个工程所有页面都不需要滚动，
> 默认的滚动条会在 `remove_style_all` 后失效但仍占逻辑空间。

## 3.3 `mk_txt()` 与 `mk_tr()`（第 153-167 行）

```c
static lv_obj_t *mk_txt(lv_obj_t *parent, const char *txt, const lv_font_t *f,
                        lv_color_t col, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt ? txt : "");       /* ← NULL 保护 */
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
```

> **`txt ? txt : ""` 是 NULL 保护**：`lv_label_set_text(NULL, ...)` 会崩。
> 传进来的可能来自 `TR()`（理论上不会返回 NULL，但防御一下）。
>
> **绝对定位而非 `lv_obj_align`**：大部分元素用 `set_pos` 精确控制。
> 只有菜单行的文字用 `lv_obj_align(LEFT_MID)`（行高固定，居中更稳）。
>
> **`mk_tr` 是 i18n 快捷方式**：`TR(tid)` → `ui_tr(tid)` → `UI_STR[g_ui_lang][tid]`。
> **切语言后必须重建 label**（指针已烤进 label，见 ANNOT_04 的 `ui_rebuild`）。

## 3.4 `mk_card()` — 圆角卡片（第 169-184 行）

```c
static lv_obj_t *mk_card(lv_obj_t *parent, int x, int y, int w, int h,
                         lv_color_t accent, lv_opa_t border_opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, 6, LV_PART_MAIN);           /* 圆角 6px */
    lv_obj_set_style_bg_color(o, C_BG, LV_PART_MAIN);      /* 卡片底=页面底（视觉分层靠边框） */
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(o, accent, LV_PART_MAIN);
    lv_obj_set_style_border_opa(o, border_opa, LV_PART_MAIN);
    lv_obj_set_scrollable(o, false);
    return o;
}
```

> **卡片设计**：黑色填充 + 1px 半透明强调色边框。
> **底色和页面同色**（都是黑），层次完全靠边框和阴影撑起来 —— 这就是"扁平+霓虹"风格。
>
> **`border_opa` 参数化**：不同页面用不同透明度。
> 例如 PINS 页用 `LV_OPA_40`（40% 不透明度）比较含蓄，FLASH 页某些用 `LV_OPA_30`。
>
> **卡片是所有内容页（PINS/STATUS/ABOUT/SETUP）的容器**，
> 内部再用 `mk_bar` 画分隔线。

## 3.5 `mk_section()` 与 `mk_hint()`（第 186-195 行）

```c
static void mk_section(lv_obj_t *parent, const char *txt, lv_color_t accent, int y)
{
    mk_bar(parent, 6, y + 3, 3, 12, accent);      /* 3px 竖色条 */
    mk_txt(parent, txt, F14, C_GRAY, 15, y);      /* 12px 灰字 */
}

static void mk_hint(lv_obj_t *p, const char *txt)
{
    mk_txt(p, txt, F14, C_DIM, 10, HINT_Y);        /* y=203, x=10 */
}
```

> **`mk_section`** = 3px 强调色竖条 + 小节名。是页面内的分组标题。
>
> **`mk_hint`** = 屏幕底部的操作提示（"短按 上下 移动 · 长按 上 进入 · 返回 菜单"）。
> **位置固定 y=203**，所有页面统一，方便用户形成肌肉记忆。

## 3.6 `mk_page()` — 页面容器 + 标题栏（第 197-224 行）

```c
static lv_obj_t *mk_page(int i)
{
    lv_obj_t *p = lv_obj_create(s_scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, SCR_W, SCR_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollable(p, false);

    if (i == PG_BOOT) return p;              /* ★ 开机页无标题栏 */

    mk_bar(p, 8, 8, 3, 17, pac(i));                        /* 左侧强调色竖条 */
    s_title[i] = mk_tr(p, PINFO[i].tid, F16, C_TEXT, 18, 5); /* 标题文字 */

    lv_obj_t *glow = lv_obj_create(p);                     /* 渐变辉光条 */
    lv_obj_remove_style_all(glow);
    lv_obj_set_size(glow, SCR_W, 3);
    lv_obj_set_pos(glow, 0, TITLE_H - 3);                  /* y=29 */
    lv_obj_set_style_bg_color(glow, pac(i), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(glow, C_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(glow, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(glow, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollable(glow, false);

    mk_bar(p, 0, TITLE_H, SCR_W, 1, C_LINE);                /* y=32, 1px 分隔线 */
    return p;
}
```

> **★ `if (i == PG_BOOT) return p;` 是关键分支**：开机页是"品牌闪现"，
> 不要标题栏/强调色竖条/辉光条/分隔线，只要黑底 + 内容。
>
> **渐变辉光条**：`bg_grad_color = C_BG`（黑）+ `LV_GRAD_DIR_HOR`（水平渐变）
> = 强调色从左到右渐隐到黑，制造"光从左边打过来"的效果。3px 高的细条。
>
> **标题栏的固定结构**（所有内容页一致）：
> ```
> y=5..25   标题文字 (F16, 行高20)
> y=8..25   左侧 3px 强调色竖条 (x=8)
> y=29..32  渐变辉光条 (全宽 320)
> y=32      1px 分隔线 (全宽)
> y=34+     内容区
> ```

---

# 四、页 0：BOOT 开机页（第 226-243 行）

```c
static lv_obj_t *s_boot_seg[7];

static void build_boot(void)
{
    lv_obj_t *p = s_page[PG_BOOT];
    /* ⚠️ 行距必须留够：F16 行高 20、F14 行高 17。2026-10-02 抓图实测，
     *    原来 92/114/132 的间距只有 2px/1px → 两行 F14 在屏上**视觉粘成一行**
     *    （离线 OCR 把 "AI WIRELESS DEBUGGER" 和 "ESP32-S3 LVGL" 识别成一个字符串）。 */
    mk_bar(p, 26, 78, 4, 46, C_CYAN);                        /* 左侧青色竖条 */
    mk_tr(p, TID_W_BRAND, F16, C_TEXT, 44, 82);              /* 82 + 20 = 102 */
    mk_txt(p, "AI WIRELESS DEBUGGER", F14, C_GRAY, 44, 116); /* 116 + 17 = 133 */
    mk_txt(p, "ESP32-S3  LVGL", F14, C_DIM, 44, 140);        /* 140 + 17 = 157 */
    mk_tr(p, TID_W_BOOTING, F14, C_CYAN, 44, 170);
    for (int k = 0; k < 7; k++)
        s_boot_seg[k] = mk_bar(p, 44 + k * 16, 196, 12, 4, C_LINE);  /* 7 段进度条 */
}
```

> **这段注释是"抓图自证"的成果**。原来三行 y = 92/114/132，间距只有 22px/18px，
> 而 F14 行高 17px ⇒ 行间只剩 5px/1px ⇒ **在 320×240 的小屏上视觉上粘成一行**。
>
> **怎么发现的**：用 `shot_probe` 抓 PNG → macOS Vision 框架 OCR →
> 读出 `AI WIRELESS DEBUGGER ESP32-S3 LVGL` 是一个连续字符串 ⇒ 判定粘行。
>
> **⇒ 教训**："代码里的 y 值间距够大"不等于"屏上看起来分得开"。
> **必须看渲染结果**。行高是字体属性，不是猜得出来的。
>
> **改后的排版**（注释里标了行高，便于验证）：
> ```
> y=82   F16 品牌名（"AI 无线调试器"）    行高 20 → 底 102
> y=116  F14 副标题（间隔 14px）         行高 17 → 底 133
> y=140  F14 芯片型号（间隔 7px）         行高 17 → 底 157
> y=170  F14 "启动中…"（间隔 13px）
> y=196  7 段进度条（12×4px，间距 16px）
> ```
>
> **7 段进度条** `s_boot_seg[7]`：颜色从 `C_LINE`（暗）逐步变 `C_CYAN`（亮），
> 由 `tick_cb` 驱动（每 300ms 亮一段，2.4s 全部亮完 → 进主菜单）。
>
> **左侧 4px 青色竖条** y=78 高 46 = 覆盖三行文字的高度，起"视觉锚"作用。

---

# 五、页 1：WIZ 首启向导（第 245-404 行）

## 5.1 状态与常量（第 246-258 行）

```c
#define WIZ_TFIELD  5      /* 时间字段数：年 月 日 时 分 */
#define WIZ_ROWS    8      /* 总行数：2 语言 + 5 字段 + 1 下一步 */

static int  s_wiz_step = 0;      /* 0=选语言 1=设时间 2=完成 */
static int  s_wiz_sel  = 0;      /* 当前选中行 */
static int  s_wiz_t[WIZ_TFIELD]; /* 5 个时间字段的值 */
static lv_obj_t *s_wiz_row[WIZ_ROWS];
static lv_obj_t *s_wiz_cursor = NULL;   /* 反色高亮背景块 */
static lv_obj_t *s_wiz_foot = NULL;     /* 底部提示 */

static const uint16_t WIZ_FIELD_TID[WIZ_TFIELD] = {
    TID_W_TIME_YEAR, TID_W_TIME_MONTH, TID_W_TIME_DAY, TID_W_TIME_HOUR, TID_W_TIME_MIN
};
```

> **`WIZ_ROWS = 8` 的构成**：
> - `s_wiz_row[0]`、`[1]` = 中文 / English
> - `s_wiz_row[2..6]` = 5 个时间字段
> - `s_wiz_row[7]` = "下一步"
>
> **向导只有 3 步**（`s_wiz_step` 0/1/2），第 0 步只显示前 2 行，
> 第 1 步显示后 6 行，第 2 步是完成提示。

## 5.2 时间转换（第 260-285 行）

```c
static void wiz_time_from_now(void)
{
    int64_t e = ui_cfg_now();
    time_t tt = (time_t)(e > UI_CFG_MIN_EPOCH ? e : UI_CFG_MIN_EPOCH);
    struct tm tmv;
    gmtime_r(&tt, &tmv);
    s_wiz_t[0] = tmv.tm_year + 1900;   /* tm_year 是「年-1900」 */
    s_wiz_t[1] = tmv.tm_mon + 1;       /* tm_mon 是「月-1」（0-11） */
    s_wiz_t[2] = tmv.tm_mday;
    s_wiz_t[3] = tmv.tm_hour;
    s_wiz_t[4] = tmv.tm_min;
}
```

> **`e > UI_CFG_MIN_EPOCH ? e : UI_CFG_MIN_EPOCH`**：时间没设定时
> （`ui_cfg_now()` 返回 -1）用 2020-01-01 兜底，避免 `gmtime_r` 拿到负数。
>
> **两个 "+1" 是 C 时间库的经典陷阱**：
> - `tm_year` = 年份 - 1900（2026 → 126）
> - `tm_mon` = 月份 - 1（1 月 → 0）
>
> **`gmtime_r` 而不是 `gmtime`**：`gmtime` 返回静态缓冲区（多线程不安全），
> `gmtime_r` 写入调用者提供的结构体。**`r` = reentrant。**

```c
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
```

> **`memset` 是必须的**：`struct tm` 有 9 个字段，只填了 5 个。
> 剩下的（`tm_sec`/`tm_wday`/`tm_yday`/`tm_gmtoff`/`tm_zone`）是**栈垃圾**，
> `mktime` 会用到其中几个 ⇒ 必须清零。
>
> **`tm_isdst = 0`**：明确不用夏令时。配合 `app_main` 里的
> `setenv("TZ", "UTC0", 1)`，保证口径一致。
>
> **`(t > 0) ? t : MIN_EPOCH`**：转换失败兜底。

## 5.3 `build_wiz()`（第 287-305 行）

```c
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
```

> **⚠️ 所有 8 行都建出来，然后靠 `lv_obj_set_hidden` 控制显示**
> ——第 0 步隐藏 2~7 行，第 1 步隐藏 0~1 行。
>
> **为什么不分开建两个页面**：切换步骤时不用重建对象，只是显隐变化，更快更稳。
>
> **时间字段 y = 56 + i*27**（间隔 27px，F16 行高 20 ⇒ 间隙 7px）✓ 符合行距规则。
>
> **`lv_obj_move_background(s_wiz_cursor)`** ★ 关键：
> 光标是后建的，会盖在文字上面。`move_background` 把它**移到最后面**（z 轴最底），
> 这样文字在上、青色块在下 ⇒ 视觉上是"反色高亮"。
>
> **`lv_obj_set_style_pad_all(..., 3, ...)`**：给语言选项加 3px 内边距，
> 让反色块比文字大一圈，更像"按钮"。

## 5.4 `wiz_refresh()`（第 307-349 行）

```c
static void wiz_refresh(void)
{
    const bool is_lang = (s_wiz_step == 0);
    const int rows = is_lang ? 2 : (WIZ_TFIELD + 1);

    for (int i = 0; i < WIZ_ROWS; i++) {
        if (!s_wiz_row[i]) continue;
        lv_obj_set_hidden(s_wiz_row[i], is_lang ? (i >= 2) : (i < 2));
    }
```

> **显隐逻辑**：
> - `is_lang`（第 0 步）→ 隐藏 `i >= 2`（显示 0,1 = 两个语言选项）
> - 非第 0 步 → 隐藏 `i < 2`（显示 2~7 = 5 个字段 + 下一步）
>
> **`if (!s_wiz_row[i]) continue;`**：防御性检查。理论上所有行都建了，
> 但如果将来改成条件建对象，这里会崩。

```c
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
```

> **两套布局参数**（语言步 vs 时间步）：
> | | 语言步 | 时间步 |
> |---|---|---|
> | 光标 x | 112 | 104 |
> | 光标 y 基准 | 82 | 54 |
> | 行间距 | 36 | 27 |
> | 光标尺寸 | 96×26 | 170×23 |
>
> **时间步的选中效果用"黑字"而非"反色块"**：`mk_wiz_cursor` 的青色块在时间步
> 太宽（170px）不美观，所以只靠 `C_BLACK` 黑字表示选中（其他是 `C_TEXT` 亮白）。
> **语言步只有两个大选项，用整块反色更直观。**
>
> **语言步的 `bg_opa` 三元**：选中 `LV_OPA_COVER`（不透明青底）+
> 黑字；未选中 `LV_OPA_TRANSP`（透明）+ 亮白字。
>
> **`s_wiz_sel == WIZ_TFIELD`（=5）时"下一步"变黑字** = 选中了第 6 行。

```c
    char b[40];
    for (int i = 0; i < WIZ_TFIELD; i++) {
        snprintf(b, sizeof(b), (i == 0) ? "%s  %04d" : "%s  %02d",
                 TR(WIZ_FIELD_TID[i]), s_wiz_t[i]);
        lv_label_set_text(s_wiz_row[2 + i], b);
    }
```

> **格式化技巧**：年份是 4 位（`%04d`），月/日/时/分是 2 位（`%02d`）。
> 用三元在同一个 `snprintf` 里切换格式串。
>
> **`TR(WIZ_FIELD_TID[i])` 每次现取** —— 不缓存，切语言后自动正确。
> （但 label 一旦 set_text 就烤进了指针，所以切语言仍需 `ui_rebuild`。）

```c
    lv_label_set_text(s_title[PG_WIZ],
        s_wiz_step == 0 ? TR(TID_W_STEP_LANG) :       /* "第 1 步 · 语言" */
        s_wiz_step == 1 ? TR(TID_W_STEP_TIME) : TR(TID_W_STEP_DONE));
    if (s_wiz_step == 2)              lv_label_set_text(s_wiz_foot, TR(TID_W_ENTER_MENU));
    else if (is_lang)                 lv_label_set_text(s_wiz_foot, TR(TID_S_HINT_NAV));
    else if (s_wiz_sel == WIZ_TFIELD) lv_label_set_text(s_wiz_foot, TR(TID_W_TIME_NEXT));
    else                              lv_label_set_text(s_wiz_foot, TR(TID_S_HINT_EDIT));
    (void)rows;
}
```

> **标题随步骤变**："第 1 步 · 语言" → "第 2 步 · 时间" → "完成"。
>
> **底部提示四选一**：完成/导航/下一步/编辑。
>
> **⚠️ `(void)rows;` 是遗留物**：`rows` 在函数开头算出来但**从未被使用**
> （显隐逻辑用的是 `i >= 2` / `i < 2` 而不是 `i < rows`）。这是当初重构的残留。
> `-Wunused-variable` 会警告，用 `(void)x` 消音。
> **无害，但如果你要清理，这是一个候选。**

## 5.5 `wiz_page_key()` — 向导的按键处理（第 351-404 行）

```c
static int wiz_page_key(btn_id_t id, bool is_long)
{
    if (id == BTN_BACK && !is_long) {
        if (s_wiz_step > 0) { s_wiz_step--; s_wiz_sel = 0; wiz_refresh(); }
        else { s_in_menu = true; ui_goto(PG_HOME); home_apply_sel(); }
        return 0;
    }
    if (id == BTN_BACK) return 1;
```

> **短按 BACK = 上一步 / 退出向导**：
> - 第 1 步按 → 退回第 0 步（选语言）
> - 第 0 步按 → 放弃向导，进主菜单
>
> **长按 BACK 返回 1（交回全局）** —— 全局会把长按 DOWN 当"返回上一级"，
> 长按 BACK 当"切 AUTO"。在向导里这两个都不合适，所以直接交出去。

```c
    const int rows = (s_wiz_step == 0) ? 2 : (WIZ_TFIELD + 1);

    if (!is_long) {
        s_wiz_sel = (s_wiz_sel + rows + ((id == BTN_UP) ? -1 : 1)) % rows;
        wiz_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;
```

> **`(s_wiz_sel + rows + delta) % rows` 是循环移位的标准写法**：
> - `+rows` 是为了避免负数（C 的 `%` 对负数的行为是实现定义的）
> - `-1` 是 UP（向上移），`+1` 是 DOWN（向下移）
>
> **长按 DOWN = 返回上一级**（交回全局）—— 向导里长按下不改变字段值。

```c
    if (s_wiz_step == 0) {                       /* 选语言 → 存 + 重建 */
        g_ui_lang = (uint8_t)s_wiz_sel;
        (void)ui_cfg_set_lang((uint8_t)s_wiz_sel);
        wiz_time_from_now();
        s_wiz_step = 1; s_wiz_sel = 0;
        ui_rebuild();                            /* ★ 文案要换 → 整树重建 */
        return 0;
    }
```

> **★ 选完语言必须 `ui_rebuild()`**。
> 原因：`TR()` 返回的字符串指针已经被烤进所有 label。改 `g_ui_lang` 之后，
> 已存在的 label 还是旧语言。只有整树重建才能让所有文案换过来。
>
> **`ui_cfg_set_lang()` 立刻落 NVS** —— 即使用户后面不点"完成"，
> 语言选择也保住了。
>
> **`wiz_time_from_now()`** 在进第 1 步时用当前时间预填 5 个字段，
> 用户只需微调不用从零输。

```c
    if (s_wiz_step == 1) {
        if (s_wiz_sel == WIZ_TFIELD) {           /* 选中"下一步" → 完成 */
            s_wiz_step = 2; s_wiz_sel = 0;
            ui_cfg_set_now(wiz_time_epoch());
            wiz_refresh();
        } else {                                 /* 选中字段 → 加 1（循环） */
            static const int lo[WIZ_TFIELD] = { 2024, 1, 1, 0, 0 };
            static const int hi[WIZ_TFIELD] = { 2099, 12, 31, 23, 59 };
            int v = s_wiz_t[s_wiz_sel] + 1;
            if (v > hi[s_wiz_sel]) v = lo[s_wiz_sel];
            s_wiz_t[s_wiz_sel] = v;
            wiz_refresh();
        }
        return 0;
    }
```

> **`lo`/`hi` 边界表**：年份 2024-2099，月 1-12，日 1-31，时 0-23，分 0-59。
> **注意日没有按月份区分**（2 月 31 日这种非法值能输进去）。
> 这是刻意的简化——真实时钟芯片也不校验。**在注释里说清楚是有意为之。**
>
> **每次长按只 +1**（没有快速调节）。真实产品应该按住后加速，
> 但三个按键没有"加速"的表达空间。

```c
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
```

> **第 2 步（完成）长按 UP = 提交并进主菜单**。
>
> **⚠️ 为什么要显式调 `ui_cfg_save()`**：
> `ui_cfg_set_lang()` 已经保存过一次了，但 `ui_cfg_set_now()` 只改运行期基准**不落盘**。
> 这里把"语言 + 时间 + SNTP"打包成完整配置一次性写入。
>
> **为什么还要调 `ui_cfg_set_now(wiz_time_epoch())` 两次**：
> 一次在 step1 选"下一步"时（预览），一次在这里（最终提交）。
> 第二次是为了确保基准和写入 NVS 的值一致。
>
> **`s_in_menu = true; s_sel = 0;`** = 进主菜单时光标在第一项。
> **`home_apply_sel()`** 必须调，否则菜单文字还是未赋色状态（黑字黑底）。

---

# 六、页 2：HOME 主菜单（第 406-477 行）

## 6.1 菜单数据（第 407-414 行）

```c
#define MENU_N 7
static const uint16_t MENU_TID[MENU_N] = {
    TID_M_PROBE, TID_M_FLASH, TID_M_PINS, TID_M_STATUS, TID_M_SETUP, TID_M_ABOUT, TID_M_WIZ
};
static const int   MENU_PG[MENU_N] = { PG_PROBE, PG_FLASH, PG_PINS, PG_STATUS, PG_SETUP, PG_ABOUT, PG_WIZ };
static const char *MENU_GLYPH[MENU_N] = { "○", "↓", "■", "◎", "☆", "◆", "↻" };

static lv_obj_t *s_menu_row[MENU_N];
```

> **三张平行表**：`MENU_TID`（文案）/ `MENU_PG`（目标页）/ `MENU_GLYPH`（图标）。
> 索引一一对应。
>
> ⚠️ **这 7 行的顺序是 `prog_ui_probe.c` 和 `shot_probe.c` 的硬依赖**：
> "菜单行 1 = 烧录"这样的假设写死在探针里。**插入新菜单项会让两个探针失效。**
>
> **图标全是字体里确实存在的几何字符**（`○↓■◎☆◆↻`），
> 不是 `LV_SYMBOL_*`（见文件头 1.2 的说明）。

## 6.2 `build_home()`（第 416-457 行）

```c
static void build_home(void)
{
    lv_obj_t *p = s_page[PG_HOME];
    const int rh = 20, gap = 2;          /* 行高 20，间隔 2 */
    int y = BODY_Y + 2;                  /* 从 y=36 开始 */

    for (int k = 0; k < MENU_N; k++) {
        lv_obj_t *row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, SCR_W - 16, rh);      /* 304px 宽 */
        lv_obj_set_pos(row, 8, y);                 /* 左右各留 8px */
        lv_obj_set_style_radius(row, 4, LV_PART_MAIN);
        lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
        lv_obj_set_scrollable(row, false);
        s_menu_row[k] = row;

        lv_obj_t *g = lv_label_create(row);
        lv_label_set_text(g, MENU_GLYPH[k]);
        lv_obj_set_style_text_font(g, F16, LV_PART_MAIN);
        lv_obj_align(g, LV_ALIGN_LEFT_MID, 8, 0);      /* 左边距 8，垂直居中 */

        lv_obj_t *t = lv_label_create(row);
        lv_label_set_text(t, TR(MENU_TID[k]));
        lv_obj_set_style_text_font(t, F16, LV_PART_MAIN);
        lv_obj_align(t, LV_ALIGN_LEFT_MID, 32, 0);     /* 左 32px，避开图标 */

        lv_obj_t *a = lv_label_create(row);
        lv_label_set_text(a, "→");
        lv_obj_set_style_text_font(a, F14, LV_PART_MAIN);
        lv_obj_align(a, LV_ALIGN_RIGHT_MID, -8, 0);    /* 右边距 8 */

        y += rh + gap;      /* 22px 一步 */
    }
    mk_hint(p, TR(TID_S_HINT_NAV));

    /* ⚠️ 必须在这里就上一次色 …（详见下方说明） */
    home_apply_sel();
}
```

> **布局计算**：
> ```
> y 起 36，行高 20，间隔 2 → 每行步进 22px
> 第 0 行: 36-56   第 1 行: 58-78   第 2 行: 80-100
> 第 3 行: 102-122 第 4 行: 124-144 第 5 行: 146-166
> 第 6 行: 168-188
> 底部提示 y=203（不冲突）
> ```
>
> **行内三个子元素都用 `lv_obj_align(LEFT_MID/RIGHT_MID)`**：
> 父容器 `row` 高 20（固定），`*_MID` 对齐保证垂直居中，
> 不受字体行高影响。比 `set_pos` 更稳。
>
> **图标 x=8，文字 x=32**：32-8 = 24px，足够放一个 16px 图标 + 间距。
>
> **★ `home_apply_sel()` 必须在这里调**（注释很长，值得完整看）：
> ```c
> /* ⚠️ 必须在这里就上一次色：菜单项的文字是在 home_apply_sel() 里才被赋色的，
>  *    而那一层又是用 lv_obj_remove_style_all() 建的（无默认样式）。
>  *    如果只等「用户按键时」才调用，首次进入主菜单会看到**一整页隐形的菜单**
>  *    （黑字画在黑底上）—— 2026-10-02 抓图探针实测踩到，非黑像素只有 3219。
>  *    旧代码本来就在 build_home 末尾调了一次，重写时漏掉，这里补回并写清原因。 */
> ```
> **机制**：`lv_obj_remove_style_all()` 清掉了默认样式，label 的文字颜色变成
> **默认的黑/透明**。`home_apply_sel()` 负责把每行文字设成 `col` 或 `C_BLACK`。
> 不调它 = 黑字画在黑底上 = 整页隐形。
>
> **这个 bug 极其隐蔽**：编译零错误、日志正常、按键逻辑正常，
> **只有看屏幕才能发现**。这就是"抓图自证"的价值。

## 6.3 `home_apply_sel()` — 菜单选中样式（第 459-477 行）

```c
static void home_apply_sel(void)
{
    for (int k = 0; k < MENU_N; k++) {
        const bool sel = (k == s_sel);
        lv_color_t col = lv_color_hex(PINFO[MENU_PG[k]].accent);   /* 该项对应页的强调色 */
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
```

> **★ 视觉设计的关键洞察**：
> 每行的强调色 = **它所指向的功能页的强调色**。
> 所以主菜单上：
> ```
> 探针检测 → 青（PROBE 页是青）
> 烧录器   → 薄荷绿（FLASH 页是绿）
> 引脚档案 → 黄（PINS 页是黄）
> 状态信息 → 青
> 系统设置 → 品红（SETUP 页是品红）
> 关于     → 青
> 设置向导 → 青
> ```
> **用户在主菜单就能"预知"进去之后的页面配色**。这是把 `PINFO` 表复用到菜单的巧思。
>
> **选中态三件套**：
> - 边框不透明度 `COVER`（未选中是 30%）
> - 背景填充强调色（未选中透明）
> - 10px 辉光阴影
>
> **子元素统一改色**：
> ```c
> for (uint32_t i = 0; i < lv_obj_get_child_count(row); i++)
>     lv_obj_set_style_text_color(lv_obj_get_child(row, i), sel ? C_BLACK : col, ...);
> ```
> 每行有 3 个子 label（图标/文字/箭头），选中时全变黑字（压在彩色底上），
> 未选中时全用该行的强调色。**一次循环处理完，不用手动记 3 个指针。**
>
> **`lv_obj_get_child_count` / `lv_obj_get_child`**：v9 的 API。
> 在父对象没有额外子元素的前提下这是安全的。
