/**
 * ESP32-C3 + ST7789 240x240 + LVGL 9.6 电压/电流/功率检测界面
 * - TFT SPI : SCLK=GPIO6  MOSI=GPIO7  CS=GPIO10  DC=GPIO4  RST=GPIO5  BL=GPIO0
 * - INA226  : I2C SDA=GPIO8 SCL=GPIO9（读不到自动切模拟数据模式）
 */
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "ina226.h"

static const char *TAG = "tft_va";

/* ---------- 引脚定义 ---------- */
#define PIN_SCLK    6
#define PIN_MOSI    7
#define PIN_CS      10
#define PIN_DC      4
#define PIN_RST     5
#define PIN_BL      0
#define PIN_SDA     8
#define PIN_SCL     9

#define LCD_H_RES   240
#define LCD_V_RES   240

/* ---------- UI 全局 ---------- */
typedef struct { lv_obj_t *value; lv_obj_t *unit; } card_t;
static card_t g_card_v, g_card_a, g_card_p;
static lv_chart_series_t *g_ser_v, *g_ser_i;
static lv_obj_t *g_chart, *g_dot, *g_st;

static float g_sim_t = 0.0f;
static int   g_fail_cnt = 0;
static bool  g_sim_mode = false;

/* ---------- INA226 ---------- */
static void update_status(bool online)
{
    lv_obj_set_style_bg_color(g_dot, lv_color_hex(online ? 0x66bb6a : 0xff5252), 0);
    lv_label_set_text(g_st, online ? "INA226" : "SIM");
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    ina226_data_t d;
    float v, c;

    if (ina226_read(&d)) {
        g_fail_cnt = 0;
        if (g_sim_mode) { g_sim_mode = false; update_status(true); }
        v = d.volt; c = d.curr;
    } else {
        g_fail_cnt++;
        if (g_fail_cnt >= 5 && !g_sim_mode) { g_sim_mode = true; update_status(false); }
        /* 模拟数据：慢速正弦，UI 动态可见 */
        g_sim_t += 0.1f;
        v = 5.0f + 0.2f * sinf(g_sim_t * 0.50f);
        c = 0.5f + 0.3f * sinf(g_sim_t * 0.35f + 1.0f);
    }

    lv_label_set_text_fmt(g_card_v.value, "%.2f", v);
    lv_label_set_text_fmt(g_card_v.unit,  "V");
    lv_label_set_text_fmt(g_card_a.value, "%.0f", c * 1000.0f);
    lv_label_set_text_fmt(g_card_a.unit,  "mA");
    float p = v * c;
    if (p >= 1.0f) {
        lv_label_set_text_fmt(g_card_p.value, "%.2f", p);
        lv_label_set_text_fmt(g_card_p.unit,  "W");
    } else {
        lv_label_set_text_fmt(g_card_p.value, "%.0f", p * 1000.0f);
        lv_label_set_text_fmt(g_card_p.unit,  "mW");
    }

    /* 双曲线：电压 0~5V→0~100，电流 0~2A→0~100 */
    lv_chart_set_next_value(g_chart, g_ser_v, (int32_t)(v * 20.0f));
    lv_chart_set_next_value(g_chart, g_ser_i, (int32_t)(c * 50.0f));
}

/* ---------- UI 构建 ---------- */
static card_t mk_card(lv_obj_t *parent, int y, const char *name,
                      const lv_font_t *vfont, lv_color_t vcolor)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, 8, y);
    lv_obj_set_size(card, 224, 30);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);

    lv_obj_t *name_lbl = lv_label_create(card);
    lv_label_set_text(name_lbl, name);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0x8a94b8), 0);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *val = lv_label_create(card);
    lv_obj_set_style_text_font(val, vfont, 0);
    lv_obj_set_style_text_color(val, vcolor, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, -10, 0);

    lv_obj_t *unit = lv_label_create(card);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(0x8a94b8), 0);
    lv_obj_align_to(unit, val, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    return (card_t){ .value = val, .unit = unit };
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1a1a2e), 0);

    /* 标题栏 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "V/A MONITOR");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    /* 状态点 + 状态文本 */
    g_dot = lv_obj_create(scr);
    lv_obj_set_size(g_dot, 8, 8);
    lv_obj_set_style_radius(g_dot, 4, 0);
    lv_obj_set_style_border_width(g_dot, 0, 0);
    lv_obj_set_style_bg_color(g_dot, lv_color_hex(0x66bb6a), 0);
    lv_obj_align(g_dot, LV_ALIGN_TOP_RIGHT, -56, 14);

    g_st = lv_label_create(scr);
    lv_label_set_text(g_st, "INA226");
    lv_obj_set_style_text_font(g_st, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_st, lv_color_hex(0x8a94b8), 0);
    lv_obj_align_to(g_st, g_dot, LV_ALIGN_OUT_LEFT_MID, -6, 0);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_pos(line, 0, 34);
    lv_obj_set_size(line, 240, 1);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x23264a), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);

    /* 三个数据卡片 */
    g_card_v = mk_card(scr, 40, "VOLTAGE", &lv_font_montserrat_28, lv_color_hex(0xffa726));
    g_card_a = mk_card(scr, 74, "CURRENT", &lv_font_montserrat_20, lv_color_hex(0x26c6da));
    g_card_p = mk_card(scr, 108, "POWER",   &lv_font_montserrat_20, lv_color_hex(0x66bb6a));

    /* 趋势图 */
    g_chart = lv_chart_create(scr);
    lv_obj_set_pos(g_chart, 8, 146);
    lv_obj_set_size(g_chart, 224, 86);
    lv_obj_set_style_bg_color(g_chart, lv_color_hex(0x0d0f22), 0);
    lv_obj_set_style_radius(g_chart, 8, 0);
    lv_obj_set_style_border_width(g_chart, 0, 0);
    lv_obj_set_style_shadow_width(g_chart, 0, 0);
    lv_obj_set_style_pad_all(g_chart, 8, 0);
    lv_obj_set_style_line_width(g_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(g_chart, lv_color_hex(0x2a2a4a), LV_PART_MAIN);
    lv_obj_set_style_line_opa(g_chart, LV_OPA_COVER, LV_PART_MAIN);

    lv_chart_set_type(g_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g_chart, 32);
    lv_chart_set_div_line_count(g_chart, 3, 5);
    lv_chart_set_axis_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    g_ser_v = lv_chart_add_series(g_chart, lv_color_hex(0xffa726), LV_CHART_AXIS_PRIMARY_Y);
    g_ser_i = lv_chart_add_series(g_chart, lv_color_hex(0x26c6da), LV_CHART_AXIS_PRIMARY_Y);

    /* 数据刷新定时器 200ms */
    lv_timer_create(refresh_cb, 200, NULL);
}

/* ---------- LCD 纯色自检（二分定位：接线/驱动 vs LVGL 层） ---------- */
static void lcd_selftest(esp_lcd_panel_handle_t panel)
{
    /* 分块画：整屏一次 240*240*2=115KB 超出 C3 内部 RAM，按 20 行分块
     * ST7789 SPI 大端传输，需 bswap16（0xF800 内存序为 00 F8，屏收到 00 F8=蓝） */
    static uint16_t buf[240 * 20];
    const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000 };
    const char *names[] = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };

    ESP_LOGI(TAG, "selftest: solid color sweep");
    for (int c = 0; c < 5; c++) {
        for (int i = 0; i < 240 * 20; i++) buf[i] = __builtin_bswap16(colors[c]);
        for (int y = 0; y < 240; y += 20) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, 240, y + 20, buf);
        }
        ESP_LOGI(TAG, "selftest: %s", names[c]);
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    /* 回到深蓝背景，接 LVGL */
    for (int i = 0; i < 240 * 20; i++) buf[i] = __builtin_bswap16(0x1A2E);
    for (int y = 0; y < 240; y += 20) {
        esp_lcd_panel_draw_bitmap(panel, 0, y, 240, y + 20, buf);
    }
}

/* ---------- 主入口 ---------- */
void app_main(void)
{
    ESP_LOGI(TAG, "boot: ST7789 240x240 + LVGL 9.6 + INA226");

    /* 背光常亮 */
    gpio_set_direction(PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BL, 1);

    /* SPI 总线 */
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * 16 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* LCD panel IO */
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 20 * 1000 * 1000,   /* 杜邦线走线，40MHz→20MHz 更稳 */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io_handle));

    /* ST7789 面板 */
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .color_space = ESP_LCD_COLOR_SPACE_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* ★ 补发完整 ST7789 初始化序列（移植自 stc32-tft 已验证点亮配置）
     * ESP-IDF 内置 init 只有 SLPOUT/MADCTL/COLMOD/RAMCTRL 四步，
     * 缺电源/伽马寄存器，部分模组会闪烁/暗屏 */
    {
        struct { uint8_t cmd; const uint8_t *data; uint8_t len; } init_seq[] = {
            { 0x36, (uint8_t[]){ 0x00 }, 1 },          /* MADCTL 竖屏 */
            { 0x3A, (uint8_t[]){ 0x05 }, 1 },          /* COLMOD 16bit */
            { 0xB2, (uint8_t[]){ 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 5 }, /* PORCTRL */
            { 0xB7, (uint8_t[]){ 0x35 }, 1 },          /* GCTRL */
            { 0xBB, (uint8_t[]){ 0x19 }, 1 },          /* VCOMS */
            { 0xC0, (uint8_t[]){ 0x2C }, 1 },          /* LCMCTRL */
            { 0xC2, (uint8_t[]){ 0x01 }, 1 },          /* VDVVRHEN */
            { 0xC3, (uint8_t[]){ 0x12 }, 1 },          /* VRHS */
            { 0xC4, (uint8_t[]){ 0x20 }, 1 },          /* VDVS */
            { 0xC6, (uint8_t[]){ 0x0F }, 1 },          /* FRCTRL2 */
            { 0xD0, (uint8_t[]){ 0xA4, 0xA1 }, 2 },    /* PWCTRL1 */
            { 0xE0, (uint8_t[]){ 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23 }, 14 }, /* PVGAMCTRL */
            { 0xE1, (uint8_t[]){ 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23 }, 14 }, /* NVGAMCTRL */
        };
        for (size_t i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++) {
            ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, init_seq[i].cmd,
                                                      init_seq[i].data, init_seq[i].len));
        }
        ESP_LOGI(TAG, "st7789 full init seq applied (stc32-tft port)");
    }

    esp_lcd_panel_invert_color(panel, true);   /* ST7789 必须反色（STC32 同款配置） */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    /* ★ 纯色自检：红→绿→蓝→白→黑，肉眼确认 LCD 驱动与接线 */
    lcd_selftest(panel);

    /* LVGL 移植层 */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 12,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .swap_bytes = true, .full_refresh = false },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    /* INA226（读不到自动模拟） */
    if (ina226_init(PIN_SDA, PIN_SCL, I2C_NUM_0)) {
        ESP_LOGI(TAG, "INA226 ready");
    } else {
        ESP_LOGW(TAG, "INA226 offline -> SIM mode");
    }

    /* 构建 UI（LVGL 任务已启动，需加锁） */
    lvgl_port_lock(0);
    build_ui();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "UI ready");
}
