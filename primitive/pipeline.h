/**
 * pipeline.h — 第三阶段: 5 级流水线 CPU
 *
 * 前面两阶段是「单周期」计算机: 一条指令取指→译码→执行→访存→写回
 * 一气呵成, 同一时刻只有一条指令在运行, 大部分硬件单元在闲置。
 *
 * 流水线的核心思想: 把指令执行拆成 5 个独立阶段, 让 5 条指令
 * 同时在流水线的不同阶段推进 —— 就像工厂流水线, 每个工位都在
 * 处理不同的工件。理想情况下每个周期完成一条指令 (吞吐量 ×5)。
 *
 *   阶段:   IF(取指) → ID(译码) → EX(执行) → MEM(访存) → WB(写回)
 *  段寄存器:  IF/ID     ID/EX      EX/MEM     MEM/WB
 *
 * 两类「冒险」(流水线最经典的问题):
 *   1. 数据冒险 — 后一条指令需要用前一条还没写回的结果
 *      解决: 旁路 (Forwarding) — 直接从 EX/MEM、MEM/WB 截获结果
 *   2. 控制冒险 — 分支/跳转改变了指令流向, 已取的分支后续指令作废
 *      解决: 分支预测 (2-bit 饱和计数器) + 预测失败时冲刷流水线
 * ================================================================ */

#ifndef PIPELINE_H
#define PIPELINE_H

#include <stdint.h>
#include <stdbool.h>

/* ---- 指令集 (MIPS 风格子集, 4 位操作码) ---- */
enum {
    OP_NOP = 0x0,            /* 空操作 (流水线气泡) */
    OP_ADD = 0x1,            /* ADD  Rd, Rs, Rt   : Rd = Rs + Rt */
    OP_SUB = 0x2,            /* SUB  Rd, Rs, Rt   : Rd = Rs - Rt */
    OP_LW  = 0x3,            /* LW   Rd, Rs, imm  : Rd = MEM[Rs+imm] */
    OP_SW  = 0x4,            /* SW   Rs, Rt, imm  : MEM[Rs+imm] = Rt */
    OP_BEQ = 0x5,            /* BEQ  Rs, Rt, off  : if Rs==Rt PC+=off */
    OP_J   = 0x6,            /* J    off          : PC = off */
    OP_HLT = 0x7,            /* HLT : 停机 */
};

/* ---- 指令编码: 16 位定长 (b0=opcode|field0, b1=field1|field2) ----
 *  以「操作数占位」区分:
 *    ADD/SUB : field0=rd   field1=rs  field2=rt            (R型)
 *    LW      : field0=rd   field1=rs  field2=imm           (I型, rt 复用为偏移)
 *    SW/BEQ/J: field0=imm  field1=rs  field2=rt            (I型, rd 复用为偏移)
 */
typedef struct {
    uint8_t opcode;
    uint8_t rd;      /* 目标寄存器 (LW/ADD/SUB) */
    uint8_t rs;      /* 源寄存器 1 */
    uint8_t rt;      /* 源寄存器 2 */
    uint8_t imm;     /* 立即数 / 分支偏移 */
} Ins;

/* 一条指令对应的编码字节 (两个 8 位字) */
static inline void ins_encode(const Ins* ins, uint8_t* b0, uint8_t* b1) {
    switch (ins->opcode) {
        case OP_LW:    /* rd, rs, imm(→rt) */
            *b0 = (OP_LW << 4) | (ins->rd & 0xF);
            *b1 = (ins->rs << 4) | (ins->imm & 0xF);
            break;
        case OP_SW:    /* imm(→rd), rs, rt */
            *b0 = (OP_SW << 4) | (ins->imm & 0xF);
            *b1 = (ins->rs << 4) | (ins->rt & 0xF);
            break;
        case OP_BEQ:   /* imm(→rd), rs, rt */
            *b0 = (OP_BEQ << 4) | (ins->imm & 0xF);
            *b1 = (ins->rs << 4) | (ins->rt & 0xF);
            break;
        case OP_J:     /* imm(→rd) */
            *b0 = (OP_J << 4) | (ins->imm & 0xF);
            *b1 = 0;
            break;
        default:       /* ADD/SUB/NOP/HLT: rd, rs, rt */
            *b0 = (ins->opcode << 4) | (ins->rd & 0xF);
            *b1 = (ins->rs << 4) | (ins->rt & 0xF);
            break;
    }
}

/* 从内存字节译码一条指令 (pc 为指令地址, 每条 2 字节) */
static inline Ins ins_decode(const uint8_t* mem, uint16_t pc) {
    Ins ins;
    uint8_t b0 = mem[pc * 2];
    uint8_t b1 = mem[pc * 2 + 1];
    ins.opcode = (b0 >> 4) & 0xF;
    ins.rd     = b0 & 0xF;
    ins.rs     = (b1 >> 4) & 0xF;
    ins.rt     = b1 & 0xF;
    if (ins.opcode == OP_LW) {
        ins.imm = b1 & 0xF;               /* LW: 偏移在 rt 字段 */
    } else {
        ins.imm = b0 & 0xF;               /* SW/BEQ/J: 偏移在 rd 字段 */
    }
    return ins;
}

/* ---- 五级流水线 CPU ---- */
#define REGS  16
#define MEMSZ 256        /* 内存 256 字节 (128 条指令) */

typedef struct {
    /* 架构状态 */
    uint8_t reg[REGS];
    uint8_t mem[MEMSZ];
    uint16_t pc, npc;    /* 当前 PC 与下一条 PC */
    bool     halted;

    /* ---- 段寄存器 ---- */
    /* IF/ID */
    uint16_t ifid_pc;
    Ins      ifid_ins;
    bool     ifid_valid;

    /* ID/EX */
    uint16_t idex_pc;
    Ins      idex_ins;
    uint8_t  idex_a, idex_b;  /* 读出的操作数 */
    bool     idex_valid;

    /* EX/MEM */
    uint16_t exmem_pc;
    Ins      exmem_ins;
    uint8_t  exmem_alu;       /* ALU 结果 / 有效地址 */
    uint8_t  exmem_b;         /* 写内存的数据 (SW) */
    bool     exmem_valid;

    /* MEM/WB */
    uint16_t memwb_pc;
    Ins      memwb_ins;
    uint8_t  memwb_value;     /* 从内存/ALU 得到的结果 */
    bool     memwb_valid;

    /* ---- 分支预测器 (2-bit 饱和计数器) ---- */
    uint8_t bp_state;         /* 0..3: 00强不取 01弱不取 10弱取 11强取 */
    uint16_t pred_pc;         /* 预测要跳的地址 (用于冲刷判断) */
    bool     branch_taken_this_cycle;

    /* ---- 统计 ---- */
    int cycles;
    int completed;            /* 完成的指令数 */
    int stalls;               /* 停顿周期 (数据冒险) */
    int flushes;              /* 冲刷周期 (分支预测失败) */
    int forwards;             /* 旁路转发次数 */
} PipelineCPU;

/* API */
void pipeline_init(PipelineCPU* c);
void pipeline_load(PipelineCPU* c, const Ins* prog, int n);
void pipeline_clock(PipelineCPU* c);   /* 推进一个时钟周期 */
bool pipeline_done(PipelineCPU* c);
void pipeline_run(PipelineCPU* c, int max_cycles);

/* 便利: 取当前某级正在处理什么 (可视化用) */
const char* stage_name(uint8_t opcode);

#endif /* PIPELINE_H */
