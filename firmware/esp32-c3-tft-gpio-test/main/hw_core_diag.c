/**
 * hw_core_diag.c — ESP32-C3 真机接入 hw_core 内核 DNA 模块 (验证桥)
 *
 * 目的: 全跨式验证 hw_core 在真实 ESP-IDF 固件里的三件事:
 *   1. 自动模式探测 → 应自动命中 ESP32 (CONFIG_IDF_TARGET_ESP32C3)
 *   2. freestanding 核心链接进真实固件 (riscv32-esp-elf, 无崩溃)
 *   3. 黄金校验和/表序/槽位生命周期 在真机上与宿主逐位一致
 *
 * hw_core 是纯数据编码层 (无 BSP 回调, 同 hw_token),
 * 真机验证 = 模式探测 + selftest fails==0 + DNA 卡摘要。
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hw_core.h"

static const char *TAG = "hwcore";

static void hw_core_diag_task(void *arg);   /* 前置声明 (xTaskCreate 前) */

/* putf 通道: selftest 文本去尾换行后走 ESP_LOGI (统一 IDF 日志) */
static int core_diag_puts(const char *s)
{
    char buf[160];
    size_t n = (s) ? strlen(s) : 0;
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    if (n) memcpy(buf, s, n);
    buf[n] = '\0';
    if (n) ESP_LOGI(TAG, "%s", buf);
    return (int)n;
}

static void hw_core_diag_task(void *arg)
{
    uint32_t i;
    int fails;
    (void)arg;

    ESP_LOGI(TAG, "==== hw_core 真机验证桥 (ESP32-C3) ====");

    /* [1] 模式探测: CONFIG_IDF_TARGET_ESP32C3 → 应命中 HW_CORE_MODE_ESP32 */
    uint8_t mode = hw_core_mode();
    ESP_LOGI(TAG, "[1] mode = %s (%u) %s", hw_core_mode_str(mode),
             (unsigned)mode,
             (mode == HW_CORE_MODE_ESP32) ? "OK (自动命中 ESP32)"
                                          : "FAIL (应命中 ESP32!)");

    /* [2] 黄金校验和: 应与宿主 Python 对拍锁定值 0xA5618C4A 逐位一致 */
    uint32_t ck = hw_core_checksum();
    ESP_LOGI(TAG, "[2] dna cksum = 0x%08X %s", (unsigned)ck,
             (ck == 0xA5618C4Au) ? "OK (golden 真机一致)"
                                 : "FAIL (真机校验和漂移!)");

    /* [3] DNA 卡: 纯值数组迭代 + 基类名 (真机坑回归: ODT 0x0100 区间判定) */
    {
        const core_dna* t = hw_core_dna_table();
        for (i = 0; i < HW_CORE_MAX; i++) {
            ESP_LOGI(TAG, "[3] DNA[%u] %-4s = 0x%04X (%s)", (unsigned)i,
                     hw_core_dna_name((uint8_t)i), (unsigned)t[i],
                     hw_core_base_name((uint16_t)t[i]));
        }
    }

    /* [4] 槽位生命周期: init 复位 → 装载 SID → 计数 → 卸载 → 归零 */
    hw_core_init(NULL);
    core_slot_load(3, CORE_DNA_SID);
    uint32_t used1 = core_slots_used();
    core_slot_free(3);
    uint32_t used2 = core_slots_used();
    ESP_LOGI(TAG, "[4] slot: load SID→used=%u, free→used=%u %s",
             (unsigned)used1, (unsigned)used2,
             (used1 == 1 && used2 == 0) ? "OK" : "FAIL");

    /* [5] 命令分发: count/ok/idx (VM OP_HW_CORE_CALL 同款路径) */
    ESP_LOGI(TAG, "[5] cmd: count=%d ok=%d idx0=%d %s",
             hw_core_cmd("count", NULL), hw_core_cmd("ok", NULL),
             hw_core_cmd("idx 0", NULL),
             (hw_core_cmd("count", NULL) == 10 &&
              hw_core_cmd("ok", NULL) == 1 &&
              hw_core_cmd("idx 0", NULL) == 0x0DEF) ? "OK" : "FAIL");

    /* [6] selftest 全量 (黄金锁定/表序/基类/槽位/命令分发), fails 应为 0 */
    fails = hw_core_selftest(core_diag_puts);
    ESP_LOGI(TAG, "[6] selftest fails = %d %s", fails,
             (fails == 0) ? "=> ALL PASS (真机 == 宿主逐位一致)" : "=> FAIL!");

    ESP_LOGI(TAG, "==== hw_core 真机验证 %s ====",
             (mode == HW_CORE_MODE_ESP32 && ck == 0xA5618C4Au && fails == 0)
                 ? "PASS ✔" : "FAIL ✘");

    vTaskDelete(NULL);
}

void hw_core_diag_start(void)
{
    xTaskCreate(hw_core_diag_task, "hwcore", 4096, NULL, 5, NULL);
}
