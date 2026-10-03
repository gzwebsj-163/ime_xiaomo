/**
 * app_main.c — ESP32-S3 宿主：hw_wdbg 验证桥 + 2.0" TFT 屏（档案驱动）
 *
 * 屏幕档案：PANEL_GM1020_05_10P（★ 10 脚实物）
 *   金逸晨 2.0" TFT · ST7789P3 · 240x320 · 4线SPI · 10 脚
 *   针脚（1..10）：GND RS(DC) CS SCL SDA RESET VDD GND LED+ LED-
 *   接线（本板）：SDA=7 SCL=6 DC=4 RST=5 CS=10 BL=-1(背光常亮,固件不控)
 *                VDD → 3V3；LED+ → 3V3(串限流电阻)；LED- → GND
 *   避开 hw_wdbg 真机 BSP 占用的 UART1=2/3 与 LEDC=1。
 *
 * 换屏只改一行 ACTIVE_PANEL：
 *   &PANEL_GM1020_05_10P  2.0" 240x320 十脚（当前）
 *   &PANEL_GM1020_05_14P  2.0" 240x320 十四脚插接版
 *   &PANEL_GM13_240x240   1.3" 240x240 方屏（偏移 0,80）
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "panel_lcd.h"
#include "hw_wdbg_diag.h"

static const char *TAG = "s3_host";

#define ACTIVE_PANEL (&PANEL_GM1020_05_10P)   /* ★ 10 脚实物 */
#define ACTIVE_ROT   0

static const panel_wire_t WIRE = {
    .sda = 7, .scl = 6, .dc = 4, .rst = 5, .cs = 10,
    /* 背光 bl 的语义（10 脚裸屏：LED+/LED− 是**背光电源脚**，不是控制脚）：
     *   -1 → 固件不碰任何 GPIO；背光靠电源轨常亮：
     *        LED+ → 3V3(串 10~33Ω 限流) | LED− → GND      ← 推荐、最省事
     *   >=0 → 固件把该 GPIO 拉高，用于**高边直驱**或**低边 NPN/MOS**调光：
     *        LED− → NPN 集电极 / 漏极，GND → 发射极/源极，GPIO → 基极/栅极
     *   ⚠️ 别用 GPIO0/3/45/46 —— ESP32-S3 是 strapping 脚，影响启动且带内部上拉。
     *   ⚠️ 别指望 GPIO 直驱背光：S3 单脚推挽仅约 20mA，裸屏背光常需 20~40mA，
     *      且 3.3V 对白光 LED 本就吃紧 → 电流不够时表现为「全黑、屏不亮」。 */
    .bl  = -1,
};

static void env_delay(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
static void env_log(const char *line) { ESP_LOGI(TAG, "%s", line); }
static const panel_lcd_env_t ENV = { .delay_ms = env_delay, .log = env_log };

void app_main(void)
{
    int fails;

    ESP_LOGI(TAG, "boot: ESP32-S3 host (hw_wdbg + panel_lcd)");
    panel_lcd_env(&ENV);

    /* 协议层自检：3 档案 × 4 旋转，逐条比对字节流黄金（dry-run，不扰屏）。
     * 与宿主 C11/C++17 及 tools/panel_stream_golden.py 三方逐位一致才算过。 */
    {
        int sf = panel_lcd_stream_selftest();
        ESP_LOGI(TAG, "stream_selftest fails=%d (协议层 SPI 字节流黄金)", sf);
    }

    fails = panel_lcd_selftest(ACTIVE_PANEL, &WIRE);
    ESP_LOGI(TAG, "selftest(%s) fails=%d", ACTIVE_PANEL->id, fails);
    panel_lcd_dump(ACTIVE_PANEL, &WIRE);

    if (panel_lcd_boot(ACTIVE_PANEL, &WIRE, ACTIVE_ROT) == 0) {
        ESP_LOGI(TAG, "panel_lcd_boot OK (rotation=%u)", (unsigned)ACTIVE_ROT);
    } else {
        ESP_LOGE(TAG, "panel_lcd_boot FAILED");
    }

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
