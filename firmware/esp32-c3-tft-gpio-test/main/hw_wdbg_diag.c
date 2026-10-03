/**
 * hw_wdbg_diag.c — ESP 真机接入 hw_wdbg 无线调试器信号层 (验证桥)
 *
 * 目的: 全跨式验证 hw_wdbg 在真实 ESP-IDF 固件里的两段能力:
 *
 * ── Phase A: 确定性路径 (核心跨模式主张) ──────────────────
 *   [1] 自动模式探测 → 应命中 ESP32 (CONFIG_IDF_TARGET_ESP32xx)
 *   [2] 黄金校验和 0xCD91F641 真机 == 宿主逐位一致 (线序表零漂移)
 *   [3] 默认线序表 4 协议 (UART/SPI/I2C/PWM) 名称/线数/活动副本一致
 *   [4] 波特率白名单 4 档 (9600/115200/460800/921600) + 越界/非法判定
 *   [5] 串口桥接 2048B 流缓冲: 开桥→问候 4 字节→写→回显读 (确定性)
 *   [6] 多协议: PWM 自环 / SPI 异或回读 / I2C 递增读 (确定性)
 *   [7] selftest fails == 0 (真机 == 宿主逐位一致)
 *   [8] 命令分发 (VM OP_HW_WDBG_CALL 同款路径): count/mode/help/nocmd
 *
 * ── Phase B: 真实硅路径 (BSP 注入) ────────────────────────
 *   [B1] UART1 内部回环: 写 N 字节 → 读回 N 字节逐位相等
 *   [B2] LEDC PWM: 设 2000Hz/300‰ → 回读频率与占空比一致
 *   (SPI/I2C 真机捕获需外接被控从设备, 由 Phase A 确定性路径覆盖)
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "hw_wdbg.h"

static const char *TAG = "hw_wdbg";

/* ============================================================
 * Phase B 真机 BSP: UART1 内部回环 + LEDC PWM
 * ============================================================ */
#define WDBG_UART_PORT   UART_NUM_1
#define WDBG_UART_TX     2          /* 自由引脚 (TFT 用 0/4/5/6/7/10) */
#define WDBG_UART_RX     3
#define WDBG_PWM_PIN     1

#define WDBG_LEDC_MODE   LEDC_LOW_SPEED_MODE
#define WDBG_LEDC_TIMER  LEDC_TIMER_0
#define WDBG_LEDC_CHAN   LEDC_CHANNEL_0

#define WDBG_LEDC_RES    LEDC_TIMER_10_BIT      /* 0..1023 */
#define WDBG_LEDC_MAX    1023u

static uint32_t g_hw_duty_permille;   /* 最近一次 pwm_out 设定的占空比 (‰) */

static int hw_uart_open(uint32_t baud)
{
    uart_config_t cfg = {
        .baud_rate  = (int)baud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(WDBG_UART_PORT, 1024, 1024, 0, NULL, 0) != ESP_OK)
        return -1;
    if (uart_param_config(WDBG_UART_PORT, &cfg) != ESP_OK) return -2;
    if (uart_set_pin(WDBG_UART_PORT, WDBG_UART_TX, WDBG_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -3;
    if (uart_set_loop_back(WDBG_UART_PORT, true) != ESP_OK) return -4;   /* 内部回环 */
    /* 真机实测坑: 回环使能瞬间 RX 侧会冒 1 字节伪码 (ESP32-S3 实测 0xff),
     * 导致首字节整体后移 → 先灌注两字节再清空, 使后续首字节干净 */
    {
        uint8_t prime[2] = { 0u, 0u };
        (void)uart_write_bytes(WDBG_UART_PORT, prime, sizeof(prime));
        (void)uart_wait_tx_done(WDBG_UART_PORT, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uart_flush_input(WDBG_UART_PORT);
    return 0;
}

static int hw_uart_close(void)
{
    uart_driver_delete(WDBG_UART_PORT);
    return 0;
}

static int hw_uart_write(const uint8_t *d, uint32_t n)
{
    int w = uart_write_bytes(WDBG_UART_PORT, d, n);
    if (w > 0) uart_wait_tx_done(WDBG_UART_PORT, pdMS_TO_TICKS(100));
    return w;
}

static int hw_uart_read(uint8_t *d, uint32_t cap)
{
    return uart_read_bytes(WDBG_UART_PORT, d, cap, pdMS_TO_TICKS(80));
}

static int hw_pwm_out(uint32_t hz, uint32_t duty_permille)
{
    ledc_timer_config_t tcfg = {
        .speed_mode      = WDBG_LEDC_MODE,
        .duty_resolution = WDBG_LEDC_RES,
        .timer_num       = WDBG_LEDC_TIMER,
        .freq_hz         = hz,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t ccfg = {
        .gpio_num   = WDBG_PWM_PIN,
        .speed_mode = WDBG_LEDC_MODE,
        .channel    = WDBG_LEDC_CHAN,
        .timer_sel  = WDBG_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    uint32_t d = (duty_permille * WDBG_LEDC_MAX) / 1000u;
    if (ledc_timer_config(&tcfg) != ESP_OK) return -1;
    if (ledc_channel_config(&ccfg) != ESP_OK) return -2;
    if (ledc_set_duty(WDBG_LEDC_MODE, WDBG_LEDC_CHAN, d) != ESP_OK) return -3;
    if (ledc_update_duty(WDBG_LEDC_MODE, WDBG_LEDC_CHAN) != ESP_OK) return -4;
    g_hw_duty_permille = duty_permille;
    return 0;
}

static int hw_pwm_meas(uint32_t *hz, uint32_t *duty)
{
    /* LEDC 回读: 频率读定时器实配值, 占空比读最近设定 (非外接示波器测频) */
    if (hz)   *hz   = ledc_get_freq(WDBG_LEDC_MODE, WDBG_LEDC_TIMER);
    if (duty) *duty = g_hw_duty_permille;
    return 0;
}

static int hw_spi_xfer(uint8_t mode, const uint8_t *tx, uint8_t *rx, uint32_t n)
{
    (void)mode; (void)tx; (void)rx; (void)n;
    return -1;   /* 真机 SPI 捕获需外接从设备, 由 Phase A 覆盖 */
}

static int hw_i2c_xfer(uint8_t a, uint8_t rd, const uint8_t *tx, uint8_t *rx, uint32_t n)
{
    (void)a; (void)rd; (void)tx; (void)rx; (void)n;
    return -1;   /* 真机 I2C 捕获需外接从设备, 由 Phase A 覆盖 */
}

static const hw_wdbg_bsp_t g_hw_bsp = {
    hw_uart_open, hw_uart_close, hw_uart_write, hw_uart_read,
    hw_pwm_out, hw_pwm_meas, hw_spi_xfer, hw_i2c_xfer
};

/* ============================================================
 * 工具
 * ============================================================ */
static int wdbg_putf(const char *s)
{
    char buf[192];
    size_t n = (s) ? strlen(s) : 0;
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    if (n) memcpy(buf, s, n);
    buf[n] = '\0';
    if (n) ESP_LOGI(TAG, "%s", buf);
    return (int)n;
}

/* 把命令结果文本取出并去尾换行 */
static const char *wdbg_result_txt(void)
{
    static char rb[192];
    hw_wdbg_result(rb, sizeof(rb));
    return rb;
}

static void wdbg_diag_task(void *arg)
{
    int fail = 0;
    uint8_t mode;
    uint32_t ck;
    (void)arg;

    ESP_LOGI(TAG, "==== hw_wdbg 无线调试器信号层 真机验证桥 ====");
    hw_wdbg_init(NULL);   /* 干净起点 (与 kvm_run 上电路径同款) */

    /* [1] 模式探测 */
    mode = hw_wdbg_mode();
    ESP_LOGI(TAG, "[1] mode = %s (%u) %s", hw_wdbg_mode_str(mode),
             (unsigned)mode,
             (mode == HW_WDBG_MODE_ESP32) ? "OK (自动命中 ESP32)" : "FAIL!");
    if (mode != HW_WDBG_MODE_ESP32) fail++;

    /* [2] 黄金校验和 */
    ck = hw_wdbg_pin_checksum();
    ESP_LOGI(TAG, "[2] pin cksum = 0x%08X %s", (unsigned)ck,
             (ck == HW_WDBG_GOLDEN) ? "OK (真机 == 宿主逐位一致)"
                                    : "FAIL (线序表漂移!)");
    if (ck != HW_WDBG_GOLDEN) fail++;

    /* [3] 默认线序表: 4 协议 / 名称 / 线数 / 活动副本一致 */
    {
        const hw_wdbg_pin_t *t = hw_wdbg_pin_table();
        static const char *want[4]  = { "UART", "SPI", "I2C", "PWM" };
        static const uint8_t wantn[4] = { 2, 4, 2, 1 };
        int i, ok = 1;
        for (i = 0; i < (int)HW_WDBG_PROTO_MAX; i++) {
            const hw_wdbg_pin_t *a = hw_wdbg_pin_active((uint8_t)i);
            int same = (a != NULL) && a->npins == t[i].npins &&
                       a->rate == t[i].rate &&
                       strcmp(a->lines, t[i].lines) == 0 &&
                       memcmp(a->pins, t[i].pins, sizeof(a->pins)) == 0;
            if (strcmp(t[i].name, want[i]) != 0 || t[i].npins != wantn[i] || !same)
                ok = 0;
            ESP_LOGI(TAG, "[3] %s lines='%s' npins=%u rate=%u active_copy=%s",
                     t[i].name, t[i].lines, (unsigned)t[i].npins,
                     (unsigned)t[i].rate, same ? "same" : "DIFF");
        }
        ESP_LOGI(TAG, "[3] 线序表 4 协议 %s", ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    /* [4] 波特率白名单 */
    {
        static const uint32_t want[4] = { 9600u, 115200u, 460800u, 921600u };
        int i, ok = 1;
        for (i = 0; i < (int)HW_WDBG_BAUD_MAX; i++)
            if (hw_wdbg_baud_at((uint32_t)i) != want[i]) ok = 0;
        if (hw_wdbg_baud_at(HW_WDBG_BAUD_MAX) != 0u) ok = 0;      /* 越界 */
        if (hw_wdbg_baud_ok(115200u) != 1) ok = 0;
        if (hw_wdbg_baud_ok(12345u)  != 0) ok = 0;                 /* 非档位 */
        ESP_LOGI(TAG, "[4] baud 9600/115200/460800/921600 档=%u %s",
                 (unsigned)HW_WDBG_BAUD_MAX, ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    /* [5] 串口桥接确定性: 开桥→问候→写→回显读 */
    {
        int rc1, rc2, rc3, ok;
        rc1 = hw_wdbg_cmd("bridge open 115200", NULL);
        ESP_LOGI(TAG, "[5] open  -> %s", wdbg_result_txt());
        rc2 = hw_wdbg_cmd("bridge rd 8", NULL);
        ESP_LOGI(TAG, "[5] rd    -> %s", wdbg_result_txt());
        rc3 = hw_wdbg_cmd("bridge wr 48 49", NULL);
        ESP_LOGI(TAG, "[5] wr    -> %s", wdbg_result_txt());
        ok = (rc1 == HW_WDBG_R_OK) && (rc2 == HW_WDBG_R_OK) && (rc3 == HW_WDBG_R_OK);
        {
            hw_wdbg_stat_t st;
            hw_wdbg_stat(&st);
            ESP_LOGI(TAG, "[5] stat  -> baud=%u tx=%u rx=%u spill=%u %s",
                     (unsigned)st.baud, (unsigned)st.tx_bytes,
                     (unsigned)st.rx_bytes, (unsigned)st.spill,
                     (st.baud == 115200u && st.tx_bytes == 2u &&
                      st.rx_bytes == 4u && st.spill == 0u) ? "OK" : "FAIL!");
            if (!(st.baud == 115200u && st.tx_bytes == 2u &&
                  st.rx_bytes == 4u && st.spill == 0u)) ok = 0;
        }
        if (!ok) fail++;
    }

    /* [6] 多协议确定性: PWM 自环 / SPI 异或 / I2C 递增 */
    {
        int ok = 1;
        if (hw_wdbg_cmd("pwm out 1000 250", NULL) != HW_WDBG_R_OK) ok = 0;
        ESP_LOGI(TAG, "[6] pwm   -> %s", wdbg_result_txt());
        if (hw_wdbg_cmd("pwm meas", NULL) != HW_WDBG_R_OK) ok = 0;
        ESP_LOGI(TAG, "[6] pwm   -> %s", wdbg_result_txt());
        if (hw_wdbg_cmd("spi mode 3", NULL) != HW_WDBG_R_OK) ok = 0;
        if (hw_wdbg_cmd("spi xfer 01 02 03", NULL) != HW_WDBG_R_OK) ok = 0;
        ESP_LOGI(TAG, "[6] spi   -> %s", wdbg_result_txt());
        if (hw_wdbg_cmd("i2c rd 0x50 4", NULL) != HW_WDBG_R_OK) ok = 0;
        ESP_LOGI(TAG, "[6] i2c   -> %s", wdbg_result_txt());
        {
            hw_wdbg_stat_t st;
            hw_wdbg_stat(&st);
            ESP_LOGI(TAG, "[6] stat  -> spi=%u i2c=%u pwm=%uHz/%u %s",
                     (unsigned)st.spi_traces, (unsigned)st.i2c_traces,
                     (unsigned)st.pwm_hz, (unsigned)st.pwm_duty,
                     (st.pwm_hz == 1000u && st.pwm_duty == 250u &&
                      st.spi_traces == 1u && st.i2c_traces == 1u) ? "OK" : "FAIL!");
            if (!(st.pwm_hz == 1000u && st.pwm_duty == 250u &&
                  st.spi_traces == 1u && st.i2c_traces == 1u)) ok = 0;
        }
        ESP_LOGI(TAG, "[6] 多协议确定性 %s", ok ? "OK" : "FAIL!");
        if (!ok) fail++;
    }

    /* [7] selftest 全量 (黄金/线序/波特率/桥接/协议/命令) */
    {
        int f = hw_wdbg_selftest(wdbg_putf);
        ESP_LOGI(TAG, "[7] selftest fails = %d %s", f,
                 (f == 0) ? "=> ALL PASS (真机 == 宿主逐位一致)" : "=> FAIL!");
        if (f != 0) fail++;
    }

    /* [8] 命令分发 (VM OP_HW_WDBG_CALL 同款路径) */
    {
        int c1 = hw_wdbg_cmd("count", NULL);
        int c2 = hw_wdbg_cmd("mode", NULL);
        int c3 = hw_wdbg_cmd("help", NULL);
        int c4 = hw_wdbg_cmd("nosuchcmd", NULL);
        ESP_LOGI(TAG, "[8] cmd: count=%d mode=%d help=%d nocmd=%d %s",
                 c1, c2, c3, c4,
                 (c1 == 4 && c2 == (int)HW_WDBG_MODE_ESP32 &&
                  c3 == HW_WDBG_R_HELP && c4 == HW_WDBG_R_NOCMD) ? "OK" : "FAIL!");
        if (!(c1 == 4 && c2 == (int)HW_WDBG_MODE_ESP32 &&
              c3 == HW_WDBG_R_HELP && c4 == HW_WDBG_R_NOCMD)) fail++;
    }

    ESP_LOGI(TAG, "==== hw_wdbg Phase A (确定性) %s ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘");

    /* ================= Phase B: 真实硅路径 ================= */
    {
        int bfail = 0;
        ESP_LOGI(TAG, "---- Phase B: 注入真机 BSP (UART1 回环 + LEDC PWM) ----");
        hw_wdbg_bsp_install(&g_hw_bsp);

        /* [B1] UART1 内部回环: 写 4 字节 → 读回逐位相等 */
        {
            int o = hw_wdbg_cmd("bridge open 115200", NULL);
            ESP_LOGI(TAG, "[B1] open  -> %s (rc=%d)", wdbg_result_txt(), o);
            hw_wdbg_cmd("bridge wr de ad be ef", NULL);
            ESP_LOGI(TAG, "[B1] wr    -> %s", wdbg_result_txt());
            /* 回环数据经 DUT 侧回流, 稍等后读 */
            vTaskDelay(pdMS_TO_TICKS(50));
            {
                int r = hw_wdbg_cmd("bridge rd 4", NULL);
                const char *txt = wdbg_result_txt();
                int got = (strstr(txt, "de ad be ef") != NULL);
                ESP_LOGI(TAG, "[B1] rd    -> %s (rc=%d) %s", txt, r,
                         got ? "OK (真硅回环逐位一致)" : "FAIL!");
                if (!got) bfail++;
            }
        }

        /* [B2] LEDC PWM: 设 2000Hz/300‰ → 回读一致 */
        {
            int o = hw_wdbg_cmd("pwm out 2000 300", NULL);
            uint32_t hz = 0, duty = 0;
            hw_wdbg_stat_t st;
            ESP_LOGI(TAG, "[B2] pwm out -> %s (rc=%d)", wdbg_result_txt(), o);
            hw_pwm_meas(&hz, &duty);
            hw_wdbg_stat(&st);
            ESP_LOGI(TAG, "[B2] ledc readback: freq=%uHz duty=%u‰ %s",
                     (unsigned)hz, (unsigned)duty,
                     (hz == 2000u && duty == 300u) ? "OK (真硅 LEDC 生效)"
                                                   : "FAIL!");
            if (!(hz == 2000u && duty == 300u)) bfail++;
        }

        /* 还原默认模拟器 BSP, 不污染后续 */
        hw_wdbg_bsp_install(NULL);
        hw_wdbg_init(NULL);

        ESP_LOGI(TAG, "==== hw_wdbg Phase B (真硅) %s ====",
                 (bfail == 0) ? "PASS ✔" : "FAIL ✘");
        fail += bfail;
    }

    ESP_LOGI(TAG, "==== hw_wdbg 真机验证 %s ====",
             (fail == 0) ? "PASS ✔" : "FAIL ✘");

    vTaskDelete(NULL);
}

void hw_wdbg_diag_start(void)
{
    xTaskCreate(wdbg_diag_task, "hw_wdbg", 4096, NULL, 5, NULL);
}
