/*
 * xiaomo - AST 节点定义
 */
#ifndef XIAOMO_AST_H
#define XIAOMO_AST_H

#include "token.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NODE_PROGRAM,          /* 程序根 */
    NODE_VAR_DECL,         /* void name : type = value */
    NODE_CONST_DECL,       /* const name : type = addr : value */
    NODE_ASSIGN,           /* name = expr */
    NODE_PRINT,            /* >> print >> ... */
    NODE_IF,               /* if cond: ... */
    NODE_WHILE,            /* while cond: ... */
    NODE_FOR,              /* for ... */
    NODE_FN_DECL,          /* fn name(params) { } */
    NODE_CALL,             /* name(args) */
    NODE_EXPR,             /* 表达式 */
    NODE_INSTRUCTION,      /* 汇编指令 addr : inst = op */
    NODE_DATA_DECL,        /* >> uint32 : ... >> uint16 : ... */
    NODE_BLOCK,            /* 语句块 */
    NODE_TEMPLATE_REF,     /* ${name} 或 ${0xNN} */
    NODE_BINOP,            /* 二元运算 */
    NODE_LITERAL,          /* 字面量 */
    NODE_INDEX,            /* (旧) 数组输出拼接 */
    NODE_ARRAY_LIT,        /* 数组字面量 [e1, e2, ...] */
    NODE_INDEX_ACCESS,     /* 下标访问 base[index] */
    NODE_RETURN,           /* return */
    NODE_BREAK,
    NODE_CONTINUE
} NodeType;

typedef enum {
    TYPE_UNKNOWN, TYPE_INT, TYPE_STR, TYPE_FLOAT, TYPE_BOOL, TYPE_BYTES,
    TYPE_UINT8, TYPE_UINT16, TYPE_UINT32, TYPE_UINT64,
    TYPE_INT8, TYPE_INT16, TYPE_INT32, TYPE_INT64
} MoType;

typedef struct AstNode AstNode;

typedef struct {
    AstNode** items;
    int count;
    int capacity;
} NodeList;

struct AstNode {
    NodeType type;
    int line;
    int col;
    /* 字面量/标识符 */
    char* text;         /* 标识符名 / 字符串内容 */
    long ival;
    double fval;
    MoType mtype;
    /* 子节点 */
    AstNode* left;
    AstNode* right;
    AstNode* cond;
    AstNode* then_block;
    AstNode* else_block;
    NodeList body;      /* 语句块 */
    NodeList args;      /* 调用参数 / 声明列表 */
    /* 指令附加信息 */
    TokenType inst;     /* 指令类型 (TOK_INST_*) */
};

/* 创建节点 */
AstNode* ast_new(NodeType type, int line, int col);
void ast_set_text(AstNode* n, const char* text);
void ast_set_text_len(AstNode* n, const char* text, int len);
void node_list_add(NodeList* list, AstNode* node);
void ast_free(AstNode* node);
void node_list_free(NodeList* list);

/* 调试打印 AST */
void ast_dump(const AstNode* node, int depth);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_AST_H */
