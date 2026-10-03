/* usbpd_hw.c —— ESP32-S3 真机 BSP 实现
 *
 * 通道 → 引脚映射 (S3 ADC1):
 *   D+  → GPIO4  (ADC1_CH3)   悬空, 不接分压电阻
 *   D-  → GPIO5  (ADC1_CH4)   悬空
 *   CC1 → GPIO6  (ADC1_CH6)   悬空
 *   CC2 → GPIO7  (ADC1_CH7)   悬空
 *
 * 🕳️ 为什么选 GPIO4~7 而不是随便挑 (不是"能用就行"):
 *   选它们是因为本板这四脚【确实没接任何东西】—— 而"悬空脚读到什么"
 *   正是本工程要验的东西。若挑到 LCD 已占的脚, 读到的是屏模块的偏置,
 *   验的就不是悬空闸门而是屏电路了。选脚必须服从被验物理量。
 *
 * ⚠️ i2c / biphase 一律【不装】(置 NULL):
 *   本板无 FUSB302, 装一个假的只会让 ERR_UNSUPPORT 变成假 OK。
 *   留 NULL 让模块走诚实路径 = 保持"未接硬件"这个事实可见。
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "usbpd_hw.h"

#define TAG "usbpd_hw"

static adc_oneshot_unit_handle_t s_adc  = NULL;
static adc_cali_handle_t        s_cali = NULL;
static int                      s_adc_ready = 0;

static const int CH_TO_GPIO[HW_USBPD_CH_MAX] = { 4, 5, 6, 7 };
static const int CH_TO_ADC1[HW_USBPD_CH_MAX] = {
    ADC_CHANNEL_3, ADC_CHANNEL_4, ADC_CHANNEL_6, ADC_CHANNEL_7
};

/* ---------- ADC 初始化 ---------- */
static int adc_up(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };

    if (s_adc_ready) return 0;
    if (adc_oneshot_new_unit(&ucfg, &s_adc) != ESP_OK) {
        printf("[bsp] adc_oneshot_new_unit 失败\n");
        return -1;
    }
    /* 校准: 无 eFuse 曲线则退化线性 —— 两者都给 mV, 不需要手工换算 */
    adc_cali_curve_fitting_config_t ccfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&ccfg, &s_cali) != ESP_OK) {
        /* 退化: 没有校准曲线就放弃校准, 但【不能静默】—— 打印出来 */
        printf("[bsp] 校准曲线不可用, 退回未校准读数(精度下降但可运行)\n");
        s_cali = NULL;
    }
    s_adc_ready = 1;
    return 0;
}

/* ---------- BSP 回调: analog_mv ---------- */
static int bsp_analog_mv(int ch, int* out_mv)
{
    int raw = 0, mv = 0;
    if (!out_mv || ch < 0 || ch >= HW_USBPD_CH_MAX) return HW_USBPD_ERR_PARAM;
    if (!s_adc_ready) return HW_USBPD_ERR_HW;

    esp_err_t err = adc_oneshot_read(s_adc, CH_TO_ADC1[ch], &raw);
    if (err != ESP_OK) {
        printf("[bsp] ch%d(GPIO%d) 读失败: %s\n", ch, CH_TO_GPIO[ch], esp_err_to_name(err));
        return HW_USBPD_ERR_HW;
    }
    if (s_cali) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) mv = raw * 3300 / 4095;
    } else {
        mv = raw * 3300 / 4095;
    }
    *out_mv = mv;
    return 0;
}

/* ---------- BSP 回调: i2c / biphase (故意不装) ---------- */
/* 保持 NULL: 本板无 FUSB302 / 无 CC 比特捕获硬件。
 * 装桩 = 把"没接"伪装成"接了", 那是锚点 L 型的自欺。 */
static int bsp_i2c_read_unused(uint8_t r, uint8_t* b, uint32_t l) { (void)r;(void)b;(void)l; return HW_USBPD_ERR_HW; }
static int bsp_i2c_write_unused(uint8_t r, const uint8_t* b, uint32_t l) { (void)r;(void)b;(void)l; return HW_USBPD_ERR_HW; }
static int bsp_biphase_unused(uint8_t* b, uint32_t n) { (void)b;(void)n; return HW_USBPD_ERR_HW; }

/* ---------- 安装 / 卸载 ---------- */
int usbpd_hw_install(void)
{
    static hw_usbpd_bsp_t bsp;
    if (adc_up() != 0) return -1;
    memset(&bsp, 0, sizeof(bsp));
    bsp.name       = "esp32s3-adc1-gpio4-7";
    bsp.analog_mv  = bsp_analog_mv;
    /* 故意留 NULL: 见文件头说明 */
    (void)bsp_i2c_read_unused; (void)bsp_i2c_write_unused; (void)bsp_biphase_unused;
    hw_usbpd_bsp_install(&bsp);
    printf("[bsp] 已装: %s  (i2c/biphase 留 NULL = 本板无 FUSB302)\n", bsp.name);
    return 0;
}

void usbpd_hw_uninstall(void)
{
    hw_usbpd_bsp_install(NULL);
    printf("[bsp] 已卸载, 回落 SIM\n");
}

/* ---------- 引脚体检: 同一个脚, 配 vs 不配, 读数是否真的不同 ----------
 * 🕳️ 这条检查存在的理由 = hw_dc 家族的实测教训:
 *   adc_oneshot_config_channel() 会【独占 pad 并关掉数字输出驱动】,
 *   所以"配了 ADC 的脚自己驱动自己去读 ADC"结构上不可能。
 *   必须用两个【都未配置】的脚做阴性对照, 靠外部驱动/接地产生差异。
 *   没有外部跳线时, 这一项只能打印读数供人工判断, 不断言。
 */
int usbpd_hw_pin_scan(void)
{
    printf("\n=== ADC 引脚体检 ===\n");
    printf("%-6s %-8s %-10s %-10s\n", "ch", "gpio", "raw", "mV");
    for (int ch = 0; ch < HW_USBPD_CH_MAX; ch++) {
        int raw = -1, mv = -1;
        if (s_adc) {
            if (adc_oneshot_read(s_adc, CH_TO_ADC1[ch], &raw) != ESP_OK) raw = -1;
        }
        if (s_cali && raw >= 0) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) mv = -1;
        } else if (raw >= 0) {
            mv = raw * 3300 / 4095;
        }
        printf("%-6d %-8d %-10d %-10d\n", ch, CH_TO_GPIO[ch], raw, mv);
    }
    printf("  注: 本板这四脚悬空, 读数应为几十~几百 mV 噪声, 非 0 非 3300\n");
    return 0;
}
