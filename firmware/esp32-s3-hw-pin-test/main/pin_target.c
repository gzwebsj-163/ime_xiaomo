/**
 * pin_target.c — UART2 上的 AN3155 STM32 器件模型（测试仪器）
 *
 * 状态机逐字节镜像 hw_pin 内建模拟器（sim_isp_byte），
 * 但搬运通道换成真实 UART2 外设 + 独立 FreeRTOS 任务。
 */
#include "pin_target.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "hw_pin.h"          /* 复用同一套 AN3155 常量与黄金值 */

/* ---- 器件参数 ---- */
#define TGT_FLASH_BASE HW_PIN_ISP_FLASH_BASE
#define TGT_MEM_CAP    HW_PIN_ISP_MEM_CAP
#define TGT_PAGE       HW_PIN_ISP_PAGE
#define TGT_PID        HW_PIN_ISP_PID
#define TGT_BOOTVER    HW_PIN_ISP_BOOTVER

/* ---- 相位（与模拟器一致）---- */
#define T_ST_SYNC  0
#define T_ST_CMD   1
#define T_ST_CMDC  2
#define T_ST_ADDR  3
#define T_ST_ADDC  4
#define T_ST_LEN   5
#define T_ST_LENC  6
#define T_ST_DATA  7
#define T_ST_DATC  8
#define T_ST_PAGES 9
#define T_ST_PGLST 10
#define T_ST_PGCHK 11

static const uint8_t t_cmds[HW_PIN_ISP_NCMDS] = {
    HW_PIN_ISP_CMD_GET, HW_PIN_ISP_CMD_GVR, HW_PIN_ISP_CMD_GID,
    HW_PIN_ISP_CMD_RM,  HW_PIN_ISP_CMD_GO,  HW_PIN_ISP_CMD_WM,
    HW_PIN_ISP_CMD_ER,  HW_PIN_ISP_CMD_WP,  HW_PIN_ISP_CMD_WRU,
    0x82u, 0x92u
};

static uint8_t  s_mem[TGT_MEM_CAP];
static int      s_fmt;
static int      s_st;
static uint8_t  s_cmd;
static uint8_t  s_xor;
static uint32_t s_addr;
static uint32_t s_cnt;
static uint32_t s_ln;
static uint32_t s_pg;
static int      s_run;
static volatile uint32_t s_rx_n, s_tx_n;
static TaskHandle_t s_task;

/* ---- 出口：真的写到 UART2 的发送 FIFO ---- */
static void t_push(int b)
{
    uint8_t v = (uint8_t)(b & 0xFF);
    (void)uart_write_bytes(PIN_TGT_UART_PORT, (const char*)&v, 1);
    s_tx_n++;
}

static void t_fmt(void)
{
    if (!s_fmt) { memset(s_mem, 0xFF, sizeof(s_mem)); s_fmt = 1; }
}

static int t_mem_ok(uint32_t addr, uint32_t* idx)
{
    if (addr < TGT_FLASH_BASE) return 0;
    *idx = addr - TGT_FLASH_BASE;
    return (*idx < TGT_MEM_CAP) ? 1 : 0;
}

static void t_push_mem(uint32_t addr, uint32_t n)
{
    uint32_t i, idx;
    t_fmt();
    for (i = 0; i < n; i++) {
        uint32_t a = addr + i;
        t_push(t_mem_ok(a, &idx) ? (int)s_mem[idx] : 0xFF);
    }
}

void pin_target_reset(void)
{
    s_fmt = 0; s_st = T_ST_SYNC;
    s_cmd = 0; s_xor = 0; s_addr = 0; s_cnt = 0; s_ln = 0; s_pg = 0;
    t_fmt();
}

static void t_dispatch(void)
{
    switch (s_cmd) {
    case HW_PIN_ISP_CMD_GET:
        t_push(HW_PIN_ISP_ACK);
        t_push((int)HW_PIN_ISP_NCMDS - 1);
        { uint32_t i; for (i = 0; i < HW_PIN_ISP_NCMDS; i++) t_push(t_cmds[i]); }
        s_st = T_ST_CMD;
        break;
    case HW_PIN_ISP_CMD_GVR:
        t_push(HW_PIN_ISP_ACK);
        t_push((int)TGT_BOOTVER);
        t_push((int)HW_PIN_ISP_NCMDS - 1);
        { uint32_t i; for (i = 0; i < HW_PIN_ISP_NCMDS; i++) t_push(t_cmds[i]); }
        s_st = T_ST_CMD;
        break;
    case HW_PIN_ISP_CMD_GID:
        t_push(HW_PIN_ISP_ACK);
        t_push(0x01);
        t_push((int)(TGT_PID & 0xFFu));
        t_push((int)((TGT_PID >> 8) & 0xFFu));
        s_st = T_ST_CMD;
        break;
    case HW_PIN_ISP_CMD_RM:
    case HW_PIN_ISP_CMD_WM:
    case HW_PIN_ISP_CMD_GO:
        t_push(HW_PIN_ISP_ACK);
        s_addr = 0; s_xor = 0; s_cnt = 0; s_st = T_ST_ADDR;
        break;
    case HW_PIN_ISP_CMD_ER:
    case HW_PIN_ISP_CMD_WP:
        t_push(HW_PIN_ISP_ACK);
        s_st = T_ST_PAGES;
        break;
    case HW_PIN_ISP_CMD_WRU:
    case 0x82u: case 0x92u:
        t_push(HW_PIN_ISP_ACK);
        s_st = T_ST_CMD;
        break;
    default:
        t_push(HW_PIN_ISP_NACK);
        s_st = T_ST_CMD;
        break;
    }
}

static void t_after_len(void)
{
    uint32_t n = s_ln + 1u;
    if (s_cmd == HW_PIN_ISP_CMD_RM) {
        t_push_mem(s_addr, n);
        s_st = T_ST_CMD;
    } else {
        s_cnt = n; s_xor = 0; s_st = T_ST_DATA;
    }
}

/* 收到 1 字节 → 状态机。返回出队完毕即写进 UART2。 */
static void t_feed(uint8_t in)
{
    if (in == (uint8_t)HW_PIN_ISP_SYNC &&
        (s_st == T_ST_SYNC || s_st == T_ST_CMD)) {
        s_st = T_ST_CMD;
        t_push(HW_PIN_ISP_ACK);
        return;
    }

    switch (s_st) {
    case T_ST_SYNC:
        return;

    case T_ST_CMD:
        s_cmd = in; s_xor = (uint8_t)(in ^ 0xFFu); s_st = T_ST_CMDC; return;

    case T_ST_CMDC:
        if (in != s_xor) { t_push(HW_PIN_ISP_NACK); s_st = T_ST_CMD; return; }
        {
            uint32_t i; int known = 0;
            for (i = 0; i < HW_PIN_ISP_NCMDS; i++)
                if (t_cmds[i] == s_cmd) { known = 1; break; }
            if (!known) { t_push(HW_PIN_ISP_NACK); s_st = T_ST_CMD; return; }
        }
        t_dispatch();
        return;

    case T_ST_ADDR:
        s_addr = (s_addr << 8) | (uint32_t)in;
        s_xor ^= in;
        if (++s_cnt >= 4u) { s_cnt = 0; s_st = T_ST_ADDC; }
        return;

    case T_ST_ADDC:
        if (in != s_xor) { t_push(HW_PIN_ISP_NACK); s_st = T_ST_CMD; }
        else {
            t_push(HW_PIN_ISP_ACK);
            if (s_cmd == HW_PIN_ISP_CMD_GO) s_st = T_ST_CMD;
            else { s_ln = 0; s_st = T_ST_LEN; }
        }
        return;

    case T_ST_LEN:
        s_ln = in; s_xor = (uint8_t)(in ^ 0xFFu); s_st = T_ST_LENC; return;

    case T_ST_LENC:
        if (in != s_xor) { t_push(HW_PIN_ISP_NACK); s_st = T_ST_CMD; return; }
        t_push(HW_PIN_ISP_ACK);
        t_after_len();
        return;

    case T_ST_DATA: {
        uint32_t idx;
        t_fmt();
        if (t_mem_ok(s_addr, &idx)) s_mem[idx] = in;
        s_addr++; s_xor ^= in;
        if (--s_cnt == 0u) s_st = T_ST_DATC;
        return;
    }

    case T_ST_DATC:
        if (in != s_xor) t_push(HW_PIN_ISP_NACK);
        else t_push(HW_PIN_ISP_ACK);
        s_st = T_ST_CMD;
        return;

    case T_ST_PAGES:
        s_ln = in; s_xor = in; s_pg = 0;
        if (in == 0xFFu) {
            t_fmt(); memset(s_mem, 0xFF, sizeof(s_mem));
            s_st = T_ST_PGCHK;
        } else s_st = T_ST_PGLST;
        return;

    case T_ST_PGLST:
        s_xor ^= in;
        s_addr = TGT_FLASH_BASE + (uint32_t)in * TGT_PAGE;
        t_fmt();
        if (s_addr >= TGT_FLASH_BASE &&
            (s_addr - TGT_FLASH_BASE) + TGT_PAGE <= sizeof(s_mem))
            memset(s_mem + (s_addr - TGT_FLASH_BASE), 0xFF, TGT_PAGE);
        if (++s_pg >= (s_ln + 1u)) s_st = T_ST_PGCHK;
        return;

    case T_ST_PGCHK:
        if (in != s_xor) t_push(HW_PIN_ISP_NACK);
        else t_push(HW_PIN_ISP_ACK);
        s_st = T_ST_CMD;
        return;

    default:
        s_st = T_ST_SYNC;
        return;
    }
}

static void target_task(void* arg)
{
    (void)arg;
    while (s_run) {
        uint8_t b;
        int r = uart_read_bytes(PIN_TGT_UART_PORT, &b, 1, pdMS_TO_TICKS(50));
        if (r == 1) {
            s_rx_n++;
            t_feed(b);
        }
    }
    vTaskDelete(NULL);
}

int pin_target_start(void)
{
    uart_config_t c;

    if (s_run) return 0;

    memset(&c, 0, sizeof(c));
    c.baud_rate  = 115200;
    c.data_bits  = UART_DATA_8_BITS;
    c.parity     = UART_PARITY_DISABLE;
    c.stop_bits  = UART_STOP_BITS_1;
    c.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;

    if (uart_driver_install(PIN_TGT_UART_PORT, 2048, 2048, 0, NULL, 0) != ESP_OK) return -1;
    if (uart_param_config(PIN_TGT_UART_PORT, &c) != ESP_OK) return -1;
    if (uart_set_pin(PIN_TGT_UART_PORT, PIN_TGT_TX, PIN_TGT_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) return -1;

    /* ⚠️⚠️ pad 方向争用（真机 Phase C C3~C7 全 NOTGT 的真根因，2026-10-02）
     *
     * 现象：target 侧 rx=0 / tx=0，线上一个字节都没有 → ISP 握手全 NOTGT。
     *
     * 机理：Phase C 让两个 UART **复用同一对 pad，角色正好相反**：
     *     UART1 (ISP 主机) TX=IO18 / RX=IO8
     *     UART2 (器件模型) TX=IO8  / RX=IO18
     * 而 ESP-IDF 的 uart_set_pin() 会顺手把 TX 脚设 GPIO_MODE_OUTPUT、
     * RX 脚设 GPIO_MODE_INPUT。UART1 先配完是 (IO18=OUT, IO8=IN)，
     * 紧接着这里配 UART2 又把 IO18 拽回 INPUT、IO8 拽回 OUTPUT ——
     * **正好把 UART1 的方向整个反转**，于是 UART1 的 TX 信号虽然已
     * 经进了 GPIO matrix，却因为 pad 变成输入而不被驱动出去。
     *
     * 修法：两边都配完之后，把这两个 pad 显式恢复成 INPUT_OUTPUT。
     * 这**不是**输出-输出争用 —— 每个 pad 只有一路驱动：
     *     IO18 由 UART1 TX 驱动，UART2 RX 只听；
     *     IO8  由 UART2 TX 驱动，UART1 RX 只听。
     * 而 pad 的输入缓冲器会把本 pad 上的输出电平喂给对端 RX，
     * 于是信号真刀真枪地走了一遍「pad 输出 → pad 输入缓冲 → GPIO
     * matrix → 对端 UART RX」，链路证据成立。
     */
    {
        const uint64_t pins = (1ULL << PIN_TGT_TX) | (1ULL << PIN_TGT_RX);
        gpio_config_t pc = {
            .pin_bit_mask = pins,
            .mode         = GPIO_MODE_INPUT_OUTPUT,   /* 双向：既驱动又采样 */
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        if (gpio_config(&pc) != ESP_OK) {
            ESP_LOGE("pin_target", "pad 方向恢复失败");
            return -1;
        }
    }

    s_rx_n = s_tx_n = 0;
    pin_target_reset();
    s_run = 1;
    if (xTaskCreate(target_task, "pin_tgt", 4096, NULL, 4, &s_task) != pdPASS) {
        s_run = 0;
        return -1;
    }
    /* 等任务进入读循环，避免第一帧被漏掉 */
    esp_rom_delay_us(5000);
    uart_flush_input(PIN_TGT_UART_PORT);
    return 0;
}

void pin_target_stop(void)
{
    if (!s_run) return;
    s_run = 0;
    vTaskDelay(pdMS_TO_TICKS(120));      /* 让任务自然退出（不用强杀） */
    uart_driver_delete(PIN_TGT_UART_PORT);
    s_task = NULL;
}

uint32_t pin_target_rx_count(void) { return s_rx_n; }
uint32_t pin_target_tx_count(void) { return s_tx_n; }
