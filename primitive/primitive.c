/**
 * primitive.c — 最原始构造体实现
 *
 * 三大核心模块：
 *   1. 栈管理器 — SP + 边界检查，4 行代码搞定
 *   2. 堆管理器 — 空闲链表，首次适配
 *   3. 控制单元 — 操作码真值表，唯一决策者
 *
 * 外加：归一化入口点扫描器
 */

#include "primitive.h"
#include "interrupt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================
 * 栈管理器 — 不需要复杂逻辑
 * ================================================================ */

void stack_init(StackManager* s, uint8_t* mem, uint32_t size) {
    s->memory    = mem;
    s->size      = size;
    s->base      = size - 1;        /* 栈底 = 最高地址 */
    s->sp        = size;            /* SP 初始指向栈外 (空栈) */
    s->overflow  = false;
    s->underflow = false;
}

void stack_push(StackManager* s, uint8_t value) {
    if (s->sp == 0) {
        s->overflow = true;
        return;
    }
    s->sp--;
    s->memory[s->sp] = value;
}

uint8_t stack_pop(StackManager* s) {
    if (s->sp >= s->size) {
        s->underflow = true;
        return 0;
    }
    uint8_t value = s->memory[s->sp];
    s->sp++;
    return value;
}

uint8_t stack_peek(StackManager* s) {
    if (s->sp >= s->size) return 0;
    return s->memory[s->sp];
}


/* ================================================================
 * 堆管理器 — 空闲链表 (首次适配)
 * ================================================================ */

void heap_init(HeapManager* h, uint8_t* mem, uint32_t size) {
    h->memory     = mem;
    h->total_size = size;

    /* 整个内存就是一个大空闲块 */
    HeapBlock* first = (HeapBlock*)mem;
    first->size  = size - sizeof(HeapBlock);
    first->used  = false;
    first->next  = NULL;
    h->free_list = first;
}

void* heap_alloc(HeapManager* h, uint32_t size) {
    HeapBlock* prev = NULL;
    HeapBlock* curr = h->free_list;

    /* 首次适配：找到第一个足够大的空闲块 */
    while (curr) {
        if (!curr->used && curr->size >= size) {
            /* 检查是否需要分裂 */
            uint32_t remaining = curr->size - size;
            if (remaining > sizeof(HeapBlock) + 4) {
                /* 分裂：在 curr 后面创建新空闲块 */
                HeapBlock* new_block = (HeapBlock*)((uint8_t*)curr + sizeof(HeapBlock) + size);
                new_block->size = remaining - sizeof(HeapBlock);
                new_block->used = false;
                new_block->next = curr->next;
                curr->size = size;
                curr->next = new_block;
            }

            curr->used = true;

            /* 从空闲链表中移除 */
            if (prev) {
                prev->next = curr->next;
            } else {
                h->free_list = curr->next;
            }

            return (uint8_t*)curr + sizeof(HeapBlock);
        }
        prev = curr;
        curr = curr->next;
    }

    return NULL;  /* 没有足够大的块 */
}

void heap_free(HeapManager* h, void* ptr) {
    if (!ptr) return;

    HeapBlock* block = (HeapBlock*)((uint8_t*)ptr - sizeof(HeapBlock));
    block->used = false;

    /* 合并右邻居 */
    if (block->next && !block->next->used) {
        block->size += sizeof(HeapBlock) + block->next->size;
        block->next = block->next->next;
    }

    /* 放回空闲链表 (头插法) */
    block->next = h->free_list;
    h->free_list = block;
}

void heap_dump(HeapManager* h) {
    printf("堆状态:\n");
    HeapBlock* curr = h->free_list;
    printf("  空闲链表: ");
    while (curr) {
        printf("[%p size=%u] -> ", (void*)curr, curr->size);
        curr = curr->next;
    }
    printf("NULL\n");

    /* 遍历所有块 */
    uint8_t* ptr = h->memory;
    printf("  所有块:\n");
    while (ptr < h->memory + h->total_size) {
        HeapBlock* b = (HeapBlock*)ptr;
        if (b->size == 0) break;
        printf("    地址=%p size=%u used=%d\n", (void*)b, b->size, b->used);
        ptr += sizeof(HeapBlock) + b->size;
        if (b->next == NULL) break;
    }
}

/* ================================================================
 * 控制单元 — 真值表译码
 *
 * 核心思想：操作码 → 控制信号。这就是 CU 的全部工作。
 * 在硬件里，这是一堆与或非门；在这里，是一个 switch。
 * 本质一样——都是查表。
 * ================================================================ */

MicroCode cu_decode(Opcode op) {
    MicroCode mc = {0};  /* 全部清零 */

    switch (op) {
    case OP_NOP:
        /* 什么都不做 */
        break;

    case OP_ADD:
        mc.reg_write = true;   /* 结果写回寄存器 */
        mc.alu_op    = false;  /* 加法 */
        break;

    case OP_SUB:
        mc.reg_write = true;
        mc.alu_op    = true;   /* 减法 */
        break;

    case OP_LD:
        mc.reg_write = true;
        mc.mem_read  = true;   /* 读内存 */
        break;

    case OP_ST:
        mc.mem_write = true;   /* 写内存 */
        break;

    case OP_JMP:
        mc.branch = true;      /* 无条件跳转 */
        break;

    case OP_JZ:
        mc.cond_branch = true; /* 条件跳转 (ZF=1) */
        break;

    case OP_JNZ:
        mc.cond_branch = true; /* 条件跳转 (ZF=0) */
        break;

    case OP_PUSH:
        mc.stack_push = true;  /* 压栈 */
        mc.mem_write  = true;
        break;

    case OP_POP:
        mc.stack_pop  = true;  /* 弹栈 */
        mc.reg_write  = true;
        mc.mem_read   = true;
        break;

    case OP_CALL:
        mc.stack_push = true;  /* 压返回地址 */
        mc.mem_write  = true;
        mc.call       = true;
        break;

    case OP_RET:
        mc.stack_pop  = true;  /* 弹返回地址 */
        mc.mem_read   = true;
        mc.ret        = true;
        break;

    case OP_IRET:
        mc.iret       = true;  /* 中断返回 */
        break;

    case OP_STI:
        mc.set_if     = true;  /* 开中断 */
        break;

    case OP_CLI:
        mc.clear_if   = true;  /* 关中断 */
        break;

    case OP_HALT:
        mc.halt = true;
        break;
    }

    return mc;
}

/* ================================================================
 * 执行引擎 — 取指 → 译码 → 执行
 * ================================================================ */

void computer_step(PrimitiveComputer* c) {
    /* 0. 中断检查 (在取指之前)
     * 这是计算机从「计算器」变成「计算机」的关键：
     * 外部事件可以在这个时刻插入，改变执行流 */
    if (c->cpu.iflag && pic_has_pending(&c->pic)) {
        if (intr_handle(&c->pic, &c->cpu.pc, c->cpu.reg,
                        &c->cpu.zf, &c->cpu.cf,
                        &c->cpu.iflag, &c->intr_ctx)) {
            return;  /* 本周期被中断占用，不执行原指令 */
        }
    }

    /* 1. 取指 */
    Instruction instr;
    if (c->cpu.pc * 2 + 1 >= MEMORY_SIZE) {
        c->cpu.running = false;
        return;
    }
    uint8_t* ip = &c->memory[c->cpu.pc * 2];  /* 每条指令 2 字节 */
    instr.opcode = (ip[0] >> 4) & 0x0F;
    instr.dst    = ip[0] & 0x0F;
    instr.src_a  = (ip[1] >> 4) & 0x0F;
    instr.src_b  = ip[1] & 0x0F;

    /* 2. 译码 — CU 的核心工作 */
    MicroCode uop = cu_decode(instr.opcode);
    c->current_uop = uop;

    /* 3. 执行 */
    uint8_t pc_next = c->cpu.pc + 1;  /* 默认 PC+1 */

    /* 计算地址 (src_a:src_b 拼接) */
    uint8_t addr = (instr.src_a << 4) | instr.src_b;

    if (uop.halt) {
        c->cpu.running = false;
        return;
    }

    /* ALU 操作 */
    if (uop.reg_write && !uop.mem_read && !uop.stack_pop) {
        uint8_t a = c->cpu.reg[instr.src_a];
        uint8_t b = c->cpu.reg[instr.src_b];
        uint8_t result = uop.alu_op ? (a - b) : (a + b);
        c->cpu.reg[instr.dst] = result;

        /* 更新标志 */
        c->cpu.zf = (result == 0);
        c->cpu.cf = (uop.alu_op ? (a < b) : (a + b > 0xFF));
    }

    /* 内存读 */
    if (uop.mem_read && !uop.stack_pop) {
        c->cpu.reg[instr.dst] = c->memory[addr];
    }

    /* 内存写 — 使用 src_a 寄存器的值作为地址 */
    if (uop.mem_write && !uop.stack_push) {
        uint8_t mem_addr = c->cpu.reg[instr.src_a];
        c->memory[mem_addr] = c->cpu.reg[instr.dst];
    }

    /* 栈操作 */
    if (uop.stack_push) {
        if (uop.call) {
            /* CALL: 压入返回地址 = PC+1 */
            stack_push(&c->stack, pc_next);
        } else {
            /* PUSH: 压入寄存器值 */
            stack_push(&c->stack, c->cpu.reg[instr.src_a]);
        }
    }

    if (uop.stack_pop) {
        if (uop.ret) {
            /* RET: 弹出返回地址 */
            pc_next = stack_pop(&c->stack);
        } else {
            /* POP: 弹出到寄存器 */
            c->cpu.reg[instr.dst] = stack_pop(&c->stack);
        }
    }

    /* 分支 */
    if (uop.branch) {
        pc_next = addr;
    }

    if (uop.cond_branch) {
        bool take = false;
        if (instr.opcode == OP_JZ)  take = c->cpu.zf;
        if (instr.opcode == OP_JNZ) take = !c->cpu.zf;
        if (take) pc_next = addr;
    }

    /* 中断返回 */
    if (uop.iret) {
        intr_restore_context(&c->intr_ctx, &pc_next, c->cpu.reg,
                             &c->cpu.zf, &c->cpu.cf,
                             &c->cpu.iflag, &c->pic.imr);
        /* 清除当前 ISR 位 */
        c->pic.isr = 0;
    }

    /* 开/关中断 */
    if (uop.set_if)   c->cpu.iflag = true;
    if (uop.clear_if) c->cpu.iflag = false;

    c->cpu.pc = pc_next;
}

void computer_load(PrimitiveComputer* c, Instruction* prog, uint8_t len) {
    /* 把程序写入内存 */
    uint8_t* mem = c->memory;
    for (int i = 0; i < len; i++) {
        mem[i * 2]     = (prog[i].opcode << 4) | prog[i].dst;
        mem[i * 2 + 1] = (prog[i].src_a << 4) | prog[i].src_b;
    }

    /* 初始化栈区域：内存后半部分 */
    uint8_t stack_start = 128;   /* 栈从地址 128 开始 */
    uint8_t stack_size  = 128;   /* 栈大小 128 字节 */
    stack_init(&c->stack, &c->memory[stack_start], stack_size);

    /* 初始化堆区域：内存前半部分 (程序之后) */
    /* 堆在程序代码和数据之间 */
    uint8_t heap_start = 64;
    uint8_t heap_size  = 64;
    heap_init(&c->heap, &c->memory[heap_start], heap_size);

    /* CPU 初始化 */
    memset(c->cpu.reg, 0, sizeof(c->cpu.reg));
    c->cpu.pc = 0;
    c->cpu.zf = false;
    c->cpu.cf = false;
    c->cpu.iflag = false;  /* 初始关中断 */
    c->cpu.running = true;

    /* 中断系统初始化 */
    pic_init(&c->pic);
    memset(&c->intr_ctx, 0, sizeof(c->intr_ctx));
}

void computer_run(PrimitiveComputer* c) {
    int steps = 0;
    while (c->cpu.running && steps < 1000) {
        computer_step(c);
        steps++;
    }
    printf("执行了 %d 条指令后%s\n", steps,
           c->cpu.running ? " (超时停止)" : " (HALT)");
}

void computer_dump(PrimitiveComputer* c) {
    printf("\n══════ 计算机状态 ══════\n");
    printf("PC = 0x%02X    ZF=%d CF=%d IF=%d\n",
           c->cpu.pc, c->cpu.zf, c->cpu.cf, c->cpu.iflag);
    printf("寄存器: ");
    for (int i = 0; i < 16; i++) {
        printf("R%d=0x%02X ", i, c->cpu.reg[i]);
        if (i == 7) printf("\n        ");
    }
    printf("\n栈: SP=0x%02X 溢出=%d 下溢=%d\n",
           c->stack.sp, c->stack.overflow, c->stack.underflow);
    printf("════════════════════════\n");
}


/* ================================================================
 * 归一化入口点查找器
 *
 * 核心思想：在未知内存中，每种 CPU 的入口点都有「归一化特征」。
 * 将多个特征量化评分 → 加权求和 → 排除低分 → 排序输出。
 * ================================================================ */

/* 特征 1: 栈指针初始化模式
 * 入口点附近通常有「加载立即数到 SP」的指令模式。
 * 这里简化：检测内存中连续递增的地址模式 (栈通常在高地址) */
float feature_sp_init(uint8_t* mem, uint32_t offset, uint32_t size) {
    if (offset + 4 > size) return 0.0;

    /* 检查是否看起来像指针值 (指向高地址) */
    uint32_t val = (mem[offset] << 24) | (mem[offset+1] << 16) |
                   (mem[offset+2] << 8)  | mem[offset+3];
    uint32_t max_addr = size - 1;

    /* 如果值接近内存顶部，可能是栈指针初始化 */
    if (val > max_addr * 0.7 && val <= max_addr) {
        return 0.6 + 0.4 * (float)(val - max_addr * 0.7) / (max_addr * 0.3);
    }
    return 0.0;
}

/* 特征 2: 中断向量表结构
 * 入口点附近常有连续地址模式 (跳转表) */
float feature_ivt(uint8_t* mem, uint32_t offset, uint32_t size) {
    if (offset + 16 > size) return 0.0;

    int addr_count = 0;
    /* 检测连续 4 个看起来像地址的值 */
    for (int i = 0; i < 4; i++) {
        uint32_t val = (mem[offset + i*4] << 24) | (mem[offset + i*4 + 1] << 16) |
                       (mem[offset + i*4 + 2] << 8)  | mem[offset + i*4 + 3];
        /* 地址应该在内存范围内，且不是零 */
        if (val > 0 && val < size && val % 2 == 0) {
            addr_count++;
        }
    }
    return (float)addr_count / 4.0;
}

/* 特征 3: 代码密度 — 检测有效指令分布
 * 指令字节的分布模式不同于数据 */
static bool is_plausible_opcode(uint8_t byte) {
    /* 我们的 ISA 操作码范围 0x00-0x0F (高 4 位) */
    uint8_t op = (byte >> 4) & 0x0F;
    return op <= 0x0F && op != 0xE;  /* 0xE 未使用 */
}

float feature_code_density(uint8_t* mem, uint32_t offset, uint32_t size) {
    int window = 16;  /* 检查 16 字节窗口 */
    if (offset + window > size) window = size - offset;
    if (window <= 0) return 0.0;

    int valid = 0;
    for (int i = 0; i < window; i++) {
        if (is_plausible_opcode(mem[offset + i])) valid++;
    }
    return (float)valid / window;
}

/* 特征 4: 复位向量位置
 * 许多 CPU 在固定地址有复位向量。
 * 对于小内存 (256B)，检查内存中是否有地址指向当前 offset */
float feature_reset_vector(uint8_t* mem, uint32_t offset, uint32_t size) {
    /* 常见复位向量在内存中的偏移 (相对于内存起始) */
    uint32_t reset_offsets[] = {
        0x00,   /* 内存起始 (ARM Cortex-M 向量表) */
        0x04,   /* 偏移 4 (Reset_Handler) */
    };

    /* 检查: 内存中某个固定位置的值是否等于当前 offset */
    for (int i = 0; i < 2; i++) {
        uint32_t ro = reset_offsets[i];
        if (ro + 4 > size) continue;
        /* 读 4 字节小端值 */
        uint32_t target = mem[ro] | (mem[ro+1] << 8) |
                          (mem[ro+2] << 16) | (mem[ro+3] << 24);
        if (target == offset) return 1.0;
        /* 接近也算 */
        if (target > 0 && abs((int)target - (int)offset) < 4) return 0.7;
    }
    return 0.0;
}

int find_entry_points(
    uint8_t* memory, uint32_t size,
    EntryCandidate* results, int max_results
) {
    /* 归一化权重 — 调低代码密度权重，提高 IVT/复位向量权重 */
    const float W_SP    = 0.20;  /* 栈指针初始化 */
    const float W_IVT   = 0.30;  /* 中断向量表 */
    const float W_CODE  = 0.15;  /* 代码密度 (降低: 伪随机数据也可能像指令) */
    const float W_RESET = 0.35;  /* 复位向量 (最高权重: 入口点最强信号) */

    const float THRESHOLD = 0.2;  /* 最低分数阈值 */

    int count = 0;

    /* 扫描所有可能的入口位置 (4 字节对齐) */
    for (uint32_t offset = 0; offset < size && count < max_results; offset += 4) {
        float s_sp    = feature_sp_init(memory, offset, size);
        float s_ivt   = feature_ivt(memory, offset, size);
        float s_code  = feature_code_density(memory, offset, size);
        float s_reset = feature_reset_vector(memory, offset, size);

        /* 归一化加权求和 */
        float total = W_SP * s_sp + W_IVT * s_ivt +
                      W_CODE * s_code + W_RESET * s_reset;

        /* 排除低于阈值 */
        if (total < THRESHOLD) continue;

        /* 记录候选 */
        results[count].address = offset;
        results[count].score   = total;

        /* 生成原因描述 */
        char* r = results[count].reason;
        int pos = 0;
        if (s_sp > 0.5)    pos += snprintf(r + pos, 64 - pos, "SP_init=%.2f ", s_sp);
        if (s_ivt > 0.5)   pos += snprintf(r + pos, 64 - pos, "IVT=%.2f ", s_ivt);
        if (s_code > 0.5)  pos += snprintf(r + pos, 64 - pos, "Code=%.2f ", s_code);
        if (s_reset > 0.5) pos += snprintf(r + pos, 64 - pos, "Reset=%.2f ", s_reset);
        if (pos == 0) snprintf(r, 64, "综合=%.2f", total);

        count++;
    }

    /* 排序：按分数降序 (冒泡，因为候选数很少) */
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (results[j].score > results[i].score) {
                EntryCandidate tmp = results[i];
                results[i] = results[j];
                results[j] = tmp;
            }
        }
    }

    return count;
}