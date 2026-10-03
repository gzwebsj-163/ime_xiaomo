/**
 * lcd_hw.c — SPI + ST7789P3 + LVGL 对接实现
 */
#include "lcd_hw.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

static const char *TAG = "lcd_hw";

static esp_lcd_panel_io_handle_t s_io    = NULL;
static esp_lcd_panel_handle_t    s_panel = NULL;

/* DMA 完成回调需要一个 display 句柄，但 esp_lcd 的 user_ctx 必须在
 * “创建 panel_io 时”就固定，而 display 只能在 panel_io 之后创建 ——
 * 时序上无解。故用文件内静态全局兜底（回调只在首帧绘制后才会触发，
 * 那时 s_disp 必然已赋值），彻底避开 NULL 解引用。 */
static lv_display_t *s_disp = NULL;

/* LVGL 刷新任务是否已启动（幂等保护）——见 lcd_hw_start() */
static bool s_lvgl_started = false;

/* 刷新任务实际跑过的次数（竞态排查用：证明刷新任务确实与 ui_init 并发过） */
static volatile uint32_t s_refresh_iters = 0;

/* ------------------------------------------------------------------
 * 厂商初始化序列（除 IDF 内置的 SLPOUT/MADCTL/COLMOD/RAMCTRL 之外的部分）
 * 逐条照搬 panel_lcd.c INIT_240x320 —— 该序列已在真机验证并锁定黄金值。
 * ------------------------------------------------------------------ */
typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t data[14];
} lcd_init_cmd_t;

static const lcd_init_cmd_t VENDOR_INIT[] = {
    { 0xB2, 5,  { 0x0C, 0x0C, 0x00, 0x33, 0x33 } },                  /* PORCTRL  */
    { 0xB7, 1,  { 0x35 } },                                           /* GCTRL    */
    { 0xBB, 1,  { 0x28 } },                                           /* VCOMS    */
    { 0xC0, 1,  { 0x2C } },                                           /* LCMCTRL  */
    { 0xC2, 1,  { 0x01 } },                                           /* VDVVRHEN */
    { 0xC3, 1,  { 0x12 } },                                           /* VRHS     */
    { 0xC4, 1,  { 0x20 } },                                           /* VDVS     */
    { 0xC6, 1,  { 0x0F } },                                           /* FRCTRL2  */
    { 0xD0, 2,  { 0xA4, 0xA1 } },                                     /* PWCTRL1  */
    { 0xE0, 14, { 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B,
                  0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23 } }, /* PVGAMCTRL */
    { 0xE1, 14, { 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C,
                  0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23 } }, /* NVGAMCTRL */
    { 0x21, 0,  { 0 } },                                              /* INVON    */
    { 0x13, 0,  { 0 } },                                              /* NORON    */
};

static void send_vendor_init(void)
{
    for (size_t i = 0; i < sizeof(VENDOR_INIT) / sizeof(VENDOR_INIT[0]); i++) {
        const lcd_init_cmd_t *c = &VENDOR_INIT[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, c->cmd,
                                                  c->len ? c->data : NULL, c->len));
    }
    ESP_LOGI(TAG, "vendor init: %u 条已下发（PORCTRL/gamma/INVON 等）",
             (unsigned)(sizeof(VENDOR_INIT) / sizeof(VENDOR_INIT[0])));
}

/* ------------------------------------------------------------------
 * LVGL flush：LVGL 出 RGB565 小端，而 LCD 走 SPIMODEE 大端 → 必须 swap。
 * 这就是「LCD_RGB_ENDIAN_BGR/RGB」之外最容易漏的一步（颜色整体错乱/花屏）。
 * ------------------------------------------------------------------ */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    if (s_disp) lv_display_flush_ready(s_disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* 1) 字节序：RGB565 高低字节对调 */
    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(px_map, w * h);

    /* 2) 送给面板（异步 DMA；完成回调里 flush_ready） */
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
}

static uint32_t lvgl_tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);   /* µs → ms */
}

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

esp_err_t lcd_hw_init(void)
{
    /* ---------- 1. SPI 总线 ---------- */
    spi_bus_config_t bus = {
        .sclk_io_num     = LCD_PIN_SCLK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,              /* 只用写 */
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * 80 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi_bus");

    /* ---------- 2. 面板 IO ---------- */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = LCD_PIN_CS,
        .dc_gpio_num         = LCD_PIN_DC,
        .spi_mode            = 0,
        .pclk_hz             = LCD_SPI_HZ,
        .trans_queue_depth   = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx            = NULL,   /* 稍后回填 display */
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                                 &io_cfg, &s_io), TAG, "panel_io");

    /* ---------- 3. 面板（ST7789 内置驱动只做基础初始化）---------- */
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &dev, &s_panel), TAG, "panel");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");

    /* 关键顺序：厂商序列必须在 panel_init 之前 —— 其中 0x21 INVON 要生效在
     * 任何像素写入之前，否则首帧颜色是反的。 */
    send_vendor_init();

    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    /* ---------- 4. 横屏 320x240 ---------- */
    /* MV|MX = 0x60，是 240x320 面板转横屏的常用组合。
     * 若方向不对（左右/上下翻），改下面两个 mirror 参数即可。 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, true), TAG, "swap_xy");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 0, 0), TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, false), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "disp_on");

    /* ---------- 5. LVGL ---------- */
    lv_init();
    lv_tick_set_cb(lvgl_tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    if (!disp) return ESP_FAIL;
    s_disp = disp;          /* 供 DMA 完成回调使用，见文件头注释 */

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

    /* 回填 user_ctx 对 esp_lcd 无效（创建后不可改），回调走 s_disp，
     * 见 on_color_trans_done。 */

    ESP_LOGI(TAG, "LCD 就绪: %dx%d 横屏, SPI %d MHz, MADCTL 0x60",
             LCD_H_RES, LCD_V_RES, LCD_SPI_HZ / 1000000);

    /* ⚠️ 这里【不】启动 LVGL 刷新任务！
     * 见 lcd_hw_start() 的说明：刷新任务必须等 ui_init() 建完对象再启动，
     * 否则 lv_timer_handler 会与 ui_init 并发遍历/修改同一棵对象树 → 崩溃。 */
    return ESP_OK;
}

esp_err_t lcd_hw_start(void)
{
    if (s_lvgl_started) return ESP_OK;

    /* LVGL 不是线程安全的！刷新任务一旦跑起来就会持续遍历对象树，
     * 此时若另一个任务还在调用 lv_* 建对象（ui_init），对象图会被撕裂，
     * 表现为 lv_display_refr_timer → lv_obj_get_style_prop_internal 里
     * NULL 解引用（LoadProhibited，EXCVADDR 很小）。
     *
     * 该故障是**时序竞态**：同一二进制时而崩时而不崩（实测首次启动崩、
     * panic 重启后正常跑完）。必须在所有 UI 构建完成后才启动刷新任务。
     */
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);
    s_lvgl_started = true;
    ESP_LOGI(TAG, "LVGL 刷新任务已启动（UI 构建完成之后）");
    return ESP_OK;
}

/* 竞态排查探针：刷新任务到此刻一共调用过多少次 lv_timer_handler */
uint32_t lcd_hw_refresh_iters(void)
{
    return s_refresh_iters;
}
