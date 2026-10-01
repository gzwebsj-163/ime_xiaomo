/*
 * xiaomo - .mo → Kills 字节码 编译器
 *
 * 作用: .mo 源码 → AST → KillsProgram(字节码) → kvm_run 执行
 * 打通解释器层(AST)与字节码内核层(Kills). 依赖 Kills 的 CALL/RET
 * 全量寄存器保存恢复实现帧隔离, 从而天然支持函数/递归.
 *
 * 寄存器约定:
 *   R0              返回值寄存器 (Kills 约定, RET 不覆盖)
 *   R1 .. R40       变量池 (全局 + 局部 + 形参暂存, 唯一递增分配)
 *   R_TMP=45        表达式结果
 *   R_AUX=46        BINOP / 比较 左操作数暂存
 *
 * 函数值传递约定:
 *   形参 i 在函数体中映射到暂存寄存器 P_i(池中唯一).
 *   函数入口先执行 MOV P_i, R_i (CALL 已把实参从操作数栈弹入 R0..).
 *   这样 R0 始终留给返回值, 形参在调用结束后仍稳定.
 *
 * 正确性依据: Kills 的 CALL 保存全部 64 寄存器、RET 恢复, 故无论递归
 * 多少层, 函数内对任何寄存器的改动在返回后都会还原. 因此全部变量
 * 共用同一寄存器池是安全的(靠帧隔离).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mo2kbc.h"

#define R_RET        0
#define R_VAR_BASE   1
#define R_VAR_MAX    56
#define R_TMP        57
#define R_AUX        58
/* 工作区(变量池外, KILLS_NREG=96): 数组打印循环变量 + print 两遍法暂存 */
#define R_LOOP_BASE  59
#define R_OUT_BASE   64
/* 全局变量区: 位于 VM CALL/RET 快照边界(KILLS_SNAP_NREG=72)之外,
 * 函数内写全局在 RET 后保留(穿透); 局部/参数仍在快照区内 → 递归安全。
 * 局部变量池 R1..56 与全局区 72..95 物理隔离, 互不干扰。 */
#define R_GLOBAL_BASE 72
/* 设备签名寄存器 R127 (KVM_REG_SIG) 为 OEM 熔丝固件区:
 * 全局变量分配上限封顶 126, 编译器永不触碰 R127 —— 永不变化保证之一。 */
#define R_GLOBAL_MAX  126

/* ---------- 符号表 ---------- */
typedef struct { char* name; int reg; int is_array; } SymEntry;
typedef struct { SymEntry* items; int count, cap; } SymTab;

static int sym_lookup(SymTab* t, const char* name) {
    for (int i = 0; i < t->count; i++)
        if (strcmp(t->items[i].name, name) == 0) return t->items[i].reg;
    return -1;
}
static int sym_is_array(SymTab* t, const char* name) {
    for (int i = 0; i < t->count; i++)
        if (strcmp(t->items[i].name, name) == 0) return t->items[i].is_array;
    return 0;
}
static void sym_add(SymTab* t, const char* name, int reg, int is_array) {
    if (t->count >= t->cap) { t->cap = t->cap ? t->cap * 2 : 16; t->items = (SymEntry*)realloc(t->items, t->cap * sizeof(SymEntry)); }
    t->items[t->count].name = strdup(name);
    t->items[t->count].reg = reg;
    t->items[t->count].is_array = is_array;
    t->count++;
}
static void sym_free(SymTab* t) {
    for (int i = 0; i < t->count; i++) free(t->items[i].name);
    free(t->items); memset(t, 0, sizeof(*t));
}

/* ---------- 编译期函数表 ---------- */
typedef struct { char* name; int index; int entry_pc; int nparams; int ret_float; } CgFunc;
typedef struct { CgFunc* items; int count, cap; } CgFuncTab;
static CgFunc* cgfunc_find(CgFuncTab* t, const char* name) {
    for (int i = 0; i < t->count; i++) if (strcmp(t->items[i].name, name) == 0) return &t->items[i];
    return NULL;
}
static CgFunc* cgfunc_add(CgFuncTab* t, const char* name) {
    if (t->count >= t->cap) { t->cap = t->cap ? t->cap * 2 : 16; t->items = (CgFunc*)realloc(t->items, t->cap * sizeof(CgFunc)); }
    CgFunc* f = &t->items[t->count++];
    f->name = strdup(name); f->index = t->count - 1; f->entry_pc = -1; f->nparams = 0;
    f->ret_float = 0;   /* 必须显式清零: realloc 槽位是脏堆内存, 残留值会把 CALL 结果
                           随机标成浮点 → print 走 %g 打裸整数 → 反常数乱码
                           (长驻进程如 xdebugd 堆复用后 ~50% 概率复现) */
    return f;
}

/* ---------- Label (跳转回填) ---------- */
typedef struct { int target_pc; int* pending; int np, cap; } Label;
typedef struct { Label* items; int count, cap; } LabelTab;
static int label_new(LabelTab* t) {
    if (t->count >= t->cap) { t->cap = t->cap ? t->cap * 2 : 16; t->items = (Label*)realloc(t->items, t->cap * sizeof(Label)); }
    Label* L = &t->items[t->count++];
    memset(L, 0, sizeof(*L)); L->target_pc = -1;
    return t->count - 1;
}

/* ---------- 编译器上下文 ---------- */
typedef struct {
    KillsProgram* p;
    SymTab globals;
    SymTab locals;
    int next_reg;
    int next_global;   /* 全局变量区分配游标 (R72..95) */
    CgFuncTab funcs;
    LabelTab labels;
    int cur_func_index;   /* -1 = 全局区(main) */
    int error;
    AstNode* cur_node;    /* 调试: 当前正在编译的节点 (fail 时打印定位) */
    int break_lbls[64];   /* break 标签栈: WHILE/FOR 压入 l_end */
    int break_depth;
    /* 字符串变量名集合(编译期类型追踪): 字符串变量寄存器里存负常量标记
       -(ci+1), 打印须走非 raw 分支还原常量串; 整数变量用 raw 打印允许负值。 */
    char** strvars;
    int strvar_count, strvar_cap;
    /* 浮点变量名集合(2026-09-07 浮点扩展): 浮点变量寄存器存 double 位模式。
       对齐解释器语义: / 除法恒浮点、浮点字面量走 fval、print 浮点用 %g。 */
    char** fvars;
    int fvar_count, fvar_cap;
    /* 浮点数组名集合: 数组字面量含浮点元素 → 打印数组时元素按 %g */
    char** farrs;
    int farr_count, farr_cap;
    /* 字符串数组名集合: 数组字面量含字符串元素 → 元素是负常量标记, 打印须还原 */
    char** sarrs;
    int sarr_count, sarr_cap;
    /* 最近一个表达式的编译期类型: 0=整数 1=浮点 2=字符串 (print/赋值/比较用) */
    int last_ty;
} Cg;

/* 通用名字集合: 标记/查询 (strvar/fvar/farr/sarr 共用逻辑) */
static void cg_mark_name(char*** arr, int* count, int* cap, const char* name) {
    if (!name || !name[0]) return;
    for (int i = 0; i < *count; i++) if (strcmp((*arr)[i], name) == 0) return;
    if (*count >= *cap) { *cap = *cap ? *cap * 2 : 8; *arr = (char**)realloc(*arr, *cap * sizeof(char*)); }
    (*arr)[(*count)++] = strdup(name);
}
static int cg_has_name(char** arr, int count, const char* name) {
    if (!name) return 0;
    for (int i = 0; i < count; i++) if (strcmp(arr[i], name) == 0) return 1;
    return 0;
}

static int cg_is_strvar(Cg* cg, const char* name) {
    if (!name) return 0;
    for (int i = 0; i < cg->strvar_count; i++)
        if (strcmp(cg->strvars[i], name) == 0) return 1;
    return 0;
}

static void fail(Cg* cg) {
    if (!cg->error && cg->cur_node) {
        const char* tn = "?";
        switch (cg->cur_node->type) {
        case NODE_LITERAL: tn = "LITERAL"; break;
        case NODE_TEMPLATE_REF: tn = "TEMPLATE_REF"; break;
        case NODE_EXPR: tn = "EXPR"; break;
        case NODE_CALL: tn = "CALL"; break;
        case NODE_BINOP: tn = "BINOP"; break;
        case NODE_INDEX_ACCESS: tn = "INDEX_ACCESS"; break;
        case NODE_VAR_DECL: tn = "VAR_DECL"; break;
        case NODE_CONST_DECL: tn = "CONST_DECL"; break;
        case NODE_ASSIGN: tn = "ASSIGN"; break;
        case NODE_PRINT: tn = "PRINT"; break;
        case NODE_RETURN: tn = "RETURN"; break;
        case NODE_WHILE: tn = "WHILE"; break;
        case NODE_IF: tn = "IF"; break;
        case NODE_BLOCK: tn = "BLOCK"; break;
        case NODE_FN_DECL: tn = "FN_DECL"; break;
        case NODE_ARRAY_LIT: tn = "ARRAY_LIT"; break;
        case NODE_PROGRAM: tn = "PROGRAM"; break;
        default: tn = "UNKNOWN"; break;
        }
        fprintf(stderr, "[mo2kbc] FAIL @ node=%s(type=%d) text=%s inst=%d\n",
                tn, cg->cur_node->type, cg->cur_node->text ? cg->cur_node->text : "(null)", cg->cur_node->inst);
    }
    cg->error = 1;
}

/* ---------- 指令发射 ---------- */
static void emit(Cg* cg, uint8_t op, int32_t a, int32_t b, int64_t imm) {
    kprog_add_ins(cg->p, op, a, b, imm);
}
static void emit_jmp_lbl(Cg* cg, uint8_t op, int32_t a, int32_t b, int lbl) {
    int idx = cg->p->code_count;
    kprog_add_ins(cg->p, op, a, b, 0);
    Label* L = &cg->labels.items[lbl];
    if (L->target_pc >= 0) {
        cg->p->code[idx].imm = L->target_pc - idx;
    } else {
        if (L->np >= L->cap) { L->cap = L->cap ? L->cap * 2 : 8; L->pending = (int*)realloc(L->pending, L->cap * sizeof(int)); }
        L->pending[L->np++] = idx;
    }
}
static void place_label(Cg* cg, int lbl) {
    Label* L = &cg->labels.items[lbl];
    L->target_pc = cg->p->code_count;
    if (L->pending) {
        for (int i = 0; i < L->np; i++) cg->p->code[L->pending[i]].imm = L->target_pc - L->pending[i];
        free(L->pending); L->pending = NULL; L->np = L->cap = 0;
    }
}

/* ---------- 变量 ---------- */
static int alloc_reg(Cg* cg) {
    /* 局部变量池 R1..56 (CALL/RET 快照区内, 每层调用现场隔离) */
    int r = cg->next_reg++;
    if (cg->next_reg > R_VAR_MAX) {
        /* 变量池超限: 显式报错而非静默回卷 (回卷会导致数组/标量共用寄存器互踩) */
        fprintf(stderr, "[mo2kbc] 变量池超限: next_reg=%d > R_VAR_MAX=%d\n", cg->next_reg, R_VAR_MAX);
        cg->next_reg = R_VAR_BASE;
        cg->error = 1;
    }
    return r;
}
static int alloc_global_reg(Cg* cg) {
    /* 全局变量区 R72..95 (快照区外, 函数内写入穿透) */
    int r = cg->next_global++;
    if (r > R_GLOBAL_MAX) {
        fprintf(stderr, "[mo2kbc] 全局变量区超限: next_global=%d > %d\n", r, R_GLOBAL_MAX);
        cg->error = 1;
    }
    return r;
}
static int lookup_var(Cg* cg, const char* name) {
    int r = sym_lookup(&cg->locals, name);
    if (r >= 0) return r;
    return sym_lookup(&cg->globals, name);
}
static int var_is_array(Cg* cg, const char* name) {
    if (sym_is_array(&cg->locals, name)) return 1;
    return sym_is_array(&cg->globals, name);
}
static int declare_var(Cg* cg, const char* name) {
    /* 重声明复用: .mo 语义里 void 重声明 = "确保存在 + 重新赋值"。
       函数内 void g(:int)=... 重声明全局 → 必须复用全局寄存器, 否则拿到
       全新局部副本, 全局永不更新 (probe4 g 差1 / probe3 计数错乱 根因)。 */
    int r = sym_lookup(&cg->locals, name);
    if (r >= 0) return r;
    r = sym_lookup(&cg->globals, name);
    if (r >= 0) {
        if (cg->cur_func_index >= 0) sym_add(&cg->locals, name, r, 0);
        return r;
    }
    if (cg->cur_func_index >= 0) { r = alloc_reg(cg);      sym_add(&cg->locals, name, r, 0); }
    else                         { r = alloc_global_reg(cg); sym_add(&cg->globals, name, r, 0); }
    return r;
}
static int declare_array_var(Cg* cg, const char* name) {
    /* 同 declare_var: 数组重声明也复用 (保留 is_array 标记) */
    if (sym_is_array(&cg->locals, name)) return sym_lookup(&cg->locals, name);
    int r = sym_lookup(&cg->locals, name);
    if (r >= 0) { /* 同名标量已存在: 复用寄存器并升级为数组标记 */ 
        if (cg->cur_func_index >= 0) sym_add(&cg->locals, name, r, 1);
        return r;
    }
    if (sym_is_array(&cg->globals, name)) {
        r = sym_lookup(&cg->globals, name);
        if (cg->cur_func_index >= 0) sym_add(&cg->locals, name, r, 1);
        return r;
    }
    r = sym_lookup(&cg->globals, name);
    if (r >= 0) { /* 全局同名标量 → 升级 */ sym_add(&cg->locals, name, r, 1); return r; }
    if (cg->cur_func_index >= 0) { r = alloc_reg(cg);      sym_add(&cg->locals, name, r, 1); }
    else                         { r = alloc_global_reg(cg); sym_add(&cg->globals, name, r, 1); }
    return r;
}

/* ---------- 表达式 ---------- */
static void cg_expr(Cg* cg, AstNode* n);

/* ---------- 内置: 嵌入式 Linux 内核核心 (阶段六) ----------
 * .mo 里直接调用 linux_init / linux_exec / linux_read / linux_end,
 * 编译成 KVM 的 LINUX_* opcode, 让 .mo 源码直驱真 Linux 内核。
 * 参数约定: 字符串参数必须是字面量(入常量池); 句柄参数可以是任意表达式。
 */
static int linux_str_const(Cg* cg, AstNode* arg) {
    /* 注意: 空字符串字面量 "" 的 text 为 NULL (ast_set_text_len len=0 置 NULL),
     * 不能只判 arg->text, 要按 mtype 认 TYPE_STR, NULL 视为空串。 */
    if (arg && arg->type == NODE_LITERAL && arg->mtype == TYPE_STR)
        return kprog_add_const(cg->p, 1, 0, 0.0, arg->text ? arg->text : "");
    return -1;
}
static void cg_linux(Cg* cg, AstNode* n) {
    const char* name = n->text;
    AstNode** a = n->args.items;
    int cnt = n->args.count;
    if (cnt < 1) { fail(cg); return; }
    if (strcmp(name, "linux_init") == 0) {
        /* linux_init(cfg_path) → R_TMP = handle */
        int ci = linux_str_const(cg, a[0]);
        if (ci < 0) { fail(cg); return; }
        emit(cg, OP_LINUX_INIT, R_TMP, ci, 0);
    } else if (strcmp(name, "linux_exec") == 0) {
        /* linux_exec(handle, cmd, [expect]) → R_TMP = 0 成功 / 1 超时 / -1 错误 */
        cg_expr(cg, a[0]);                    /* handle → R_TMP */
        if (cnt < 2) { fail(cg); return; }
        int ci_cmd = linux_str_const(cg, a[1]);
        if (ci_cmd < 0) { fail(cg); return; }
        int ci_exp = (cnt >= 3) ? linux_str_const(cg, a[2]) : -1;
        emit(cg, OP_LINUX_EXEC, R_TMP, ci_cmd, ci_exp);
    } else if (strcmp(name, "linux_read") == 0) {
        /* linux_read(handle) → R_TMP=输出在 data 段地址, R_AUX=行数; 内容追加到 KVM 输出 */
        cg_expr(cg, a[0]);
        emit(cg, OP_LINUX_READ, R_TMP, R_AUX, 0);
    } else if (strcmp(name, "linux_end") == 0) {
        /* linux_end(handle) */
        cg_expr(cg, a[0]);
        emit(cg, OP_LINUX_END, R_TMP, 0, 0);
        emit(cg, OP_MOV, R_TMP, -1, 0);       /* 无返回值 */
    } else {
        fail(cg);
    }
}

/* double 位模式 ↔ int64 透传 */
static int64_t fbits(double d) { int64_t v; memcpy(&v, &d, 8); return v; }

/* 数组字面量 → data 段 [len:8][e0:8]... 返回起始地址。
 * VAR_DECL 初始化与 RETURN 数组字面量共用。同时按元素类型把 arrname
 * 标进 sarrs(字符串数组)/farrs(浮点数组) 供打印/下标访问类型追踪。 */
static uint32_t emit_array_literal(Cg* cg, AstNode* val, const char* arrname) {
    int has_str = 0, has_float = 0;
    int count = val->args.count;
    uint32_t addr = kprog_alloc_data(cg->p, (uint32_t)((count + 1) * 8));
    /* 存长度 */
    emit(cg, OP_MOV, R_TMP, -1, (int64_t)addr);
    emit(cg, OP_MOV, R_AUX, -1, (int64_t)count);
    emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
    /* 存元素 */
    for (int i = 0; i < count; i++) {
        AstNode* e = val->args.items[i];
        if (e->type == NODE_LITERAL && e->mtype != TYPE_STR) {
            int64_t v = (e->mtype == TYPE_FLOAT) ? fbits(e->fval) : e->ival;
            if (e->mtype == TYPE_FLOAT) has_float = 1;
            emit(cg, OP_MOV, R_TMP, -1, (int64_t)(addr + (i + 1) * 8));
            emit(cg, OP_MOV, R_AUX, -1, v);
            emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
        } else if (e->type == NODE_ARRAY_LIT) {
            /* 嵌套数组: 递归分配子数组, 存子数组地址 */
            int sub_count = e->args.count;
            uint32_t sub_addr = kprog_alloc_data(cg->p, (uint32_t)((sub_count + 1) * 8));
            /* 子数组长度 */
            emit(cg, OP_MOV, R_TMP, -1, (int64_t)sub_addr);
            emit(cg, OP_MOV, R_AUX, -1, (int64_t)sub_count);
            emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
            /* 子数组元素 */
            for (int j = 0; j < sub_count; j++) {
                AstNode* se = e->args.items[j];
                if (se->type == NODE_LITERAL && se->mtype != TYPE_STR) {
                    int64_t v = (se->mtype == TYPE_FLOAT) ? fbits(se->fval) : se->ival;
                    emit(cg, OP_MOV, R_TMP, -1, (int64_t)(sub_addr + (j + 1) * 8));
                    emit(cg, OP_MOV, R_AUX, -1, v);
                    emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
                } else { fail(cg); break; }
            }
            if (cg->error) break;
            /* 外层数组存子数组地址 */
            emit(cg, OP_MOV, R_TMP, -1, (int64_t)(addr + (i + 1) * 8));
            emit(cg, OP_MOV, R_AUX, -1, (int64_t)sub_addr);
            emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
        } else {
            /* 表达式元素: 字符串字面量/模板引用(strvar 负标记)/函数调用等 */
            cg_expr(cg, e);
            if (cg->last_ty == 1) has_float = 1;
            else if (cg->last_ty == 2) has_str = 1;
            else if (e->type == NODE_TEMPLATE_REF && e->text && cg_is_strvar(cg, e->text)) has_str = 1;
            emit(cg, OP_MOV, R_AUX, R_TMP, 0);
            emit(cg, OP_MOV, R_TMP, -1, (int64_t)(addr + (i + 1) * 8));
            emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
        }
    }
    if (arrname && arrname[0] && !cg->error) {
        if (has_str) cg_mark_name(&cg->sarrs, &cg->sarr_count, &cg->sarr_cap, arrname);
        if (has_float) cg_mark_name(&cg->farrs, &cg->farr_count, &cg->farr_cap, arrname);
    }
    return addr;
}

static void cg_binop(Cg* cg, AstNode* n) {
    /* 一元运算: parser 把 -x / !x 解析为 BinOp{op, left=operand, right=NULL} */
    if (!n->right) {
        cg_expr(cg, n->left);
        switch ((TokenType)n->inst) {
        case TOK_MINUS:
            if (cg->last_ty == 1) {
                /* 浮点取负: x * (-1.0) — -1.0 以 double 位模式透传 */
                emit(cg, OP_MOV, R_AUX, -1, fbits(-1.0));
                emit(cg, OP_FMUL, R_TMP, R_AUX, 0);
                cg->last_ty = 1;
            } else {
                /* -x = 0 - x */
                emit(cg, OP_MOV, R_AUX, -1, 0);
                emit(cg, OP_SUB, R_AUX, R_TMP, 0);
                cg->last_ty = 0;
            }
            break;
        case TOK_NOT:
            emit(cg, OP_NOT, R_TMP, R_TMP, 0);   /* ~x (kvm 位取反) */
            emit(cg, OP_MOV, R_AUX, R_TMP, 0);
            cg->last_ty = 0;
            break;
        default: fail(cg); return;
        }
        emit(cg, OP_MOV, R_TMP, R_AUX, 0);
        return;
    }
    cg_expr(cg, n->left);
    int lt = cg->last_ty;                 /* 左操作数类型 */
    emit(cg, OP_PUSH, R_TMP, -1, 0);
    cg_expr(cg, n->right);
    int rt = cg->last_ty;                 /* 右操作数类型 */
    emit(cg, OP_POP, R_AUX, -1, 0);            /* AUX=left, TMP=right */
    cg->last_ty = lt;                     /* 先恢复左类型, 下面对 busted 分支再修正 */
    switch ((TokenType)n->inst) {
    case TOK_PLUS:
    case TOK_MINUS:
    case TOK_STAR: {
        /* 解释器语义: 双 int → int; 任一 float → double 运算 */
        int isf = (lt == 1 || rt == 1);
        if (isf) {
            if (lt != 1) emit(cg, OP_I2F, R_AUX, -1, 0);
            if (rt != 1) emit(cg, OP_I2F, R_TMP, -1, 0);
            emit(cg, (TokenType)n->inst == TOK_PLUS ? OP_FADD :
                     (TokenType)n->inst == TOK_MINUS ? OP_FSUB : OP_FMUL,
                     R_AUX, R_TMP, 0);
            cg->last_ty = 1;
        } else {
            emit(cg, (TokenType)n->inst == TOK_PLUS ? OP_ADD :
                     (TokenType)n->inst == TOK_MINUS ? OP_SUB : OP_MUL,
                     R_AUX, R_TMP, 0);
            cg->last_ty = 0;
        }
        break;
    }
    case TOK_SLASH:
        /* 解释器: 除法无条件浮点 val_float(lf/rf) (7/2→3.5, 330/100→3.3) */
        if (lt != 1) emit(cg, OP_I2F, R_AUX, -1, 0);
        if (rt != 1) emit(cg, OP_I2F, R_TMP, -1, 0);
        emit(cg, OP_FDIV, R_AUX, R_TMP, 0);
        cg->last_ty = 1;
        break;
    case TOK_PERCENT:
        /* 解释器: 取余恒整数 val_int(l.ival % r.ival) */
        emit(cg, OP_MOD, R_AUX, R_TMP, 0);
        cg->last_ty = 0;
        break;
    default: fail(cg); return;
    }
    emit(cg, OP_MOV, R_TMP, R_AUX, 0);
}
static void cg_expr(Cg* cg, AstNode* n) {
    if (cg->error || !n) return;
    cg->cur_node = n;
    switch (n->type) {
    case NODE_LITERAL:
        if (n->mtype == TYPE_STR && n->text) {
            /* 字符串字面量: 入常量池, R_TMP 存常量索引(负数标记) */
            int ci = kprog_add_const(cg->p, 1, 0, 0.0, n->text);
            emit(cg, OP_MOV, R_TMP, -1, -(int64_t)ci - 1);  /* 负值 = 常量索引标记 */
            cg->last_ty = 2;
        } else if (n->mtype == TYPE_FLOAT) {
            /* 浮点字面量: double 位模式透传 (parser 真值在 fval, ival 是截断值) */
            emit(cg, OP_MOV, R_TMP, -1, fbits(n->fval));
            cg->last_ty = 1;
        } else {
            emit(cg, OP_MOV, R_TMP, -1, n->ival);
            cg->last_ty = 0;
        }
        break;
    case NODE_TEMPLATE_REF:
    case NODE_EXPR: {
        if (n->text && n->text[0]) {
            int r = lookup_var(cg, n->text);
            if (r >= 0) {
                emit(cg, OP_MOV, R_TMP, r, 0);
                /* 编译期类型恢复: 打印/比较/赋值传播按此判定 */
                if (var_is_array(cg, n->text)) cg->last_ty = 3;   /* 数组变量: 寄存器=地址 */
                else if (cg_is_strvar(cg, n->text)) cg->last_ty = 2;
                else if (cg_has_name(cg->fvars, cg->fvar_count, n->text)) cg->last_ty = 1;
                else cg->last_ty = 0;
            }
            else fail(cg);
        } else fail(cg);
        break;
    }
    case NODE_CALL: {
        /* 内置: 嵌入式 Linux 内核核心 (阶段六) — linux_* 直驱真内核 */
        if (n->text && (strcmp(n->text, "linux_init") == 0 ||
                        strcmp(n->text, "linux_exec") == 0 ||
                        strcmp(n->text, "linux_read") == 0 ||
                        strcmp(n->text, "linux_end") == 0)) {
            cg_linux(cg, n);
            break;
        }
        /* 内置: 交互输入桥 (openclaw 交互系统) — input_pending / input_read
         *   input_pending() → FFI 3, R_TMP = 0/1
         *   input_read()    → FFI 4, R_TMP = 分类 cid */
        if (n->text && strcmp(n->text, "input_pending") == 0) {
            emit(cg, OP_FFI, 3, R_TMP, 0);
            break;
        }
        if (n->text && strcmp(n->text, "input_read") == 0) {
            emit(cg, OP_FFI, 4, R_TMP, 0);
            break;
        }
        if (n->text && strcmp(n->text, "input_wait") == 0) {
            emit(cg, OP_FFI, 5, R_TMP, 0);
            break;
        }
        /* 内置: kbc 自动推理特征通道 (2026-09-13) — feat(i)
         *   feat(i) → FFI 7, R_TMP = 第 i 维特征 (Q16 presence 0/65536)
         *   语义: 最近一次消费的输入行 → 宿主提取 64 维哈希特征;
         *   实参先求值进 R_TMP, 结果回 R_TMP; VM 侧 imm-1 = 实参寄存器。 */
        if (n->text && strcmp(n->text, "feat") == 0) {
            if (n->args.count < 1) { fail(cg); break; }
            cg_expr(cg, n->args.items[0]);
            emit(cg, OP_FFI, 7, R_TMP, R_TMP + 1);
            break;
        }
        /* 内置: 真实 LLM 回复 (ESP32 版) — llm_query()
         *   llm_query() → FFI 6, R_TMP = 1 成功 / 0 失败
         *   FFI 内部: ESP32 作为 TCP client 连 Mac 网关(:9101) →
         *   SiliconFlow → 回复文本 append 到 VM output 同前行。
         *   .mo 用法: 先 print 前缀开新行, 再调 llm_query() 追加回复。 */
        if (n->text && strcmp(n->text, "llm_query") == 0) {
            emit(cg, OP_FFI, 6, R_TMP, 0);
            break;
        }
        /* 内置: hw_dev 命令分发 (2026-09-07) — hw_dev("cmd") / hw_dev("cmd", 数值)
         *   hw_dev(s)      → OP_HW_DEV_CALL R_TMP, ci, 0
         *   hw_dev(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 0x00 命中执行 / 255 (0xFF) 未命中 / 其他 HWDEV_R_*
         *   .mo 用法: void rc : int = hw_dev("echo\n\r hello")
         *             void p2 : int = hw_dev("pwm\n\r duty 0 0 20000000 ", 1500000)
         *   ⚠️ .mo 字符串不转义: 命令名带 "\n\r " 尾缀时须内嵌真实控制字节
         *   (见 examples/hwdev_test.mo, 生成时用 printf 注入)。 */
        if (n->text && strcmp(n->text, "hw_dev") == 0) {
            int ci_hw = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_hw < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_DEV_CALL, R_TMP, ci_hw, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_DEV_CALL, R_TMP, ci_hw, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        /* 内置: TFT 模组故障诊断 (2026-09-24) — hw_fault("cmd") / hw_fault("cmd", 数值)
         *   hw_fault(s)      → OP_HW_FAULT_CALL R_TMP, ci, 0
         *   hw_fault(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 诊断结果: scan=故障数 (0=健康) / pin N=故障码 /
         *           inject 0=成功 / -1 回绕 255=未识别或参数非法
         *   .mo 用法: void f1 : int = hw_fault("scan")
         *             void f2 : int = hw_fault("pin ", 7)   ← 数值动态拼接 */
        if (n->text && strcmp(n->text, "hw_fault") == 0) {
            int ci_fl = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_fl < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_FAULT_CALL, R_TMP, ci_fl, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_FAULT_CALL, R_TMP, ci_fl, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        /* 内置: 内核 DNA 编码层 (2026-09-28) — hw_core("cmd") / hw_core("cmd", 数值)
         *   hw_core(s)      → OP_HW_CORE_CALL R_TMP, ci, 0
         *   hw_core(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 结果: count=10 / ok=1 / idx N=DNA编码 / slot,free=0 /
         *           -1 回绕 255=未识别或参数非法
         *   .mo 用法: void c1 : int = hw_core("count")
         *             void c2 : int = hw_core("idx ", 3)   ← 数值动态拼接 */
        if (n->text && strcmp(n->text, "hw_core") == 0) {
            int ci_co = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_co < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_CORE_CALL, R_TMP, ci_co, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_CORE_CALL, R_TMP, ci_co, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        /* 内置: hw 家族总调度 (hw_main, 2026-09-28) — hw_main("cmd") / hw_main("cmd", 数值)
         *   hw_main(s)      → OP_HW_MAIN_CALL R_TMP, ci, 0
         *   hw_main(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 结果: count=8 / sum=1604573786(黄金) / ok=1 /
         *           idx N=类ID / find ID=表序 / probe ID=探针 / -1 回绕 255=未识别
         *   .mo 用法: void c1 : int = hw_main("count")
         *             void f1 : int = hw_main("probe ", 152)   ← 数值动态拼接 */
        if (n->text && strcmp(n->text, "hw_main") == 0) {
            int ci_mn = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_mn < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_MAIN_CALL, R_TMP, ci_mn, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_MAIN_CALL, R_TMP, ci_mn, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        /* 内置: 无线调试器信号层 (hw_wdbg, 2026-09-29) — hw_wdbg("cmd") / hw_wdbg("cmd", 数值)
         *   hw_wdbg(s)      → OP_HW_WDBG_CALL R_TMP, ci, 0
         *   hw_wdbg(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 结果码: 0=OK / 1=NOARGS / 2=BADARG / 3=IOERR / 4=STATE /
         *           -1 回绕 255=未识别 / -2 回绕 254=help
         *   .mo 用法: void r1 : int = hw_wdbg("bridge open 115200")
         *             void r2 : int = hw_wdbg("pwm out ", 1000)   ← 数值动态拼接 */
        if (n->text && strcmp(n->text, "hw_wdbg") == 0) {
            int ci_wd = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_wd < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_WDBG_CALL, R_TMP, ci_wd, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_WDBG_CALL, R_TMP, ci_wd, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        /* 内置: ESP32 ROM 下载协议烧录层 (hw_flash, 2026-09-30) —
         *       hw_flash("cmd") / hw_flash("cmd", 数值)
         *   hw_flash(s)      → OP_HW_FLASH_CALL R_TMP, ci, 0
         *   hw_flash(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 结果码: 0=OK / 1=NOARGS / 2=BADARG / 3=IOERR / 4=PROTO /
         *           5=CHECKSUM / 6=BADSIZE / -1 回绕 255=未识别 / -2 回绕 254=help
         *   .mo 用法: void r1 : int = hw_flash("run 4096")
         *             void r2 : int = hw_flash("run ", 8192)   ← 数值动态拼接 */
        if (n->text && strcmp(n->text, "hw_flash") == 0) {
            int ci_fl = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_fl < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_FLASH_CALL, R_TMP, ci_fl, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_FLASH_CALL, R_TMP, ci_fl, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        /* 内置: 引脚档案/双模驱动/编程电压层 (hw_pin, 2026-09-30) —
         *       hw_pin("cmd") / hw_pin("cmd", 数值)
         *   hw_pin(s)      → OP_HW_PIN_CALL R_TMP, ci, 0
         *   hw_pin(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 结果码: 0=OK / 1=NOARGS / 2=BADARG / 3=IOERR / 4=NOFLASH /
         *           5=VERIFY / 6=NODEV / 7=RANGE / -1 回绕 255=未识别 / -2 回绕 254=help
         *   .mo 用法: void r1 : int = hw_pin("id")
         *             void r2 : int = hw_pin("vpp ", 2)   ← 数值动态拼接 */
        if (n->text && strcmp(n->text, "hw_pin") == 0) {
            int ci_pn = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_pn < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_PIN_CALL, R_TMP, ci_pn, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_PIN_CALL, R_TMP, ci_pn, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        /* 内置: DC 电源信号层 (hw_dc, 2026-10-01) — hw_dc("cmd") / hw_dc("cmd", 数值)
         *   hw_dc(s)      → OP_HW_DC_CALL R_TMP, ci, 0
         *   hw_dc(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 结果: count=8 / datacount=7 / ok=1 / sig N=读数 /
         *           base N=参考预值 / range N=1|0 / data N=组合帧 /
         *           -1 回绕 255=未识别 / -2 回绕 254=help
         *   .mo 用法: void c1 : int = hw_dc("count")
         *             void c2 : int = hw_dc("set ", 42)   ← 数值动态拼接 */
        if (n->text && strcmp(n->text, "hw_dc") == 0) {
            int ci_dc = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_dc < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_DC_CALL, R_TMP, ci_dc, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_DC_CALL, R_TMP, ci_dc, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        /* 内置: DMC 设备管理层 (hw_dmc, 2026-10-01) — hw_dmc("cmd") / hw_dmc("cmd", 数值)
         *   hw_dmc(s)      → OP_HW_DMC_CALL R_TMP, ci, 0
         *   hw_dmc(s, num) → 数值先求值进 R_TMP, imm=R_TMP+1 编码
         *     (vm 侧 imm-1 还原寄存器, 十进制追加到命令串尾 → 动态参数)
         *   R_TMP = 结果: count=9 / states=7 / errs=8 / golden=0x169A603E /
         *           ok=1 / crcvec=1 / frame=258 / hello=1 /
         *           -1 未识别 / -2 help
         *   .mo 用法: void c1 : int = hw_dmc("count")
         *             void c2 : int = hw_dmc("send ", 42)  ← 数值动态拼接 */
        if (n->text && strcmp(n->text, "hw_dmc") == 0) {
            int ci_dmc = (n->args.count >= 1)
                ? linux_str_const(cg, n->args.items[0]) : -1;
            if (ci_dmc < 0) { fail(cg); break; }
            if (n->args.count >= 2) {
                cg_expr(cg, n->args.items[1]);               /* 数值 → R_TMP */
                emit(cg, OP_HW_DMC_CALL, R_TMP, ci_dmc, (int64_t)R_TMP + 1);
            } else {
                emit(cg, OP_HW_DMC_CALL, R_TMP, ci_dmc, 0);
            }
            cg->last_ty = 0;   /* 返回 int rc, 归零防 last_ty 残留污染类型回标 */
            break;
        }
        CgFunc* f = (n->text) ? cgfunc_find(&cg->funcs, n->text) : NULL;
        if (!f) { fail(cg); break; }
        cg->last_ty = 0;   /* 内置/默认返回整数 */
        /* 正序压参: 先压 arg[0](栈底), 最后压 arg[N-1](栈顶)。
           kvm OP_CALL 从 k=nparams-1 向 0 逆序弹参:
             先弹栈顶 arg[N-1] → regs[N-1], 最后弹 arg[0] → regs[0]。
           故 regs[i]=arg[i], 配合 cg_func 的 MOV P_i,R_i 正好对齐。
           ⚠️ 历史 bug: 原实现逆序压参 + kvm 逆序弹参 = 双重逆序 → 参数颠倒,
           单参函数不触发, 双参以上 (session_ap/agent_loop) 全错。 */
        for (int i = 0; i < n->args.count; i++) {
            cg_expr(cg, n->args.items[i]);
            emit(cg, OP_PUSH, R_TMP, -1, 0);
        }
        emit(cg, OP_CALL, f->index, 0, 0);
        emit(cg, OP_MOV, R_TMP, R_RET, 0);
        /* last_ty 必须显式归零: CALL 本身不产生确定的编译期类型,
         * 残留值(如上一条语句的 str=2)会让 VarDecl 的类型回标走错分支
         * (probe_cap T3: msg 误标 strvars 而非 sarrs → ${msg}[0] 按整数
         *  打印出字符串标记 -2)。函数返回默认按 int; 浮点函数下方覆盖。 */
        cg->last_ty = 0;
        if (f->ret_float) cg->last_ty = 1;   /* 函数返回类型 (cg_func 捕获) */
        break;
    }
    case NODE_BINOP:
        cg_binop(cg, n); break;
    case NODE_INDEX_ACCESS: {
        /* arr[index]: 算地址 = arr_base + 8 + index*8, 然后 LOAD64
           支持链式: grid[1][1] = IndexAccess(IndexAccess(grid,1),1) */
        int r = -1;
        if (n->left && n->left->type == NODE_TEMPLATE_REF && n->left->text) {
            r = lookup_var(cg, n->left->text);
        } else if (n->left && n->left->type == NODE_EXPR && n->left->text) {
            r = lookup_var(cg, n->left->text);
        } else if (n->left && n->left->type == NODE_INDEX_ACCESS) {
            /* 链式: 先递归求值内层 IndexAccess, 结果(子数组地址)在 R_TMP */
            cg_expr(cg, n->left);
            /* R_TMP = 子数组 base 地址, 保存到 R_AUX2(用 R41) */
            emit(cg, OP_MOV, 41, R_TMP, 0);
            r = 41;  /* 后续用 R41 作为 base */
        }
        if (r < 0) { fail(cg); break; }
        /* 1. index 求值 → R_TMP */
        cg_expr(cg, n->right);
        /* 2. R_AUX = index * 8 */
        emit(cg, OP_MOV, R_AUX, R_TMP, 0);
        emit(cg, OP_MOV, R_TMP, -1, 8);
        emit(cg, OP_MUL, R_AUX, R_TMP, 0);             /* R_AUX = index*8 */
        /* 3. R_AUX += arr_base + 8 (跳过 len 头) */
        emit(cg, OP_ADD, R_AUX, r, 0);                 /* R_AUX = base + index*8 */
        emit(cg, OP_ADD, R_AUX, -1, 8);                /* R_AUX += 8 (跳过 len) */
        /* 4. LOAD64 R_TMP, [R_AUX] */
        emit(cg, OP_LOAD64, R_TMP, R_AUX, 0);
        /* 元素编译期类型: 字符串数组元素=负常量标记 / 浮点数组元素=double 位模式 */
        if (n->left && n->left->text && cg_has_name(cg->sarrs, cg->sarr_count, n->left->text)) {
            if (getenv("MO2KBC_DBG")) fprintf(stderr, "[DBG idx-str] %s\n", n->left->text);
            cg->last_ty = 2;
        }
        else if (n->left && n->left->text && cg_has_name(cg->farrs, cg->farr_count, n->left->text))
            cg->last_ty = 1;
        else
            cg->last_ty = 0;
        break;
    }
    default:
        fail(cg); break;
    }
}

/* ---------- 条件: 真跳 lbl_true, 假顺序落空 ---------- */
static void cg_cond(Cg* cg, AstNode* cond, int lbl_true) {
    if (cond && cond->type == NODE_BINOP) {
        switch ((TokenType)cond->inst) {
        case TOK_EQ: case TOK_NEQ: case TOK_GT: case TOK_LT: case TOK_GE: case TOK_LE: {
            cg_expr(cg, cond->left);
            int lt = cg->last_ty;
            emit(cg, OP_PUSH, R_TMP, -1, 0);
            cg_expr(cg, cond->right);
            int rt = cg->last_ty;
            emit(cg, OP_POP, R_AUX, -1, 0);   /* AUX=left, TMP=right */
            if (lt == 1 && rt == 1) {
                /* 两边都是浮点: 位模式双双截断为整数再比 */
                emit(cg, OP_F2I, R_AUX, -1, 0);
                emit(cg, OP_F2I, R_TMP, -1, 0);
            } else if (lt == 1) {
                /* 仅左浮点(ch340_dock: vout=3.3 vs 字面量 3):
                 * 只截断浮点侧。⚠️ 不能对整数侧发 F2I —— 裸 int64
                 * 被当 double 位模式读, 3 → 1.5e-323 → 截断 0,
                 * 3!=0 误判不等 (CHECK 0 假阴性根因)。 */
                emit(cg, OP_F2I, R_AUX, -1, 0);
            } else if (rt == 1) {
                emit(cg, OP_F2I, R_TMP, -1, 0);
            }
            switch ((TokenType)cond->inst) {
            case TOK_EQ:  emit_jmp_lbl(cg, OP_JE,  R_AUX, R_TMP, lbl_true); break;
            case TOK_NEQ: emit_jmp_lbl(cg, OP_JNE, R_AUX, R_TMP, lbl_true); break;
            case TOK_GT:  emit_jmp_lbl(cg, OP_JG,  R_AUX, R_TMP, lbl_true); break;
            case TOK_LT:  emit_jmp_lbl(cg, OP_JL,  R_AUX, R_TMP, lbl_true); break;
            case TOK_GE:  emit_jmp_lbl(cg, OP_JGE, R_AUX, R_TMP, lbl_true); break;
            case TOK_LE:  emit_jmp_lbl(cg, OP_JLE, R_AUX, R_TMP, lbl_true); break;
                default: fail(cg); break;
            }
            return;
        }
        default: break;
        }
    }
    cg_expr(cg, cond);
    emit_jmp_lbl(cg, OP_JNZ, R_TMP, -1, lbl_true);
}

/* ---------- 语句 ---------- */
static void cg_stmt(Cg* cg, AstNode* n) {
    if (cg->error || !n) return;
    cg->cur_node = n;
    switch (n->type) {
    case NODE_VAR_DECL: case NODE_CONST_DECL: case NODE_ASSIGN: {
        /* 数组元素赋值 a[i] = v (parser 标记 text="[]@", left=IndexAccess, right=值) */
        if (n->text && strcmp(n->text, "[]@") == 0 && n->left && n->left->type == NODE_INDEX_ACCESS) {
            AstNode* ia = n->left;
            int r = -1;
            if (ia->left && (ia->left->type == NODE_TEMPLATE_REF || ia->left->type == NODE_EXPR) && ia->left->text)
                r = lookup_var(cg, ia->left->text);
            if (r < 0 || !ia->right) { fail(cg); break; }
            /* 1. index → R_TMP */
            cg_expr(cg, ia->right);
            /* 2. R_AUX = index*8 */
            emit(cg, OP_MOV, R_AUX, R_TMP, 0);
            emit(cg, OP_MOV, R_TMP, -1, 8);
            emit(cg, OP_MUL, R_AUX, R_TMP, 0);
            /* 3. R_AUX = base + index*8 */
            emit(cg, OP_ADD, R_AUX, r, 0);
            /* 4. R_AUX += 8 (跳过 len 头) → 元素地址 */
            emit(cg, OP_ADD, R_AUX, -1, 8);
            /* 5. 保存元素地址到 R41 (STORE64: a=addr, b=value) */
            emit(cg, OP_MOV, 41, R_AUX, 0);
            /* 6. 求值右侧 → R_TMP */
            cg_expr(cg, n->right);
            /* 6.5 编译期类型回标: 浮点值存入数组 → 数组名标进 farrs,
             * 后续元素读取/打印按 double 位模式 %g (ch340_dock: baud_k=baud/1000→115.2)。
             * 解释器按元素动态类型打印, %g 对整值浮点自动去尾零, 整数组不受影响。 */
            if (cg->last_ty == 1 && ia->left && ia->left->text)
                cg_mark_name(&cg->farrs, &cg->farr_count, &cg->farr_cap, ia->left->text);
            /* 7. STORE64 [R41], R_TMP */
            emit(cg, OP_STORE64, 41, R_TMP, 0);
            break;
        }
        if (!n->text || !n->text[0]) { fail(cg); break; }
        /* parser 将初始化表达式挂在 left (见 parse_var_decl) */
        AstNode* val = n->left ? n->left : (n->right ? n->right : (n->args.count ? n->args.items[0] : NULL));
        int is_arr = (val && val->type == NODE_ARRAY_LIT);
        int r = is_arr ? declare_array_var(cg, n->text) : declare_var(cg, n->text);
        if (is_arr) {
            /* 数组字面量: data 段布局 [len:8][e0:8][e1:8]...
               寄存器存起始地址(指向 len). 支持嵌套(元素为子数组地址),
               字符串/浮点元素自动标记 sarrs/farrs (打印/下标类型追踪) */
            uint32_t arr_addr = emit_array_literal(cg, val, n->text);
            /* 变量寄存器存数组起始地址(指向 len) */
            emit(cg, OP_MOV, r, -1, (int64_t)arr_addr);
            cg->last_ty = 3;
        } else if (val) {
            cg_expr(cg, val);
            /* 编译期类型标记: 浮点 → fvars, 字符串 → strvars (打印模式判定) */
            if (cg->last_ty == 1) cg_mark_name(&cg->fvars, &cg->fvar_count, &cg->fvar_cap, n->text);
            else if (cg->last_ty == 2) cg_mark_name(&cg->strvars, &cg->strvar_count, &cg->strvar_cap, n->text);
            /* :str 声明 + 非字面量初始化 (函数返回数组/模板引用等):
             * 值运行时是 data 段数组地址/字符串标记, 元素下标访问需按 sarrs 还原
             * (probe_cap T3: void msg : str = make_msg(...) → ${msg}[0]) */
            else if (n->mtype == TYPE_STR && val->type != NODE_LITERAL) {
                cg_mark_name(&cg->sarrs, &cg->sarr_count, &cg->sarr_cap, n->text);
                if (getenv("MO2KBC_DBG")) fprintf(stderr, "[DBG sarrs-mark] %s (mtype=%d valtype=%d)\n", n->text, n->mtype, val->type);
            }
            emit(cg, OP_MOV, r, R_TMP, 0);
        }
        break;
    }
    case NODE_CALL:
        /* 裸函数调用语句 (含 linux_* 内置直驱真内核) */
        cg_expr(cg, n);
        break;
    case NODE_PRINT: {
        /* 链式 print: >> print >> a >> b >> c → args 多段。
           首段 imm 不带追加位(新行), 后续段 |1(追加), 拼成一条完整消息。

           两遍法: 第一遍先把所有含求值的段(变量/模板/函数调用)求值并
           暂存到 R_OUT_i, 第二遍再统一输出。若边求值边打印, 段内函数
           调用体里的 print 输出会插进本条 print 中间
           (openclaw_mini 心跳文案错乱 + in-fn 行穿插的根因)。 */
        int out_arg[16];
        int out_ty[16];   /* pass1 各段求值后的编译期类型 (0=int 1=float 2=str) */
        int n_out = 0;
        /* ---- pass 1: 预求值 ---- */
        for (int ai = 0; ai < n->args.count && !cg->error; ai++) {
            AstNode* arg = n->args.items[ai];
            if (arg->type == NODE_LITERAL && arg->mtype == TYPE_STR && arg->text) continue;  /* 常量串无求值 */
            if (arg->type == NODE_ARRAY_LIT) continue;                                       /* 数组字面量 pass2 现算 */
            if ((arg->type == NODE_TEMPLATE_REF || arg->type == NODE_EXPR) &&
                arg->text && var_is_array(cg, arg->text)) continue;                          /* 数组变量 pass2 循环读 */
            if (n_out >= 16) { fail(cg); break; }
            cg_expr(cg, arg);
            emit(cg, OP_MOV, R_OUT_BASE + n_out, R_TMP, 0);
            out_arg[n_out] = ai;
            out_ty[n_out] = cg->last_ty;   /* 捕获该段编译期类型供 pass2 还原判定 */
            n_out++;
        }
        /* ---- pass 2: 统一输出 ---- */
        int first_seg = 1;
        int out_i = 0;
        for (int ai = 0; ai < n->args.count && !cg->error; ai++) {
            AstNode* arg = n->args.items[ai];
            int ap = first_seg ? 0 : 1;   /* 追加位 */
            first_seg = 0;
            /* 字符串字面量直接走常量池打印 */
            if (arg->type == NODE_LITERAL && arg->mtype == TYPE_STR && arg->text) {
                int ci = kprog_add_const(cg->p, 1, 0, 0.0, arg->text);
                emit(cg, OP_PRINT, -1, ci, ap);
            } else if (arg->type == NODE_ARRAY_LIT) {
                /* 数组字面量打印: [e1, e2, ...] 运行时拼接 */
                int ci_open  = kprog_add_const(cg->p, 1, 0, 0.0, "[");
                int ci_sep   = kprog_add_const(cg->p, 1, 0, 0.0, ", ");
                int ci_close = kprog_add_const(cg->p, 1, 0, 0.0, "]");
                emit(cg, OP_PRINT, -1, ci_open, ap);  /* "[" 首段新行/后续段追加 */
                for (int i = 0; i < arg->args.count; i++) {
                    AstNode* e = arg->args.items[i];
                    if (i > 0) emit(cg, OP_PRINT, -1, ci_sep, 1);  /* ", " 追加 */
                    if (e->type == NODE_LITERAL && e->mtype != TYPE_STR) {
                        /* 字面量: 直接作为整数打印(追加) */
                        emit(cg, OP_MOV, R_TMP, -1, e->ival);
                        emit(cg, OP_PRINT, R_TMP, -1, 3);  /* raw int + 追加 */
                    } else if (e->type == NODE_TEMPLATE_REF || e->type == NODE_EXPR) {
                        /* 变量引用: 求值后打印(追加) */
                        cg_expr(cg, e);
                        emit(cg, OP_PRINT, R_TMP, -1, 3);  /* raw int + 追加 */
                    } else { fail(cg); break; }
                }
                if (cg->error) break;
                emit(cg, OP_PRINT, -1, ci_close, 1);  /* "]" 追加 */
            } else if ((arg->type == NODE_TEMPLATE_REF || arg->type == NODE_EXPR) && arg->text && var_is_array(cg, arg->text)) {
                /* 打印数组变量: [e0, e1, ...] 从 data 段读取
                   布局: [len:8][e0:8][e1:8]... 寄存器存 base(指向 len) */
                int r = lookup_var(cg, arg->text);
                int ci_open  = kprog_add_const(cg->p, 1, 0, 0.0, "[");
                int ci_sep   = kprog_add_const(cg->p, 1, 0, 0.0, ", ");
                int ci_close = kprog_add_const(cg->p, 1, 0, 0.0, "]");
                /* 用 R_LOOP_BASE=i, +1=len, +2=addr 作循环变量(变量池外工作区) */
                #define RI   R_LOOP_BASE
                #define RLEN (R_LOOP_BASE + 1)
                #define RADDR (R_LOOP_BASE + 2)
                /* 元素打印模式: 字符串数组→负常量标记需还原(1 追加);
                   浮点数组→double 位模式 %g(5=追加|浮点); 整数→raw(3=追加|raw) */
                int arr_is_str = arg->text && cg_has_name(cg->sarrs, cg->sarr_count, arg->text);
                int arr_is_flt = arg->text && cg_has_name(cg->farrs, cg->farr_count, arg->text);
                int eimm = arr_is_str ? 1 : (arr_is_flt ? 5 : 3);
                emit(cg, OP_LOAD64, RLEN, r, 0);           /* RLEN = *base (len) */
                emit(cg, OP_MOV, RI, -1, 0);               /* i = 0 */
                emit(cg, OP_MOV, RADDR, r, 0);             /* RADDR = base */
                emit(cg, OP_ADD, RADDR, -1, 8);            /* RADDR = base + 8 (e0) */
                emit(cg, OP_PRINT, -1, ci_open, ap);       /* "[" 首段新行/后续段追加 */
                int l_loop = label_new(&cg->labels);
                int l_end  = label_new(&cg->labels);
                place_label(cg, l_loop);
                /* if i >= len goto end */
                emit_jmp_lbl(cg, OP_JGE, RI, RLEN, l_end);
                /* if i > 0 print ", " */
                int l_skip_sep = label_new(&cg->labels);
                emit_jmp_lbl(cg, OP_JE, RI, -1, l_skip_sep);  /* JE 需要 b>=0 比较寄存器, 但 -1 表示 imm=0? 看实现: JE a, b 比较 regs[a] vs regs[b], b<0 时与 0 比较 */
                emit(cg, OP_PRINT, -1, ci_sep, 1);         /* ", " */
                place_label(cg, l_skip_sep);
                /* print elem */
                emit(cg, OP_LOAD64, R_TMP, RADDR, 0);      /* R_TMP = *RADDR */
                emit(cg, OP_PRINT, R_TMP, -1, eimm);       /* 按数组类型: raw/浮点/字符串还原 */
                /* i++, addr += 8 */
                emit(cg, OP_ADD, RI, -1, 1);
                emit(cg, OP_ADD, RADDR, -1, 8);
                emit_jmp_lbl(cg, OP_JMP, 0, 0, l_loop);
                place_label(cg, l_end);
                emit(cg, OP_PRINT, -1, ci_close, 1);       /* "]" */
                #undef RI
                #undef RLEN
                #undef RADDR
            } else {
                /* 值已在 pass1 求值暂存于 R_OUT_i, 此处只输出不求值
                   (保证段内函数调用体的 print 不会插进本条中间) */
                int r = R_OUT_BASE + out_i;
                /* 字符串变量: 寄存器存负常量标记, 须走非 raw 打印还原常量串;
                   浮点变量: double 位模式 %g; 整数变量: 走 raw 打印允许负值。
                   类型判定 = pass1 捕获的 last_ty(动态求值路径, 覆盖 IndexAccess/
                   函数调用等复合表达式) + 名字集合(标量声明路径) 双保险。 */
                int is_str = ((arg->type == NODE_TEMPLATE_REF || arg->type == NODE_EXPR) &&
                              arg->text && cg_is_strvar(cg, arg->text)) || out_ty[out_i] == 2;
                int is_flt = ((arg->type == NODE_TEMPLATE_REF || arg->type == NODE_EXPR) &&
                              arg->text && cg_has_name(cg->fvars, cg->fvar_count, arg->text)) || out_ty[out_i] == 1;
                out_i++;
                int imm = is_flt ? (4 | ap) : ((is_str ? 0 : 2) | ap);
                emit(cg, OP_PRINT, r, -1, imm);
            }
        }
        break;
    }
    case NODE_RETURN: {
        /* parser 将返回值挂在 left (见 parse_return) */
        AstNode* val = n->left ? n->left : (n->args.count ? n->args.items[0] : NULL);
        if (val && val->type == NODE_ARRAY_LIT) {
            /* return [数组]: data 段构造数组, 返回 base 地址(指向 len) */
            uint32_t addr = emit_array_literal(cg, val, NULL);
            emit(cg, OP_MOV, R_TMP, -1, (int64_t)addr);
            emit(cg, OP_MOV, R_RET, R_TMP, 0);
        } else if (val) { cg_expr(cg, val); emit(cg, OP_MOV, R_RET, R_TMP, 0); }
        emit(cg, OP_RET, 0, 0, 0);
        break;
    }
    case NODE_WHILE: {
        int l_cond = label_new(&cg->labels);
        int l_body = label_new(&cg->labels);
        int l_end  = label_new(&cg->labels);
        place_label(cg, l_cond);
        cg_cond(cg, n->cond, l_body);
        emit_jmp_lbl(cg, OP_JMP, 0, 0, l_end);
        place_label(cg, l_body);
        /* break 指向循环结尾 */
        if (cg->break_depth < 64) cg->break_lbls[cg->break_depth++] = l_end;
        cg_stmt(cg, n->then_block);
        cg->break_depth--;
        emit_jmp_lbl(cg, OP_JMP, 0, 0, l_cond);
        place_label(cg, l_end);
        break;
    }
    case NODE_IF: {
        int l_then = label_new(&cg->labels);
        int l_end  = label_new(&cg->labels);
        int l_else = n->else_block ? label_new(&cg->labels) : -1;
        cg_cond(cg, n->cond, l_then);
        if (l_else >= 0) emit_jmp_lbl(cg, OP_JMP, 0, 0, l_else);
        else             emit_jmp_lbl(cg, OP_JMP, 0, 0, l_end);
        place_label(cg, l_then);
        cg_stmt(cg, n->then_block);
        if (l_else >= 0) {
            emit_jmp_lbl(cg, OP_JMP, 0, 0, l_end);
            place_label(cg, l_else);
            cg_stmt(cg, n->else_block);
        }
        place_label(cg, l_end);
        break;
    }
    case NODE_BLOCK:
        for (int i = 0; i < n->body.count; i++) cg_stmt(cg, n->body.items[i]);
        break;
    case NODE_FN_DECL:
        break; /* 函数单独编译 */
    case NODE_BREAK:
        if (cg->break_depth > 0) {
            emit_jmp_lbl(cg, OP_JMP, 0, 0, cg->break_lbls[cg->break_depth - 1]);
        } else {
            fail(cg);  /* break 不在循环内 */
        }
        break;
    case NODE_CONTINUE:
        /* 简化: 不支持 continue (需要循环条件标签栈), 报错避免误编 */
        fail(cg);
        break;
    default:
        fail(cg); break;
    }
}

/* ---------- 编译函数体 ---------- */
static void cg_func(Cg* cg, AstNode* fn) {
    if (cg->error) return;
    const char* fname = fn->text ? fn->text : "";
    CgFunc* f = cgfunc_find(&cg->funcs, fname);
    if (!f) { fail(cg); return; }
    cg->cur_func_index = f->index;
    f->entry_pc = cg->p->code_count;
    /* 每个函数独立作用域: 清空上一函数残留的局部符号表。
     * 否则 sym_lookup(线性返回第一个同名) 会把本函数参数/局部变量
     * 错绑到前面函数的同名寄存器 (如 agent_reply 的 cid 错绑到
     * session_ap 的 cid → if 比较恒假 → 函数体内 PRINT 全被跳过)。 */
    sym_free(&cg->locals);
    /* 形参入口拷贝: 两遍法。
     * 第一遍按源码顺序声明全部形参 (分配 R_VAR_BASE 起的连续局部寄存器);
     * 第二遍【逆序】发射 MOV P_i, R_i。
     * ⚠️ 必须逆序: VM 已把 arg_i 装进 R0..R_{n-1}, 正序发射时
     * MOV R1,R0 先执行会把 R1 从 arg1 覆盖成 arg0,
     * 随后 MOV R2,R1 读到的是被污染的 R1 (probe3 set_at: v 存成 2 而非 99)。
     * 单参数函数无级联故此前未暴露。 */
    int n = fn->args.count;
    if (n > 32) { fail(cg); return; }   /* 函数参数过多(>32), 由 mo2kbc_compile 统一报错 */
    {
        int preg[32];
        for (int i = 0; i < n; i++) {
            const char* pname = fn->args.items[i]->text ? fn->args.items[i]->text : "";
            preg[i] = declare_var(cg, pname);
        }
        for (int i = n - 1; i >= 0; i--) {
            emit(cg, OP_MOV, preg[i], i, 0);   /* MOV P_i, R_i (逆序, 防源寄存器被覆盖) */
        }
    }
    /* 编译函数体: FN_DECL 的语句在 body 列表中 */
    for (int i = 0; i < fn->body.count; i++) {
        cg_stmt(cg, fn->body.items[i]);
    }
    /* 函数体末尾兜底: 若无显式 RET, 返回 0 */
    emit(cg, OP_MOV, R_RET, -1, 0);
    emit(cg, OP_RET, 0, 0, 0);
    cg->cur_func_index = -1;
}

/* ---------- 表释放 ---------- */
static void labels_free(LabelTab* t) {
    for (int i = 0; i < t->count; i++) free(t->items[i].pending);
    free(t->items); memset(t, 0, sizeof(*t));
}
static void cgfuncs_free(CgFuncTab* t) {
    for (int i = 0; i < t->count; i++) free(t->items[i].name);
    free(t->items); memset(t, 0, sizeof(*t));
}

/* ---------- 入口 ---------- */
KillsProgram* mo2kbc_compile(AstNode* program, char* err, int errlen) {
    if (!program || program->type != NODE_PROGRAM) return NULL;

    Cg cg; memset(&cg, 0, sizeof(cg));
    cg.p = kprog_new();
    cg.next_reg = R_VAR_BASE;
    cg.next_global = R_GLOBAL_BASE;
    cg.cur_func_index = -1;
    if (!cg.p) return NULL;

    /* 第一遍: 登记所有函数 (index + nparams), 使 CALL 可前向引用 */
    for (int i = 0; i < program->body.count; i++) {
        AstNode* s = program->body.items[i];
        if (s && s->type == NODE_FN_DECL && s->text) {
            CgFunc* f = cgfunc_add(&cg.funcs, s->text);
            f->nparams = s->args.count;
        }
    }

    /* 编译 main: 所有非 fn 的顶层语句 */
    for (int i = 0; i < program->body.count; i++) {
        AstNode* s = program->body.items[i];
        if (s && s->type != NODE_FN_DECL) cg_stmt(&cg, s);
    }
    /* main 流末尾 HALT: 隔断 main 与函数体区。
     * 否则 main 最后一条语句执行完 pc 顺延进第一个函数体入口,
     * 函数体会被当作 main 流再执行一遍(且无 RET 收尾 → 越界跑飞)
     * —— openclaw_mini 双执行/输出错乱的根因之一。 */
    emit(&cg, OP_HALT, 0, 0, 0);

    /* 编译各函数体 */
    for (int i = 0; i < program->body.count; i++) {
        AstNode* s = program->body.items[i];
        if (s && s->type == NODE_FN_DECL) cg_func(&cg, s);
    }

    if (cg.error) {
        if (err && errlen) snprintf(err, errlen, "遇到不支持的语句或表达式");
        kprog_free(cg.p);
        sym_free(&cg.globals); sym_free(&cg.locals);
        cgfuncs_free(&cg.funcs); labels_free(&cg.labels);
        for (int i = 0; i < cg.strvar_count; i++) free(cg.strvars[i]);
        free(cg.strvars);
        return NULL;
    }

    /* 末尾 HALT 防越界 */
    emit(&cg, OP_HALT, 0, 0, 0);

    /* 调试: MO2KBC_DUMP=1 时反汇编指令流到 stderr */
    if (getenv("MO2KBC_DUMP")) {
        int dcap = (int)cg.p->code_count * 72 + 1024;
        char* dis = (char*)malloc((size_t)dcap);
        if (dis) {
            kvm_disassemble(cg.p, dis, dcap);
            fputs(dis, stderr);
            free(dis);
        }
    }

    /* 注册函数表 */
    for (int i = 0; i < cg.funcs.count; i++) {
        CgFunc* f = &cg.funcs.items[i];
        if (f->entry_pc < 0) { /* 缺省: 若函数从未被编译则跳过 */ f->entry_pc = 0; }
        kprog_add_func(cg.p, f->name, (uint32_t)f->entry_pc, (uint32_t)f->nparams);
    }

    sym_free(&cg.globals); sym_free(&cg.locals);
    cgfuncs_free(&cg.funcs); labels_free(&cg.labels);
    for (int i = 0; i < cg.strvar_count; i++) free(cg.strvars[i]);
    free(cg.strvars);
    return cg.p;
}
