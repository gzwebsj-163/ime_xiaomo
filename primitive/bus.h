/**
 * bus.h — 第四阶段: 系统总线 + DMA 控制器
 *
 * 从「CPU 独占访存」升级为「系统总线架构」。
 *
 * 核心新增:
 *   1. 系统总线:  地址总线 (ADDR) + 数据总线 (DATA) + 控制总线 (CTRL)
 *   2. 总线仲裁:  固定优先级 — DMA 总线请求 (BREQ) 优先于 CPU
 *   3. 端口 I/O:  OUT/IN 指令读写外设端口
 *   4. DMA 控制器: 4 通道, 源/目标/长度, 传输完触发中断
 *
 * 演示场景:
 *   - DMA 通道 1 把内存块搬运到另一块 (速度 = 周期/块)
 *   - 传输完成触发 DMA 中断, 若 CPU 开了中断则被抢占执行 ISR
 *   - CPU 通过 OUT 写外设寄存器 + IN 读回 (端口模型)
 *   - 总线争用: DMA 与 CPU 同时请求, 仲裁保证 DMA 优先
 * ================================================================ */
#ifndef BUS_H
#define BUS_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * 端口映射 (端口 I/O 的地址空间, 与内存分离)
 * ================================================================ */
enum {
    PORT_STATUS   = 0x00,  /* 只读: 外设状态寄存器 (bit0=DMA busy, bit1=数据就绪) */
    PORT_DATA     = 0x01,  /* 可读写: 外设数据寄存器 */
    PORT_DMA_CMD  = 0x02,  /* 写 DMA 命令 (自增源/目标地址) */
};

/* ================================================================
 * 系统总线 — 三组分离的线 + 共享的访问仲裁
 * 一个周期只能有一个主设备 (master) 占用总线。
 * ================================================================ */
#define BUS_ADDR_BITS 16
#define BUS_DATA_BITS 8

/* 总线主设备枚举 */
enum {
    MASTER_CPU = 0,   /* CPU 占用总线 */
    MASTER_DMA = 1,   /* DMA 占用总线 */
};

typedef struct {
    /* 主设备握手 */
    bool      cpu_req;      /* CPU 请求总线 */
    bool      dma_req;      /* DMA 请求总线 */
    uint8_t   master;       /* 当前总线主人: 0=CPU 1=DMA */

    /* 地址总线 (16 位) */
    uint16_t  addr;

    /* 数据总线 (8 位, 双向) */
    uint8_t   data;

    /* 控制总线 */
    bool      ctrl_read;    /* 读选通 */
    bool      ctrl_write;   /* 写选通 */
    bool      ctrl_mem;     /* memory 空间 (0=端口 I/O) */

    /* 统计 */
    uint32_t  cpu_grants;   /* CPU 获得总线次数 */
    uint32_t  dma_grants;   /* DMA 获得总线次数 */
} SystemBus;

/* ================================================================
 * 外设 — 挂在端口地址上的一套寄存器
 * ================================================================ */
#define PERIPH_IO_SIZE 16
#define PERIPH_DATA_BUF 64
typedef struct {
    uint8_t reg[PERIPH_IO_SIZE];       /* 端口寄存器 */
    uint8_t fifo[PERIPH_DATA_BUF];     /* 外设数据缓冲 */
    uint32_t fifo_len;
    uint32_t reads;                    /* 已读取字数 (统计) */
    uint32_t writes;                   /* 已写入字数 (统计) */
} Periph;

/* ================================================================
 * DMA 控制器 — 4 通道
 * 每个通道: 源地址(内存) / 目标地址(内存) / 传输长度
 * ================================================================ */
#define DMA_CHANNELS 4

typedef struct {
    bool      enabled;      /* 通道使能 */
    bool      busy;         /* 正在传输 (置位后 CPU 无法抢总线) */
    uint16_t  src;          /* 源内存地址 (读侧) */
    uint16_t  dst;          /* 目标内存地址 (写侧) */
    uint16_t  len;          /* 剩余待传字节数 */
    uint16_t  total;        /* 本次传输总长度 (统计) */
} DmaChannel;

typedef struct {
    DmaChannel ch[DMA_CHANNELS];
    uint8_t    irq;         /* DMA 完成触发的中断号 */
    uint32_t   cycles;      /* 传输消耗的周期数 */
    uint32_t   transfers;   /* 已完成传输次数 */
} DmaCtrl;

/* ================================================================
 * 完整系统
 * ================================================================ */
#define DMEM_SIZE 512
typedef struct {
    SystemBus bus;
    Periph    p;
    DmaCtrl   dma;
    uint8_t   mem[DMEM_SIZE];   /* 统一内存 (DMA 源/目标都在这里) */

    /* CPU 侧 */
    uint8_t   reg[4];
    uint16_t  pc;
    bool      running;

    /* 中断挂钩: IF 开中断, DMA 完成后触发 */
    bool      iflag;
    bool      irq_pending;
    uint16_t  isr_pc;        /* DMA 中断服务程序入口 */
    bool      in_isr;
    uint16_t  saved_pc;      /* 进入 ISR 前的 PC */
} PicoBus;

/* ---- 总线操作 ---- */
void bus_init(SystemBus* b);
/* 仲裁: 固定优先级 — DMA 恒优先于 CPU */
uint8_t bus_arbitrate(SystemBus* b, bool dma_wants, bool cpu_wants);
/* CPU 读内存 */
uint8_t bus_cpu_read(PicoBus* s, uint16_t addr);
/* CPU 写内存 */
void    bus_cpu_write(PicoBus* s, uint16_t addr, uint8_t v);
/* 端口 I/O */
uint8_t bus_port_in (PicoBus* s, uint8_t port);
void    bus_port_out(PicoBus* s, uint8_t port, uint8_t v);

/* ---- DMA 操作 ---- */
void dma_init(DmaCtrl* d);
/* 配置一个通道并启动 */
void dma_start(PicoBus* s, int ch, uint16_t src, uint16_t dst, uint16_t len);
/* 推进一次传输 (返回本次是否做了搬运), 完成后触发中断 */
bool dma_tick(PicoBus* s);
/* 通道是否仍在忙 */
bool dma_busy(const DmaCtrl* d, int ch);

/* ---- 微型 CPU (演示用) ---- */
void pico_reset(PicoBus* s);
/* 把一段程序拷贝进内存并对齐加载 */
void pico_load_prog(PicoBus* s, const uint8_t* prog, int len, uint16_t base);
/* 单步: 若 DMA 请求总线则 DMA 优先; CPU 可经 OUT/IN 操作端口 */
void pico_step(PicoBus* s);

#endif /* BUS_H */
