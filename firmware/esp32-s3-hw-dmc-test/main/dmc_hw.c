/**
 * dmc_hw.c — hw_dmc 真机 BSP 实现（ESP32-S3）
 *
 * 设计要点（都是踩过坑换来的，别随手改）：
 *
 *   ① **SIM / REAL 的分水岭就是那个 NULL**：
 *        hw_dmc 的 `dmc_bsp_tx()` 里 `if (g_bsp.tx) return g_bsp.tx(ch);`
 *        SIM  → BSP 全 NULL → 走确定性环回（可回归）；
 *        REAL → 装了回调  → 走片上 UART 外设。
 *      这是「不假装成功」的结构性保证，不靠上层自觉。
 *
 *   ② **now_ms 必须真走时基**：默认环回用确定性 tick 计数器；
 *      真机必须换成 esp_timer，否则链路超时判定会失真。
 *
 *   ③ **rx_ready 装上**：让状态机能在「有字节」与「超时」之间做非阻塞判定，
 *      而不是每次无条件阻塞等待。
 *
 *   ④ **伪码必须喂掉**：uart_set_loop_back(enable) 瞬间 RX 冒 0xff，
 *      见 dmc_hw.h 说明。这是 hw_wdbg 踩过的同一坑的复用护栏。
 */
#include "dmc_hw.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"

/* ============================================================
 * 状态
 * ============================================================ */
static int s_real      = 0;   /* 是否装了真机 BSP */
static int s_opened    = 0;   /* UART1 是否已打开 */
static int s_loopback  = 0;   /* 是否内部回环（0 = 走真实引脚） */

/* ============================================================
 * BSP 四回调 → UART1
 * ============================================================ */
static int uart_tx(uint8_t ch)
{
    int w = uart_write_bytes(DMC_UART_PORT, (const char*)&ch, 1);
    return (w == 1) ? 0 : -1;
}

static int uart_rx(uint32_t timeout_ms)
{
    uint8_t b;
    int r = uart_read_bytes(DMC_UART_PORT, &b, 1, pdMS_TO_TICKS(timeout_ms));
    return (r == 1) ? (int)b : -1;      /* 超时/无数据 → 负值 */
}

static uint32_t uart_now(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int uart_ready(void)
{
    size_t n = 0;
    if (uart_get_buffered_data_len(DMC_UART_PORT, &n) != ESP_OK) return 0;
    return (n > 0) ? 1 : 0;
}

static const hw_dmc_bsp_t s_real_bsp = { uart_tx, uart_rx, uart_now, uart_ready };

/* ============================================================
 * UART1 打开 / 关闭
 * ============================================================ */
int dmc_hw_uart_open_ex(bool loopback)
{
    uart_config_t cfg = {
        .baud_rate  = DMC_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    const uint8_t purge[2] = { 0x00, 0x00 };

    if (s_opened) return 0;

    if (uart_driver_install(DMC_UART_PORT, 1024, 1024, 0, NULL, 0) != ESP_OK) return -1;
    if (uart_param_config(DMC_UART_PORT, &cfg) != ESP_OK) return -1;
    if (uart_set_pin(DMC_UART_PORT, DMC_UART_TX, DMC_UART_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -1;

    uart_set_loop_back(DMC_UART_PORT, loopback);
    s_loopback = loopback ? 1 : 0;

    /* 喂掉使能瞬间的伪码（0xff），否则后续数据整体后移一位。
     * ⚠️ 仅在回环模式下需要：真连线时这两字节会真的发到从设备，
     *    属于往总线上塞垃圾，故只在 loopback 时灌。 */
    uart_flush_input(DMC_UART_PORT);
    if (loopback) {
        uart_write_bytes(DMC_UART_PORT, (const char*)purge, sizeof(purge));
        uart_wait_tx_done(DMC_UART_PORT, pdMS_TO_TICKS(100));
        esp_rom_delay_us(2000);
        uart_flush_input(DMC_UART_PORT);
    }
    if (loopback) {
        /* 回环自证：{0xA5,0x5A} 灌进去必须原样读回来，不通就不开。
         * 目的是**开局就把「线是通的」变成已知前提**，而不是让常驻任务
         * 拿着一条断线假装在跑。 */
        static const uint8_t probe[2] = { 0xA5, 0x5A };
        uint8_t back[2] = { 0, 0 };
        uart_write_bytes(DMC_UART_PORT, (const char*)probe, 2);
        uart_wait_tx_done(DMC_UART_PORT, pdMS_TO_TICKS(100));
        if (uart_read_bytes(DMC_UART_PORT, back, 2, pdMS_TO_TICKS(50)) != 2 ||
            back[0] != 0xA5 || back[1] != 0x5A) {
            uart_driver_delete(DMC_UART_PORT);
            s_opened = 0; s_loopback = 0;
            return -1;
        }
    }

    s_opened = 1;
    return 0;
}

int dmc_hw_uart_open(void) { return dmc_hw_uart_open_ex(true); }

void dmc_hw_uart_close(void)
{
    if (!s_opened) return;
    uart_set_loop_back(DMC_UART_PORT, false);
    uart_driver_delete(DMC_UART_PORT);
    s_opened = 0;
}

void dmc_hw_uart_flush(void)
{
    if (s_opened) uart_flush_input(DMC_UART_PORT);
}

int dmc_hw_uart_roundtrip(const uint8_t* out, int n,
                          uint8_t* in, int cap, int timeout_ms)
{
    int got = 0;

    if (out == NULL || n <= 0 || in == NULL || cap <= 0) return -1;

    /* 每帧前清空输入，保证读到的是本帧（回环下不会引入外来数据，
     * 但上一帧残留必须清掉） */
    uart_flush_input(DMC_UART_PORT);

    if (uart_write_bytes(DMC_UART_PORT, (const char*)out, (size_t)n) != n) return -1;
    if (uart_wait_tx_done(DMC_UART_PORT, pdMS_TO_TICKS(100)) != ESP_OK) return -1;

    while (got < n && got < cap) {
        uint8_t b;
        int r = uart_read_bytes(DMC_UART_PORT, &b, 1, pdMS_TO_TICKS(timeout_ms));
        if (r <= 0) break;                 /* 超时/无数据 → 结束 */
        in[got++] = b;
    }
    return got;
}

/* ============================================================
 * 装载 / 卸载
 * ============================================================ */
int dmc_hw_install_ex(bool real, bool loopback)
{
    if (real) {
        if (dmc_hw_uart_open_ex(loopback) != 0) return -1;
        hw_dmc_bsp_install(&s_real_bsp);
        s_real = 1;
    } else {
        hw_dmc_bsp_install(NULL);          /* 卸载回默认环回 */
        dmc_hw_uart_close();
        s_real = 0;
    }
    return 0;
}

int dmc_hw_install(bool real) { return dmc_hw_install_ex(real, true); }

uint32_t dmc_hw_now_ms(void) { return uart_now(); }

void dmc_hw_dump(void)
{
    printf("dmc_hw: real=%d uart%d=%s loopback=%d tx=IO%d rx=IO%d baud=%d\n",
           s_real, DMC_UART_PORT, s_opened ? "open" : "closed", s_loopback,
           DMC_UART_TX, DMC_UART_RX, DMC_UART_BAUD);
}
