/**
 * app_main.c — ESP32-S3 宿主：hw_dc（DC 电源信号层）真机验证
 *
 * 只做一件事：把 xiaomo 的 hw_dc 模块编进真实 ESP-IDF 固件，
 * A/B 两段自检后挂起，避免干扰串口观察。
 *
 * A = 确定性路径（真机 == 宿主逐位一致：黄金校验和 / 组合数据帧 / 命令分发 / 原槽位 API）
 * B = 真实硅路径（注入真机 BSP → 片上 ADC + GPIO，且**不依赖任何外部器件**自证）
 * C = DCPP 帧层真机传输（UART1 内部回环 → 帧真的能走线，不需外部器件）
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "dc_diag.h"

static const char *TAG = "s3_host";

void app_main(void)
{
    ESP_LOGI(TAG, "boot: ESP32-S3 host (hw_dc)");
    dc_diag_start();
    vTaskDelay(pdMS_TO_TICKS(100));
}
