/**
 * hw_main_diag.c — ESP32-C3 真机接入 hw_main 类注册表模块 (验证桥)
 *
 * 目的: 全跨式验证 hw_main 在真实 ESP-IDF 固件里的六件事:
 *   [1] 自动模式探测 → 应自动命中 ESP32 (CONFIG_IDF_TARGET_ESP32C3)
 *   [2] 类表 count = 8 (宿主锁定)
 *   [3] 黄金校验和 0x5FAD755A 真机 == 宿主逐位一致 (类表零漂移)
 *   [4] 类表迭代: name / cls / find 双向往返 (8 类)
 *   [5] 弱链接裁剪语义 (IDF 首战核心):
 *        CORE   → OK    (hw_core.c 已在固件 → 真实探针跑通)
 *        FAULT  → BAD   (hw_fault.c 已在固件, 真机 selftest 已知 13 fails 待修
 *                         → 探针逮到真问题 = 注册表诊断价值铁证)
 *        其余 6 → NOLINK (弱符号未链接 → HW_MAIN_RC_NOLINK)
 *   [6] 命令分发: count/idx/find 字符串通道 (VM OP_HW_MAIN_CALL 同款路径)
 *
 * PASS ✔ = 六步全对 (FAULT 探针 BAD 属预期, 由 hw_fault 侧 bug 说明)。
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hw_main.h"

static const char *TAG = "hwmain";

static void hw_main_diag_task(void *arg);   /* 前置声明 (xTaskCreate 前) */

/* putf 通道: selftest 文本去尾换行后走 ESP_LOGI (统一 IDF 日志) */
static int main_diag_puts(const char *s)
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

void hw_main_diag_task(void *arg)
{
    uint8_t i;
    int rc_nolink = 0, rc_ok = 0, rc_bad = 0;
    uint32_t ck;
    uint8_t mode;
    (void)arg;

    ESP_LOGI(TAG, "==== hw_main 类注册表真机验证桥 (ESP32-C3) ====");
    hw_main_init(NULL);

    /* [1] 模式探测: CONFIG_IDF_TARGET_ESP32C3 → 应命中 HW_MAIN_MODE_ESP32 */
    mode = hw_main_mode();
    ESP_LOGI(TAG, "[1] mode = %s (%u) %s", hw_main_mode_str(mode),
             (unsigned)mode,
             (mode == HW_MAIN_MODE_ESP32) ? "OK (自动命中 ESP32)"
                                          : "FAIL (应命中 ESP32!)");

    /* [2] 类表 count: 应为 8 (宿主锁定) */
    ESP_LOGI(TAG, "[2] count = %u %s", (unsigned)hw_main_class_count(),
             (hw_main_class_count() == 8) ? "OK" : "FAIL");

    /* [3] 黄金校验和: 应与宿主 Python 对拍锁定值 0x5FAD755A 逐位一致 */
    ck = hw_main_checksum();
    ESP_LOGI(TAG, "[3] golden = 0x%08X %s", (unsigned)ck,
             (ck == 0x5FAD755Au) ? "OK (真机 == 宿主逐位一致)"
                                 : "FAIL (真机类表漂移!)");

    /* [4] 类表迭代: 8 类 name/cls + find 双向往返 */
    for (i = 0; i < HW_MAIN_CLASS_MAX; i++) {
        int cls = hw_main_class_id(i);
        int rt  = hw_main_find(cls);
        ESP_LOGI(TAG, "[4] [%u] %-6s cls=0x%04X find_rt=%d %s",
                 (unsigned)i, hw_main_class_name(i),
                 (unsigned)(cls & 0xFFFF), rt,
                 (rt == (int)i) ? "OK" : "FAIL");
    }

    /* [5] 弱链接裁剪: 逐类探针 (核心验证: OK / NOLINK / BAD 三态)
     * ⚠️ 时序背景: 历史上 FAULT 探针在启动瞬态 (boot+0.8s) 跑会报假
     * OK——13-fails 只在 hw_fault_diag 注入真 BSP (boot+~2.2s) 后出现,
     * 故加 3.5s 等待测稳态。2026-09-28 hw_fault 已修 (selftest 期间
     * 卸载 BSP 恢复, 与真机扫描解耦), FAULT 探针恒 OK; 保留稳态时点纪律。 */
    ESP_LOGI(TAG, "[5] 等 3.5s 稳态时点 (历史坑: 启动瞬态探针, 见注释)...");
    vTaskDelay(pdMS_TO_TICKS(3500));

    /* [5] 弱链接裁剪: 逐类探针 (核心验证: OK / NOLINK / BAD 三态齐现) */
    for (i = 0; i < HW_MAIN_CLASS_MAX; i++) {
        int rc = hw_main_probe_one(i);
        const char *tag = (rc == HW_MAIN_RC_OK)     ? "OK"
                        : (rc == HW_MAIN_RC_NOLINK) ? "nolink"
                                                    : "BAD";
        if (rc == HW_MAIN_RC_OK)        rc_ok++;
        else if (rc == HW_MAIN_RC_NOLINK) rc_nolink++;
        else                            rc_bad++;
        ESP_LOGI(TAG, "[5] probe [%u] %-6s = %s", (unsigned)i,
                 hw_main_class_name(i), tag);
    }
    ESP_LOGI(TAG, "[5] 裁剪语义: ok=%d nolink=%d bad=%d %s", rc_ok,
             rc_nolink, rc_bad,
             (rc_ok == 2 && rc_nolink == 6 && rc_bad == 0)
                 ? "OK (CORE+FAULT真探全绿+6裁剪, hw_fault 13-fails已修)"
                 : "FAIL (期望 ok=2 nolink=6 bad=0)");

    /* [6] 命令分发: count / idx / find (VM OP_HW_MAIN_CALL 同款路径) */
    {
        int c1 = hw_main_cmd("count", NULL);
        int c2 = hw_main_cmd("idx 3", NULL);
        int c3 = hw_main_cmd("find 152", NULL);
        ESP_LOGI(TAG, "[6] cmd: count=%d idx3=0x%04X find152=%d %s",
                 c1, (unsigned)(c2 & 0xFFFF), c3,
                 (c1 == 8 && c2 == 0x01EF && c3 == 1) ? "OK" : "FAIL");
    }

    ESP_LOGI(TAG, "==== hw_main 真机验证 %s ====",
             (mode == HW_MAIN_MODE_ESP32 && ck == 0x5FAD755Au &&
              hw_main_class_count() == 8 &&
              rc_ok == 2 && rc_nolink == 6 && rc_bad == 0 &&
              hw_main_cmd("count", NULL) == 8 &&
              hw_main_cmd("idx 3", NULL) == 0x01EF &&
              hw_main_cmd("find 152", NULL) == 1)
                 ? "PASS ✔" : "FAIL ✘");

    vTaskDelete(NULL);
}

void hw_main_diag_start(void)
{
    xTaskCreate(hw_main_diag_task, "hwmain", 4096, NULL, 5, NULL);
}
