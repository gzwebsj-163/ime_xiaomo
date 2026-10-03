/**
 * dmc_cable.c — 跳线连通图自检实现。原理见 dmc_cable.h。
 *
 * ⚠️ 判据纪律：这里给出的每一个「连通」结论都是**芯片自己量出来的电平差**，
 *    不是推测；三态（接对 / 接反 / 没接）互斥且穷尽，故可直接当判据用。
 */
#include "dmc_cable.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "dmc_hw.h"
#include "dmc_slave.h"

static const char *TAG = "dmc_cable";

#define N_PIN 4

static int         s_pin[N_PIN];
static const char *s_nm [N_PIN];

static void pins_init(void)
{
    s_pin[0] = DMC_UART_TX;   s_nm[0] = "主机TX";
    s_pin[1] = DMC_UART_RX;   s_nm[1] = "主机RX";
    s_pin[2] = DMC_SLAVE_TX;  s_nm[2] = "从机TX";
    s_pin[3] = DMC_SLAVE_RX;  s_nm[3] = "从机RX";
}

/* 输入 + 内部下拉：读 0 = 悬空（弱下拉赢），读 1 = 有强驱动把脚拉高 */
static void as_in_pd(int g)
{
    gpio_config_t c = {0};
    c.pin_bit_mask = 1ULL << g;
    c.mode         = GPIO_MODE_INPUT;
    c.pull_up_en   = GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_ENABLE;
    c.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&c);
}

static void as_out_hi(int g)
{
    gpio_config_t c = {0};
    c.pin_bit_mask = 1ULL << g;
    c.mode         = GPIO_MODE_OUTPUT;
    c.pull_up_en   = GPIO_PULLUP_DISABLE;
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    c.intr_type    = GPIO_INTR_DISABLE;
    gpio_config(&c);
    gpio_set_level(g, 1);
    esp_rom_delay_us(80);
}

int dmc_cable_probe(void)
{
#if DMC_SLAVE_LINK == 0
    ESP_LOGI(TAG, "[cable] LINK=0 为芯片内 GPIO-Matrix 互连，不经外部跳线 ⇒ 跳过自检");
    return 0;
#else
    pins_init();

    for (int i = 0; i < N_PIN; i++) as_in_pd(s_pin[i]);
    esp_rom_delay_us(300);

    int base[N_PIN];
    for (int i = 0; i < N_PIN; i++) base[i] = gpio_get_level(s_pin[i]);

    int tied[N_PIN][N_PIN];
    memset(tied, 0, sizeof(tied));
    for (int i = 0; i < N_PIN; i++) {
        as_out_hi(s_pin[i]);                       /* 拉高 i */
        for (int j = 0; j < N_PIN; j++) {
            if (j != i && gpio_get_level(s_pin[j])) tied[i][j] = 1;
        }
        as_in_pd(s_pin[i]);                        /* 立刻释放，避免两脚对打 */
        esp_rom_delay_us(120);
    }

    /* ---- 连通图 ---- */
    char link[160] = {0};
    int  nconn = 0;
    for (int i = 0; i < N_PIN; i++) {
        for (int j = i + 1; j < N_PIN; j++) {
            if (tied[i][j] || tied[j][i]) {
                int n = snprintf(link + strlen(link), sizeof(link) - strlen(link),
                                 "%s%s(IO%d)↔%s(IO%d)", nconn ? ", " : "",
                                 s_nm[i], s_pin[i], s_nm[j], s_pin[j]);
                (void)n;
                nconn++;
            }
        }
    }

    char driven[96] = {0};
    for (int i = 0; i < N_PIN; i++) {
        if (base[i]) snprintf(driven + strlen(driven), sizeof(driven) - strlen(driven),
                              "%sIO%d", strlen(driven) ? "," : "", s_pin[i]);
    }

    ESP_LOGI(TAG, "[cable] 探针脚 主机TX=IO%d 主机RX=IO%d 从机TX=IO%d 从机RX=IO%d",
             DMC_UART_TX, DMC_UART_RX, DMC_SLAVE_TX, DMC_SLAVE_RX);
    ESP_LOGI(TAG, "[cable] 直连图 = %s   (共 %d 对)", nconn ? link : "无（全悬空）", nconn);
    ESP_LOGI(TAG, "[cable] 常态即被外部驱动高 = %s", driven[0] ? driven : "无");

    /* ---- 结论：三态互斥穷尽 ---- */
    /* 正确：主机TX↔从机RX 且 从机TX↔主机RX */
    int good = (tied[0][3] || tied[3][0]) + (tied[2][1] || tied[1][2]);
    /* 接反：主机TX↔从机TX 或 主机RX↔从机RX */
    int bad  = (tied[0][2] || tied[2][0]) + (tied[1][3] || tied[3][1]);

    if (good == 2) {
        ESP_LOGI(TAG, "[cable] ✅ 接线正确：TX↔RX 交叉已连通（IO%d↔IO%d / IO%d↔IO%d）",
                 DMC_UART_TX, DMC_SLAVE_RX, DMC_SLAVE_TX, DMC_UART_RX);
    } else if (bad) {
        ESP_LOGW(TAG, "[cable] ⚠️ 接成 TX↔TX / RX↔RX 了：两个输出对打 ⇒ 链路恒不通。"
                      "请改接 IO%d↔IO%d / IO%d↔IO%d",
                 DMC_UART_TX, DMC_SLAVE_RX, DMC_SLAVE_TX, DMC_UART_RX);
    } else if (nconn == 0) {
        ESP_LOGW(TAG, "[cable] ❌ 四脚全悬空 ⇒ 跳线未接。需 IO%d↔IO%d 与 IO%d↔IO%d 两根 + 共地",
                 DMC_UART_TX, DMC_SLAVE_RX, DMC_SLAVE_TX, DMC_UART_RX);
    } else {
        ESP_LOGW(TAG, "[cable] ⚠️ 只连通 %d 对（应为 2 对）：接线不完整，缺的那根见上面直连图",
                 nconn);
    }
    return good;
#endif
}
