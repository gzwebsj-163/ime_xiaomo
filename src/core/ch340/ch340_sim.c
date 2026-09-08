/*
 * ch340_sim.c - CH340 4路编程扩展坞 硬件仿真模型实现
 * 纯软件模型：电源轨 / GL3520 HUB / 4xCH340C UART 状态机 / ISP 会话。
 */
#include "ch340_sim.h"
#include <stdio.h>
#include <string.h>

#define ARRAY_LEN(a) (sizeof(a)/sizeof((a)[0]))

/* ================= 电源模型 ================= */

int ch_power_on(ch_dock_t *dk, float vin)
{
    ch_power_t *p = &dk->power;
    p->vin = vin;

    /* USB 规范: 5V +-10% = 4.5V ~ 5.5V */
    if (vin < 4.5f || vin > 5.5f) {
        p->fuse_ok = (vin > 6.0f) ? 0 : 1;  /* 严重过压熔断保险丝 */
        p->vout    = 0.0f;
        return CH_ERR_POWER;
    }
    p->fuse_ok  = 1;
    p->vout     = 3.3f;      /* AMS1117 稳压到 3.3V */
    p->i_max    = 800.0f;
    p->i_load   = 40.0f;     /* 4x CH340C 静态功耗 ~10mA each */
    p->overtemp = 0;
    return CH_OK;
}

const char *ch_power_status(const ch_dock_t *dk)
{
    static char buf[128];
    const ch_power_t *p = &dk->power;
    snprintf(buf, sizeof(buf), "vin=%.1fV fuse=%s vout=%.2fV i=%.0fmA/%0.fmA",
             p->vin, p->fuse_ok ? "OK" : "BLOWN",
             p->vout, p->i_load, p->i_max);
    return buf;
}

/* ================= GL3520 HUB ================= */

int ch_hub_mount(ch_dock_t *dk, int port, int speed_mbps)
{
    if (port < 0 || port >= 4) return CH_ERR_PARAM;
    dk->hub_ports[port].present    = 1;
    dk->hub_ports[port].enumerated = 0;
    dk->hub_ports[port].speed_mbps = speed_mbps;
    dk->hub_ports[port].addr       = 1 + port;
    return CH_OK;
}

int ch_hub_enumerate(ch_dock_t *dk)
{
    int n = 0, i;
    for (i = 0; i < 4; i++) {
        ch_port_t *pt = &dk->hub_ports[i];
        pt->enumerated = pt->present;
        if (pt->present) {
            snprintf(pt->dev_desc, sizeof(pt->dev_desc), "CH340C @ %dMbps", pt->speed_mbps);
            n++;
        }
    }
    dk->usb_detected = (n > 0);
    return n;
}

/* ================= 初始化 ================= */

void ch_dock_init(ch_dock_t *dk)
{
    int i;
    memset(dk, 0, sizeof(*dk));
    dk->power.vin     = 5.0f;
    dk->power.fuse_ok = 1;
    dk->power.vout    = 3.3f;
    dk->power.i_load  = 0.0f;
    dk->power.i_max   = 800.0f;
    dk->power.overtemp= 0;
    dk->usb_detected  = 1;
    for (i = 0; i < 4; i++) {
        /* 默认 4×CH340C 已挂载为 High-Speed(480Mbps) 并枚举 */
        dk->hub_ports[i].present    = 1;
        dk->hub_ports[i].enumerated = 1;
        dk->hub_ports[i].speed_mbps = 480;
        dk->hub_ports[i].addr       = 1 + i;
        snprintf(dk->hub_ports[i].dev_desc, sizeof(dk->hub_ports[i].dev_desc),
                 "CH340C @ 480Mbps");
        /* CH340C 通道默认参数 */
        dk->chans[i].online   = 1;
        dk->chans[i].baud     = 115200;
        dk->chans[i].databits = 8;
        dk->chans[i].stopbits = 1;
        dk->chans[i].parity   = 0;
        dk->chans[i].target   = CH_TARGET_NONE;
        dk->chans[i].state    = CH_IDLE;
        dk->chans[i].tx_pos   = 0;
        dk->chans[i].tx_len   = 0;
        dk->chans[i].pin_txd  = 1;  /* 空闲高电平 */
        dk->chans[i].pin_rxd  = 1;
        dk->chans[i].pin_rts  = 0;  /* 有源低, 空闲=低(有效) */
        dk->chans[i].pin_dtr  = 0;
    }
    dk->clock_ticks = 0;
}

/* ================= 通道配置 ================= */

int ch_chan_set_target(ch_dock_t *dk, int idx, ch_target_t t)
{
    if (idx < 0 || idx >= 4) return CH_ERR_PARAM;
    dk->chans[idx].target = t;
    dk->chans[idx].state  = CH_IDLE;
    return CH_OK;
}

int ch_chan_set_baud(ch_dock_t *dk, int idx, int baud)
{
    /* CH340C 支持 50 ~ 2,000,000 bps */
    if (idx < 0 || idx >= 4) return CH_ERR_PARAM;
    if (baud < 50 || baud > 2000000) return CH_ERR_BAUD;
    dk->chans[idx].baud = baud;
    return CH_OK;
}

int ch_chan_set_ctrl(ch_dock_t *dk, int idx, int rts, int dtr)
{
    if (idx < 0 || idx >= 4) return CH_ERR_PARAM;
    dk->chans[idx].pin_rts = rts ? 0 : 1;  /* 有源低 */
    dk->chans[idx].pin_dtr = dtr ? 0 : 1;
    return CH_OK;
}

/* ================= UART 状态机 ================= */

/* UART 帧传输耗时: 每字节 = 起始1 + 数据 + 停止位, 单位 μs */
static void uart_charge(ch_dock_t *dk, int idx, int nbytes)
{
    const ch_chan_t *c = &dk->chans[idx];
    int bits = c->databits + c->stopbits + 1;  /* +起始位 */
    if (c->parity) bits += 1;
    uint64_t us = (uint64_t)nbytes * bits * 1000000ULL / (uint64_t)c->baud;
    ch_clock_advance(dk, us);
}

int ch_uart_tx_byte(ch_dock_t *dk, int idx, uint8_t byte)
{
    ch_chan_t *c;
    if (idx < 0 || idx >= 4) return CH_ERR_PARAM;
    c = &dk->chans[idx];
    if (!c->online) return CH_ERR_PORT;
    c->tx_buf[c->tx_len++] = byte;
    c->state = CH_TX;
    c->pin_txd = 0;  /* 起始位拉低 */
    uart_charge(dk, idx, 1);
    c->pin_txd = 1;  /* 停止位拉高 */
    c->bytes_sent++;
    c->state = CH_IDLE;
    return CH_OK;
}

int ch_uart_rx_byte(ch_dock_t *dk, int idx, uint8_t byte)
{
    ch_chan_t *c;
    if (idx < 0 || idx >= 4) return CH_ERR_PARAM;
    c = &dk->chans[idx];
    if (!c->online) return CH_ERR_PORT;
    c->rx_byte = byte;
    c->bytes_recv++;
    c->state = CH_RX;
    /* 模拟接收完成复位 */
    c->state = CH_IDLE;
    return CH_OK;
}

/* ================= 仿真时钟 ================= */

void ch_clock_advance(ch_dock_t *dk, uint64_t dt_micros)
{
    dk->clock_ticks += dt_micros;
}

/* ================= ISP 会话 ================= */

/* STM32 ISP: 上电 BOOT0=1 → UART 握手 0x7F → ACK 0x79 → 写固件 → 校验 */
int ch_prog_stm32(ch_dock_t *dk, int idx, uint32_t fw_size)
{
    ch_chan_t *c;
    if (idx < 0 || idx >= 4) return CH_ERR_PARAM;
    c = &dk->chans[idx];
    if (c->target != CH_TARGET_STM32) return CH_ERR_PROTO;
    if (!c->online) return CH_ERR_PORT;

    c->state = CH_ISP_ACTIVE     ;
    /* 1. 握手: 发送 0x7F, 期望回 0x79 (ACK) */
    ch_uart_tx_byte(dk, idx, 0x7F);
    ch_uart_rx_byte(dk, idx, 0x79);

    /* 2. 写内存命令 0x31 + 地址 + 数据 */
    ch_uart_tx_byte(dk, idx, 0x31);
    ch_uart_tx_byte(dk, idx, (fw_size >> 24) & 0xFF);
    ch_uart_tx_byte(dk, idx, (fw_size >> 16) & 0xFF);
    ch_uart_tx_byte(dk, idx, (fw_size >> 8)  & 0xFF);
    ch_uart_tx_byte(dk, idx, (fw_size)       & 0xFF);

    /* 3. 校验命令 0x71 */
    ch_uart_tx_byte(dk, idx, 0x71);
    ch_uart_tx_byte(dk, idx, 0x71);

    c->state = CH_ISP_DONE;
    return CH_OK;
}

/* 8051 ISP (STC): 冷启动 → 同步头 0x46/0xB9 交替 → 下载代码 */
int ch_prog_8051(ch_dock_t *dk, int idx, uint32_t code_size)
{
    ch_chan_t *c;
    int i;
    if (idx < 0 || idx >= 4) return CH_ERR_PARAM;
    c = &dk->chans[idx];
    if (c->target != CH_TARGET_8051) return CH_ERR_PROTO;
    if (!c->online) return CH_ERR_PORT;

    c->state = CH_ISP_ACTIVE;
    /* 冷启动: DTR 拉低再拉高 (模拟断电→上电) */
    c->pin_dtr = 0;
    c->pin_dtr = 1;

    /* STC 同步头: 0x46 0xB9 交替发送 4 轮 */
    for (i = 0; i < 4; i++) {
        ch_uart_tx_byte(dk, idx, 0x46);
        ch_uart_tx_byte(dk, idx, 0xB9);
        ch_uart_rx_byte(dk, idx, 0x46);
        ch_uart_rx_byte(dk, idx, 0xB9);
    }

    /* 下载代码: 按 256 字节块发送 */
    {
        uint32_t sent = 0;
        while (sent < code_size) {
            uint32_t chunk = (code_size - sent > 256) ? 256 : (code_size - sent);
            ch_uart_tx_byte(dk, idx, (chunk - 1) & 0xFF);
            sent += chunk;
        }
    }

    c->state = CH_ISP_DONE;
    return CH_OK;
}

/* ================= 状态转储 ================= */

static const char *target_str(ch_target_t t)
{
    switch (t) {
        case CH_TARGET_STM32: return "STM32-ISP";
        case CH_TARGET_8051:  return "8051-ISP";
        case CH_TARGET_DEBUG: return "DEBUG";
        default:              return "NONE";
    }
}

static const char *state_str(ch_chan_state_t s)
{
    switch (s) {
        case CH_TX:        return "TX";
        case CH_RX:        return "RX";
        case CH_ISP_ACTIVE:return "ISP_ACTIVE";
        case CH_ISP_DONE:  return "ISP_DONE";
        default:           return "IDLE";
    }
}

void ch_dock_dump(const ch_dock_t *dk)
{
    int i;
    printf("=== CH340 4路编程扩展坞 ===\n");
    printf("[电源] %s\n", ch_power_status(dk));
    printf("[USB ] host_detected=%s  clock=%llu us\n",
           dk->usb_detected ? "YES" : "NO",
           (unsigned long long)dk->clock_ticks);
    for (i = 0; i < 4; i++) {
        const ch_port_t *pt = &dk->hub_ports[i];
        const ch_chan_t  *c  = &dk->chans[i];
        printf("[CH%d ] USB:%s%s@%dMbps addr=%d | %s | %d baud | sent=%d recv=%d | TXD=%d RXD=%d RTS#=%d DTR#=%d\n",
               i + 1,
               pt->present ? "" : "空 ", pt->present ? "CH340C" : "      ",
               pt->speed_mbps, pt->addr,
               state_str(c->state), c->baud,
               c->bytes_sent, c->bytes_recv,
               c->pin_txd, c->pin_rxd, c->pin_rts, c->pin_dtr);
    }
}
