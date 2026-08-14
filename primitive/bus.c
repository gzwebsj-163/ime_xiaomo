/**
 * bus.c — 第四阶段: 系统总线 + DMA 控制器 (实现)
 *
 * 核心机制:
 *   1. 总线仲裁 — 固定优先级: DMA 请求恒优先于 CPU
 *   2. 端口 I/O — OUT/IN 通过控制总线 ctrl_mem=0 区分内存/端口空间
 *   3. DMA — 4 通道, 一次 tick 搬 1 字节, 完成后置 irq_pending
 *
 * 微缩 CPU 指令集 (16 位定长, 仿 primitive 阶段一的风格):
 *   本演示 CPU 用最简 fetch-decode-execute, 支持:
 *     - MOV R,imm     (0x10)  R = imm
 *     - OUT port,R    (0x20)  写端口
 *     - IN  R,port    (0x21)  R = 读端口
 *     - ADD R,imm     (0x30)  R += imm
 *     - JMP addr      (0x50)  PC = addr
 *     - HLT           (0xF0)  停机
 * ================================================================ */
#include "bus.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * 总线初始化 + 仲裁
 * ================================================================ */
void bus_init(SystemBus* b) {
    memset(b, 0, sizeof(*b));
    b->master = MASTER_CPU;   /* 默认 CPU 是总线主人 */
}

/* 固定优先级仲裁: DMA 请求 → 无论 CPU 是否请求, 都让给 DMA */
uint8_t bus_arbitrate(SystemBus* b, bool dma_wants, bool cpu_wants) {
    (void)cpu_wants;
    if (dma_wants) {
        b->master = MASTER_DMA;
        b->dma_grants++;
        return MASTER_DMA;
    }
    b->master = MASTER_CPU;
    b->cpu_grants++;
    return MASTER_CPU;
}

/* ================================================================
 * CPU 经总线访问内存
 * ================================================================ */
static uint8_t do_mem_read(PicoBus* s, uint16_t addr) {
    /* 占用总线 */
    bus_arbitrate(&s->bus, s->dma.ch[0].busy || s->dma.ch[1].busy ||
                          s->dma.ch[2].busy || s->dma.ch[3].busy, true);
    s->bus.addr = addr;
    s->bus.ctrl_read = true;
    s->bus.ctrl_mem = true;
    uint8_t v = (addr < DMEM_SIZE) ? s->mem[addr] : 0x00;
    s->bus.data = v;
    return v;
}

static void do_mem_write(PicoBus* s, uint16_t addr, uint8_t v) {
    bus_arbitrate(&s->bus, s->dma.ch[0].busy || s->dma.ch[1].busy ||
                          s->dma.ch[2].busy || s->dma.ch[3].busy, true);
    s->bus.addr = addr;
    s->bus.data = v;
    s->bus.ctrl_write = true;
    s->bus.ctrl_mem = true;
    if (addr < DMEM_SIZE) s->mem[addr] = v;
}

uint8_t bus_cpu_read(PicoBus* s, uint16_t addr) { return do_mem_read(s, addr); }
void    bus_cpu_write(PicoBus* s, uint16_t addr, uint8_t v) { do_mem_write(s, addr, v); }

/* ================================================================
 * 端口 I/O (ctrl_mem = 0)
 * ================================================================ */
uint8_t bus_port_in(PicoBus* s, uint8_t port) {
    bus_arbitrate(&s->bus, s->dma.ch[0].busy || s->dma.ch[1].busy ||
                          s->dma.ch[2].busy || s->dma.ch[3].busy, true);
    s->bus.addr = port;
    s->bus.ctrl_read = true;
    s->bus.ctrl_mem = false;

    Periph* p = &s->p;
    if (port < PERIPH_IO_SIZE) {
        switch (port) {
        case PORT_STATUS: {
            /* bit0 = DMA busy, bit1 = 数据就绪 */
            uint8_t st = 0;
            if (dma_busy(&s->dma, 0)) st |= 0x01;
            if (p->fifo_len > 0) st |= 0x02;
            p->reg[PORT_STATUS] = st;
            return st;
        }
        case PORT_DATA:
            if (p->fifo_len > 0) {
                uint8_t v = p->fifo[--p->fifo_len];
                p->reads++;
                return v;
            }
            return 0x00;
        default:
            return p->reg[port];
        }
    }
    return 0x00;
}

void bus_port_out(PicoBus* s, uint8_t port, uint8_t v) {
    bus_arbitrate(&s->bus, s->dma.ch[0].busy || s->dma.ch[1].busy ||
                          s->dma.ch[2].busy || s->dma.ch[3].busy, true);
    s->bus.addr = port;
    s->bus.data = v;
    s->bus.ctrl_write = true;
    s->bus.ctrl_mem = false;

    Periph* p = &s->p;
    if (port < PERIPH_IO_SIZE) {
        switch (port) {
        case PORT_DATA:
            if (p->fifo_len < PERIPH_DATA_BUF) {
                p->fifo[p->fifo_len++] = v;
                p->writes++;
            }
            break;
        default:
            p->reg[port] = v;
            break;
        }
    }
}

/* ================================================================
 * DMA 控制器
 * ================================================================ */
void dma_init(DmaCtrl* d) {
    memset(d, 0, sizeof(*d));
    d->irq = 7;   /* DMA 中断号 */
}

/* 启动一个通道 */
void dma_start(PicoBus* s, int ch, uint16_t src, uint16_t dst, uint16_t len) {
    if (ch < 0 || ch >= DMA_CHANNELS) return;
    DmaChannel* c = &s->dma.ch[ch];
    c->enabled = true;
    c->busy = true;
    c->src = src;
    c->dst = dst;
    c->len = len;
    c->total = len;
}

bool dma_busy(const DmaCtrl* d, int ch) {
    if (ch < 0 || ch >= DMA_CHANNELS) return false;
    return d->ch[ch].busy;
}

/* 推进一个周期的传输; 返回本次是否搬了字节; 完成后触发中断 */
bool dma_tick(PicoBus* s) {
    DmaCtrl* d = &s->dma;
    for (int ch = 0; ch < DMA_CHANNELS; ch++) {
        DmaChannel* c = &d->ch[ch];
        if (!c->enabled || !c->busy) continue;

        /* DMA 拿到总线 (固定优先级胜出) */
        bus_arbitrate(&s->bus, true, false);
        s->bus.ctrl_read = true;
        uint8_t v = (c->src < DMEM_SIZE) ? s->mem[c->src] : 0x00;
        s->bus.data = v;
        s->bus.ctrl_write = true;
        if (c->dst < DMEM_SIZE) s->mem[c->dst] = v;

        c->src++;
        c->dst++;
        c->len--;
        d->cycles++;

        if (c->len == 0) {
            /* 传输完成 */
            c->busy = false;
            c->enabled = false;
            d->transfers++;
            s->irq_pending = true;   /* 触发中断 */
        }
        return true;   /* 本周期 DMA 占了总线, 只有这一个通道工作 */
    }
    return false;
}

/* ================================================================
 * 微型 CPU
 * ================================================================ */
void pico_reset(PicoBus* s) {
    memset(s, 0, sizeof(*s));
    bus_init(&s->bus);
    dma_init(&s->dma);
    s->running = true;
    s->pc = 0;
    s->iflag = false;
}

void pico_load_prog(PicoBus* s, const uint8_t* prog, int len, uint16_t base) {
    for (int i = 0; i < len && (base + i) < DMEM_SIZE; i++)
        s->mem[base + i] = prog[i];
}

/* 检查是否有 DMA 中断在等待 */
static void check_irq(PicoBus* s) {
    if (s->irq_pending && s->iflag && !s->in_isr) {
        /* 响应中断: 保存现场, 跳 ISR */
        s->in_isr = true;
        s->saved_pc = s->pc;
        s->irq_pending = false;
        s->pc = s->isr_pc;
    }
}

/* 单步执行一条 CPU 指令 (若 DMA 请求总线, DMA 在本周期占先) */
void pico_step(PicoBus* s) {
    /* 先让 DMA 有机会跑 (若 pending? 实际由 tick 驱动, 这里用 main 循环控制) */

    /* 响应待处理中断 */
    check_irq(s);

    if (!s->running) return;

    uint16_t pc = s->pc;
    uint8_t op = (pc < DMEM_SIZE) ? s->mem[pc] : 0x00;

    switch (op) {
    case 0x10: { /* MOV R,imm : [op][R][imm] */
        uint8_t R = (pc+1 < DMEM_SIZE) ? s->mem[pc+1] : 0;
        uint8_t im = (pc+2 < DMEM_SIZE) ? s->mem[pc+2] : 0;
        if (R < 4) s->reg[R] = im;
        s->pc = pc + 3;
        break;
    }
    case 0x20: { /* OUT port,R : [op][port][R] */
        uint8_t port = (pc+1 < DMEM_SIZE) ? s->mem[pc+1] : 0;
        uint8_t R = (pc+2 < DMEM_SIZE) ? s->mem[pc+2] : 0;
        uint8_t v = (R < 4) ? s->reg[R] : 0;
        bus_port_out(s, port, v);
        s->pc = pc + 3;
        break;
    }
    case 0x21: { /* IN R,port : [op][R][port] */
        uint8_t R = (pc+1 < DMEM_SIZE) ? s->mem[pc+1] : 0;
        uint8_t port = (pc+2 < DMEM_SIZE) ? s->mem[pc+2] : 0;
        uint8_t v = bus_port_in(s, port);
        if (R < 4) s->reg[R] = v;
        s->pc = pc + 3;
        break;
    }
    case 0x30: { /* ADD R,imm : [op][R][imm] */
        uint8_t R = (pc+1 < DMEM_SIZE) ? s->mem[pc+1] : 0;
        uint8_t im = (pc+2 < DMEM_SIZE) ? s->mem[pc+2] : 0;
        if (R < 4) s->reg[R] += im;
        s->pc = pc + 3;
        break;
    }
    case 0x50: { /* JMP addr : [op][addr_lo] */
        uint8_t a = (pc+1 < DMEM_SIZE) ? s->mem[pc+1] : 0;
        s->pc = a;
        break;
    }
    case 0xF0:   /* HLT */
        s->running = false;
        s->pc = pc + 1;
        break;
    default:
        /* NOP (未知) */ 
        s->pc = pc + 1;
        break;
    }
}
