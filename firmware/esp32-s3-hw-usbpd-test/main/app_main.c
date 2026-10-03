/* app_main.c —— hw_usbpd 真机测试固件入口
 *
 * ⚠️ 本固件【不带屏】。共享 S3 板上 esp32-s3-lvgl-ui 是唯一带屏固件,
 *    本测试跑完后必须回烧 build_v3/ 的三段(0x0/0x8000/0x10000)。
 *    NVS 0x9000 不动, 保留 WiFi 凭据与 lang。
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usbpd_hw.h"

void app_main(void)
{
    /* 串口先出声, 便于抓完整 boot */
    printf("\n[boot] hw_usbpd 真机测试固件启动\n");
    vTaskDelay(pdMS_TO_TICKS(200));

    int fails = usbpd_diag_run();

    printf("\n[done] fails=%d  (等待复位重跑)\n", fails);
    vTaskDelay(pdMS_TO_TICKS(500));
    if (fails != 0) {
        printf("[done] 有失败项 —— 保持此输出, 不要立刻回烧产品固件\n");
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
