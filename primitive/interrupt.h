/**
 * interrupt.h — 中断系统
 *
 * 中断是计算机从「计算器」变成「计算机」的关键一步。
 * 没有中断，CPU 只能轮询；有了中断，外部事件可以主动通知 CPU。
 *
 * 核心组件：
 *   1. 中断控制器 (PIC) — 管理 8 路中断源，仲裁优先级
 *   2. 中断向量表 (IVT) — 16 个入口，中断号 → ISR 地址
 *   3. 中断响应机制 — 保存上下文 → 查表 → 跳转 ISR → IRET 恢复
 *
 * 新增指令：IRET, STI, CLI
 * 新增寄存器：IF (中断标志), IM (中断屏蔽寄存器)
 */

#ifndef INTERRUPT_H
#define INTERRUPT_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * 中断源定义
 * ================================================================ */

/* 8 路中断源，优先级 0 最高 */
typedef enum {
    IRQ_TIMER    = 0,  /* 定时器 (最高优先级) */
    IRQ_KEYBOARD = 1,  /* 键盘 */
    IRQ_UART_RX  = 2,  /* 串口接收 */
    IRQ_UART_TX  = 3,  /* 串口发送 */
    IRQ_DMA_DONE = 4,  /* DMA 完成 */
    IRQ_DISK     = 5,  /* 磁盘 */
    IRQ_SOFTWARE = 6,  /* 软件中断 (INT 指令) */
    IRQ_EXTERNAL = 7,  /* 外部中断 (最低优先级) */
    IRQ_COUNT    = 8
} IrqSource;

/* 中断向量表 — 16 个入口，每个入口 4 字节 (地址) */
typedef struct {
    uint8_t handler_addr;  /* ISR 地址 (在 256 字节内存中) */
    uint8_t flags;         /* 标志: bit0=启用, bit1=特权级 */
} IvtEntry;

/* 中断控制器 (PIC — Programmable Interrupt Controller) */
typedef struct {
    uint8_t irr;           /* 中断请求寄存器 (Interrupt Request) — 哪些中断在请求 */
    uint8_t imr;           /* 中断屏蔽寄存器 (Interrupt Mask)    — 哪些中断被屏蔽 */
    uint8_t isr;           /* 中断服务寄存器 (In-Service)       — 正在处理的中断 */
    uint8_t priority;      /* 当前最高优先级中断 (0=无) */
    IvtEntry ivt[16];      /* 中断向量表 */
} Pic;

/* ================================================================
 * 中断上下文 — 保存/恢复 CPU 状态
 * ================================================================ */

/* 中断发生时需要保存的上下文 */
typedef struct {
    uint8_t pc;            /* 返回地址 */
    uint8_t reg[16];       /* 通用寄存器快照 */
    bool    zf;            /* 零标志 */
    bool    cf;            /* 进位标志 */
    bool    iflag;         /* 中断标志 (中断前状态) */
    uint8_t imr_saved;     /* 中断屏蔽寄存器快照 */
} InterruptContext;

/* ================================================================
 * 中断 API
 * ================================================================ */

void pic_init(Pic* pic);
void pic_set_handler(Pic* pic, uint8_t vector, uint8_t addr, uint8_t flags);
void pic_request(Pic* pic, IrqSource irq);
void pic_clear(Pic* pic, IrqSource irq);
void pic_mask(Pic* pic, IrqSource irq, bool masked);
bool pic_has_pending(Pic* pic);
uint8_t pic_get_highest(Pic* pic);

/* 中断上下文管理 */
void intr_save_context(InterruptContext* ctx, uint8_t pc, uint8_t* regs,
                       bool zf, bool cf, bool iflag, uint8_t imr);
void intr_restore_context(InterruptContext* ctx, uint8_t* pc, uint8_t* regs,
                          bool* zf, bool* cf, bool* iflag, uint8_t* imr);

/* 中断处理流程 (核心) */
bool intr_handle(Pic* pic, uint8_t* pc, uint8_t* regs, bool* zf, bool* cf,
                 bool* iflag, InterruptContext* saved_ctx);

#endif /* INTERRUPT_H */