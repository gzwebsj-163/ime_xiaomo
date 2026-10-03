/**
 * app_main.c — ESP32-S3 宿主：hw_flash（ESP32 ROM 下载协议烧录层）真机验证
 *
 * 只做一件事：把 xiaomo 的 hw_flash 模块编进真实 ESP-IDF 固件，
 * A/B 两段自检后挂起，避免干扰串口观察。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hw_flash_diag.h"

static const char *TAG = "s3_host";

void app_main(void)
{
    ESP_LOGI(TAG, "boot: ESP32-S3 host (hw_flash)");
    hw_flash_diag_start();
    vTaskDelay(pdMS_TO_TICKS(100));
}
