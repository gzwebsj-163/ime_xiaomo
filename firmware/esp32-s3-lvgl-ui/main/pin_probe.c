/**
 * pin_probe.c — 按键引脚可用性真机探针（仅排查用，产品构建不编译）
 *
 * 用户要求把「上/下」按键改到 GPIO35/GPIO36。本板八线(Octal) PSRAM 官方占用
 * GPIO33~GPIO37（SPIIO4~7 + DQS），其中 GPIO35=SPIIO6、GPIO36=SPIIO7。
 *
 * ============================ 本次实验设计 ============================
 * 已知现象（历史日志）：
 *   · 探针早期（还没 gpio_config(35,36)）对 PSRAM 的**稀疏**读写正常；
 *   · gpio_config(35,36) 之后的 PSRAM 操作会**卡死**（TG1WDT 复位）。
 * 所以本次做「同一份 PSRAM 检查代码，前后各跑一次」的对照：
 *
 *      Q5a  PSRAM 自检（稀疏 + 密集）      ← 未碰 35/36 的基线
 *      Q2   gpio_config(被测脚)             ← 唯一变量
 *      Q5b  PSRAM 自检（与 Q5a 同一份代码） ← 碰过之后
 *
 * 再跑第二遍，把「被测脚」换成**空闲的 GPIO18** 作对照组：
 *   - 若 18 组 Q5a/Q5b 都正常，而 35/36 组 Q5b 卡死
 *       ⇒ 结论：gpio_config(35,36) 会破坏 PSRAM 访问（按键用之必崩）
 *   - 若 18 组也卡 ⇒ 结论：与引脚无关，是探针/环境问题（我的锅）
 *
 * ⚠️ 踩过的两个坑（保留注释，避免重犯）：
 *   1. 首版用 esp_rom_delay_us 忙等采样 → 饿死 IDLE0 → rst:0x8 (TG1WDT_SYS_RST)。
 *      症状与「硬件坏了」几乎一样。**长循环必须 vTaskDelay 让出 CPU。**
 *   2. 症状会骗人：曾把「探针自身挂死→WDT 复位」误读成「PSRAM 被上拉搞坏」，
 *      直到做了「换顺序 + 换引脚」的对照才排掉。**单次现象不能定案。**
 * =====================================================================
 */
#include "pin_probe.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "pinprobe";

/* esp_hw_support 里的私有接口，直接声明引用，免去拉 private 头文件 */
extern bool esp_gpio_is_pin_reserved(uint32_t gpio_num);

/* ---------- 被测脚：由 PROBE_TEST_FREE_PIN 切换 ---------- */
#if PROBE_TEST_FREE_PIN
#define P_A         18          /* 对照组：空闲脚 */
#define P_B         18
#define GROUP_NAME  "对照组(空闲脚 IO18)"
#else
#define P_A         35          /* 被测：PSRAM 占用脚 */
#define P_B         36
#define GROUP_NAME  "被测组(PSRAM 占用脚 IO35/36)"
#endif

#define PSRAM_TEST_BYTES    (1024 * 1024)   /* 1MB：> ALWAYSINTERNAL(16KB) 必落 PSRAM */
#define PSRAM_STRIDE        4096

static uint32_t lcg(uint32_t *s)
{
    *s = (*s * 1664525u) + 1013904223u;
    return *s;
}

/**
 * PSRAM 自检（Q5a / Q5b 共用同一份代码，保证可比）。
 * ① 稀疏读写：1MB 范围内每 4KB 抽 1 点
 * ② 密集写：连续 4KB（这是历史卡死点，单独列出来）
 * @return 失配点数；-1 = 分配失败；-2 = 密集写未完成（正常情况会先卡死）
 */
static int psram_probe(const char *tag)
{
    uint8_t *buf = (uint8_t *)heap_caps_malloc(PSRAM_TEST_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "%s: PSRAM 分配失败！", tag);
        return -1;
    }

    /* ① 稀疏读写 */
    uint32_t s = 0xC0FFEEu;
    for (size_t off = 0; off < PSRAM_TEST_BYTES; off += PSRAM_STRIDE) {
        buf[off] = (uint8_t)(lcg(&s) >> 24);
    }
    int bad = 0;
    s = 0xC0FFEEu;
    for (size_t off = 0; off < PSRAM_TEST_BYTES; off += PSRAM_STRIDE) {
        if (buf[off] != (uint8_t)(lcg(&s) >> 24)) bad++;
    }
    ESP_LOGW(TAG, "%s: ① 稀疏抽样 %d 点，失配 %d 个 %s",
             tag, (int)(PSRAM_TEST_BYTES / PSRAM_STRIDE), bad,
             bad == 0 ? "(OK)" : "(数据出错!)");

    /* ② 密集写 4KB（每 1KB 让出一次 CPU，避免误判为卡死） */
    int64_t t0 = esp_timer_get_time();
    for (int c = 0; c < 4; c++) {
        memset(buf + c * 1024, 0x5A, 1024);
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "%s: ② 密集写 4KB 完成，用时 %lld us",
             tag, (long long)(esp_timer_get_time() - t0));

    heap_caps_free(buf);
    return bad;
}

void pin_probe_run(void)
{
    ESP_LOGW(TAG, "========== 按键引脚可用性探针 开始：%s ==========", GROUP_NAME);

    /* ---------- Q1：IDF 的保留表 ---------- */
    const int pins[] = { 33, 34, 35, 36, 37, 38, 15, 16, 17, 18 };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        bool r = esp_gpio_is_pin_reserved(pins[i]);
        ESP_LOGW(TAG, "Q1 IO%-2d reserved = %d %s", pins[i], (int)r,
                 r ? "← 被 Flash/PSRAM 占用，不可当普通 GPIO" : "← 空闲，可用");
    }

    /* ---------- Q5a：碰 35/36 之前的 PSRAM 基线 ---------- */
    ESP_LOGW(TAG, "--- Q5a 基线（尚未对 %s 做任何 gpio_config）---", GROUP_NAME);
    psram_probe("Q5a 基线");

    /* ---------- Q2：唯一变量 —— 配置被测脚为「输入 + 内部上拉」 ---------- */
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << P_A) | (1ULL << P_B),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    ESP_LOGW(TAG, "Q2 gpio_config(%d,%d) 输入+内部上拉 => %s", P_A, P_B, esp_err_to_name(err));

    /* 顺手读几次电平（只读，用来确认 pad 是否可读） */
    int ones = 0;
    for (int i = 0; i < 10; i++) { ones += gpio_get_level(P_A) ? 1 : 0; vTaskDelay(10); }
    ESP_LOGW(TAG, "Q2 IO%d 采样 10 次：高 %d 次", P_A, ones);

    /* ---------- Q5b：碰过之后的 PSRAM 复测（与 Q5a 同一份代码） ---------- */
    ESP_LOGW(TAG, "--- Q5b 复测（已对 %s 做过 gpio_config）---", GROUP_NAME);
    psram_probe("Q5b 复测");

    ESP_LOGW(TAG, "========== 探针结束：%s 走完全程（未卡死） ==========", GROUP_NAME);
    fflush(stdout);
}
