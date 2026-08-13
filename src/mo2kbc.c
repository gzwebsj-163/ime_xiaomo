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
#define R_VAR_MAX    40
#define R_TMP        45
#define R_AUX        46

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
typedef struct { char* name; int index; int entry_pc; int nparams; } CgFunc;
typedef struct { CgFunc* items; int count, cap; } CgFuncTab;
static CgFunc* cgfunc_find(CgFuncTab* t, const char* name) {
    for (int i = 0; i < t->count; i++) if (strcmp(t->items[i].name, name) == 0) return &t->items[i];
    return NULL;
}
static CgFunc* cgfunc_add(CgFuncTab* t, const char* name) {
    if (t->count >= t->cap) { t->cap = t->cap ? t->cap * 2 : 16; t->items = (CgFunc*)realloc(t->items, t->cap * sizeof(CgFunc)); }
    CgFunc* f = &t->items[t->count++];
    f->name = strdup(name); f->index = t->count - 1; f->entry_pc = -1; f->nparams = 0;
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
    CgFuncTab funcs;
    LabelTab labels;
    int cur_func_index;   /* -1 = 全局区(main) */
    int error;
} Cg;

static void fail(Cg* cg) { cg->error = 1; }

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
    int r = cg->next_reg++;
    if (cg->next_reg > R_VAR_MAX) cg->next_reg = R_VAR_BASE;
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
    int r = alloc_reg(cg);
    sym_add(cg->cur_func_index >= 0 ? &cg->locals : &cg->globals, name, r, 0);
    return r;
}
static int declare_array_var(Cg* cg, const char* name) {
    int r = alloc_reg(cg);
    sym_add(cg->cur_func_index >= 0 ? &cg->locals : &cg->globals, name, r, 1);
    return r;
}

/* ---------- 表达式 ---------- */
static void cg_expr(Cg* cg, AstNode* n);
static void cg_binop(Cg* cg, AstNode* n) {
    cg_expr(cg, n->left);
    emit(cg, OP_PUSH, R_TMP, -1, 0);
    cg_expr(cg, n->right);
    emit(cg, OP_POP, R_AUX, -1, 0);            /* AUX=left, TMP=right */
    switch ((TokenType)n->inst) {
    case TOK_PLUS:  emit(cg, OP_ADD, R_AUX, R_TMP, 0); break;
    case TOK_MINUS: emit(cg, OP_SUB, R_AUX, R_TMP, 0); break;
    case TOK_STAR:  emit(cg, OP_MUL, R_AUX, R_TMP, 0); break;
    case TOK_SLASH: emit(cg, OP_DIV, R_AUX, R_TMP, 0); break;
    default: fail(cg); return;
    }
    emit(cg, OP_MOV, R_TMP, R_AUX, 0);
}
static void cg_expr(Cg* cg, AstNode* n) {
    if (cg->error || !n) return;
    switch (n->type) {
    case NODE_LITERAL:
        if (n->mtype == TYPE_STR && n->text) {
            /* 字符串字面量: 入常量池, R_TMP 存常量索引(负数标记) */
            int ci = kprog_add_const(cg->p, 1, 0, 0.0, n->text);
            emit(cg, OP_MOV, R_TMP, -1, -(int64_t)ci - 1);  /* 负值 = 常量索引标记 */
        } else {
            emit(cg, OP_MOV, R_TMP, -1, n->ival);
        }
        break;
    case NODE_TEMPLATE_REF:
    case NODE_EXPR: {
        if (n->text && n->text[0]) {
            int r = lookup_var(cg, n->text);
            if (r >= 0) emit(cg, OP_MOV, R_TMP, r, 0);
            else fail(cg);
        } else fail(cg);
        break;
    }
    case NODE_CALL: {
        CgFunc* f = (n->text) ? cgfunc_find(&cg->funcs, n->text) : NULL;
        if (!f) { fail(cg); break; }
        for (int i = n->args.count - 1; i >= 0; i--) {
            cg_expr(cg, n->args.items[i]);
            emit(cg, OP_PUSH, R_TMP, -1, 0);
        }
        emit(cg, OP_CALL, f->index, 0, 0);
        emit(cg, OP_MOV, R_TMP, R_RET, 0);
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
            emit(cg, OP_PUSH, R_TMP, -1, 0);
            cg_expr(cg, cond->right);
            emit(cg, OP_POP, R_AUX, -1, 0);   /* AUX=left, TMP=right */
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
    switch (n->type) {
    case NODE_VAR_DECL: case NODE_CONST_DECL: case NODE_ASSIGN: {
        if (!n->text || !n->text[0]) { fail(cg); break; }
        /* parser 将初始化表达式挂在 left (见 parse_var_decl) */
        AstNode* val = n->left ? n->left : (n->right ? n->right : (n->args.count ? n->args.items[0] : NULL));
        int is_arr = (val && val->type == NODE_ARRAY_LIT);
        int r = lookup_var(cg, n->text);
        if (r < 0) r = is_arr ? declare_array_var(cg, n->text) : declare_var(cg, n->text);
        if (is_arr) {
            /* 数组字面量: data 段布局 [len:8][e0:8][e1:8]...
               寄存器存起始地址(指向 len). 支持嵌套(元素为子数组地址) */
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
                    emit(cg, OP_MOV, R_TMP, -1, (int64_t)(addr + (i + 1) * 8));
                    emit(cg, OP_MOV, R_AUX, -1, e->ival);
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
                            emit(cg, OP_MOV, R_TMP, -1, (int64_t)(sub_addr + (j + 1) * 8));
                            emit(cg, OP_MOV, R_AUX, -1, se->ival);
                            emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
                        } else { fail(cg); break; }
                    }
                    if (cg->error) break;
                    /* 外层数组存子数组地址 */
                    emit(cg, OP_MOV, R_TMP, -1, (int64_t)(addr + (i + 1) * 8));
                    emit(cg, OP_MOV, R_AUX, -1, (int64_t)sub_addr);
                    emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
                } else {
                    cg_expr(cg, e);
                    emit(cg, OP_MOV, R_AUX, R_TMP, 0);
                    emit(cg, OP_MOV, R_TMP, -1, (int64_t)(addr + (i + 1) * 8));
                    emit(cg, OP_STORE64, R_TMP, R_AUX, 0);
                }
            }
            /* 变量寄存器存数组起始地址(指向 len) */
            emit(cg, OP_MOV, r, -1, (int64_t)addr);
        } else if (val) {
            cg_expr(cg, val);
            emit(cg, OP_MOV, r, R_TMP, 0);
        }
        break;
    }
    case NODE_PRINT:
        if (n->args.count >= 1) {
            AstNode* arg = n->args.items[0];
            /* 字符串字面量直接走常量池打印 */
            if (arg->type == NODE_LITERAL && arg->mtype == TYPE_STR && arg->text) {
                int ci = kprog_add_const(cg->p, 1, 0, 0.0, arg->text);
                emit(cg, OP_PRINT, -1, ci, 0);
            } else if (arg->type == NODE_ARRAY_LIT) {
                /* 数组字面量打印: [e1, e2, ...] 运行时拼接 */
                int ci_open  = kprog_add_const(cg->p, 1, 0, 0.0, "[");
                int ci_sep   = kprog_add_const(cg->p, 1, 0, 0.0, ", ");
                int ci_close = kprog_add_const(cg->p, 1, 0, 0.0, "]");
                emit(cg, OP_PRINT, -1, ci_open, 0);  /* "[" 新行 */
                for (int i = 0; i < arg->args.count; i++) {
                    AstNode* e = arg->args.items[i];
                    if (i > 0) emit(cg, OP_PRINT, -1, ci_sep, 1);  /* ", " 追加 */
                    if (e->type == NODE_LITERAL && e->mtype != TYPE_STR) {
                        /* 字面量: 直接作为整数打印(追加) */
                        emit(cg, OP_MOV, R_TMP, -1, e->ival);
                        emit(cg, OP_PRINT, R_TMP, -1, 1);
                    } else if (e->type == NODE_TEMPLATE_REF || e->type == NODE_EXPR) {
                        /* 变量引用: 求值后打印(追加) */
                        cg_expr(cg, e);
                        emit(cg, OP_PRINT, R_TMP, -1, 1);
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
                /* 用 R42=i, R43=len, R44=addr 作循环变量(避开 R_TMP/R_AUX) */
                #define RI 42
                #define RLEN 43
                #define RADDR 44
                emit(cg, OP_LOAD64, RLEN, r, 0);           /* RLEN = *base (len) */
                emit(cg, OP_MOV, RI, -1, 0);               /* i = 0 */
                emit(cg, OP_MOV, RADDR, r, 0);             /* RADDR = base */
                emit(cg, OP_ADD, RADDR, -1, 8);            /* RADDR = base + 8 (e0) */
                emit(cg, OP_PRINT, -1, ci_open, 0);        /* "[" */
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
                emit(cg, OP_PRINT, R_TMP, -1, 1);          /* print (追加) */
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
                cg_expr(cg, arg);
                emit(cg, OP_MOV, R_RET, R_TMP, 0);
                emit(cg, OP_PRINT, R_RET, -1, 0);
            }
        }
        break;
    case NODE_RETURN: {
        /* parser 将返回值挂在 left (见 parse_return) */
        AstNode* val = n->left ? n->left : (n->args.count ? n->args.items[0] : NULL);
        if (val) { cg_expr(cg, val); emit(cg, OP_MOV, R_RET, R_TMP, 0); }
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
        cg_stmt(cg, n->then_block);
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
    /* 形参暂存寄存器 + 入口拷贝 MOV P_i, R_i */
    int n = fn->args.count;
    for (int i = 0; i < n; i++) {
        const char* pname = fn->args.items[i]->text ? fn->args.items[i]->text : "";
        int reg = declare_var(cg, pname);
        emit(cg, OP_MOV, reg, i, 0);   /* MOV P_i, R_i */
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
        return NULL;
    }

    /* 末尾 HALT 防越界 */
    emit(&cg, OP_HALT, 0, 0, 0);

    /* 注册函数表 */
    for (int i = 0; i < cg.funcs.count; i++) {
        CgFunc* f = &cg.funcs.items[i];
        if (f->entry_pc < 0) { /* 缺省: 若函数从未被编译则跳过 */ f->entry_pc = 0; }
        kprog_add_func(cg.p, f->name, (uint32_t)f->entry_pc, (uint32_t)f->nparams);
    }

    sym_free(&cg.globals); sym_free(&cg.locals);
    cgfuncs_free(&cg.funcs); labels_free(&cg.labels);
    return cg.p;
}
