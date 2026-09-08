/*
 * xiaomo - VM 执行引擎实现
 *
 * 架构: 执行 AST, 支持
 *   1) 调用栈 + 局部作用域 + 真正的函数返回值 (支持递归)
 *   2) 真正的数组类型 (VAL_ARRAY: 下标访问/长度/打印)
 *   3) 变量按作用域词法查找 (局部遮蔽全局)
 *
 * 作用域模型:
 *   vm->scopes[0] 为全局作用域; 每次函数调用 push 一个局部作用域,
 *   返回时 pop。变量查找从最内层作用域向外逐层查找。
 */
#include "vm.h"
#include "tensor.h"
#include "weights.h"
#include "nd_tensor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>

/* ---------- 值工具 ---------- */
static MoValue val_int(long v) { MoValue x; memset(&x,0,sizeof(x)); x.type = VAL_INT; x.ival = v; x.fval = (double)v; return x; }
static MoValue val_float(double v) { MoValue x; memset(&x,0,sizeof(x)); x.type = VAL_FLOAT; x.fval = v; x.ival = (long)v; return x; }
static MoValue val_bool(int b) { MoValue x; memset(&x,0,sizeof(x)); x.type = VAL_BOOL; x.ival = b; x.fval = b; return x; }
static MoValue val_str(const char* s) {
    MoValue x; memset(&x,0,sizeof(x)); x.type = VAL_STR;
    x.sval = s ? strdup(s) : NULL;
    return x;
}
static MoValue val_null(void) { MoValue x; memset(&x,0,sizeof(x)); x.type = VAL_NULL; return x; }
static MoValue val_array_empty(void) { MoValue x; memset(&x,0,sizeof(x)); x.type = VAL_ARRAY; x.arr = (MoArray*)calloc(1,sizeof(MoArray)); return x; }

static void value_free(MoValue* v) {
    if (!v) return;
    if (v->type == VAL_TENSOR && v->tensor) {
        nd_free((NdTensor*)v->tensor);
        v->tensor = NULL;
    } else if (v->type == VAL_ARRAY && v->arr) {
        for (int i = 0; i < v->arr->count; i++) value_free(&v->arr->items[i]);
        if (v->arr->items) free(v->arr->items);
        free(v->arr);
        v->arr = NULL;
    } else if (v->sval) {
        free(v->sval);
        v->sval = NULL;
    }
    v->type = VAL_NULL;
}

/* 数组追加元素 (返回 0 成功) */
static int arr_append(MoArray* a, MoValue v) {
    if (!a) return -1;
    if (a->count >= a->capacity) {
        int nc = a->capacity ? a->capacity*2 : 8;
        MoValue* ni = (MoValue*)realloc(a->items, sizeof(MoValue)*nc);
        if (!ni) return -1;
        a->items = ni; a->capacity = nc;
    }
    a->items[a->count++] = v;
    return 0;
}

static MoValue value_clone(const MoValue* v) {
    MoValue r; memset(&r,0,sizeof(r));
    if (!v) return val_null();
    switch (v->type) {
    case VAL_INT: r = val_int(v->ival); break;
    case VAL_FLOAT: r = val_float(v->fval); break;
    case VAL_BOOL: r = val_bool(v->ival); break;
    case VAL_STR: r = val_str(v->sval); break;
    case VAL_TENSOR: {
        if (v->tensor) {
            r.type = VAL_TENSOR;
            r.tensor = nd_clone((const NdTensor*)v->tensor);
        } else { r = val_null(); }
        break;
    }
    case VAL_ARRAY: {
        r = val_array_empty();
        for (int i = 0; i < v->arr->count; i++) {
            MoValue elem = value_clone(&v->arr->items[i]);
            arr_append(r.arr, elem);
        }
        break;
    }
    default: r = val_null(); break;
    }
    return r;
}

/* 递归打印 tensor 完整数据 (row-major, 与 VAL_ARRAY 打印格式一致) */
static void nd_to_str_rec(const NdTensor* t, int dim, long base, char* buf, size_t buflen, size_t* offp) {
    if (dim == t->ndim) {
        *offp += snprintf(buf+*offp, buflen-*offp, "%g", t->data[base]);
        return;
    }
    *offp += snprintf(buf+*offp, buflen-*offp, "[");
    for (int i = 0; i < t->shape[dim]; i++) {
        if (i > 0) *offp += snprintf(buf+*offp, buflen-*offp, ", ");
        nd_to_str_rec(t, dim+1, base * t->shape[dim] + i, buf, buflen, offp);
    }
    *offp += snprintf(buf+*offp, buflen-*offp, "]");
}

void mo_value_to_str(const MoValue* v, char* buf, int buflen) {
    if (!v || buflen <= 0) { if (buf && buflen>0) buf[0]='\0'; return; }
    switch (v->type) {
    case VAL_INT: snprintf(buf, buflen, "%ld", v->ival); break;
    case VAL_FLOAT: snprintf(buf, buflen, "%g", v->fval); break;
    case VAL_BOOL: snprintf(buf, buflen, "%s", v->ival ? "true" : "false"); break;
    case VAL_STR: snprintf(buf, buflen, "%s", v->sval ? v->sval : ""); break;
    case VAL_TENSOR: {
        NdTensor* t = (NdTensor*)v->tensor;
        if (!t) { snprintf(buf, buflen, "<null tensor>"); break; }
        if (t->ndim == 0 && t->data) {
            snprintf(buf, buflen, "%g", t->data[0]);
            break;
        }
        if (t->size > 0 && t->size <= 1024) {
            size_t offp = 0;
            nd_to_str_rec(t, 0, 0, buf, (size_t)buflen, &offp);
            break;
        }
        int off = snprintf(buf, buflen, "<tensor %d-dim [", t->ndim);
        for (int i = 0; i < t->ndim && off < buflen-1; i++) {
            off += snprintf(buf+off, buflen-off, "%s%d", i?",":"", t->shape[i]);
        }
        snprintf(buf+off, buflen-off, "]>");
        break;
    }
    case VAL_ARRAY: {
        int off = 0;
        off = snprintf(buf+off, buflen-off, "[");
        for (int i = 0; i < v->arr->count && off < buflen-1; i++) {
            char tmp[8192];
            mo_value_to_str(&v->arr->items[i], tmp, sizeof(tmp));
            if (i > 0) off += snprintf(buf+off, buflen-off, ", ");
            off += snprintf(buf+off, buflen-off, "%s", tmp);
        }
        if (off < buflen-1) snprintf(buf+off, buflen-off, "]");
        break;
    }
    case VAL_BYTES: snprintf(buf, buflen, "[bytes %ld]", v->ival); break;
    default: snprintf(buf, buflen, "null"); break;
    }
}

/* ---------- 变量表 ---------- */
static void var_table_init(VarTable* vt) { vt->items = NULL; vt->count = 0; vt->capacity = 0; }
static void var_table_free(VarTable* vt) {
    for (int i = 0; i < vt->count; i++) {
        free(vt->items[i].name);
        value_free(&vt->items[i].value);
    }
    if (vt->items) free(vt->items);
    vt->items = NULL; vt->count = vt->capacity = 0;
}
static VarSlot* var_table_get(VarTable* vt, const char* name) {
    for (int i = 0; i < vt->count; i++) {
        if (strcmp(vt->items[i].name, name) == 0) return &vt->items[i];
    }
    return NULL;
}
static void var_table_set(VarTable* vt, const char* name, MoValue v, MoType mt) {
    VarSlot* s = var_table_get(vt, name);
    if (s) {
        value_free(&s->value);
        s->value = v;
        s->mtype = mt;
    } else {
        if (vt->count >= vt->capacity) {
            vt->capacity = vt->capacity == 0 ? 16 : vt->capacity * 2;
            vt->items = (VarSlot*)realloc(vt->items, sizeof(VarSlot) * vt->capacity);
        }
        vt->items[vt->count].name = strdup(name);
        vt->items[vt->count].value = v;
        vt->items[vt->count].mtype = mt;
        vt->count++;
    }
}

/* ---------- 作用域栈 ---------- */
static int scope_push(VM* vm) {
    if (vm->scope_count >= vm->scope_cap) {
        vm->scope_cap = vm->scope_cap ? vm->scope_cap*2 : 4;
        vm->scopes = (VarTable*)realloc(vm->scopes, sizeof(VarTable)*vm->scope_cap);
    }
    var_table_init(&vm->scopes[vm->scope_count]);
    return vm->scope_count++;
}
static void scope_pop(VM* vm) {
    if (vm->scope_count <= 1) return; /* 保留全局作用域 */
    vm->scope_count--;
    var_table_free(&vm->scopes[vm->scope_count]);
}

/* 从最内层向外查找变量 (先当前作用域, 再外层) */
static VarSlot* scope_lookup(VM* vm, const char* name) {
    for (int i = vm->scope_count - 1; i >= 0; i--) {
        VarSlot* s = var_table_get(&vm->scopes[i], name);
        if (s) return s;
    }
    return NULL;
}

/* ---------- VM ---------- */
static void vm_error(VM* vm, const char* fmt, ...);

void vm_init(VM* vm) {
    memset(vm, 0, sizeof(VM));
    vm->scopes = NULL; vm->scope_count = 0; vm->scope_cap = 0;
    /* 全局作用域 = scopes[0] */
    scope_push(vm);
    var_table_init(&vm->functions);
    vm->output = NULL; vm->output_count = 0; vm->output_cap = 0;
    vm->fn_defs = NULL; vm->fn_def_count = 0; vm->fn_def_cap = 0;
}

void vm_free(VM* vm) {
    /* 弹出所有作用域 (含全局) */
    for (int i = 0; i < vm->scope_count; i++) var_table_free(&vm->scopes[i]);
    if (vm->scopes) free(vm->scopes);
    vm->scopes = NULL; vm->scope_count = vm->scope_cap = 0;
    var_table_free(&vm->functions);
    for (int i = 0; i < vm->output_count; i++) free(vm->output[i]);
    if (vm->output) free(vm->output);
    vm->output = NULL; vm->output_count = vm->output_cap = 0;
    if (vm->fn_defs) free(vm->fn_defs);
    vm->fn_defs = NULL; vm->fn_def_count = vm->fn_def_cap = 0;
}

static void vm_add_output(VM* vm, const char* s) {
    if (vm->output_count >= vm->output_cap) {
        vm->output_cap = vm->output_cap == 0 ? 16 : vm->output_cap * 2;
        vm->output = (char**)realloc(vm->output, sizeof(char*) * vm->output_cap);
    }
    vm->output[vm->output_count++] = strdup(s);
}

int vm_output_count(const VM* vm) { return vm->output_count; }
const char* vm_output(VM* vm, int idx) {
    if (idx < 0 || idx >= vm->output_count) return NULL;
    return vm->output[idx];
}
const MoValue* vm_get_global(const VM* vm, const char* name) {
    if (vm->scope_count <= 0) return NULL;
    const VarSlot* s = var_table_get(&vm->scopes[0], name);
    return s ? &s->value : NULL;
}

static void vm_error(VM* vm, const char* fmt, ...) {
    if (vm->error_count == 0) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(vm->error_msg, sizeof(vm->error_msg), fmt, args);
        va_end(args);
    }
    vm->error_count++;
}

/* ---------- 表达式求值 ---------- */
static MoValue eval_expr(VM* vm, const AstNode* node);
static void exec_stmt(VM* vm, const AstNode* node);
static void exec_block(VM* vm, const AstNode* block);

#define MAX_CALL_DEPTH 256

/* 查找函数定义 (per-VM 函数表) */
static const AstNode* lookup_function(VM* vm, const char* name) {
    for (int i = 0; i < vm->fn_def_count; i++) {
        const AstNode* f = vm->fn_defs[i];
        if (f && f->text && strcmp(f->text, name) == 0) return f;
    }
    return NULL;
}

/* 调用函数 (支持递归/返回值/局部作用域) */
static MoValue call_function(VM* vm, const AstNode* fn, const NodeList* args) {
    if (vm->call_depth >= MAX_CALL_DEPTH) {
        vm_error(vm, "调用栈溢出 (递归过深 > %d)", MAX_CALL_DEPTH);
        return val_null();
    }
    vm->call_depth++;

    /* 新建局部作用域 */
    scope_push(vm);

    /* 绑定参数: 形参名 -> 实参值 */
    for (int i = 0; i < fn->args.count; i++) {
        const AstNode* param = fn->args.items[i];
        const char* pname = param->text ? param->text : "";
        MoValue v = val_null();
        if (i < args->count) v = eval_expr(vm, args->items[i]);
        var_table_set(&vm->scopes[vm->scope_count-1], pname, v, param->mtype);
    }

    /* 保存调用者控制标志, 以备嵌套 return */
    int saved_ret = vm->return_flag;
    MoValue saved_retval = vm->return_value; /* 浅拷贝, 调用结束后回收前值 */
    vm->return_flag = 0;
    value_free(&vm->return_value);
    vm->return_value = val_null();

    /* 执行函数体 */
    exec_block(vm, fn);

    /* 取返回值, 克隆到调用方 */
    MoValue result = value_clone(&vm->return_value);
    value_free(&vm->return_value);

    /* 恢复外层 return_flag (如果本函数用 return 提前退出) */
    vm->return_flag = saved_ret;
    vm->return_value = saved_retval;
    /* 若 break/continue 贯穿到函数外则清掉 */
    vm->break_flag = 0;
    vm->continue_flag = 0;

    /* 弹出局部作用域 */
    scope_pop(vm);
    vm->call_depth--;

    return result;
}

static int truthy(const MoValue* v) {
    switch (v->type) {
    case VAL_INT: return v->ival != 0;
    case VAL_FLOAT: return v->fval != 0.0;
    case VAL_BOOL: return v->ival != 0;
    case VAL_STR: return v->sval != NULL && v->sval[0] != '\0';
    case VAL_ARRAY: return v->arr != NULL && v->arr->count > 0;
    case VAL_BYTES: return v->ival != 0;
    default: return 0;
    }
}

static MoValue eval_binop(VM* vm, const AstNode* node) {
    TokenType op = (TokenType)node->inst;

    /* 一元负号: 由 parse_unary 生成 (right 为 NULL, left 存操作数) */
    if (node->right == NULL) {
        MoValue l = eval_expr(vm, node->left);
        if (op == TOK_MINUS) {
            MoValue res = (l.type == VAL_FLOAT) ? val_float(-l.fval) : val_int(-l.ival);
            value_free(&l);
            return res;
        }
        if (op == TOK_NOT) {
            MoValue res = val_bool(!truthy(&l));
            value_free(&l);
            return res;
        }
        value_free(&l);
        return val_null();
    }

    MoValue l = eval_expr(vm, node->left);
    MoValue r = eval_expr(vm, node->right);

    /* 字符串拼接 */
    if (op == TOK_PLUS && l.type == VAL_STR && r.type == VAL_STR) {
        char* buf = (char*)malloc(strlen(l.sval) + strlen(r.sval) + 1);
        sprintf(buf, "%s%s", l.sval, r.sval);
        MoValue res = val_str(buf);
        free(buf);
        value_free(&l); value_free(&r);
        return res;
    }
    if (op == TOK_PLUS && l.type == VAL_STR) {
        char rs[64]; mo_value_to_str(&r, rs, sizeof(rs));
        char* buf = (char*)malloc(strlen(l.sval) + strlen(rs) + 1);
        sprintf(buf, "%s%s", l.sval, rs);
        MoValue res = val_str(buf);
        free(buf);
        value_free(&l); value_free(&r);
        return res;
    }

    double lf = (l.type == VAL_FLOAT) ? l.fval : (double)l.ival;
    double rf = (r.type == VAL_FLOAT) ? r.fval : (double)r.ival;
    int isf = (l.type == VAL_FLOAT || r.type == VAL_FLOAT);

    MoValue res;
    switch (op) {
    case TOK_PLUS: res = isf ? val_float(lf + rf) : val_int(l.ival + r.ival); break;
    case TOK_MINUS: res = isf ? val_float(lf - rf) : val_int(l.ival - r.ival); break;
    case TOK_STAR: res = isf ? val_float(lf * rf) : val_int(l.ival * r.ival); break;
    case TOK_SLASH:
        if (rf == 0) { vm_error(vm, "除零错误"); res = val_int(0); }
        else res = val_float(lf / rf);
        break;
    case TOK_PERCENT: res = val_int(l.ival % r.ival); break;
    case TOK_GT: res = val_bool(isf ? lf > rf : l.ival > r.ival); break;
    case TOK_LT: res = val_bool(isf ? lf < rf : l.ival < r.ival); break;
    case TOK_GE: res = val_bool(isf ? lf >= rf : l.ival >= r.ival); break;
    case TOK_LE: res = val_bool(isf ? lf <= rf : l.ival <= r.ival); break;
    case TOK_EQ:
        if (l.type == VAL_STR && r.type == VAL_STR)
            res = val_bool(strcmp(l.sval, r.sval) == 0);
        else res = val_bool(l.ival == r.ival);
        break;
    case TOK_NEQ:
        if (l.type == VAL_STR && r.type == VAL_STR)
            res = val_bool(strcmp(l.sval, r.sval) != 0);
        else res = val_bool(l.ival != r.ival);
        break;
    case TOK_AND_AND: res = val_bool(truthy(&l) && truthy(&r)); break;
    case TOK_OR_OR: res = val_bool(truthy(&l) || truthy(&r)); break;
    case TOK_RSHIFT: res = val_int(l.ival >> r.ival); break;
    case TOK_LSHIFT: res = val_int(l.ival << r.ival); break;
    case TOK_AMP: res = val_int(l.ival & r.ival); break;
    case TOK_PIPE: res = val_int(l.ival | r.ival); break;
    case TOK_CARET: res = val_int(l.ival ^ r.ival); break;
    case TOK_NOT: res = val_bool(!truthy(&r)); break;
    default:
        vm_error(vm, "未知运算符");
        res = val_null();
    }
    value_free(&l); value_free(&r);
    return res;
}

/* 解析模板引用 ${name} */
static MoValue eval_template_ref(VM* vm, const AstNode* node) {
    if (node->text) {
        VarSlot* s = scope_lookup(vm, node->text);
        if (s) return value_clone(&s->value); /* 克隆, 调用方负责 free */
        vm_error(vm, "模板引用未定义变量: %s", node->text);
        return val_null();
    }
    return val_int(node->ival);
}

/* 下标访问 base[index] */
static MoValue eval_index_access(VM* vm, const AstNode* node) {
    /* node->left = base 表达式, node->right 或 node->ival = 下标 */
    MoValue base = eval_expr(vm, node->left);
    long idx;
    if (node->right) {
        MoValue iv = eval_expr(vm, node->right);
        idx = iv.ival;
        value_free(&iv);
    } else {
        idx = node->ival;
    }
    MoValue res = val_null();
    if (base.type == VAL_ARRAY && base.arr) {
        if (idx < 0 || idx >= base.arr->count) {
            vm_error(vm, "数组下标越界: %ld (len=%d)", idx, base.arr->count);
        } else {
            res = value_clone(&base.arr->items[idx]);
        }
    } else if (base.type == VAL_STR && base.sval) {
        if (idx < 0 || idx >= (long)strlen(base.sval)) {
            vm_error(vm, "字符串下标越界: %ld", idx);
        } else {
            char c[2] = { base.sval[idx], '\0' };
            res = val_str(c);
        }
    } else {
        vm_error(vm, "对非数组/字符串做下标访问");
    }
    value_free(&base);
    return res;
}

/* 尝试作为原生张量算子调用。识别则返回结果(非 NULL); 否则返回 VAL_NULL。 */
/* 前向声明 (ND tensor 辅助) */
static NdTensor* __nd_get_arg(const MoValue* v);

static MoValue try_tensor_call(VM* vm, const char* name, const NodeList* args) {
    /* 需要至少 1 个参数的算子 */
    if (args->count < 1) return val_null();
    /* 求值所有参数 */
    int n = args->count;
    MoValue av[16];
    memset(av, 0, sizeof(av));
    for (int i = 0; i < n && i < 16; i++) av[i] = eval_expr(vm, args->items[i]);
    MoValue r = val_null();

    if (strcmp(name, "matmul") == 0 && n >= 2) {
        r = xm_matmul(&av[0], &av[1]);
    } else if (strcmp(name, "bias_add") == 0 && n >= 2) {
        r = xm_bias_add(&av[0], &av[1]);
    } else if (strcmp(name, "add") == 0 && n >= 2) {
        r = xm_bias_add(&av[0], &av[1]);
    } else if (strcmp(name, "relu") == 0) {
        r = xm_relu(&av[0]);
    } else if (strcmp(name, "tanh") == 0) {
        r = xm_tanh(&av[0]);
    } else if (strcmp(name, "sigmoid") == 0) {
        r = xm_sigmoid(&av[0]);
    } else if (strcmp(name, "softmax") == 0) {
        r = xm_softmax(&av[0]);
    } else if (strcmp(name, "hadamard") == 0 && n >= 2) {
        /* 逐元素乘 (Hadamard). 注意: 不能叫 "mul" —— 那是 VM 保留指令助记符
           (TOK_INST_MUL), lexer 会把它当关键字, 无法作为函数名调用。 */
        r = xm_mul(&av[0], &av[1]);
    } else if (strcmp(name, "diff") == 0 && n >= 2) {
        /* 逐元素相减 A-B. 注意: 不能叫 "sub" (TOK_INST_SUB 保留) */
        r = xm_sub(&av[0], &av[1]);
    } else if ((strcmp(name, "scale") == 0 || strcmp(name, "scale_mul") == 0) && n >= 2) {
        r = xm_scale(&av[0], av[1].fval);
    } else if (strcmp(name, "loss_mse") == 0 && n >= 2) {
        r = xm_loss_mse(&av[0], &av[1]);
    } else if (strcmp(name, "loss_crossentropy") == 0 && n >= 2) {
        r = xm_loss_crossentropy(&av[0], &av[1]);
    } else if (strcmp(name, "grad_tanh") == 0) {
        r = xm_grad_tanh(&av[0]);
    } else if (strcmp(name, "grad_mse") == 0 && n >= 2) {
        r = xm_grad_mse(&av[0], &av[1]);
    } else if (strcmp(name, "transpose") == 0) {
        r = xm_transpose(&av[0]);
    } else if (strcmp(name, "mat_zeros") == 0 && n >= 2) {
        int rr = (int)av[0].ival, cc = (int)av[1].ival;
        r = xm_mat_zeros(rr, cc);
    } else if (strcmp(name, "mat_eye") == 0 && n >= 1) {
        r = xm_mat_eye((int)av[0].ival);
    } else if (strcmp(name, "load_weights") == 0 && n >= 2) {
        /* load_weights(path, key) -> 从 JSON 权重文件加载参数 (矩阵/向量) */
        const char* path = av[0].type == VAL_STR && av[0].sval ? av[0].sval : "";
        const char* key  = av[1].type == VAL_STR && av[1].sval ? av[1].sval : "";
        char errbuf[256] = "";
        MoValue w; memset(&w, 0, sizeof(w));
        if (xm_load_weight(path, key, &w, errbuf, sizeof(errbuf)) == 0) {
            r = w;
        } else {
            vm_error(vm, "load_weights 失败: %s", errbuf[0] ? errbuf : "未知错误");
        }
    }

    /* ---- ND Tensor 算子 ---- */
    /* 从 MoValue 提取 NdTensor (兼容 VAL_TENSOR 和 VAL_ARRAY) */
    /* 注意: 返回的指针指向临时转换结果，调用方需在算子完成后释放 */
    #define ND_GET(idx) __nd_get_arg(&av[idx])
    /* (在后面定义) */

    /* 检测是否所有参数都是 VAL_ARRAY/VAL_TENSOR */
    if (r.type == VAL_NULL) {
        /* nd_create(shape..., ndim, data?) — 创建 ND 张量 */
        if (strcmp(name, "nd_create") == 0 || strcmp(name, "tensor") == 0) {
            if (n >= 1 && av[0].type == VAL_ARRAY && av[0].arr) {
                /* 从 VAL_ARRAY 嵌套数组转 ND tensor */
                NdTensor* t = nd_from_movalue(&av[0]);
                if (t) { r.type = VAL_TENSOR; r.tensor = t; }
            }
        } else if (strcmp(name, "nd_zeros") == 0 && n >= 1 && av[0].type == VAL_ARRAY) {
            /* nd_zeros([d0,d1,...]) */
            MoArray* shape_arr = av[0].arr;
            int* shape = (int*)malloc(sizeof(int) * (size_t)shape_arr->count);
            for (int i = 0; i < shape_arr->count; i++)
                shape[i] = (int)shape_arr->items[i].ival;
            NdTensor* t = nd_zeros(shape, shape_arr->count);
            free(shape);
            if (t) { r.type = VAL_TENSOR; r.tensor = t; }
        } else if ((strcmp(name, "nd_sphere_field") == 0 || strcmp(name, "sphere_field") == 0) && n >= 6) {
            int D = (int)av[0].ival, H = (int)av[1].ival, W = (int)av[2].ival;
            double radius = av[3].type == VAL_FLOAT ? av[3].fval : (double)av[3].ival;
            double cx    = av[4].type == VAL_FLOAT ? av[4].fval : (double)av[4].ival;
            double cy    = av[5].type == VAL_FLOAT ? av[5].fval : (double)av[5].ival;
            double cz    = n > 6 ? (av[6].type == VAL_FLOAT ? av[6].fval : (double)av[6].ival) : 0;
            NdTensor* t = nd_sphere_field(D, H, W, radius, cx, cy, cz);
            if (t) { r.type = VAL_TENSOR; r.tensor = t; }
        } else if ((strcmp(name, "nd_ellipsoid_field") == 0 || strcmp(name, "ellipsoid_field") == 0) && n >= 6) {
            int D = (int)av[0].ival, H = (int)av[1].ival, W = (int)av[2].ival;
            double rx = av[3].type == VAL_FLOAT ? av[3].fval : (double)av[3].ival;
            double ry = av[4].type == VAL_FLOAT ? av[4].fval : (double)av[4].ival;
            double rz = av[5].type == VAL_FLOAT ? av[5].fval : (double)av[5].ival;
            double cx = n > 6 ? (av[6].type == VAL_FLOAT ? av[6].fval : (double)av[6].ival) : 0;
            double cy = n > 7 ? (av[7].type == VAL_FLOAT ? av[7].fval : (double)av[7].ival) : 0;
            double cz = n > 8 ? (av[8].type == VAL_FLOAT ? av[8].fval : (double)av[8].ival) : 0;
            NdTensor* t = nd_ellipsoid_field(D, H, W, rx, ry, rz, cx, cy, cz);
            if (t) { r.type = VAL_TENSOR; r.tensor = t; }
        } else if ((strcmp(name, "nd_reshape") == 0 || strcmp(name, "reshape") == 0) && n >= 2) {
            NdTensor* t = ND_GET(0);
            MoArray* shape_arr = av[1].arr;
            int* shape = (int*)malloc(sizeof(int) * (size_t)shape_arr->count);
            for (int i = 0; i < shape_arr->count; i++) shape[i] = (int)shape_arr->items[i].ival;
            NdTensor* r2 = t ? nd_reshape(t, shape, shape_arr->count) : NULL;
            free(shape);
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_transpose") == 0 || strcmp(name, "transpose_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_transpose(t, NULL) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_add") == 0 || strcmp(name, "add_n") == 0) && n >= 2) {
            NdTensor *a = ND_GET(0), *b = ND_GET(1);
            NdTensor* r2 = (a && b) ? nd_add(a, b) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_sub") == 0 || strcmp(name, "sub_n") == 0) && n >= 2) {
            NdTensor *a = ND_GET(0), *b = ND_GET(1);
            NdTensor* r2 = (a && b) ? nd_sub(a, b) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_mul") == 0 || strcmp(name, "mul_n") == 0) && n >= 2) {
            NdTensor *a = ND_GET(0), *b = ND_GET(1);
            NdTensor* r2 = (a && b) ? nd_mul(a, b) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_div") == 0 || strcmp(name, "div_n") == 0) && n >= 2) {
            NdTensor *a = ND_GET(0), *b = ND_GET(1);
            NdTensor* r2 = (a && b) ? nd_div(a, b) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_relu") == 0 || strcmp(name, "relu_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_relu(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_tanh") == 0 || strcmp(name, "tanh_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_tanh(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_sigmoid") == 0 || strcmp(name, "sigmoid_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_sigmoid(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_exp") == 0 || strcmp(name, "exp_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_exp(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_log") == 0 || strcmp(name, "log_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_log(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_abs") == 0 || strcmp(name, "abs_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_abs(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_sqrt") == 0 || strcmp(name, "sqrt_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_sqrt(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_sum") == 0 || strcmp(name, "sum_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            int axis = (n >= 2) ? (int)av[1].ival : -1;
            NdTensor* r2 = t ? nd_sum(t, axis) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_mean") == 0 || strcmp(name, "mean_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            int axis = (n >= 2) ? (int)av[1].ival : -1;
            NdTensor* r2 = t ? nd_mean(t, axis) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_min") == 0 || strcmp(name, "min_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            int axis = (n >= 2) ? (int)av[1].ival : -1;
            NdTensor* r2 = t ? nd_min(t, axis) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_max") == 0 || strcmp(name, "max_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            int axis = (n >= 2) ? (int)av[1].ival : -1;
            NdTensor* r2 = t ? nd_max(t, axis) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_softmax") == 0 || strcmp(name, "softmax_n") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            int axis = (n >= 2) ? (int)av[1].ival : -1;
            NdTensor* r2 = t ? nd_softmax(t, axis) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_matmul") == 0 || strcmp(name, "matmul_n") == 0) && n >= 2) {
            NdTensor *a = ND_GET(0), *b = ND_GET(1);
            NdTensor* r2 = (a && b) ? nd_matmul(a, b) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_conv2d") == 0 || strcmp(name, "conv2d") == 0) && n >= 5) {
            NdTensor *input = ND_GET(0), *kernel = ND_GET(1);
            int ph = (int)av[2].ival, pw = (int)av[3].ival;
            int sh = (int)av[4].ival, sw = (n >= 6) ? (int)av[5].ival : sh;
            NdTensor* r2 = (input && kernel) ? nd_conv2d(input, kernel, ph, pw, sh, sw) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_maxpool2d") == 0 || strcmp(name, "maxpool2d") == 0) && n >= 5) {
            NdTensor* input = ND_GET(0);
            int kh = (int)av[1].ival, kw = (int)av[2].ival;
            int ph = (int)av[3].ival, pw = (int)av[4].ival;
            int sh = (n >= 6) ? (int)av[5].ival : kh;
            int sw = (n >= 7) ? (int)av[6].ival : kw;
            NdTensor* r2 = input ? nd_maxpool2d(input, kh, kw, ph, pw, sh, sw) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_rgb2hsv") == 0 || strcmp(name, "rgb2hsv") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_rgb2hsv(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_hsv2rgb") == 0 || strcmp(name, "hsv2rgb") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_hsv2rgb(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_grayscale") == 0 || strcmp(name, "grayscale") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_grayscale(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_slice") == 0 || strcmp(name, "slice_n") == 0) && n >= 4) {
            NdTensor* t = ND_GET(0);
            MoArray* starts_a = av[1].arr;
            MoArray* ends_a = av[2].arr;
            MoArray* steps_a = av[3].arr;
            if (t && starts_a && ends_a) {
                int* starts = (int*)malloc(sizeof(int) * (size_t)starts_a->count);
                int* ends = (int*)malloc(sizeof(int) * (size_t)ends_a->count);
                int* steps = steps_a ? (int*)malloc(sizeof(int) * (size_t)steps_a->count) : NULL;
                for (int i = 0; i < starts_a->count; i++) starts[i] = (int)starts_a->items[i].ival;
                for (int i = 0; i < ends_a->count; i++) ends[i] = (int)ends_a->items[i].ival;
                if (steps_a) for (int i = 0; i < steps_a->count; i++) steps[i] = (int)steps_a->items[i].ival;
                NdTensor* r2 = nd_slice(t, starts, ends, steps);
                free(starts); free(ends); free(steps);
                if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
            }
        } else if ((strcmp(name, "nd_to_array") == 0 || strcmp(name, "tensor_to_array") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            if (t) r = nd_to_movalue(t);
        } else if (strcmp(name, "nd_pad") == 0 && n >= 4) {
            NdTensor* t = ND_GET(0);
            int axis = (int)av[1].ival, before = (int)av[2].ival, after = (int)av[3].ival;
            NdTensor* r2 = t ? nd_pad(t, axis, before, after) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_concat") == 0 || strcmp(name, "concat_n") == 0) && n >= 3) {
            NdTensor *a = ND_GET(0), *b = ND_GET(1);
            int axis = (int)av[2].ival;
            if (a && b) {
                const NdTensor* arr[2] = {a, b};
                NdTensor* r2 = nd_concat(arr, 2, axis);
                if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
            }
        } else if ((strcmp(name, "nd_upsample_nearest") == 0 || strcmp(name, "upsample_n") == 0) && n >= 3) {
            NdTensor* t = ND_GET(0);
            int sh = (int)av[1].ival, sw = (int)av[2].ival;
            NdTensor* r2 = t ? nd_upsample_nearest(t, sh, sw) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_gradient3d") == 0 || strcmp(name, "gradient3d") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            NdTensor* r2 = t ? nd_gradient3d(t) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_marching_cubes") == 0 || strcmp(name, "marching_cubes") == 0) && n >= 2) {
            NdTensor* t = ND_GET(0);
            double iso = av[1].type == VAL_FLOAT ? av[1].fval : (double)av[1].ival;
            NdTensor* r2 = t ? nd_marching_cubes(t, iso) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_rasterize_tri") == 0 || strcmp(name, "rasterize_tri") == 0) && n >= 4) {
            NdTensor* verts = ND_GET(0);
            NdTensor* tris = NULL;
            if (av[1].type == VAL_TENSOR) tris = (NdTensor*)av[1].tensor;
            int w = (int)av[2].ival;
            int h = (int)av[3].ival;
            NdTensor* r2 = verts ? nd_rasterize_triangles(verts, tris, w, h) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "nd_rasterize_wireframe") == 0 || strcmp(name, "rasterize_wire") == 0) && n >= 4) {
            NdTensor* verts = ND_GET(0);
            NdTensor* tris = NULL;
            if (av[1].type == VAL_TENSOR) tris = (NdTensor*)av[1].tensor;
            int w = (int)av[2].ival;
            int h = (int)av[3].ival;
            NdTensor* r2 = verts ? nd_rasterize_wireframe(verts, tris, w, h) : NULL;
            if (r2) { r.type = VAL_TENSOR; r.tensor = r2; }
        } else if ((strcmp(name, "render_raytrace") == 0 || strcmp(name, "raytrace") == 0) && n >= 14) {
            int W = (int)av[0].ival, H = (int)av[1].ival;
            double cx = av[2].type == VAL_FLOAT ? av[2].fval : (double)av[2].ival;
            double cy = av[3].type == VAL_FLOAT ? av[3].fval : (double)av[3].ival;
            double cz = av[4].type == VAL_FLOAT ? av[4].fval : (double)av[4].ival;
            double sphere_r = av[5].type == VAL_FLOAT ? av[5].fval : (double)av[5].ival;
            double cam_x = av[6].type == VAL_FLOAT ? av[6].fval : (double)av[6].ival;
            double cam_y = av[7].type == VAL_FLOAT ? av[7].fval : (double)av[7].ival;
            double cam_z = av[8].type == VAL_FLOAT ? av[8].fval : (double)av[8].ival;
            double light_x = av[9].type == VAL_FLOAT ? av[9].fval : (double)av[9].ival;
            double light_y = av[10].type == VAL_FLOAT ? av[10].fval : (double)av[10].ival;
            double light_z = av[11].type == VAL_FLOAT ? av[11].fval : (double)av[11].ival;
            double ambient = av[12].type == VAL_FLOAT ? av[12].fval : (double)av[12].ival;
            double diffuse_k = av[13].type == VAL_FLOAT ? av[13].fval : (double)av[13].ival;
            NdTensor* img = nd_render_raytrace(W, H, cx, cy, cz, sphere_r,
                                                cam_x, cam_y, cam_z,
                                                light_x, light_y, light_z,
                                                ambient, diffuse_k);
            if (img) { r.type = VAL_TENSOR; r.tensor = img; }
        } else if ((strcmp(name, "nd_print") == 0 || strcmp(name, "nd_info") == 0) && n >= 1) {
            NdTensor* t = ND_GET(0);
            if (t) { nd_print_info(t, ""); }
            r = val_int(0);
        } else if ((strcmp(name, "nd_dim") == 0 || strcmp(name, "dim_n") == 0) && n >= 2) {
            NdTensor* t = ND_GET(0);
            int axis = (int)av[1].ival;
            int d = t ? nd_dim(t, axis) : 0;
            r = val_int((long)d);
        }
    }

    /* 释放临时参数 (算子已克隆/复制所需数据) */
    for (int i = 0; i < n && i < 16; i++) value_free(&av[i]);
    return r;
}
#undef ND_GET

/* 辅助宏: 从 MoValue 提取 NdTensor* (兼容 VAL_TENSOR/VAL_ARRAY/VAL_FLOAT/VAL_INT) */
static NdTensor* __nd_get_arg(const MoValue* v) {
    if (!v) return NULL;
    if (v->type == VAL_TENSOR) return (NdTensor*)v->tensor;
    if (v->type == VAL_ARRAY) return nd_from_movalue(v);
    if (v->type == VAL_FLOAT) return nd_scalar(v->fval);
    if (v->type == VAL_INT) return nd_scalar((double)v->ival);
    return NULL;
}

static MoValue eval_expr(VM* vm, const AstNode* node) {
    if (!node) return val_null();
    switch (node->type) {
    case NODE_LITERAL:
        if (node->mtype == TYPE_STR) return val_str(node->text ? node->text : "");
        if (node->mtype == TYPE_FLOAT) return val_float(node->fval);
        if (node->mtype == TYPE_BOOL) return val_bool(node->ival != 0);
        return val_int(node->ival);
    case NODE_EXPR: {
        VarSlot* s = scope_lookup(vm, node->text);
        if (s) return value_clone(&s->value);
        vm_error(vm, "未定义变量: %s", node->text);
        return val_null();
    }
    case NODE_TEMPLATE_REF:
        return eval_template_ref(vm, node);
    case NODE_BINOP:
        return eval_binop(vm, node);
    case NODE_ARRAY_LIT: {
        MoValue arr = val_array_empty();
        for (int i = 0; i < node->args.count; i++) {
            MoValue e = eval_expr(vm, node->args.items[i]);
            arr_append(arr.arr, e);
        }
        return arr;
    }
    case NODE_INDEX_ACCESS:
        return eval_index_access(vm, node);
    case NODE_INDEX: {
        /* (旧) 数组输出拼接 [a, b] -> 字符串 */
        char buf[1024] = "";
        for (int i = 0; i < node->args.count; i++) {
            MoValue v = eval_expr(vm, node->args.items[i]);
            char tmp[128]; mo_value_to_str(&v, tmp, sizeof(tmp));
            if (i > 0) strncat(buf, ", ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
            value_free(&v);
        }
        return val_str(buf);
    }
    case NODE_CALL: {
        /* 函数调用作为表达式 -> 返回函数返回值 */
        const AstNode* fn = lookup_function(vm, node->text ? node->text : "");
        if (!fn) {
            /* 内置 print */
            if (node->text && strcmp(node->text, "print") == 0) {
                size_t bsz = 8 * 1024 * 1024 + 64;
                char* buf = (char*)malloc(bsz);
                for (int i = 0; i < node->args.count; i++) {
                    MoValue v = eval_expr(vm, node->args.items[i]);
                    mo_value_to_str(&v, buf, bsz);
                    vm_add_output(vm, buf);
                    value_free(&v);
                }
                free(buf);
                return val_null();
            }
            /* 原生张量算子 (模型推理内核) */
            if (node->text) {
                MoValue r = try_tensor_call(vm, node->text, &node->args);
                if (r.type != VAL_NULL || node->args.count == 0) {
                    /* 算子被识别 (返回非-null 结果); 空参数视为未识别 */
                    if (r.type != VAL_NULL) return r;
                }
            }
            vm_error(vm, "调用未定义函数: %s", node->text ? node->text : "?");
            return val_null();
        }
        return call_function(vm, fn, &node->args);
    }
    default:
        return val_null();
    }
}

/* ---------- 指令执行 (简化内存模拟) ---------- */
static unsigned char g_mem[65536];

static void exec_instruction(VM* vm, const AstNode* node) {
    long addr = 0;
    if (node->text && node->text[0] == '0' && node->text[1] == 'x') {
        addr = strtol(node->text, NULL, 16);
    } else if (node->text) {
        addr = 0;
        for (const char* c = node->text; *c; c++) addr = (addr * 31 + *c) & 0xFFFF;
    }
    long op = node->left ? node->left->ival : 0;
    TokenType inst = (TokenType)node->inst;

    switch (inst) {
    case TOK_INST_MOV:
        if (addr < (long)sizeof(g_mem)) g_mem[addr % sizeof(g_mem)] = (unsigned char)(op & 0xFF);
        break;
    case TOK_INST_ADD: g_mem[addr % sizeof(g_mem)] += (unsigned char)(op & 0xFF); break;
    case TOK_INST_SUB: g_mem[addr % sizeof(g_mem)] -= (unsigned char)(op & 0xFF); break;
    case TOK_INST_MUL: g_mem[addr % sizeof(g_mem)] *= (unsigned char)(op & 0xFF); break;
    case TOK_INST_AND: g_mem[addr % sizeof(g_mem)] &= (unsigned char)(op & 0xFF); break;
    case TOK_INST_OR:  g_mem[addr % sizeof(g_mem)] |= (unsigned char)(op & 0xFF); break;
    case TOK_INST_XOR: g_mem[addr % sizeof(g_mem)] ^= (unsigned char)(op & 0xFF); break;
    case TOK_INST_NOP: break;
    default: break;
    }
    (void)vm;
}

/* ---------- 数据声明 ---------- */
static void exec_data_decl(VM* vm, const AstNode* node) {
    char buf[1024] = "";
    for (int i = 0; i < node->args.count; i++) {
        AstNode* item = node->args.items[i];
        if (item->type == NODE_LITERAL && item->mtype == TYPE_STR) {
            strncat(buf, item->text ? item->text : "", sizeof(buf) - strlen(buf) - 1);
        } else if (item->type == NODE_LITERAL && item->mtype == TYPE_INT) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%02X", (unsigned int)(item->ival & 0xFF));
            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
            if (i < node->args.count - 1) strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
        }
    }
    vm_add_output(vm, buf);
}

/* ---------- 语句执行 ---------- */
void exec_block(VM* vm, const AstNode* block) {
    if (!block) return;
    for (int i = 0; i < block->body.count; i++) {
        exec_stmt(vm, block->body.items[i]);
        if (vm->break_flag || vm->continue_flag || vm->return_flag || vm->error_count) return;
    }
}

static void exec_fn_decl(VM* vm, const AstNode* node) {
    /* 函数声明在 vm_run 第一遍已收集到 vm->fn_defs, 这里无需执行 */
    (void)vm; (void)node;
}

static void exec_stmt(VM* vm, const AstNode* node) {
    if (!node || vm->error_count) return;
    switch (node->type) {
    case NODE_VAR_DECL: {
        MoValue v = val_null();
        if (node->left) v = eval_expr(vm, node->left);
        if (node->mtype == TYPE_STR && v.type != VAL_STR && v.type != VAL_ARRAY) {
            char buf[64]; mo_value_to_str(&v, buf, sizeof(buf));
            value_free(&v);
            v = val_str(buf);
        }
        /* 重声明语义 = "确保存在 + 赋值": 先沿作用域链找已有定义,
         * 命中(含全局/外层, 函数内重声明全局)则原地更新 —— 与编译器
         * declare_var 复用全局寄存器语义一致 (probe4/probe3);
         * 未命中才在最内层作用域新声明。 */
        VarSlot* existing = scope_lookup(vm, node->text);
        if (existing) {
            value_free(&existing->value);
            existing->value = v;
            existing->mtype = node->mtype;
        } else {
            var_table_set(&vm->scopes[vm->scope_count-1], node->text, v, node->mtype);
        }
        break;
    }
    case NODE_CONST_DECL:
        var_table_set(&vm->scopes[vm->scope_count-1], node->text, val_int(node->ival), node->mtype);
        break;
    case NODE_ASSIGN: {
        /* 数组元素赋值 a[i] = v (节点 text 标记为 "[]@") */
        if (node->text && strcmp(node->text, "[]@") == 0 && node->left &&
            node->left->type == NODE_INDEX_ACCESS) {
            const AstNode* ia = node->left;
            /* 求下标 */
            long idx;
            if (ia->right) { MoValue iv = eval_expr(vm, ia->right); idx = iv.ival; value_free(&iv); }
            else idx = ia->ival;
            MoValue v = eval_expr(vm, node->right);
            /* 取被索引的数组 (直接改原变量引用) */
            MoArray* target_arr = NULL;
            /* 情况1: 纯变量名 a[i] */
            if (ia->left && ia->left->type == NODE_EXPR && ia->left->text) {
                VarSlot* s = scope_lookup(vm, ia->left->text);
                if (s && s->value.type == VAL_ARRAY && s->value.arr) target_arr = s->value.arr;
            }
            /* 情况2: 模板引用 ${a}[i] */
            if (!target_arr && ia->left && ia->left->type == NODE_TEMPLATE_REF && ia->left->text) {
                VarSlot* s = scope_lookup(vm, ia->left->text);
                if (s && s->value.type == VAL_ARRAY && s->value.arr) target_arr = s->value.arr;
            }
            if (target_arr) {
                if (idx < 0 || idx >= target_arr->count) { vm_error(vm, "数组下标越界(赋值): %ld", idx); value_free(&v); }
                else { value_free(&target_arr->items[idx]); target_arr->items[idx] = v; }
            } else {
                vm_error(vm, "对非数组变量做元素赋值");
                value_free(&v);
            }
            break;
        }
        MoValue v = eval_expr(vm, node->left);
        VarSlot* s = scope_lookup(vm, node->text);
        if (s) {
            value_free(&s->value);
            s->value = v;
        } else {
            var_table_set(&vm->scopes[vm->scope_count-1], node->text, v, TYPE_UNKNOWN);
        }
        break;
    }
    case NODE_PRINT: {
        size_t bsz = 8 * 1024 * 1024 + 64;
        char* buf = (char*)malloc(bsz);
        buf[0] = '\0'; size_t off = 0;
        for (int i = 0; i < node->args.count; i++) {
            MoValue v = eval_expr(vm, node->args.items[i]);
            size_t cap = bsz - off;
            if (cap > 16) mo_value_to_str(&v, buf + off, (int)cap);
            off = strlen(buf);
            value_free(&v);
        }
        vm_add_output(vm, buf);
        free(buf);
        break;
    }
    case NODE_IF: {
        MoValue c = eval_expr(vm, node->cond);
        if (truthy(&c)) exec_block(vm, node->then_block);
        else if (node->else_block) exec_block(vm, node->else_block);
        break;
    }
    case NODE_WHILE: {
        while (1) {
            if (vm->error_count) break;
            MoValue c = eval_expr(vm, node->cond);
            if (!truthy(&c)) break;
            exec_block(vm, node->then_block);
            if (vm->break_flag) { vm->break_flag = 0; break; }
            if (vm->continue_flag) { vm->continue_flag = 0; continue; }
            if (vm->return_flag) break;
        }
        break;
    }
    case NODE_BLOCK:
        exec_block(vm, node);
        break;
    case NODE_RETURN:
        if (node->left) {
            MoValue v = eval_expr(vm, node->left);
            value_free(&vm->return_value);
            vm->return_value = v;
        }
        vm->return_flag = 1;
        break;
    case NODE_BREAK:
        vm->break_flag = 1;
        break;
    case NODE_CONTINUE:
        vm->continue_flag = 1;
        break;
    case NODE_INSTRUCTION:
        exec_instruction(vm, node);
        break;
    case NODE_DATA_DECL:
        exec_data_decl(vm, node);
        break;
    case NODE_FN_DECL:
        exec_fn_decl(vm, node);
        break;
    case NODE_CALL: {
        /* 函数调用作为语句 (丢弃返回值) */
        const AstNode* fn = lookup_function(vm, node->text ? node->text : "");
        if (fn) {
            MoValue r = call_function(vm, fn, &node->args);
            value_free(&r);
        } else {
            MoValue r = eval_expr(vm, node); /* 处理内置 print 等 */
            value_free(&r);
        }
        break;
    }
    case NODE_EXPR: {
        MoValue v = eval_expr(vm, node);
        value_free(&v);
        break;
    }
    default:
        break;
    }
}

int vm_run(VM* vm, const AstNode* program) {
    if (!program || program->type != NODE_PROGRAM) {
        vm_error(vm, "无效程序");
        return 1;
    }
    /* 第一遍: 收集函数定义到 per-VM 函数表 */
    vm->fn_def_count = 0;
    for (int i = 0; i < program->body.count; i++) {
        const AstNode* stmt = program->body.items[i];
        if (stmt->type == NODE_FN_DECL) {
            if (vm->fn_def_count >= vm->fn_def_cap) {
                vm->fn_def_cap = vm->fn_def_cap ? vm->fn_def_cap*2 : 16;
                vm->fn_defs = (const AstNode**)realloc(vm->fn_defs, sizeof(const AstNode*)*vm->fn_def_cap);
            }
            vm->fn_defs[vm->fn_def_count++] = stmt;
        }
    }
    /* 第二遍: 执行 (顶层函数声明不执行) */
    for (int i = 0; i < program->body.count; i++) {
        const AstNode* stmt = program->body.items[i];
        if (stmt->type == NODE_FN_DECL) continue;
        exec_stmt(vm, stmt);
        if (vm->error_count) return 1;
    }
    return 0;
}
