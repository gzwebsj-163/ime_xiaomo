/**
 * app_main.c — ESP32-S3 宿主：hw_dmc（DMC 主从链路协议层）真机固件
 *
 * 两种运行模式，**编译期二选一**（默认常驻）：
 *
 *   DMC_MODE_RESIDENT=1（默认）→ dmc_resident 常驻建链
 *       上电起一个永不退出的任务：建链 → 保活 → 掉线重连。
 *       这是「上电必须通过 DMC 协议通信」的落地形态。
 *
 *   DMC_MODE_RESIDENT=0 → dmc_diag 一次性验证
 *       跑完 Phase A/B 打总判即 vTaskDelete（历史行为，保留可复现）。
 *
 * ⚠️ 为什么**不能两个都开**（不是保守，是结构性原因）：
 *   hw_dmc 内部的收/发缓冲是 `static uint8_t buf[HW_DMC_MAX_FRAME]`
 *   且**非重入**（见 hw_dmc.c dmc_link_send / dmc_link_wait）。两个任务
 *   并发调用会互相踩缓冲 → 帧错乱、超时假象、难查的偶发失败。
 *   hw_dmc 的定位是**单链路单任务**驱动，要多路请在模块层加锁或加实例。
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#ifndef DMC_MODE_RESIDENT
#define DMC_MODE_RESIDENT 1
#endif

#if DMC_MODE_RESIDENT
#include "dmc_resident.h"
#include "dmc_slave.h"
#include "dmc_cable.h"
#else
#include "dmc_diag.h"
#endif

static const char *TAG = "s3_host";

void app_main(void)
{
#if DMC_MODE_RESIDENT
    ESP_LOGI(TAG, "boot: ESP32-S3 host (hw_dmc 常驻建链模式)");
#if DMC_SLAVE_ON
    /* ★ 跳线连通图自检：**必须最先跑** —— 此刻主机 UART1 与从机 UART0 都还没开，
     *   4 个链路脚都空闲，才能安全地轮流拉高测「谁跟谁短接」。
     *   🕳️ 第一版错挂在 resident_task 里：那时从机 UART0 已开（204ms），
     *      探针等于在动一个正在工作的 UART 的引脚，实测设备在 gpio_config
     *      中途就没了任何输出（UART0 被捅出中断风暴 ⇒ 整个板子看起来「死机」）。
     *      ⇒ 模块头注释写了前置条件，**调用点不遵守等于没有**。 */
    dmc_cable_probe();

    /* 🔬 判别探针**必须最先跑**：在从机任务和常驻任务都还没起来时，
     *   独占收发端做 A/B 两向测试。放在任务之后会被从机的 20ms 轮询
     *   抢走字节，探针就会误报「从机收不到」。 */
#if DMC_SLAVE_LINK == 0
    {
        int ok = dmc_wire_probe();
        ESP_LOGW(TAG, "探针 %d/2 向通；以下 [link]/[stat] 才是真实协议层结果", ok);
    }
#endif
    /* 板载从机**先起**：它优先级更高且独立运行，主机握手时若对端已在跑就
     * 直接进 UP；若主机抢先也会在 200ms 超时窗口内被从机服务到。
     * 顺序颠倒不会导致失败，但先起从机让「上线时刻」更确定、更好复现。 */
    if (dmc_slave_start() != 0) {
        ESP_LOGE(TAG, "板载从机仿真器创建失败（常驻模式将只会走失败路径）");
    }
#endif
    if (dmc_resident_start() != 0) {
        ESP_LOGE(TAG, "常驻任务创建失败");
    }
#else
    ESP_LOGI(TAG, "boot: ESP32-S3 host (hw_dmc 一次性验证模式)");
    dmc_diag_start();
#endif
    vTaskDelay(pdMS_TO_TICKS(100));
}
