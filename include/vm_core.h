/*
 * xiaomo - Kills 字节码 VM 内核
 *
 * 方向: 1) 栈式执行内核(下沉 C 层) 2) 函数调用 3) 完整指令集 4) 二进制执行引擎
 *
 * 模型:
 *   - 64 个 64 位通用寄存器 (R0..R63)
 *   - 操作数栈 (vm_stack) : 表达式/传参/返回
 *   - 调用栈 (vm_stack)   : 函数调用现场(保存返回 PC + 旧寄存器)
 *   - 线性内存 (数据段)   : LOAD/STORE 访问
 *
 * 字节码格式 (Kills binary):
 *   [魔数 "KILLS" 5B] [版本 u8] [flags u8]
 *   [寄存器数 u32] [数据段大小 u32] [指令数 u32]
 *   [常量池...] [数据段...] [指令表...]
 *
 * 指令编码 (变长, 每指令以 opcode 起始):
 *   opcode u8 | 操作数字段 (依指令而异)
 */
#ifndef XIAOMO_VM_CORE_H
#define XIAOMO_VM_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "vm_stack.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KILLS_MAGIC      "KILLS"
#define KILLS_VERSION    1
#define KILLS_NREG       64

/* 指令操作码 */
typedef enum {
    OP_NOP = 0,
    /* 寄存器立即数/寄存器移动 */
    OP_MOV,      /* MOV dst_reg, imm64 | MOV dst_reg, src_reg */
    /* 算术/逻辑 (dst = src1 op src2 或 dst op= imm) */
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_SHL, OP_SHR,
    /* 栈操作 */
    OP_PUSH,     /* PUSH src_reg | PUSH imm */
    OP_POP,      /* POP dst_reg */
    /* 内存 */
    OP_LOAD,     /* LOAD dst_reg, [addr_reg] (字节) */
    OP_STORE,    /* STORE [addr_reg], src_reg (字节) */
    OP_LOAD64,   /* LOAD64 dst_reg, [addr_reg] (8 字节, 小端) */
    OP_STORE64,  /* STORE64 [addr_reg], src_reg (8 字节, 小端) */
    /* 控制流 */
    OP_JMP,      /* JMP offset */
    OP_JZ, OP_JNZ, OP_JE, OP_JNE, OP_JG, OP_JGE, OP_JL, OP_JLE,
    /* 函数调用 */
    OP_CALL,     /* CALL fn_index  (从函数表查) */
    OP_RET,      /* RET */
    /* 外部函数 (FFI) */
    OP_FFI,      /* FFI idx */
    /* 系统 */
    OP_PRINT,    /* PRINT dst_reg (打印寄存器到输出) */
    OP_HALT,
    /* ---- 硬件直访 (Hardware Direct Access) ---- */
    OP_HW_PCI_ENUM,  /* 枚举 PCI 设备, 输出到 VM output */
    OP_HW_USB_ENUM,  /* 枚举 USB 设备 */
    OP_HW_SERIAL_ENUM, /* 枚举串口设备 */
    OP_HW_CPU_INFO,  /* CPU 信息 */
    OP_HW_PCI_RD,    /* PCI 配置空间读: a=dst_reg, b=bus, imm=(dev<<16|func<<8|offset) */
    OP_HW_UART_OPEN, /* 打开串口: a=baud_reg, b=path_const_idx */
    OP_HW_UART_CLOSE,/* 关闭串口: a=handle_reg */
    OP_HW_UART_RD,   /* 读串口: a=dst_reg, b=handle_reg, imm=maxlen */
    OP_HW_UART_WR,   /* 写串口: a=handle_reg, b=data_reg, imm=len */
    OP_HW_SYS_INFO   /* 系统信息: 物理内存等 */
} KillsOp;

/* 指令 (内部表示, 供解释器/编译器用) */
typedef struct {
    uint8_t op;
    int32_t a;   /* 目的/第一操作数 */
    int32_t b;   /* 第二操作数 */
    int64_t imm; /* 立即数/偏移 */
} KillsIns;

/* 函数表项 */
typedef struct {
    char* name;      /* 函数名 */
    uint32_t pc;     /* 入口指令下标 */
    uint32_t nparams;/* 参数个数 */
} KillsFunc;

/* 常量表项 */
typedef struct {
    uint8_t type;    /* 0=int, 1=str, 2=float */
    int64_t iv;
    double  fv;
    char*   sv;
} KillsConst;

/* 字节码程序 */
typedef struct {
    KillsIns*   code;
    uint32_t    code_count;
    KillsConst* consts;
    uint32_t    const_count;
    uint8_t*    data;        /* 线性内存数据段 */
    uint32_t    data_size;
    KillsFunc*  funcs;
    uint32_t    func_count;
} KillsProgram;

/* VM 实例 */
typedef struct {
    int64_t regs[KILLS_NREG];
    VmStack operand;      /* 操作数栈 */
    VmStack callstack;    /* 调用栈 */
    const KillsProgram* prog;
    uint32_t pc;
    /* 输出 */
    char** output;
    int output_count, output_cap;
    /* 执行控制 */
    int halted;
    int error_count;
    char error_msg[1024];
    uint32_t step_limit;  /* 防死循环 */
    uint32_t steps;
} KillsVM;

/* ---- 程序构建 ---- */
KillsProgram* kprog_new(void);
void kprog_add_ins(KillsProgram* p, uint8_t op, int32_t a, int32_t b, int64_t imm);
int  kprog_add_const(KillsProgram* p, uint8_t type, int64_t iv, double fv, const char* sv);
int  kprog_add_func(KillsProgram* p, const char* name, uint32_t pc, uint32_t nparams);
uint32_t kprog_alloc_data(KillsProgram* p, uint32_t size);
void kprog_free(KillsProgram* p);

/* ---- 序列化/反序列化 (方向 4) ---- */
/* 序列化为二进制缓冲区 (malloc), 返回长度, 失败返回 -1 */
long kprog_serialize(const KillsProgram* p, uint8_t** out);
/* 从二进制反序列化 */
KillsProgram* kprog_deserialize(const uint8_t* buf, long len, char* err, int errlen);

/* ---- 执行 ---- */
void kvm_init(KillsVM* vm);
void kvm_free(KillsVM* vm);
int  kvm_run(KillsVM* vm, const KillsProgram* prog);
const char* kvm_output(KillsVM* vm, int idx);
int  kvm_output_count(const KillsVM* vm);

/* ---- 反汇编 (调试) ---- */
void kvm_disassemble(const KillsProgram* p, char* buf, int buflen);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_VM_CORE_H */
