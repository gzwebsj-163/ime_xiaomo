/**
 * pipeline.c — 5 级流水线 CPU 实现
 *
 * 每个时钟周期五级同时推进: 各级从 "前一个" 段寄存器取数,
 * 周期末尾统一锁存到 "后一个" 段寄存器 (类似锁存器边沿触发)。
 *
 * 两类冒险及处理:
 *   1. 数据冒险 — 后一条指令需要前一条尚未写回的结果
 *      ① 旁路 (Forwarding): 读操作数时直接从 EX/MEM、MEM/WB
 *         段寄存器截获最新值 (对 ADD/SUB 这种 ALU 结果即时可用)
 *      ② load-use 停顿: LW 在 EX 阶段算出地址、MEM 才读出数据,
 *         如果紧接着的指令要用该目标寄存器, 旁路来不及 → 停顿。
 *   2. 控制冒险 — 分支/跳转改变指令流向
 *      - 分支在 EX 阶段解析真实方向与目标
 *      - 2-bit 饱和计数器预测下一分支方向; 预测对则 IF 已取对目标,
 *        预测错则冲刷 IF/ID + ID/EX 两个阶段 (罚 2 周期)。
 * ================================================================ */

#include "pipeline.h"
#include <string.h>
#include <stdio.h>

const char* stage_name(uint8_t opcode) {
    static const char* names[] = {
        "NOP","ADD","SUB","LW","SW","BEQ","J","HLT"
    };
    return (opcode <= OP_HLT) ? names[opcode] : "??";
}

/* 该 opcode 是否把结果写回寄存器堆 */
static bool writes_reg(uint8_t op) {
    return op == OP_ADD || op == OP_SUB || op == OP_LW;
}

/* ================================================================
 * 初始化 / 装载
 * ================================================================ */
void pipeline_init(PipelineCPU* c) {
    memset(c, 0, sizeof(*c));
    c->bp_state = 1;       /* 初始 "弱不取" (01) --- 循环默认顺序 */
}

void pipeline_load(PipelineCPU* c, const Ins* prog, int n) {
    if (n > MEMSZ / 2) n = MEMSZ / 2;
    for (int i = 0; i < n; i++) {
        uint8_t b0, b1;
        ins_encode(&prog[i], &b0, &b1);
        c->mem[i * 2]     = b0;
        c->mem[i * 2 + 1] = b1;
    }
    c->pc = 0;
}

bool pipeline_done(PipelineCPU* c) {
    return c->halted;
}

/* ================================================================
 * 旁路辅助 1: 读寄存器值, 优先从流水线阶段转发
 *   - EX/MEM 的 ADD/SUB 结果 (exmem_alu) 可直接转发
 *   - EX/MEM 的 LW 只有"有效地址"还没数据, 不可转发 → 返回 false 表示需停顿
 *   - MEM/WB 的值 (memwb_value) 对 ADD/SUB/LW 都可用
 * 返回 true 表示拿到了有效值。
 * ================================================================ */
static bool fwd_read_reg(PipelineCPU* c, uint8_t reg, uint8_t* out) {
    if (reg == 0) { *out = 0; return true; }
    /* EX/MEM: 仅非-LW 的写寄存器指令结果可用 */
    if (c->exmem_valid &&
        (c->exmem_ins.opcode == OP_ADD || c->exmem_ins.opcode == OP_SUB) &&
        c->exmem_ins.rd == reg) {
        c->forwards++;
        *out = c->exmem_alu;
        return true;
    }
    /* MEM/WB: ADD/SUB/LW 结果都可用 */
    if (c->memwb_valid && writes_reg(c->memwb_ins.opcode) &&
        c->memwb_ins.rd == reg) {
        c->forwards++;
        *out = c->memwb_value;
        return true;
    }
    *out = c->reg[reg];
    return true;
}

/* ================================================================
 * 旁路辅助 2: 一条指令是否用到某寄存器作为源
 * ================================================================ */
static bool reads_reg(const Ins* ins, uint8_t reg) {
    if (reg == 0) return false;
    switch (ins->opcode) {
        case OP_ADD: case OP_SUB:
            return (ins->rs == reg) || (ins->rt == reg);
        case OP_LW:
            return ins->rs == reg;
        case OP_SW:
            return (ins->rs == reg) || (ins->rt == reg);
        case OP_BEQ:
            return (ins->rs == reg) || (ins->rt == reg);
        default:
            return false;
    }
}

/* 判断: EX/MEM 阶段的 LW 是否正要把某寄存器写回 (load-use 危险源) */
static bool exmem_lw_writes(PipelineCPU* c, uint8_t reg) {
    return c->exmem_valid && c->exmem_ins.opcode == OP_LW &&
           c->exmem_ins.rd == reg && reg != 0;
}

/* ================================================================
 * pipeline_clock — 推进一个时钟周期
 *
 * 用「先算 next 暂存 → 后统一提交」的模式, 避免边推进边覆盖。
 * ================================================================ */
void pipeline_clock(PipelineCPU* c) {
    c->cycles++;

    /* 捕获本轮开始时的各段寄存器 (提交时用旧值, 避免自覆盖) */
    uint16_t cur_ifid_pc    = c->ifid_pc;
    Ins      cur_ifid_ins   = c->ifid_ins;
    bool     cur_ifid_valid = c->ifid_valid;
    uint16_t cur_idex_pc    = c->idex_pc;
    uint16_t cur_exmem_pc   = c->exmem_pc;

    /* ---------- ① WB (写回寄存器堆) ---------- */
    if (c->memwb_valid && writes_reg(c->memwb_ins.opcode) &&
        c->memwb_ins.rd != 0) {
        c->reg[c->memwb_ins.rd] = c->memwb_value;
    }

    /* ---------- ② MEM → MEM/WB (访存) ---------- */
    Ins     next_memwb_ins   = c->exmem_ins;
    uint8_t next_memwb_value = c->exmem_alu;
    bool    next_memwb_valid = c->exmem_valid;
    if (c->exmem_valid) {
        if (c->exmem_ins.opcode == OP_LW) {
            next_memwb_value = c->mem[c->exmem_alu];  /* LW: 读内存 */
        } else if (c->exmem_ins.opcode == OP_SW) {
            c->mem[c->exmem_alu] = c->exmem_b;        /* SW: 写内存 */
            next_memwb_valid = false;                 /* SW 不写回寄存器 */
        }
    }

    /* ---------- ③ EX → EX/MEM (ALU + 分支解析) ---------- */
    Ins     next_exmem_ins = c->idex_ins;
    uint8_t next_exmem_alu = 0;
    uint8_t next_exmem_b   = c->idex_b;
    bool    next_exmem_valid = c->idex_valid;
    bool    branch_this_ex    = false;  /* 本轮 EX 是否解析了分支 */
    bool    flush_pipeline    = false;  /* 分支→冲刷 IF/ID+ID/EX */
    uint16_t real_target      = 0;

    if (c->idex_valid) {
        uint8_t op = c->idex_ins.opcode;
        uint8_t a  = c->idex_a, b = c->idex_b;
        switch (op) {
            case OP_ADD: next_exmem_alu = a + b; break;
            case OP_SUB: next_exmem_alu = a - b; break;
            case OP_LW:  next_exmem_alu = a + c->idex_ins.imm; break;
            case OP_SW:  next_exmem_alu = a + c->idex_ins.imm; break;

            case OP_BEQ: case OP_J: {
                bool taken = (op == OP_J) ? true : (a == b);
                uint16_t target = c->idex_pc + 1 + c->idex_ins.imm;
                branch_this_ex = true;

                /* 2-bit 饱和计数器更新 (仅用于统计展示命中率) */
                if (taken) { if (c->bp_state < 3) c->bp_state++; }
                else       { if (c->bp_state > 0) c->bp_state--; }

                /* 本实现不预取分支目标, 所以每次分支都固定冲刷
                 * 已取入 IF/ID+ID/EX 的错误路径指令 (罚 2 周期) */
                flush_pipeline = true;
                c->flushes++;
                real_target = taken ? target : (c->idex_pc + 1);
                next_exmem_alu = (uint8_t)(real_target & 0xFF);
                break;
            }
            case OP_HLT: c->halted = true; break;
            default: break;   /* NOP */
        }
    }

    /* ---------- ④ ID → ID/EX (读寄存器 + load-use 检测) ---------- */
    uint8_t next_idex_a = 0, next_idex_b = 0;
    bool    next_idex_valid = c->ifid_valid;
    bool    stall = false;

    if (c->ifid_valid && !flush_pipeline) {
        Ins* ins = &c->ifid_ins;
        /* 数据冒险停顿 (两类):
         *   A. ID/EX 是写寄存器指令 (ADD/SUB/LW), 其目标 rd 被本指令当源用:
         *      ALU 结果要到下周期才进 EX/MEM 可旁路, LW 更晚 → 需停顿
         *   B. EX/MEM 是 LW 且 rd 是本指令源: LW 在 EX/MEM 阶段只有有效
         *      地址还没读回数据 → 需停顿
         * 其余 (EX/MEM 的 ALU 结果) 由旁路 fwd_read_reg 直接转发 */
        bool hit_a = c->idex_valid && writes_reg(c->idex_ins.opcode) &&
                     reads_reg(ins, c->idex_ins.rd);
        bool hit_b = exmem_lw_writes(c, ins->rs) ||
                     exmem_lw_writes(c, ins->rt);
        if (hit_a || hit_b) {
            stall = true;
            c->stalls++;
        } else {
            /* 从寄存器堆/旁路读操作数 */
            fwd_read_reg(c, ins->rs, &next_idex_a);
            fwd_read_reg(c, ins->rt, &next_idex_b);
        }
    }

    /* ---------- ⑤ IF → IF/ID (取指) ---------- */
    /* 本周期取指地址:
     *   若本次 EX 分支预测错 → 从真实目标重取 (冲刷后 IF 也要修正)
     *   否则 → 顺序取 pc */
    uint8_t  fetch_pc     = (flush_pipeline) ? (uint8_t)real_target
                                             : (uint8_t)c->pc;
    Ins      next_ifid_ins   = ins_decode(c->mem, fetch_pc);
    uint16_t next_ifid_pc    = fetch_pc;
    bool     next_ifid_valid = !c->halted;

    /* ---------- ⑥ 统一提交 (锁存) ---------- */
    bool bubble = (stall || flush_pipeline) ? true : false;

    /* IF/ID: 停顿(stall)时冻结, 保留本轮指令供下周期重进 ID;
     * 否则存新取指的指令 (flush 时冲刷为气泡) */
    if (stall) {
        c->ifid_pc    = cur_ifid_pc;      /* 冻结: 不取新指令 */
        c->ifid_ins   = cur_ifid_ins;
        c->ifid_valid = cur_ifid_valid;
    } else {
        c->ifid_pc    = next_ifid_pc;
        c->ifid_ins   = next_ifid_ins;
        c->ifid_valid = flush_pipeline ? false : next_ifid_valid;
    }

    /* ID/EX: 本轮 ID 阶段处理的指令 (来自 cur_ifid, 即推进前的 IF/ID);
     *   stall → 注入气泡 (下周期重取同一条) */
    c->idex_pc    = cur_ifid_pc;
    c->idex_ins   = bubble ? (Ins){OP_NOP,0,0,0,0} : cur_ifid_ins;
    c->idex_a     = bubble ? 0 : next_idex_a;
    c->idex_b     = bubble ? 0 : next_idex_b;
    c->idex_valid = bubble ? false : next_idex_valid;

    /* EX/MEM: 本轮 EX 阶段处理的指令 (来自 cur_idex / next_exmem);
     *   stall 不冻结 EX/MEM, 正常推进 */
    c->exmem_pc    = cur_idex_pc;
    c->exmem_ins   = next_exmem_ins;
    c->exmem_alu   = next_exmem_alu;
    c->exmem_b     = next_exmem_b;
    c->exmem_valid = next_exmem_valid;

    /* MEM/WB: 本轮 MEM 阶段处理的指令 (来自 next_memwb) */
    c->memwb_pc    = cur_exmem_pc;
    c->memwb_ins   = next_memwb_ins;
    c->memwb_value = next_memwb_value;
    c->memwb_valid = next_memwb_valid;

    /* ---------- ⑦ 更新 PC ---------- */
    if (flush_pipeline) {
        c->pc = real_target;         /* 跳到真实目标 */
    } else if (stall) {
        /* 停顿: IF 冻结, PC 不变 (下一周期重新取同一条) */
        c->pc = c->pc;
    } else if (branch_this_ex) {
        c->pc = real_target;         /* 预测对的分支: 跳目标 */
    } else {
        c->pc = c->pc + 1;           /* 顺序取指 */
    }

    /* 记录(可视化用) 最近一个分支的真实方向 */
    c->branch_taken_this_cycle = branch_this_ex;
}

/* ================================================================
 * pipeline_run — 运行到程序结束 (带周期上限)
 *
 * HLT 在 EX 阶段置 halted, 但流水线里 HLT 之前的指令还没写完回,
 * 必须让流水线"排空"(四段寄存器全变空) 才真正结束, 否则结果缺失。
 * ================================================================ */
void pipeline_run(PipelineCPU* c, int max_cycles) {
    while (c->cycles < max_cycles) {
        pipeline_clock(c);
        /* 结束条件: 已 halt 且流水线排空 */
        if (c->halted &&
            !c->ifid_valid && !c->idex_valid &&
            !c->exmem_valid && !c->memwb_valid) {
            break;
        }
    }
}
