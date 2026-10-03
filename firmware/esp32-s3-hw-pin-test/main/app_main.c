/**
 * app_main.c — ESP32-S3 宿主：hw_pin（引脚档案层 / 双模驱动 / 编程电压层）真机验证
 *
 * 只做一件事：把 xiaomo 的 hw_pin 模块编进真实 ESP-IDF 固件，
 * A/B/C/L0 四段自检后挂起，避免干扰串口观察。
 *
 *   A  = 确定性路径（真机 == 宿主逐位一致：黄金 / 档案 / selftest / 命令分发）
 *   B  = 真实硅引脚面（GPIO 内部上下拉自证 + 模块==硬件对拍 + 诚实失败 + 上电不卸载）
 *   C  = 真实异步链路（UART1 内部回环 + UART1↔UART2 真链路跑完整 AN3155 ISP 烧录）
 *   L0 = 编程电压层（LEDC 占空比 + ADC 回读闭环；一根本地跳线即硬证）
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "pin_diag.h"
#include "pin_spi_ab.h"
#include "pin_g2_diag.h"

static const char *TAG = "s3_host";

void app_main(void)
{
    ESP_LOGI(TAG, "boot: ESP32-S3 host (hw_pin)");

#if PIN_G2_DIAG
    /* 独立模式：只跑 G2 根因诊断矩阵。
     * 唯一目的 = 定位 [AB0] 「驱动低读到低 200/200, 驱动高读到高 0/200」
     * 的根因是【脚】(IO41 外部干扰) 还是【方法】(读不到自身驱动值)。
     * 两者的处置完全不同：前者换脚，后者整个阳性对照写法要重做。 */
    pin_g2_diag_run();
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP_LOGI(TAG, "G2 诊断模式：全部完成，停留观察");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
#elif PIN_SPI_AB_ONLY
    /* 独立模式：只跑 SPI matrix A/B 裁决实验，不跑 Phase A/B/C/L0。
     * 用于「唯一变量 = 引脚组」的受控实验，避免其它阶段先动过 SPI 总线
     * 或占用相关引脚，污染 A/B 两臂的初始状态。 */
    {
        int verdict = 0;
        (void)pin_spi_ab_run(&verdict);
    }
    vTaskDelay(pdMS_TO_TICKS(600));
    ESP_LOGI(TAG, "SPI A/B 模式：全部完成，停留观察");
    while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
#else
    pin_diag_start();
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
}
