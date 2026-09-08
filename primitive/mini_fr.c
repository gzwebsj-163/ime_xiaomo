/* 最小复现：1 个任务 + vTaskDelay，测最基础切换路径 */
#include "freertos.h"
#include <stdio.h>

static void task_a(void* arg) {
    uint32_t n = 0;
    while (1) {
        printf("  [A] run %u tick=%u\n", n++, (unsigned)xTaskGetTickCount());
        fflush(stdout);
        vTaskDelay(5);
    }
}

int main(void) {
    printf("[mini] create\n"); fflush(stdout);
    xTaskCreate(task_a, "A", 0, NULL, 1);
    printf("[mini] start scheduler\n"); fflush(stdout);
    vTaskStartScheduler();
    printf("[mini] done\n");
    return 0;
}
