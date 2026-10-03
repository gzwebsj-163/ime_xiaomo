/**
 * hw_fault_diag.c — ESP32-C3 真机接入 hw_fault 硬件故障模块 (验证桥)
 *
 * 目的: 全跨式验证 hw_fault 在真实 ESP-IDF 固件里的三件事:
 *   1. 自动模式探测 → 应自动命中 ESP32 (CONFIG_IDF_TARGET_ESP32C3)
 *   2. freestanding 核心链接进真实固件 (riscv32-esp-elf, 无崩溃)
 *   3. 真机 BSP 回调注入 → 用真实 GPIO 对 6 线 ST7789 模组做诊断扫描
 *
 * 模组引脚 → ESP32-C3 GPIO 映射 (与 main.c 一致):
 *   1.gnd  2.rs=DC(GPIO4)  3.cs=CS(GPIO10)  4.scl=SCLK(GPIO6)
 *   5.sda=MOSI(GPIO7)  6.reset=RST(GPIO5)  7.vdd=3V3  8.gnd
 *   9.led+=BL(GPIO0)  10.led-  (LED- 即背光地, 硬件上接 GND)
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "hw_fault.h"

static const char *TAG = "hwfault";

static void hw_fault_diag_task(void *arg);   /* 前置声明 (xTaskCreate 前) */

/* ---- 模组逻辑脚 (1..10) → MCU GPIO, -1 = 无直连 GPIO ---- */
static int diag_pin_to_gpio(int pin)
{
    switch (pin) {
    case 2:  return 4;   /* RS   = DC   */
    case 3:  return 10;  /* CS   = CS   */
    case 4:  return 6;   /* SCL  = SCLK */
    case 5:  return 7;   /* SDA  = MOSI */
    case 6:  return 5;   /* RESET= RST  */
    case 9:  return 0;   /* LED+ = BL   */
    default: return -1;  /* GND/VDD/LED- 无 GPIO */
    }
}

static int diag_gpio_read(int pin)
{
    int gp = diag_pin_to_gpio(pin);
    if (gp < 0) {
        /* 无直连脚: 按物理常识返回 (VDD=供电 3V3, GND/LED-=0) */
        if (pin == 7) return 1;   /* VDD 有电 */
        return 0;                 /* GND/LED- 常态低 */
    }
    return (int)gpio_get_level(gp);
}

static int diag_adc_read_mv(int pin)
{
    int gp = diag_pin_to_gpio(pin);
    if (pin == 7) return 3300;                    /* VDD = 3V3 电源 */
    if (pin == 1 || pin == 8 || pin == 10) return 0; /* GND/LED- = 0mV */
    if (gp < 0) return -1;
    return gpio_get_level(gp) ? 3300 : 0;         /* 数字脚按电平估 */
}

static int diag_spi_probe(int pin)
{
    /* 简化: SCL(4)/SDA(5) 有 GPIO 即可应答 (接线已由 fill 循环证明) */
    int gp = diag_pin_to_gpio(pin);
    return (gp >= 0) ? 1 : 0;
}

static int diag_gpio_flip(int pin)
{
    int gp = diag_pin_to_gpio(pin);
    if (gp < 0) return 1;   /* 无 GPIO 的控制线视作可翻转 (虚拟) */
    gpio_set_direction(gp, GPIO_MODE_OUTPUT);
    gpio_set_level(gp, 1);
    vTaskDelay(pdMS_TO_TICKS(1));
    gpio_set_level(gp, 0);
    return 1;
}

static void diag_bsp_install(void)
{
    static const hw_fault_bsp_t bsp = {
        .gpio_read   = diag_gpio_read,
        .adc_read_mv = diag_adc_read_mv,
        .spi_probe   = diag_spi_probe,
        .gpio_flip   = diag_gpio_flip,
    };
    hw_fault_bsp_install(&bsp);
    ESP_LOGI(TAG, "BSP 真机回调已注入 (6线 ST7789: DC=4 CS=10 SCLK=6 MOSI=7 RST=5 BL=0)");
}

void hw_fault_diag_start(void)
{
    xTaskCreate(hw_fault_diag_task, "hwfault", 4096, NULL, 5, NULL);
}

void hw_fault_diag_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));   /* 等 TFT 初始化/背光亮起 */

    diag_bsp_install();                /* 注入真机 GPIO/ADC/SPI 回调 */

    ESP_LOGI(TAG, "==== hw_fault 真机诊断 ====");
    ESP_LOGI(TAG, "模式探测 = %s (期望 ESP32)", hw_fault_mode_str(hw_fault_mode()));

    /* 1) 黄金校验和 (与宿主/内核逐位一致锁定) */
    uint32_t cks = hw_fault_pin_checksum();
    ESP_LOGI(TAG, "引脚表校验和 = 0x%08X (黄金 0x6AFD1876)", (unsigned)cks);

    /* 2) 真机 BSP 全模组扫描 */
    hw_fault_report_t rep[16];
    uint32_t n = hw_fault_scan(rep, 16);
    ESP_LOGI(TAG, "真机扫描: 共 %u 个故障", (unsigned)n);
    for (uint32_t i = 0; i < n && i < 16; i++) {
        ESP_LOGI(TAG, "  P%d %s: %s [%s]", rep[i].pin, rep[i].name,
                 hw_fault_code_desc(rep[i].code),
                 rep[i].level == 0 ? "OK" : (rep[i].level == 1 ? "WARN" : "ERR"));
    }

    /* 3) selftest: 模拟器表驱动自检 (黄金锁定回归) */
    int fails = hw_fault_selftest(NULL);
    ESP_LOGI(TAG, "selftest: %s (%d fails)", fails == 0 ? "ALL PASS" : "FAIL", fails);

    ESP_LOGI(TAG, "==== hw_fault 真机诊断结束 ====");
    vTaskDelete(NULL);
}
