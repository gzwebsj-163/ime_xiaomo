/**
 * dmc_hw.h — hw_dmc 真机 BSP（ESP32-S3）：真实 UART 传输面
 *
 * 这一层是 hw_dmc 的「从确定性环回到真实硅」那一跳：
 *   hw_dmc 本体（帧打包/CRC 校验/链路状态机/9 命令）只认 BSP 四回调，
 *   从来不知道自己跑在哪 —— 默认走确定性环回（主从互通自证），
 *   真机固件只要 hw_dmc_bsp_install() 注入 UART 回调即可全链生效。
 *
 * ┌──────────┬──────────────────────────────────────────────┐
 * │ 回调      │ 真机实现                                       │
 * ├──────────┼──────────────────────────────────────────────┤
 * │ tx(ch)   │ uart_write_bytes(1B)                          │
 * │ rx(to)   │ uart_read_bytes(1B, 超时 ms)                  │
 * │ now_ms   │ esp_timer_get_time()/1000                     │
 * │ rx_ready │ uart_get_buffered_data_len() > 0              │
 * └──────────┴──────────────────────────────────────────────┘
 *
 * ⚠️ 为什么用 **UART1 内部回环**（uart_set_loop_back）：
 *   本层要证的不是「协议逻辑对不对」（那是 Phase A 的事），而是
 *   「帧真的能走线」—— 字节经过**真实 UART 外设**（FIFO / 波特率采样 /
 *   起始位同步）后仍逐位一致。内部回环把这条路径补上，且**不需要任何
 *   外部器件**（TX 与 RX 在芯片内部短接），是「不需要跳线也能自证」的硬证据。
 *
 * 引脚：TX=IO17 / RX=IO18（空闲脚，避开 hw_pin 的 SPI 脚、hw_dc 的
 *       ADC 1/3 与 21/38/39、LCD 4/5/6/7/10、PSRAM 33~37、
 *       strapping 0/3/45/46、USB-JTAG 19/20）。
 *
 * ⚠️ hw_wdbg 实测坑（复用记录）：uart_set_loop_back(enable) 的一瞬间
 *   RX 会先冒一个伪字节（0xff），flush 拦不住 → 会让后续数据整体后移 1 位。
 *   对策：使能后先「灌注 2 字节 0x00 + 清空」把伪码喂掉，再开始正式传输。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hw_dmc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 板级 UART 配置（换线只改这一段）----
 *
 * 🔴 引脚选择的硬约束（读 IDF 5.2 源码定案，components/soc/esp32s3/…）：
 *   uart_set_pin() 走哪条路，看 uart_periph_signal[uart].pins[i].default_gpio
 *   是否 == 传入的 io_num：
 *     · 匹配 ⇒ **IOMUX 路**（gpio_iomux_out / gpio_iomux_in，bypass matrix）
 *     · 不匹配 ⇒ **GPIO-Matrix 路**
 *   实测 S3 表：UART1 TX=17 / RX=18，UART2 TX=-1 / RX=-1。
 *
 * ⇒ IO17/IO18 是 UART1 的**专属 IOMUX 脚**，pad18 的输入源被
 *   gpio_iomux_in(18, U1RXD_IN_IDX) 硬接在 U1RXD 信号上并 bypass 掉
 *   matrix。后果（两条都实测到了）：
 *     1) 任何走 matrix 的 TX 一落到 IO18，第一件事
 *        gpio_hal_iomux_func_sel(18, PIN_FUNC_GPIO) 就把 IOMUX 路由顶掉
 *        ⇒ 主机 UART1 RX 变成**自环**，实测 tx=66 rx=0；
 *     2) 任何走 matrix 的 RX 一落到 IO17，gpio_set_direction(17, INPUT)
 *        会关掉 pad17 输出驱动 ⇒ 主机 TX 发不出去，实测从机 rx 66→0。
 *   ⇒ **免接线互连绝不能用 IO17/IO18**，这不是配错，是结构性冲突。
 *
 * 免接线（LINK=0）：两边都用**非默认脚**⇒ 双双走 matrix，matrix 路
 *   不碰 IOMUX 输入源，互不破坏：
 *        主机 UART1: TX=IO9  RX=IO8
 *        从机 UART2: TX=IO8  RX=IO9      （电气上 TX↔RX 对接）
 *
 * 跳线（LINK=1）：主机回 IO17/IO8 真引脚，走 IOMUX + 真实 pad，
 *   外接 DM40A 看得到波形 —— 这是最硬的外部证据。
 */
/* 免接线 / 跳线 由 DMC_SLAVE_LINK 决定；独立编译本头时兜底为 0。 */
#ifndef DMC_SLAVE_LINK
#define DMC_SLAVE_LINK 0
#endif

#define DMC_UART_PORT   1
#if DMC_SLAVE_LINK == 0
#define DMC_UART_TX     9      /* 免接线：非默认脚 → matrix 路（实测 S3 上无效）*/
#define DMC_UART_RX     8
#else
#define DMC_UART_TX     17     /* IOMUX 专属脚：UART1 唯一 TX pad */
#define DMC_UART_RX     18     /* IOMUX 专属脚：UART1 唯一 RX pad */
#endif
#define DMC_UART_BAUD   115200

/* 打开 UART1（115200 8N1 + 内部回环 + 喂掉伪码）。返回 0 / -1。 */
int  dmc_hw_uart_open(void);
/* 打开 UART1；loopback=true 内部回环（Phase B 自证用），
 * loopback=false 走**真实引脚**（常驻建链模式用，须外接线 + 真从设备）。
 * ⚠️ 内部回环只在芯片内把 TX 短接回 RX，对外没有任何波形 ——
 *    常驻模式要证明「帧真的上了线」，必须关掉它。返回 0 / -1。 */
int  dmc_hw_uart_open_ex(bool loopback);
/* 关闭并释放 UART1。 */
void dmc_hw_uart_close(void);

/* 清空 UART 接收缓冲（BSP 直收路径前调用，防上一帧残留污染）。 */
void dmc_hw_uart_flush(void);

/* 把 out[0..n) 写进 UART → 回环读回。返回读到的字节数；<0 = 出错。
 *   timeout_ms: 每字节等待超时；收齐 n 字节即返回。 */
int  dmc_hw_uart_roundtrip(const uint8_t* out, int n,
                           uint8_t* in, int cap, int timeout_ms);

/* 装载真机 BSP（real=true，自动开 UART1）或卸载回默认环回（real=false）。
 *   ⚠️ 这个 NULL 就是 SIM/REAL 的分水岭：hw_dmc 的 dmc_bsp_tx/rx 里
 *      `if (g_bsp.tx) ...` —— 装了就走真机 UART。 */
int  dmc_hw_install(bool real);
/* 同上，但可指定是否内部回环（常驻建链模式走 _ex(true, false)）。 */
int  dmc_hw_install_ex(bool real, bool loopback);

/* 毫秒时基（BSP now_ms 用；也供诊断打印） */
uint32_t dmc_hw_now_ms(void);

/* 打印当前真机面状态（诊断） */
void dmc_hw_dump(void);

#ifdef __cplusplus
}
#endif
