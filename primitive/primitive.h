/**
 * primitive.h — 最原始构造体：CU + 堆栈管理器 + 归一化入口点查找
 *
 * 目标：用最少的代码实现一台完整计算机的核心三要素：
 *   1. 控制单元 (CU) — 真值表译码，唯一决策者
 *   2. 栈管理器 — SP + 边界检查，LIFO 线性区
 *   3. 堆管理器 — 空闲链表，动态分配
 *
 * 外加：归一化入口点扫描器 — 在未知内存中自动找到程序入口
 */

#ifndef PRIMITIVE_H
#define PRIMITIVE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ================================================================
 * 第一部分：自定义指令集 (ISA)
 * ================================================================ */

/* 指令集 — 16 条指令，4 位操作码 */
typedef enum {
    OP_NOP  = 0x0,  /* 空操作 */
    OP_ADD  = 0x1,  /* R[dst] = R[a] + R[b] */
    OP_SUB  = 0x2,  /* R[dst] = R[a] - R[b] */
    OP_LD   = 0x3,  /* R[dst] = MEM[addr] */
    OP_ST   = 0x4,  /* MEM[addr] = R[src] */
    OP_JMP  = 0x5,  /* PC = addr (无条件跳转) */
    OP_JZ   = 0x6,  /* if ZF=1: PC = addr */
    OP_JNZ  = 0x7,  /* if ZF=0: PC = addr */
    OP_PUSH = 0x8,  /* 栈压入 */
    OP_POP  = 0x9,  /* 栈弹出 */
    OP_CALL = 0xA,  /* PUSH PC+1; PC = addr */
    OP_RET  = 0xB,  /* POP PC */
    OP_IRET = 0xC,  /* 中断返回 (恢复上下文) */
    OP_STI  = 0xD,  /* 开中断 (IF=1) */
    OP_CLI  = 0xE,  /* 关中断 (IF=0) */
    OP_HALT = 0xF,  /* 停机 */
} Opcode;

/* 指令格式：16 位定长 */
typedef struct {
    uint8_t  opcode : 4;   /* 操作码 (4 bit) */
    uint8_t  dst    : 4;   /* 目标寄存器 (4 bit → 16 个寄存器) */
    uint8_t  src_a  : 4;   /* 源寄存器 A */
    uint8_t  src_b  : 4;   /* 源寄存器 B (或地址高 4 位) */
} Instruction;

/* 微指令 — 控制信号集 */
typedef struct {
    bool reg_write;        /* 是否写寄存器 */
    bool mem_read;         /* 是否读内存 */
    bool mem_write;        /* 是否写内存 */
    bool alu_op;           /* ALU 操作类型: 0=加 1=减 */
    bool use_imm;          /* 是否使用立即数 */
    bool branch;           /* 是否分支指令 */
    bool cond_branch;      /* 是否条件分支 */
    bool stack_push;       /* 是否压栈 */
    bool stack_pop;        /* 是否弹栈 */
    bool call;             /* 是否调用 */
    bool ret;              /* 是否返回 */
    bool iret;             /* 是否中断返回 */
    bool set_if;           /* 是否设置 IF */
    bool clear_if;         /* 是否清除 IF */
    bool halt;             /* 是否停机 */
} MicroCode;


/* ================================================================
 * 第二部分：栈管理器
 * ================================================================ */

/* 栈的本质：SP + 一块连续内存 + 两条规则 */
typedef struct {
    uint8_t* memory;       /* 栈内存区 */
    uint32_t  size;         /* 栈总大小 (字节) */
    uint32_t  sp;           /* 栈指针 (指向栈顶) */
    uint32_t  base;         /* 栈基址 (栈底) */
    bool      overflow;     /* 溢出标志 */
    bool      underflow;    /* 下溢标志 */
} StackManager;

/* 栈操作：PUSH 和 POP 就是全部 */
void stack_init(StackManager* s, uint8_t* mem, uint32_t size);
void stack_push(StackManager* s, uint8_t value);
uint8_t stack_pop(StackManager* s);
uint8_t stack_peek(StackManager* s);


/* ================================================================
 * 第三部分：堆管理器（空闲链表）
 * ================================================================ */

/* 堆块头部 — 每个分配块前面都有这个 */
typedef struct HeapBlock {
    uint32_t size;              /* 块大小 (不含头部) */
    bool     used;              /* 是否在使用 */
    struct HeapBlock* next;     /* 下一块 (空闲链表) */
} HeapBlock;

/* 堆管理器 */
typedef struct {
    uint8_t*   memory;    /* 堆内存区 */
    uint32_t    total_size; /* 总大小 */
    HeapBlock* free_list; /* 空闲链表头 */
} HeapManager;

void  heap_init(HeapManager* h, uint8_t* mem, uint32_t size);
void* heap_alloc(HeapManager* h, uint32_t size);
void  heap_free(HeapManager* h, void* ptr);
void  heap_dump(HeapManager* h);


/* ================================================================
 * 第四部分：控制单元 (CU) — 唯一决策者
 * ================================================================ */

/* 计算机状态 */
typedef struct {
    uint8_t  reg[16];        /* 16 个通用寄存器 */
    uint8_t  pc;             /* 程序计数器 */
    bool     zf;             /* 零标志 */
    bool     cf;             /* 进位标志 */
    bool     iflag;          /* 中断使能标志 */
    bool     running;        /* 运行标志 */
} CPUState;

#include "interrupt.h"

/* 主内存 */
#define MEMORY_SIZE 256

/* 控制单元 + 完整计算机 */
typedef struct {
    CPUState          cpu;
    StackManager      stack;
    HeapManager       heap;
    Pic               pic;              /* 中断控制器 */
    InterruptContext  intr_ctx;         /* 中断上下文保存区 */
    uint8_t           memory[MEMORY_SIZE];  /* 统一内存 */
    MicroCode         current_uop;          /* 当前微指令 (调试用) */
} PrimitiveComputer;

/* 控制单元核心：真值表译码 */
MicroCode cu_decode(Opcode op);

/* 执行一条指令 */
void computer_step(PrimitiveComputer* c);

/* 加载程序并运行 */
void computer_load(PrimitiveComputer* c, Instruction* prog, uint8_t len);
void computer_run(PrimitiveComputer* c);
void computer_dump(PrimitiveComputer* c);


/* ================================================================
 * 第五部分：归一化入口点查找器
 * ================================================================ */

/* 在未知内存中自动识别程序入口 */
typedef struct {
    uint32_t address;
    float    score;
    char     reason[64];
} EntryCandidate;

#define MAX_CANDIDATES 16

/* 归一化算法：特征提取 + 加权评分 + 排除 → 排序 */
int find_entry_points(
    uint8_t* memory, uint32_t size,
    EntryCandidate* results, int max_results
);

/* 特征提取器 (内部使用) */
float feature_sp_init(uint8_t* mem, uint32_t offset, uint32_t size);
float feature_ivt(uint8_t* mem, uint32_t offset, uint32_t size);
float feature_code_density(uint8_t* mem, uint32_t offset, uint32_t size);
float feature_reset_vector(uint8_t* mem, uint32_t offset, uint32_t size);

#endif /* PRIMITIVE_H */