# 逐行代码注解（一）启动与显示层

> 对应源码：`main/app_main.c`(151行) + `main/lcd_hw.c`(223行) + `main/lcd_hw.h`(57行)
> 代码基线：`build_v3` md5 `fe0b41df2e45f33013cc3e85f575b3e4`
> 读法：左边是源码，右边是「这行在干什么 / 为什么这么写」。

---

# 一、`app_main.c` — 程序入口（151 行）

## 1.1 文件头注释（第 1-17 行）

```c
/**
 * app_main.c — 入口
 *
 * ⚠️ 顺序是硬约束，不能调换：
 *    1. lcd_hw_init()  建 SPI→panel_io→panel→LVGL display（不含刷新任务）
 *    2. ui_init()      建全部 UI 对象（此时无并发，安全）
 *    3. lcd_hw_start() 最后才启动 LVGL 刷新任务
 * ...
 */
```

> **这段注释本身就是重要资产。** 它记录了一次"受控实验后的定论"，包括推翻旧假设的过程。
> 如果将来有人（或未来的我）想"优化"启动顺序，这段注释就是拦路虎。

## 1.2 include 段（第 18-42 行）

```c
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#include "lcd_hw.h"
#include "ui.h"
#include "wifi_sta.h"    /* 真实 WiFi STA（2026-09-29 新增） */
#include "ui_cfg.h"      /* P0 NVS 配置层（2026-10-01） */
#include "i18n.h"        /* g_ui_lang */
#if PIN_PROBE
#include "pin_probe.h"
#endif
```

> **看什么**：裸引号是本工程模块；`<>` 是 IDF/标准库。
> `PIN_PROBE` / `SHOT_PROBE` / `PROG_UI_TEST` 是 CMake 传进来的宏，**产品构建全是 0**，
> 所以这三段 include 根本不参与编译。

```c
#if PROG_UI_TEST
/* 烧录页「按键 → 后端」写路径自检（仅排查构建） */
void prog_ui_probe_start(void);
#endif
```

> **为什么这里手写声明而不 include**：`prog_ui_probe.c` 在产品构建下是**空翻译单元**
> （整个文件包在 `#if PROG_UI_TEST` 里），include 它会得到一个空文件，手写声明更直接。

```c
#include "cjk_probe.h"    /* 产品构建下这两个是空实现 */

static const char *TAG = "app";
```

> `TAG` 是 `ESP_LOGx` 用的日志标签，会打印成 `I (1234) app: ...`。

## 1.3 `app_main()` 主体（第 44-151 行）

### ① 字形覆盖自检（第 46-47 行）

```c
ESP_LOGI(TAG, "=== AI 远程调试器 UI (LVGL) 启动 ===");
cjk_probe_report();        /* 刷机前就能跑：字形覆盖自检，不依赖屏幕 */
```

> **为什么放在最前**：这个检查不碰屏幕、不碰 LVGL，纯查字形表。
> 放在最前 = 如果中文字体有问题，**串口第一条日志就报出来**，不用等 30 秒看黑屏。
>
> 产品构建下 `CJK_SMOKETEST=0` → `cjk_probe_report()` 是空函数，零开销。

### ② 载入 NVS 配置（第 49-61 行）

```c
ui_cfg_t cfg;
bool cfg_loaded = ui_cfg_init(&cfg);
g_ui_lang = cfg.lang;      /* 出厂默认中文（cfg_loaded=false 时也是 0） */
```

> **这是整个 `app_main` 里最容易改错的一行。** `g_ui_lang` 必须在 `ui_init()` **之前**灌进去。
>
> **为什么**：`ui_init()` → `build_all()` → `build_home()` 会立刻调 `TR(MENU_TID[k])`
> 取文案并写进 label。如果语言晚一步，标签已经用中文建好了，之后再改成英文就成了
> "半中半英"或者需要整树重建才生效。
>
> `cfg_loaded` 为 false 时 `cfg` 也被填了出厂默认值（`defaults()` 做的），所以
> `g_ui_lang = cfg.lang` 两种情况都安全。

```c
{
    int64_t now = ui_cfg_now();
    ESP_LOGI(TAG, "配置: %s lang=%u(%s) sntp=%u 时间=%s",
             cfg_loaded ? "已载入" : "首启/无记录",
             cfg.lang, UI_LANG_NAME[cfg.lang], cfg.sntp,
             (now > 0) ? "已设定" : "未定(待首启向导)");
}
```

> **为什么要用 `{ }` 包起来**：C 里这是给变量划作用域，让 `now`/`cfg` 不泄漏到后面。
> 纯风格，但大项目里能减少"变量名撞车"。
>
> `UI_LANG_NAME[cfg.lang]` 是 `{"中文", "English"}` 数组，**故意不缓存**（每次现取）。

### ③ 探针区（第 63-82 行）

```c
#if UI_CFG_SELFTEST
    { int f = ui_cfg_selftest(); ... }
#endif
#if UI_CFG_PERSIST_TEST
    { int p = ui_cfg_persist_probe(); ... }
#endif
#if PIN_PROBE
    pin_probe_run();
#endif
```

> **两个 selftest 的顺序有意义**：`UI_CFG_SELFTEST` 在前，`PERSIST_TEST` 在后。
> 反过来的话，selftest 会擦 NVS 再还原，把写进去的配置覆盖掉。

### ④ 初始化屏幕（第 84-88 行）

```c
esp_err_t err = lcd_hw_init();
if (err != ESP_OK) {
    ESP_LOGE(TAG, "lcd_hw_init 失败: %s —— UI 不启动", esp_err_to_name(err));
    return;
}
```

> **检查返回值并早退**：`lcd_hw_init` 失败的话，屏幕没有 display，
> 后面 `ui_init()` 里的 `lv_display_create` 上下文不存在，会崩。
> **早退比崩好**——至少串口上有明确的错误原因。

### ⑤ 产品分支 vs 竞态放大分支（第 90-148 行）

```c
#if RACE_AMPLIFY
    /* 反面教材：故意用「旧顺序」—— 刷新任务先跑起来，然后才建 UI 对象，
       且在每次 lv_obj_create 后让出 CPU */
    ESP_LOGW(TAG, "!!! RACE_AMPLIFY 已启用：旧顺序 + ui_init 让出点（仅排查用）!!!");
    err = lcd_hw_start();            /* 旧顺序：刷新任务先启动 */
    if (err != ESP_OK) { ...; return; }
    ui_init();                       /* 与刷新任务并发遍历同一棵对象树 */
    {
        uint32_t iters = lcd_hw_refresh_iters();
        UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGW(TAG, "RACE_AMPLIFY 结果: ui_init 期间刷新任务已跑 %u 轮"
                      "（>100 即确证并发）| app_main 栈剩余 %u 字(%u 字节)",
                 (unsigned)iters, (unsigned)hw, (unsigned)(hw * sizeof(StackType_t)));
        ESP_LOGW(TAG, "RACE_AMPLIFY: ui_init 竟然跑完了（未崩溃）");
    }
#else
```

> **这是全工程最有价值的一段"反面教材"**，务必读懂：
>
> 1. 这里的代码顺序是**故意写错的**（刷新任务先启动）。
> 2. 假设是"崩溃 = 刷新任务 vs ui_init 竞态"。
> 3. `lcd_hw_refresh_iters()` 返回刷新任务跑过多少轮 `lv_timer_handler`，
>    用来**证明并发确实发生了**（不是"假设有并发"）。
> 4. `uxTaskGetStackHighWaterMark` 打印栈水位，顺手排掉"栈溢出"这条线。
> 5. **实测结果：14/14 不崩 → 假设被证伪。** 真根因是 LVGL 内置 64KB TLSF 堆撑爆
>    （`CONFIG_LV_USE_CLIB_MALLOC=y` 修复），不是竞态。
>
> 保留这段代码的价值：将来谁再想论证"这是竞态"，直接跑这个开关就知道了。

### ⑥ 产品分支（`#else` 之后，第 114-141 行）

```c
ui_init();                       /* ① 建对象：此刻刷新任务还没跑，独占 LVGL */

if (!cfg_loaded) {
    ESP_LOGI(TAG, "首次使用：进入设置向导（语言 → 时间 → 进入主菜单）");
    ui_start_wizard();
}

cjk_probe_init();                /* 冒烟屏覆盖到屏上（产品构建=空实现） */

wifi_st_start();

err = lcd_hw_start();            /* ② 对象建完，才放刷新任务出来 */
if (err != ESP_OK) { ...; return; }
```

> **`ui_start_wizard()` 在 `ui_init()` 之后**：`ui_init()` 已经把 9 页全建好了，
> 这里只是 `ui_goto(PG_WIZ)` 切页 + 初始化向导状态变量。**不是**"在 ui_init 之前
> 决定要进向导"——那样做不到，因为页还没建。
>
> **`wifi_st_start()` 为什么在这里**：`wifi_st_start()` 只做两件事（建互斥量 + 起任务），
> 立刻返回，耗时不长。真正耗时的 NVS 初始化/扫描/连接全在 `wifi_mgr_task` 里跑。
> 放在 `ui_init()` 之后是为了让 UI 先把内部 RAM 占稳。
>
> **`lcd_hw_start()` 最后**：见 1.3 ⑤ 的说明。

### ⑦ 写路径探针（第 143-147 行）

```c
#if PROG_UI_TEST
    /* 刷新任务已在跑 → 此时注入合成按键才有人消费 */
    prog_ui_probe_start();
#endif
```

> **必须在 `lcd_hw_start()` 之后**：合成按键进队列后，需要 `lvgl` 任务里的
> `tick_cb → handle_buttons()` 来消费。刷新任务没起来 = 没人消费 = 探针白跑。

---

# 二、`lcd_hw.h` — 显示层接口（57 行）

## 2.1 文件头注释（第 1-10 行）

```c
/**
 * lcd_hw.h — 2.0" ST7789P3 (240x320, 10 脚) 硬件层 + LVGL 对接
 *
 * 接线（与你已验证的 panel_lcd 保持一致）：
 *   MOSI/SDA = GPIO7   SCLK/SCL = GPIO6   DC/RS = GPIO4   RST = GPIO5   CS = GPIO10
 *   背光 LED+ -> 3V3(串限流) | LED- -> GND   （纯电源，固件不控）
 *
 * 初始化序列 = 移植自已实测锁定的 panel_lcd.c INIT_240x320（golden 0xA36B04D1）
 */
```

> **`golden 0xA36B04D1` 是怎么来的**：一条 14 字节的初始化序列（`VENDOR_INIT` 表）
> 用 FNV-1a-32 算出的校验值。这个值在**真机上验证过并锁定**，
> 意味着"如果哪天这个表被改坏了，会立刻发现"。改初始化序列前先想清楚。

## 2.2 常量定义（第 19-28 行）

```c
#define LCD_H_RES   320     /* 横屏宽 */
#define LCD_V_RES   240     /* 横屏高 */

/* 屏幕接线（改屏只改这里） */
#define LCD_PIN_MOSI  7
#define LCD_PIN_SCLK  6
#define LCD_PIN_DC    4
#define LCD_PIN_RST   5
#define LCD_PIN_CS    10
#define LCD_SPI_HZ    (20 * 1000 * 1000)   /* 杜邦线留余量；排线可提到 40M */
```

> **换屏 = 只改这 7 行**。这是本工程的"档案"设计思想（与 `hw_pin` 的引脚档案同源）。
>
> **20MHz 而非 40MHz**：杜邦线在 40MHz 下会有信号完整性问题（花屏/丢字节）。
> 如果换成排线，40MHz 没问题。**改这个值前先确认线材**。

## 2.3 两个 API 的语义（第 30-47 行）

```c
esp_err_t lcd_hw_init(void);   /* ① 建硬件+display，不启动刷新任务 */
esp_err_t lcd_hw_start(void);  /* ② 启动 lv_timer_handler 循环，幂等 */
```

> **为什么强制拆成两步**：`lcd_hw_init` 之后 LVGL 可以用了但没人驱动它；
> `lcd_hw_start` 之后开始有第二个任务碰 LVGL 了。
>
> 这个拆分让 `app_main` 能把"建 UI 对象"完全放在"第二个任务出现之前"，
> 从而彻底避开 LVGL 非线程安全的问题。
>
> `lcd_hw_start()` **幂等**（内部有 `s_lvgl_started` 标志），重复调安全。

## 2.4 竞态探针（第 49-51 行）

```c
uint32_t lcd_hw_refresh_iters(void);
```

> 只有一个用途：给 `RACE_AMPLIFY` 证明"刷新任务确实与 ui_init 并发过"。
> **不能用"假设有并发"来论证"消除了并发"**——必须实测。

---

# 三、`lcd_hw.c` — 显示层实现（223 行）

## 3.1 全局状态（第 20-35 行）

```c
static const char *TAG = "lcd_hw";

static esp_lcd_panel_io_handle_t s_io    = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;

/* DMA 完成回调需要一个 display 句柄，但 esp_lcd 的 user_ctx 必须在
 * "创建 panel_io 时"就固定，而 display 只能在 panel_io 之后创建 ——
 * 时序上无解。故用文件内静态全局兜底 */
static lv_display_t *s_disp = NULL;

static bool s_lvgl_started = false;
static volatile uint32_t s_refresh_iters = 0;
```

> **`s_disp` 为什么不能是 `user_ctx`**：
> `esp_lcd_panel_io_spi_config_t.user_ctx` 必须在**创建 panel_io 时**填好，
> 而 `lv_display_create()` 必须**在 panel_io 之后**才能调（flush_cb 要用 panel）。
> 鸡生蛋问题，时序上无解。
>
> 解法：文件内静态全局。**安全性论证**：DMA 完成回调只在首帧绘制后才触发，
> 那时 `s_disp` 必然已在 `lcd_hw_init` 里赋值（第 175 行）。
>
> `volatile` 是因为 `s_refresh_iters` 被 `lvgl` 任务写、被 `app_main` 任务读。

## 3.2 厂商初始化序列（第 41-74 行）

```c
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[14];
} lcd_init_cmd_t;

static const lcd_init_cmd_t VENDOR_INIT[] = {
    { 0xB2, 5,  { 0x0C, 0x0C, 0x00, 0x33, 0x33 } },   /* PORCTRL  */
    { 0xB7, 1,  { 0x35 } },                            /* GCTRL    */
    { 0xBB, 1,  { 0x28 } },                            /* VCOMS    */
    { 0xC0, 1,  { 0x2C } },                            /* LCMCTRL  */
    { 0xC2, 1,  { 0x01 } },                            /* VDVVRHEN */
    { 0xC3, 1,  { 0x12 } },                            /* VRHS     */
    { 0xC4, 1,  { 0x20 } },                            /* VDVS     */
    { 0xC6, 1,  { 0x0F } },                            /* FRCTRL2  */
    { 0xD0, 2,  { 0xA4, 0xA1 } },                      /* PWCTRL1  */
    { 0xE0, 14, { 0xD0, 0x04, 0x0D, ... } },          /* PVGAMCTRL 正伽马 */
    { 0xE1, 14, { 0xD0, 0x04, 0x0C, ... } },          /* NVGAMCTRL 负伽马 */
    { 0x21, 0,  { 0 } },                               /* INVON    */
    { 0x13, 0,  { 0 } },                               /* NORON    */
};
```

> **这是 ST7789 的 14 条厂商私有命令**，IDF 内置的 `esp_lcd_new_panel_st7789` 不做这些。
>
> **逐条是干什么的**：
> - `0xB2 PORCTRL`：POR 控制（行驱动时序）
> - `0xB7 GCTRL`：门控控制（灰度质量）
> - `0xBB VCOMS`：虚拟 COM 电平
> - `0xC0 LCMCTRL`：液晶模式控制
> - `0xC2/C3/C4`：`VDVVRHEN`/`VRHS`/`VDVS` = 垂直/水平电压范围 → **决定对比度**
> - `0xC6 FRCTRL2`：帧率控制
> - `0xD0 PWCTRL1`：电源控制
> - `0xE0/0xE1`：**正/负伽马校正**（14 字节一个通道）→ 决定灰度曲线
> - `0x21 INVON`：反色开
> - `0x13 NORON`：普通（非睡眠）模式

```c
static void send_vendor_init(void)
{
    for (size_t i = 0; i < sizeof(VENDOR_INIT) / sizeof(VENDOR_INIT[0]); i++) {
        const lcd_init_cmd_t *c = &VENDOR_INIT[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, c->cmd,
                                                  c->len ? c->data : NULL, c->len));
    }
    ESP_LOGI(TAG, "vendor init: %u 条已下发（PORCTRL/gamma/INVON 等）", ...);
}
```

> **`c->len ? c->data : NULL`**：长度为 0 的命令（`INVON`/`NORON`）不带参数，
> 传 `NULL` 而不是 `c->data`（避免传一个无意义的指针）。
>
> **`ESP_ERROR_CHECK`**：出错直接 panic 并打印。初始化序列发失败 = 屏幕配置错了，
> 继续跑下去只会显示花屏，不如立刻崩掉给出明确信息。
>
> `sizeof(x)/sizeof(x[0])` 是 C 的标准写法：拿数组总字节数除以单元素字节数 = 元素个数。

## 3.3 flush 回调（第 76-97 行）

```c
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    if (s_disp) lv_display_flush_ready(s_disp);
    return false;
}
```

> **这是全文件最容易写错的地方。** `lv_display_flush_ready()` **必须**在 DMA 完成
> 回调里调用，**不能**在 `flush_cb` 末尾调用。
>
> **为什么**：LVGL 的双缓冲机制是"我给你一个缓冲，你画完告诉我可以还我了"。
> 如果在 `flush_cb` 里就告诉 LVGL "画完了"，LVGL 会立刻复用这个缓冲去画下一帧，
> **而 DMA 还在往里写** → 撕裂/花屏。
>
> `(void)xxx` 是 C 里"显式忽略未使用参数"的惯用法，能关掉 `-Wunused-parameter` 警告。
>
> `return false` = 不需要更多队列空间（这个回调不申请队列）。

```c
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* 1) 字节序：RGB565 高低字节对调 */
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, w * h);

    /* 2) 送给面板（异步 DMA；完成回调里 flush_ready） */
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}
```

> **`lv_draw_sw_rgb565_swap` 是必做的一步**：LVGL 输出 RGB565 **小端**（低字节在前），
> 而 SPI 走 ST7789 的 SPIMODEE 是**大端**。不换字节序 = **颜色整体错乱/花屏**。
> 这是除了 `LCD_RGB_ENDIAN_BGR/RGB` 之外最容易漏的一步。
>
> `w * h` 是像素数，`lv_draw_sw_rgb565_swap` 按这个数量逐个交换高低字节。
>
> **`area->x2 + 1`**：LVGL 的 `area->x2` 是**包含式**的右边界，
> 但 `esp_lcd_panel_draw_bitmap` 的 `x_end` 是**开区间** → 要 +1。
>
> `disp` 参数没用到（用了静态 `s_panel`），未使用不报错是因为 LVGL 回调签名固定。

## 3.4 LVGL 心跳与刷新任务（第 99-114 行）

```c
static uint32_t lvgl_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);   /* µs → ms */
}
```

> LVGL 需要一个毫秒心跳来驱动动画/定时器。没有独立定时器，
> 直接每次问 `esp_timer_get_time()`（微秒）除 1000 变毫秒。**零成本、零额外资源。**

```c
static void lvgl_task(void *arg)
{
    (void)arg;
    for (;;) {
        s_refresh_iters++;
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 16) delay_ms = 16;    /* 上限 ~60fps，避免饿死其他任务 */
        if (delay_ms < 2)  delay_ms = 2;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
```

> **`lv_timer_handler()` 返回值是"建议下次多久后再调"**。
> - 上限 16ms = ~60fps。再快没有意义，还占 CPU。
> - 下限 2ms = 防止 LVGL 返回 0 时**空转烧 CPU**（100% 占用）。
>
> `pdMS_TO_TICKS(2)` 在 `FREERTOS_HZ=1000` 时就是 2 tick。
> `FREERTOS_HZ=1000` 意味着 1 tick = 1ms（见 sdkconfig.defaults）。

## 3.5 `lcd_hw_init()` 五步（第 116-199 行）

### 第 1 步：SPI 总线（第 119-127 行）

```c
spi_bus_config_t bus = {
    .sclk_io_num     = LCD_PIN_SCLK,
    .mosi_io_num     = LCD_PIN_MOSI,
    .miso_io_num     = -1,              /* 只用写 */
    .quadwp_io_num   = -1,
    .quadhd_io_num   = -1,
    .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
};
ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi_bus");
```

> **`-1` = 不用这个信号**。屏是纯写设备（LVGL 画好直接送过去，不需要读回），所以 MISO 不用。
>
> **`max_transfer_sz = 320 × 80 × 2 = 51200`**：LVGL 缓冲是 1/10 屏（320×24），
> 但 DMA 有对齐/分块要求，留够 80 行余量。这个值必须 ≥ 实际最大传输量，否则 DMA 失败。

### 第 2 步：面板 IO（第 130-142 行）

```c
esp_lcd_panel_io_spi_config_t io_cfg = {
    .cs_gpio_num         = LCD_PIN_CS,
    .dc_gpio_num         = LCD_PIN_DC,
    .spi_mode            = 0,            /* CPOL=0 CPHA=0，ST7789 通用 */
    .pclk_hz             = LCD_SPI_HZ,   /* 20MHz */
    .trans_queue_depth   = 10,           /* 最多排队 10 笔传输 */
    .on_color_trans_done = on_color_trans_done,
    .user_ctx            = NULL,         /* 稍后回填 display（实际回填不了，见注释） */
    .lcd_cmd_bits        = 8,
    .lcd_param_bits      = 8,
};
```

> **`.dc_gpio_num` 是 D/C（Data/Command）**：ST7789 用它区分"这串是命令还是数据"。
> 高=数据，低=命令。
>
> **`trans_queue_depth = 10`**：SPI 传输队列深度。太浅会导致 `esp_lcd_panel_draw_bitmap`
> 频繁阻塞等待；10 够 LVGL 双缓冲用。
>
> **`lcd_cmd_bits=8` / `lcd_param_bits=8`**：ST7789 的命令/参数都是 8 位
> （有些屏是 16 位，SPI mode 也不同）。

### 第 3 步：面板 + 初始化（第 145-158 行）

```c
esp_lcd_panel_dev_config_t dev = {
    .reset_gpio_num = LCD_PIN_RST,
    .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
    .bits_per_pixel = 16,
};
ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &dev, &s_panel), TAG, "panel");
ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");

/* 关键顺序：厂商序列必须在 panel_init 之前 —— 其中 0x21 INVON 要生效在
   任何像素写入之前，否则首帧颜色是反的。 */
send_vendor_init();

ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
```

> **`send_vendor_init()` 必须在 `panel_init()` 之前** —— 这是本文件最关键的顺序约束。
>
> **为什么**：`0x21 INVON`（反色开）必须**在任何像素写入之前**生效，
> 否则第一帧就是反的。`esp_lcd_panel_init()` 内部会做 `MADCTL`/`COLMOD` 等基础设置，
> 如果 INVON 在它之后发，第一帧的像素已经用错误配置写出去了。

### 第 4 步：横屏 320×240（第 160-167 行）

```c
ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap_xy");
ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "gap");
ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, false), TAG, "invert");
ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on");
```

> **`swap_xy(true)`** = 交换 X/Y → 240×320 竖屏变 320×240 横屏。
>
> **`mirror(true, false)`** = 水平镜像开，垂直镜像关。等价于 MADCTL 的 `MX` 位置 1。
> 合起来 = `MADCTL = 0x60`（MX|MV）。**如果方向不对（左右/上下翻），改这两个 mirror 参数。**
>
> **`set_gap(0, 0)`** = 无偏移。320×240 屏正好 320×240，内存里的坐标 1:1 对应屏幕，
> 不用像某些屏那样扣掉边距。
>
> **`invert_color(false)`** = 不反色（配合前面的 `INVON`，这里保持"正常"）。
>
> **`disp_on_off(true)`** = 开显示。放在最后，前面都配好了再开屏。

### 第 5 步：LVGL（第 169-198 行）

```c
lv_init();
lv_tick_set_cb(lvgl_tick_cb);

lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
if (!disp) return ESP_FAIL;
s_disp = disp;          /* 供 DMA 完成回调使用 */
```

> **`lv_display_create` 可能返回 NULL**（内存不足）—— 必须检查。
> LVGL 用了 `CLIB_MALLOC` 后一般不会失败，但 `ASSERT_MALLOC` 开着时会直接 panic
> 而不是返回 NULL。这个检查是双保险。

```c
/* 双缓冲：各 1/10 屏，放内部 DMA 内存（SPI+DMA 要求） */
const size_t buf_px = LCD_H_RES * (LCD_V_RES / 10);
void *buf1 = heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
void *buf2 = heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
if (!buf1 || !buf2) {
    ESP_LOGE(TAG, "LVGL 缓冲分配失败");
    return ESP_ERR_NO_MEM;
}
lv_display_set_buffers(disp, buf1, buf2, buf_px * sizeof(uint16_t),
                       LV_DISPLAY_RENDER_MODE_PARTIAL);
lv_display_set_flush_cb(disp, lvgl_flush_cb);
```

> **`buf_px = 320 × 24 = 7680` 像素，每个缓冲 7680×2 = 15360 字节。**
>
> **`MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL` 是必须的**：SPI DMA 控制器不能直接访问 PSRAM
> （外部 RAM），只能访问内部 RAM。不加 `MALLOC_CAP_DMA` 会拿到一个可能在 PSRAM 的指针
> → DMA 传输失败或数据损坏。
>
> **`LV_DISPLAY_RENDER_MODE_PARTIAL`**：部分刷新模式。LVGL 每次只画变化区域到缓冲里，
> 不需要重画整屏。配合 1/10 屏的小缓冲省内存。
>
> **为什么两个缓冲**：双缓冲 = LVGL 画 A 缓冲时屏幕显示 B 缓冲，画完交换。
> 少一个会有撕裂。`flush_cb` 里没有 `flush_ready` 的问题见 3.3。

```c
ESP_LOGI(TAG, "LCD 就绪: %dx%d 横屏, SPI %d MHz, MADCTL 0x60", ...);

/* ⚠️ 这里【不】启动 LVGL 刷新任务！见 lcd_hw_start() 的说明 */
return ESP_OK;
```

> **"MADCTL 0x60" 是硬编码在日志里的**（`swap_xy(true)` + `mirror(true,false)` 的效果）。
> 如果改了 mirror 参数，这条日志会变成"撒谎的"。改的时候记得同步。

## 3.6 `lcd_hw_start()`（第 201-217 行）

```c
esp_err_t lcd_hw_start(void)
{
    if (s_lvgl_started) return ESP_OK;     /* 幂等 */

    /* LVGL 不是线程安全的！刷新任务一旦跑起来就会持续遍历对象树，
     * 此时若另一个任务还在调用 lv_* 建对象（ui_init），对象图会被撕裂 */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);
    s_lvgl_started = true;
    ESP_LOGI(TAG, "LVGL 刷新任务已启动（UI 构建完成之后）");
    return ESP_OK;
}
```

> **栈 8192 = 8KB**：LVGL 的 `lv_timer_handler` 会调 draw 流程，有一定栈深度需求。
> 比 `btn`/`prog` 任务大是合理的。
>
> **优先级 4**：`btn`(5) / `prog`(5) / `wifi_mgr`(5) 都比它高。
> 意思是"按键、烧录这些实时性要求高的任务先跑，LVGL 刷新可以稍等"。
> **但 LVGL 是 4（不是最低）**，保证它在高优先级任务不占满 CPU 时能正常刷新。
>
> **幂等保护**：`app_main` 里 `lcd_hw_start()` 只调一次，但 `RACE_AMPLIFY` 分支
> 和正常分支都可能走到，加个标志位更安全。

## 3.7 探针接口（第 219-223 行）

```c
uint32_t lcd_hw_refresh_iters(void)
{
    return s_refresh_iters;
}
```

> 就是返回 `s_refresh_iters`。给 `RACE_AMPLIFY` 用。
> 判断标准：`>100` = `ui_init` 期间刷新任务跑了 100+ 轮 = 并发确实发生了。
