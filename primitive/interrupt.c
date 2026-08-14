/**
 * interrupt.c — 中断系统实现
 *
 * PIC 的工作流程：
 *   1. 设备置 IRR 位 → 中断请求中
 *   2. PIC 检查 IMR → 未被屏蔽？
 *   3. PIC 检查 ISR → 没有更高优先级在处理？
 *   4. 向 CPU 发送 INTR 信号
 *   5. CPU 响应 (IF=1 时)：保存上下文 → 查 IVT → 跳转 ISR
 *   6. ISR 执行 → 结束时置 EOI → IRET 恢复
 */

#include "interrupt.h"
#include <string.h>

/* ================================================================
 * PIC — 中断控制器
 * ================================================================ */

void pic_init(Pic* pic) {
    memset(pic, 0, sizeof(Pic));
    /* 默认屏蔽所有中断，由程序自行开启 */
    pic->imr = 0xFF;  /* 全屏蔽 */
}

void pic_set_handler(Pic* pic, uint8_t vector, uint8_t addr, uint8_t flags) {
    if (vector < 16) {
        pic->ivt[vector].handler_addr = addr;
        pic->ivt[vector].flags = flags;
    }
}

void pic_request(Pic* pic, IrqSource irq) {
    pic->irr |= (1 << irq);
}

void pic_clear(Pic* pic, IrqSource irq) {
    pic->irr &= ~(1 << irq);
    pic->isr &= ~(1 << irq);
}

void pic_mask(Pic* pic, IrqSource irq, bool masked) {
    if (masked) {
        pic->imr |= (1 << irq);
    } else {
        pic->imr &= ~(1 << irq);
    }
}

/* 检查是否有待处理的中断 (未被屏蔽且 IRR 置位) */
bool pic_has_pending(Pic* pic) {
    uint8_t pending = pic->irr & ~pic->imr;
    return pending != 0;
}

/* 获取最高优先级的中断号 (0=irq0 最高, 7=irq7 最低)
 * 如果有更高优先级的中断正在服务，不返回新中断 */
uint8_t pic_get_highest(Pic* pic) {
    uint8_t pending = pic->irr & ~pic->imr;  /* 未被屏蔽的请求 */

    if (pending == 0) return 0xFF;  /* 无中断 */

    /* 找到最高优先级 (数值最小) 的 pending 位 */
    for (int i = 0; i < 8; i++) {
        if (pending & (1 << i)) {
            /* 检查是否有更高优先级正在服务 */
            if (pic->isr != 0) {
                /* 找到正在服务的最高优先级 */
                for (int j = 0; j < 8; j++) {
                    if (pic->isr & (1 << j)) {
                        /* 只有比正在服务的优先级更高才抢占 */
                        if (i < j) return i;
                        return 0xFF;  /* 不抢占 */
                    }
                }
            }
            return i;
        }
    }
    return 0xFF;
}

/* ================================================================
 * 中断上下文
 * ================================================================ */

void intr_save_context(InterruptContext* ctx, uint8_t pc, uint8_t* regs,
                       bool zf, bool cf, bool iflag, uint8_t imr) {
    ctx->pc = pc;
    memcpy(ctx->reg, regs, 16);
    ctx->zf = zf;
    ctx->cf = cf;
    ctx->iflag = iflag;
    ctx->imr_saved = imr;
}

void intr_restore_context(InterruptContext* ctx, uint8_t* pc, uint8_t* regs,
                          bool* zf, bool* cf, bool* iflag, uint8_t* imr) {
    *pc = ctx->pc;
    memcpy(regs, ctx->reg, 16);
    *zf = ctx->zf;
    *cf = ctx->cf;
    *iflag = ctx->iflag;
    *imr = ctx->imr_saved;
}

/* ================================================================
 * 中断处理核心流程
 *
 * 返回 true 表示处理了中断 (PC 已被修改为 ISR 地址)
 * 返回 false 表示没有中断需要处理
 * ================================================================ */
bool intr_handle(Pic* pic, uint8_t* pc, uint8_t* regs, bool* zf, bool* cf,
                 bool* iflag, InterruptContext* saved_ctx) {
    /* 1. 检查是否有待处理中断 */
    if (!pic_has_pending(pic)) return false;

    /* 2. 获取最高优先级中断 */
    uint8_t irq = pic_get_highest(pic);
    if (irq == 0xFF) return false;

    /* 3. 检查中断向量表是否配置 */
    if (pic->ivt[irq].handler_addr == 0 && pic->ivt[irq].flags == 0) {
        return false;  /* 未配置处理程序 */
    }

    /* 4. 保存上下文 */
    intr_save_context(saved_ctx, *pc, regs, *zf, *cf, *iflag, pic->imr);

    /* 5. 标记为正在服务 */
    pic->isr |= (1 << irq);
    pic->irr &= ~(1 << irq);  /* 清除请求 */

    /* 6. 关中断 (进入 ISR 后自动关中断) */
    *iflag = false;

    /* 7. 跳转到 ISR */
    *pc = pic->ivt[irq].handler_addr;

    return true;
}