/**
 * pin_target.h — 测试仪器：UART2 上的「AN3155 STM32 引导装载程序」器件模型
 *
 * 为什么需要它：
 *   hw_pin 的 ISP 插件（L3 #2）在模拟器模式下由**内部器件模型**供应答；
 *   上了真机，应答得从**真实串口**来。本文件把那个模型搬到 UART2 上，
 *   用 UART1↔UART2 的两根线（IO18↔IO8 交叉）当成一条真实的异步链路，
 *   于是 hw_pin 的 ISP 走的是：模块 → BSP.uart_putc → UART1 外设 → 导线
 *   → UART2 外设 → 本器件模型 → 原路返回。
 *   ⇒ 这条路上「帧真的走了两个真实 UART 外设」，不是内存里过一遍。
 *
 * ⚠️ 这不是「自问自答」：器件模型是**独立任务/独立外设**，
 *    与主机侧完全异步，时序、FIFO、波特率误差都在场。
 *    它证不了「能烧真 STM32」（那需要真芯片），但能证
 *    「ISP 的帧结构在真实异步串口上无损、且会正确拒绝坏帧/错误校验」。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* UART2：TX=IO8（被 UART1.RX 读）, RX=IO18（读 UART1.TX） */
#define PIN_TGT_UART_PORT 2
#define PIN_TGT_TX        8
#define PIN_TGT_RX        18

/* 启动/停止目标器件任务。返回 0 / -1。 */
int  pin_target_start(void);
void pin_target_stop(void);

/* 复位器件状态机（回到「等 0x7F 握手」）。
 * ⚠️ 主机侧也要同步 hw_pin_isp_reset_sync()，两边才不会错位。 */
void pin_target_reset(void);

/* 最近一次收到的字节数（证明目标真的接收到了主机发出的字节） */
uint32_t pin_target_rx_count(void);
uint32_t pin_target_tx_count(void);

#ifdef __cplusplus
}
#endif
