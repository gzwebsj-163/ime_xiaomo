/*
 * xiaomo - VM 执行引擎
 * 执行 AST，复用 linux-xiaomo 的 stack_memory 作为执行栈
 */
#ifndef XIAOMO_VM_H
#define XIAOMO_VM_H

#include "ast.h"
#include "token.h"

#ifdef __cplusplus
extern "C" {
#endif

/* VM 值类型 */
typedef enum {
    VAL_INT, VAL_FLOAT, VAL_STR, VAL_BOOL, VAL_BYTES, VAL_ARRAY, VAL_TENSOR, VAL_NULL
} ValueType;

/* 前向声明 */
typedef struct NdTensor NdTensor;

/* 数组值: 元素为 MoValue (同构或异构) */
typedef struct {
    struct MoValueInner* items;   /* 动态数组 */
    int count;
    int capacity;
} MoArray;

struct MoValueInner {
    ValueType type;
    long ival;
    double fval;
    char* sval;   /* 字符串 */
    MoArray* arr; /* 数组 (VAL_ARRAY 时用) */
    void* tensor; /* NdTensor* (VAL_TENSOR 时用) */
};
typedef struct MoValueInner MoValue;

typedef struct {
    char* name;
    MoValue value;
    MoType mtype;
} VarSlot;

typedef struct {
    VarSlot* items;
    int count;
    int capacity;
} VarTable;

typedef struct {
    VarTable vars;      /* 全局变量 */
    VarTable functions; /* 函数 (值存函数索引) */
    /* 作用域栈: scopes[0] 为全局, 后续为调用产生的局部作用域 */
    VarTable* scopes;
    int scope_count;
    int scope_cap;
    int call_depth;     /* 当前调用深度 (限制递归防止爆栈) */
    int frame_scope;    /* 当前函数帧起始作用域下标 */
    /* 函数定义表 (per-VM, 存 NODE_FN_DECL 节点指针) */
    const struct AstNode** fn_defs;
    int fn_def_count;
    int fn_def_cap;

    char** output;      /* 输出行 */
    int output_count;
    int output_cap;
    /* 执行控制 */
    int break_flag;
    int continue_flag;
    int return_flag;
    MoValue return_value;
    /* 错误 */
    int error_count;
    char error_msg[1024];
} VM;

/* 初始化 VM */
void vm_init(VM* vm);
/* 执行 AST 程序 */
int vm_run(VM* vm, const AstNode* program);
/* 输出获取 */
const char* vm_output(VM* vm, int idx);
int vm_output_count(const VM* vm);
/* 释放 VM */
void vm_free(VM* vm);
/* 全局变量查询 (调试) */
const MoValue* vm_get_global(const VM* vm, const char* name);

/* 值打印到缓冲区 */
void mo_value_to_str(const MoValue* v, char* buf, int buflen);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_VM_H */
