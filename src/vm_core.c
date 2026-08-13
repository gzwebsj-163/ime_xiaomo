/*
 * xiaomo - Kills 字节码 VM 内核实现
 *
 * 对应 vm_core.h 设计:
 *   - 64 个 64 位通用寄存器 (R0..R63)
 *   - 操作数栈 (VmStack): 表达式/传参/返回
 *   - 调用栈   (VmStack): 函数调用现场 (保存返回 PC + 全部寄存器快照)
 *   - 线性内存 (数据段): LOAD/STORE 访问
 *
 * 字节码格式 (Kills binary):
 *   [魔数 "KILLS" 5B] [版本 u8] [flags u8]
 *   [寄存器数 u32] [数据段大小 u32] [指令数 u32]
 *   [常量池...] [数据段...] [指令表...]
 *
 * 指令编码: opcode u8 | a i32 | b i32 | imm i64  (内部表示为 KillsIns)
 */
#include "vm_core.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================= 程序构建 ================= */
KillsProgram* kprog_new(void) {
    KillsProgram* p = (KillsProgram*)calloc(1, sizeof(KillsProgram));
    return p;
}

void kprog_add_ins(KillsProgram* p, uint8_t op, int32_t a, int32_t b, int64_t imm) {
    if (!p) return;
    p->code = (KillsIns*)realloc(p->code, sizeof(KillsIns) * (p->code_count + 1));
    KillsIns* ins = &p->code[p->code_count++];
    ins->op = op; ins->a = a; ins->b = b; ins->imm = imm;
}

int kprog_add_const(KillsProgram* p, uint8_t type, int64_t iv, double fv, const char* sv) {
    if (!p) return -1;
    p->consts = (KillsConst*)realloc(p->consts, sizeof(KillsConst) * (p->const_count + 1));
    KillsConst* c = &p->consts[p->const_count++];
    c->type = type; c->iv = iv; c->fv = fv;
    c->sv = sv ? strdup(sv) : NULL;
    return p->const_count - 1;
}

int kprog_add_func(KillsProgram* p, const char* name, uint32_t pc, uint32_t nparams) {
    if (!p) return -1;
    p->funcs = (KillsFunc*)realloc(p->funcs, sizeof(KillsFunc) * (p->func_count + 1));
    KillsFunc* f = &p->funcs[p->func_count++];
    f->name = strdup(name);
    f->pc = pc; f->nparams = nparams;
    return p->func_count - 1;
}

void kprog_free(KillsProgram* p) {
    if (!p) return;
    free(p->code); p->code = NULL; p->code_count = 0;
    if (p->consts) { for (uint32_t i = 0; i < p->const_count; i++) if (p->consts[i].sv) free(p->consts[i].sv); free(p->consts); }
    p->consts = NULL; p->const_count = 0;
    free(p->data); p->data = NULL; p->data_size = 0;
    if (p->funcs) { for (uint32_t i = 0; i < p->func_count; i++) if (p->funcs[i].name) free(p->funcs[i].name); free(p->funcs); }
    p->funcs = NULL; p->func_count = 0;
    free(p);
}

/* ================= 值工具 ================= */
static void kvm_add_output_ex(KillsVM* vm, const char* s, int append) {
    if (append && vm->output_count > 0) {
        /* 追加到最后一行 */
        char* old = vm->output[vm->output_count - 1];
        size_t newlen = strlen(old) + strlen(s) + 1;
        char* buf = (char*)malloc(newlen);
        snprintf(buf, newlen, "%s%s", old, s);
        free(old);
        vm->output[vm->output_count - 1] = buf;
        return;
    }
    if (vm->output_count >= vm->output_cap) {
        vm->output_cap = vm->output_cap ? vm->output_cap * 2 : 16;
        vm->output = (char**)realloc(vm->output, sizeof(char*) * vm->output_cap);
    }
    vm->output[vm->output_count++] = strdup(s);
}
static void kvm_add_output(KillsVM* vm, const char* s) { kvm_add_output_ex(vm, s, 0); }

/* ================= FFI 表 (内置外部函数) ================= */
/* FFI idx: 0=print_int, 1=print_str, 2=sqrt, 3=halt_print_reg */
typedef struct { int64_t result; const char* str; } FFIRet;

static long ffi_int_arg(KillsVM* vm, int reg) { return (long)vm->regs[reg]; }

static void kvm_ffi(KillsVM* vm, int idx, int result_reg) {
    switch (idx) {
    case 0: { /* print_int from reg a */
        char buf[64]; snprintf(buf, sizeof(buf), "%lld", (long long)ffi_int_arg(vm, vm->regs[63] & 63));
        kvm_add_output(vm, buf); break;
    }
    case 1: { /* print str from const via reg a */
        int ci = (int)(vm->regs[63] & 63);
        if (ci >= 0 && ci < (int)vm->prog->const_count && vm->prog->consts[ci].type == 1 && vm->prog->consts[ci].sv)
            kvm_add_output(vm, vm->prog->consts[ci].sv);
        break;
    }
    case 2: { /* sqrt from reg a into result_reg */
        double v = (double)ffi_int_arg(vm, vm->regs[63] & 63);
        vm->regs[result_reg] = (int64_t)(v * v); break; /* 简单平方 */
    }
    default: break;
    }
}

/* ================= 执行 ================= */
void kvm_init(KillsVM* vm) {
    memset(vm, 0, sizeof(KillsVM));
    vstack_init(&vm->operand, 4096);
    vstack_init(&vm->callstack, 65536);
    vm->step_limit = 100000000;
}

void kvm_free(KillsVM* vm) {
    vstack_destroy(&vm->operand);
    vstack_destroy(&vm->callstack);
    for (int i = 0; i < vm->output_count; i++) free(vm->output[i]);
    if (vm->output) free(vm->output);
    vm->output = NULL; vm->output_count = vm->output_cap = 0;
}

int kvm_output_count(const KillsVM* vm) { return vm->output_count; }
const char* kvm_output(KillsVM* vm, int idx) {
    if (idx < 0 || idx >= vm->output_count) return NULL;
    return vm->output[idx];
}

/* 读取第二个操作数值: b>=0 为寄存器, b<0 用 imm 全无符号(简化: 用 flag 区分由调用侧决定) */
/* 本内核统一: 当 ins->b >= 0 视为寄存器下标; 否则用 ins->imm */
static int is_reg_operand(int32_t b) { return b >= 0; }

/* 执行加载立即数或寄存器到 dst */
int kvm_run(KillsVM* vm, const KillsProgram* prog) {
    if (!vm || !prog) return 1;
    vm->prog = prog;
    vm->pc = 0;
    vm->halted = 0;
    vm->steps = 0;
    vm->error_count = 0;
    vstack_reset(&vm->operand);
    vstack_reset(&vm->callstack);
    memset(vm->regs, 0, sizeof(vm->regs));

    /* 每帧保存: 返回 PC + 64 寄存器 = 1+64 个 u64 */
    while (!vm->halted) {
        if (vm->steps++ > vm->step_limit) {
            snprintf(vm->error_msg, sizeof(vm->error_msg), "step limit exceeded (%u)", vm->step_limit);
            vm->error_count = 1;
            return 1;
        }
        if (vm->pc >= prog->code_count) break;
        KillsIns* ins = &prog->code[vm->pc];
        uint32_t next_pc = vm->pc + 1;

        switch (ins->op) {
        case OP_NOP: break;
        case OP_MOV:
            vm->regs[ins->a] = is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm;
            break;
        case OP_ADD: vm->regs[ins->a] = vm->regs[ins->a] + (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_SUB: vm->regs[ins->a] = vm->regs[ins->a] - (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_MUL: vm->regs[ins->a] = vm->regs[ins->a] * (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_DIV:
            { int64_t d = is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm;
              if (d == 0) { snprintf(vm->error_msg,sizeof(vm->error_msg),"divide by zero at pc %u",vm->pc); vm->error_count=1; return 1; }
              vm->regs[ins->a] = vm->regs[ins->a] / d; break; }
        case OP_MOD:
            { int64_t d = is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm;
              if (d == 0) { snprintf(vm->error_msg,sizeof(vm->error_msg),"mod by zero at pc %u",vm->pc); vm->error_count=1; return 1; }
              vm->regs[ins->a] = vm->regs[ins->a] % d; break; }
        case OP_AND: vm->regs[ins->a] = vm->regs[ins->a] & (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_OR:  vm->regs[ins->a] = vm->regs[ins->a] | (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_XOR: vm->regs[ins->a] = vm->regs[ins->a] ^ (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_NOT: vm->regs[ins->a] = ~vm->regs[ins->a]; break;
        case OP_SHL: vm->regs[ins->a] = vm->regs[ins->a] << (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_SHR: vm->regs[ins->a] = (int64_t)((uint64_t)vm->regs[ins->a] >> (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm)); break;
        case OP_PUSH:
            { int64_t v = is_reg_operand(ins->a) ? vm->regs[ins->a] : ins->imm;
              vstack_push_u64(&vm->operand, (uint64_t)v); break; }
        case OP_POP:
            { uint64_t v; if (vstack_pop_u64(&vm->operand, &v) == 0) vm->regs[ins->a] = (int64_t)v; break; }
        case OP_LOAD:
            { uint64_t addr = (uint64_t)vm->regs[ins->b];
              if (addr >= prog->data_size) { snprintf(vm->error_msg,sizeof(vm->error_msg),"LOAD out of bounds %llu at pc %u",(unsigned long long)addr,vm->pc); vm->error_count=1; return 1; }
              vm->regs[ins->a] = (int8_t)prog->data[addr]; break; }
        case OP_STORE:
            { uint64_t addr = (uint64_t)vm->regs[ins->a];
              if (addr >= prog->data_size) { snprintf(vm->error_msg,sizeof(vm->error_msg),"STORE out of bounds %llu at pc %u",(unsigned long long)addr,vm->pc); vm->error_count=1; return 1; }
              prog->data[addr] = (uint8_t)(vm->regs[ins->b] & 0xFF); break; }
        case OP_JMP: next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JZ:  if (vm->regs[ins->a] == 0) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JNZ: if (vm->regs[ins->a] != 0) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JE:  if (vm->regs[ins->a] == (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JNE: if (vm->regs[ins->a] != (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JG:  if (vm->regs[ins->a] >  (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JGE: if (vm->regs[ins->a] >= (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JL:  if (vm->regs[ins->a] <  (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JLE: if (vm->regs[ins->a] <= (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_CALL:
            { int fi = ins->a;
              if (fi < 0 || fi >= (int)prog->func_count) { snprintf(vm->error_msg,sizeof(vm->error_msg),"CALL bad func idx %d",fi); vm->error_count=1; return 1; }
              KillsFunc* fn = &prog->funcs[fi];
              /* 从操作数栈弹 nparams 个实参 (逆序压入参数寄存器 R0..) */
              for (int k = fn->nparams - 1; k >= 0; k--) { uint64_t a; if (vstack_pop_u64(&vm->operand, &a) == 0) vm->regs[k] = (int64_t)a; }
              /* 保存返回现场: 返回 PC + 64 寄存器 */
              vstack_push_u64(&vm->callstack, (uint64_t)next_pc);
              for (int r = 0; r < KILLS_NREG; r++) vstack_push_u64(&vm->callstack, (uint64_t)vm->regs[r]);
              next_pc = fn->pc;
              break; }
        case OP_RET:
            { /* 恢复现场但不覆盖 R0 (R0 约定为返回值寄存器) */
              for (int r = KILLS_NREG - 1; r >= 1; r--) { uint64_t v; if (vstack_pop_u64(&vm->callstack, &v) == 0) vm->regs[r] = (int64_t)v; }
              uint64_t r0; if (vstack_pop_u64(&vm->callstack, &r0) == 0) { /* 丢弃保存的 R0, 保留当前 R0=返回值 */ }
              uint64_t retpc; if (vstack_pop_u64(&vm->callstack, &retpc) == 0) next_pc = (uint32_t)retpc; break; }
        case OP_PRINT:
            { int append = (ins->imm == 1);
              if (ins->b >= 0) {
                  /* PRINT -1, const_idx : 打印常量池字符串 */
                  int ci = ins->b;
                  if (ci < (int)prog->const_count && prog->consts[ci].type == 1 && prog->consts[ci].sv) {
                      kvm_add_output_ex(vm, prog->consts[ci].sv, append);
                  } else {
                      char buf[64]; snprintf(buf, sizeof(buf), "<bad const %d>", ci); kvm_add_output_ex(vm, buf, append);
                  }
              } else {
                  int64_t v = vm->regs[ins->a];
                  if (v < 0) {
                      /* R_TMP < 0 = 常量索引标记 (值 -(ci+1)) */
                      int ci = (int)(-v - 1);
                      if (ci < (int)prog->const_count && prog->consts[ci].type == 1 && prog->consts[ci].sv) {
                          kvm_add_output_ex(vm, prog->consts[ci].sv, append);
                      } else {
                          char buf[64]; snprintf(buf, sizeof(buf), "<bad const %d>", ci); kvm_add_output_ex(vm, buf, append);
                      }
                  } else {
                      char buf[64]; snprintf(buf, sizeof(buf), "%lld", (long long)v); kvm_add_output_ex(vm, buf, append);
                  }
              }
              break; }
        case OP_FFI:
            kvm_ffi(vm, ins->a, ins->b >= 0 ? ins->b : 0);
            break;
        case OP_HALT:
            vm->halted = 1; break;
        default:
            snprintf(vm->error_msg, sizeof(vm->error_msg), "unknown opcode %u at pc %u", ins->op, vm->pc);
            vm->error_count = 1; return 1;
        }
        vm->pc = next_pc;
    }
    vm->halted = 1;
    return 0;
}

/* ================= 反汇编 (调试) ================= */
static const char* kvm_opname(uint8_t op) {
    switch (op) {
    case OP_NOP: return "NOP"; case OP_MOV: return "MOV";
    case OP_ADD: return "ADD"; case OP_SUB: return "SUB"; case OP_MUL: return "MUL";
    case OP_DIV: return "DIV"; case OP_MOD: return "MOD"; case OP_AND: return "AND";
    case OP_OR: return "OR"; case OP_XOR: return "XOR"; case OP_NOT: return "NOT";
    case OP_SHL: return "SHL"; case OP_SHR: return "SHR"; case OP_PUSH: return "PUSH";
    case OP_POP: return "POP"; case OP_LOAD: return "LOAD"; case OP_STORE: return "STORE";
    case OP_JMP: return "JMP"; case OP_JZ: return "JZ"; case OP_JNZ: return "JNZ";
    case OP_JE: return "JE"; case OP_JNE: return "JNE"; case OP_JG: return "JG";
    case OP_JGE: return "JGE"; case OP_JL: return "JL"; case OP_JLE: return "JLE";
    case OP_CALL: return "CALL"; case OP_RET: return "RET"; case OP_FFI: return "FFI";
    case OP_PRINT: return "PRINT"; case OP_HALT: return "HALT";
    default: return "?";
    }
}

void kvm_disassemble(const KillsProgram* p, char* buf, int buflen) {
    if (!p || !buf || buflen <= 0) return;
    int off = 0;
    off += snprintf(buf + off, buflen - off, "; Kills program: %u ins, %u consts, %u funcs, data=%u\n",
                    p->code_count, p->const_count, p->func_count, p->data_size);
    for (uint32_t i = 0; i < p->code_count && off < buflen - 2; i++) {
        KillsIns* ins = &p->code[i];
        off += snprintf(buf + off, buflen - off, "  %4u: %-4s a=%d b=%d imm=%lld\n",
                        i, kvm_opname(ins->op), ins->a, ins->b, (long long)ins->imm);
    }
}

/* ================= 序列化 ================= */
/* 布局: [KILLS(5)][ver u8][flags u8][nreg u32][data_size u32][code_count u32]
        [const_count u32][func_count u32]
        常量池: 每项 [type u8][iv i64] (str 另含 sv)  -- 简化: int/float 内联, str 存 sv
        数据段: data_size 字节
        指令表: 每项 [op u8][a i32][b i32][imm i64]
        函数表: 每项 [name C-string][pc u32][nparams u32] */
long kprog_serialize(const KillsProgram* p, uint8_t** out) {
    if (!p || !out) return -1;
    size_t cap = 512;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return -1;
    size_t len = 0;

#define PUT(d, n) do { if (len + (n) > cap) { cap = cap*2 + (n); buf = (uint8_t*)realloc(buf, cap); } memcpy(buf+len, (d), (n)); len += (n); } while(0)

    memcpy(buf, KILLS_MAGIC, 5); len = 5;
    uint8_t ver = KILLS_VERSION, flags = 0;
    PUT(&ver, 1); PUT(&flags, 1);
    uint32_t nreg = KILLS_NREG;
    PUT(&nreg, 4); PUT(&p->data_size, 4); PUT(&p->code_count, 4);
    PUT(&p->const_count, 4); PUT(&p->func_count, 4);

    /* constants */
    for (uint32_t i = 0; i < p->const_count; i++) {
        KillsConst* c = &p->consts[i];
        PUT(&c->type, 1);
        PUT(&c->iv, 8);
        if (c->type == 1) { /* str */
            uint32_t sl = c->sv ? (uint32_t)strlen(c->sv) : 0;
            PUT(&sl, 4); if (sl) PUT(c->sv, sl);
        } else {
            PUT(&c->fv, 8);
        }
    }
    /* data */
    if (p->data_size) PUT(p->data, p->data_size);
    /* code */
    for (uint32_t i = 0; i < p->code_count; i++) {
        KillsIns* ins = &p->code[i];
        PUT(&ins->op, 1); PUT(&ins->a, 4); PUT(&ins->b, 4); PUT(&ins->imm, 8);
    }
    /* funcs */
    for (uint32_t i = 0; i < p->func_count; i++) {
        KillsFunc* f = &p->funcs[i];
        uint32_t nl = f->name ? (uint32_t)strlen(f->name) : 0;
        PUT(&nl, 4); if (nl) PUT(f->name, nl);
        PUT(&f->pc, 4); PUT(&f->nparams, 4);
    }
#undef PUT
    *out = buf;
    return (long)len;
}

KillsProgram* kprog_deserialize(const uint8_t* buf, long len, char* err, int errlen) {
    if (!buf || len < 21) { if (err) snprintf(err, errlen, "bad header"); return NULL; }
    if (memcmp(buf, KILLS_MAGIC, 5) != 0) { if (err) snprintf(err, errlen, "bad magic"); return NULL; }
    long o = 5;
    uint8_t ver = buf[o++];
    uint8_t flags = buf[o++];
    (void)flags;
    if (ver != KILLS_VERSION) { if (err) snprintf(err, errlen, "version mismatch %d", ver); return NULL; }
    uint32_t nreg, dsz, ccode, cconst, cfunc;
    memcpy(&nreg, buf + o, 4); o += 4;
    memcpy(&dsz, buf + o, 4); o += 4;
    memcpy(&ccode, buf + o, 4); o += 4;
    memcpy(&cconst, buf + o, 4); o += 4;
    memcpy(&cfunc, buf + o, 4); o += 4;
    (void)nreg;
    KillsProgram* p = kprog_new();
    if (!p) { if (err) snprintf(err, errlen, "alloc fail"); return NULL; }
    p->data_size = dsz;

    /* constants */
    for (uint32_t i = 0; i < cconst; i++) {
        uint8_t type = buf[o++];
        int64_t iv; memcpy(&iv, buf + o, 8); o += 8;
        double fv = 0;
        char* sv = NULL;
        if (type == 1) { uint32_t sl; memcpy(&sl, buf + o, 4); o += 4; sv = (char*)malloc(sl + 1); if (sl) memcpy(sv, buf + o, sl); sv[sl] = 0; o += sl; }
        else { memcpy(&fv, buf + o, 8); o += 8; }
        kprog_add_const(p, type, iv, fv, sv);
        free(sv); sv = NULL;
    }
    /* data */
    if (dsz) { p->data = (uint8_t*)malloc(dsz); memcpy(p->data, buf + o, dsz); o += dsz; p->data_size = dsz; }
    /* code */
    for (uint32_t i = 0; i < ccode; i++) {
        uint8_t op = buf[o++];
        int32_t a, b; int64_t imm;
        memcpy(&a, buf + o, 4); o += 4;
        memcpy(&b, buf + o, 4); o += 4;
        memcpy(&imm, buf + o, 8); o += 8;
        kprog_add_ins(p, op, a, b, imm);
    }
    /* funcs */
    for (uint32_t i = 0; i < cfunc; i++) {
        uint32_t nl; memcpy(&nl, buf + o, 4); o += 4;
        char name[256] = ""; if (nl < sizeof(name)) { memcpy(name, buf + o, nl); name[nl] = 0; }
        o += nl;
        uint32_t pc, nparams; memcpy(&pc, buf + o, 4); o += 4; memcpy(&nparams, buf + o, 4); o += 4;
        kprog_add_func(p, name, pc, nparams);
    }
    return p;
}
