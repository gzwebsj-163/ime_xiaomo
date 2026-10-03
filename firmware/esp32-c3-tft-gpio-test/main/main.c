/**
 * main.c — ESP32-C3 TFT 台架（档案驱动版）
 *
 * 由「硬编码魔数」改为「面板档案」驱动：
 *   换屏 = 换一条 panel_profile_t（分辨率/偏移/MADCTL/针脚/初始化序列全部随档案走），
 *   换板 = 换一条 panel_wire_t（信号 → GPIO）。
 *
 * 当前档案：PANEL_GM1020_05_10P（★ 10 脚实物）
 *   金逸晨 2.0" TFT · ST7789P3 · 240x320 · 4线SPI · 10 脚
 *   针脚定义（1..10）：GND RS(DC) CS SCL SDA RESET VDD GND LED+ LED-
 *   接线：SDA=7 SCL=6 DC=4 RST=5 CS=10 BL=0
 *         VDD → 3V3；LED+ → 3V3(串限流电阻)；LED- → GND
 *
 * 换屏只改一行 ACTIVE_PANEL：
 *   &PANEL_GM1020_05_10P  2.0" 240x320 十脚（当前）
 *   &PANEL_GM1020_05_14P  2.0" 240x320 十四脚插接版
 *   &PANEL_GM13_240x240   1.3" 240x240 方屏（偏移 0,80）
 *
 * 其余为各 hw 模块真机验证桥（hw_fault / hw_core / hw_main / hw_wdbg）。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "panel_lcd.h"
#include "hw_fault_diag.h"
#include "hw_core_diag.h"
#include "hw_main_diag.h"
#include "hw_wdbg_diag.h"

static const char *TAG = "panel";

/* ---------------- 档案 + 接线（换屏/换板只改这两行） ---------------- */
#define ACTIVE_PANEL   (&PANEL_GM1020_05_10P)   /* ★ 10 脚实物；换 14 脚插接版就改这一行 */
#define ACTIVE_ROT     0            /* 0 / 90 / 180 / 270 */

static const panel_wire_t WIRE = {
    .sda = 7, .scl = 6, .dc = 4, .rst = 5, .cs = 10,
    .bl  = 0,                       /* 台架 BL 由 GPIO0 控；改常亮可填 -1 */
};

/* ---------------- 环境注入（保持面板模块平台无关） ---------------- */
static void env_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void env_log(const char *line) { ESP_LOGI(TAG, "%s", line); }

static const panel_lcd_env_t ENV = { .delay_ms = env_delay, .log = env_log };

void app_main(void)
{
    int fails;

    ESP_LOGI(TAG, "boot: 档案驱动 TFT 台架 (panel_lcd)");

    panel_lcd_env(&ENV);

    /* 协议层自检：3 档案 × 4 旋转，逐条比对字节流黄金（dry-run，不扰屏）。
     * 与宿主 C11/C++17 及 tools/panel_stream_golden.py 三方逐位一致才算过。 */
    {
        int sf = panel_lcd_stream_selftest();
        ESP_LOGI(TAG, "stream_selftest fails=%d (协议层 SPI 字节流黄金)", sf);
    }

    /* 档案自检：不点屏也能验证档案完整性与黄金校验和 */
    fails = panel_lcd_selftest(ACTIVE_PANEL, &WIRE);
    ESP_LOGI(TAG, "selftest(%s) fails=%d", ACTIVE_PANEL->id, fails);
    panel_lcd_dump(ACTIVE_PANEL, &WIRE);

    if (panel_lcd_boot(ACTIVE_PANEL, &WIRE, ACTIVE_ROT) != 0) {
        ESP_LOGE(TAG, "panel_lcd_boot FAILED");
    } else {
        ESP_LOGI(TAG, "panel_lcd_boot OK (rotation=%u)", (unsigned)ACTIVE_ROT);
    }

    /* 各 hw 模块真机验证桥 */
    hw_fault_diag_start();
    hw_core_diag_start();
    hw_main_diag_start();
    hw_wdbg_diag_start();

    for (;;) {
        static const uint16_t colors[] = { 0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000 };
        static const char    *names[]  = { "RED", "GREEN", "BLUE", "WHITE", "BLACK" };
        int i;
        for (i = 0; i < 5; i++) {
            ESP_LOGI(TAG, ">> FILL %s", names[i]);
            panel_lcd_fill(colors[i]);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}
