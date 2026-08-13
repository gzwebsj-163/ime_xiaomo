/*
 * xiaomo - Mo/XScript 语言 token 定义
 * 依据 lang_spec.md (XScript Language Specification v1.0)
 *
 * 作者: xiaomo 项目
 * 架构: 纯 C 实现 .mo 解析 + VM 执行
 */
#ifndef XIAOMO_TOKEN_H
#define XIAOMO_TOKEN_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Token 类型枚举 */
typedef enum {
    TOK_EOF = 0,

    /* 字面量 */
    TOK_IDENT,      /* 标识符: name, _x64, 0xNN_name */
    TOK_INT,        /* 十进制/十六进制整数 */
    TOK_STR,        /* 字符串 "..." */
    TOK_FLOAT,      /* 浮点数 */
    TOK_BYTES,      /* bytes 数据 */

    /* 关键字 */
    TOK_VOID,       /* void */
    TOK_CONST,      /* const */
    TOK_FN,         /* fn */
    TOK_FUNC,       /* func (旧版) */
    TOK_IF,         /* if */
    TOK_ELSE,       /* else */
    TOK_WHILE,      /* while */
    TOK_FOR,        /* for */
    TOK_RETURN,     /* return */
    TOK_NULL,       /* null */
    TOK_TRY,        /* try */
    TOK_CATCH,      /* catch */
    TOK_SWITCH,     /* switch */
    TOK_CASE,       /* case */
    TOK_DEFAULT,    /* default */
    TOK_BREAK,      /* break */
    TOK_CONTINUE,   /* continue */
    TOK_CLASS,      /* class */
    TOK_EXTENDS,    /* extends */
    TOK_THIS,       /* this */
    TOK_SUPER,      /* super */
    TOK_PRINT,      /* print */
    TOK_TRUE,       /* true */
    TOK_FALSE,      /* false */

    /* 类型 */
    TOK_TYPE_INT,   /* int */
    TOK_TYPE_STR,   /* str */
    TOK_TYPE_FLOAT, /* float */
    TOK_TYPE_BOOL,  /* bool */
    TOK_TYPE_BYTES, /* bytes */
    TOK_TYPE_UINT8,  /* uint8 */
    TOK_TYPE_UINT16, /* uint16 */
    TOK_TYPE_UINT32, /* uint32 */
    TOK_TYPE_UINT64, /* uint64 */
    TOK_TYPE_INT8,   /* int8 */
    TOK_TYPE_INT16,  /* int16 */
    TOK_TYPE_INT32,  /* int32 */
    TOK_TYPE_INT64,  /* int64 */

    /* 指令 (汇编风格) */
    TOK_INST_JMP,   /* jmp */
    TOK_INST_JNZ,   /* jnz */
    TOK_INST_JZ,    /* jz */
    TOK_INST_JE,    /* je */
    TOK_INST_JNE,   /* jne */
    TOK_INST_JG,    /* jg */
    TOK_INST_JL,    /* jl */
    TOK_INST_MOV,   /* mov */
    TOK_INST_DNP,   /* dnp dynamic pointer */
    TOK_INST_PUSH,  /* push */
    TOK_INST_POP,   /* pop */
    TOK_INST_ADD,   /* add */
    TOK_INST_SUB,   /* sub */
    TOK_INST_MUL,   /* mul */
    TOK_INST_DIV,   /* div */
    TOK_INST_AND,   /* and */
    TOK_INST_OR,    /* or */
    TOK_INST_XOR,   /* xor */
    TOK_INST_NOT,   /* not */
    TOK_INST_CALL,  /* call */
    TOK_INST_RET,   /* ret */
    TOK_INST_NOP,   /* nop */

    /* 标点/运算符 */
    TOK_COLON,      /* : */
    TOK_SEMICOLON,  /* ; */
    TOK_COMMA,      /* , */
    TOK_DOT,        /* . */
    TOK_LPAREN,     /* ( */
    TOK_RPAREN,     /* ) */
    TOK_LBRACE,     /* { */
    TOK_RBRACE,     /* } */
    TOK_LBRACKET,   /* [ */
    TOK_RBRACKET,   /* ] */
    TOK_ASSIGN,     /* = */
    TOK_PLUS,       /* + */
    TOK_MINUS,      /* - */
    TOK_STAR,       /* * */
    TOK_SLASH,      /* / */
    TOK_PERCENT,    /* % */
    TOK_EQ,         /* == */
    TOK_NEQ,        /* != */
    TOK_GT,         /* > */
    TOK_LT,         /* < */
    TOK_GE,         /* >= */
    TOK_LE,         /* <= */
    TOK_AND_AND,    /* && */
    TOK_OR_OR,      /* || */
    TOK_NOT,        /* ! */
    TOK_RSHIFT,     /* >> (右移/输出流/链接) */
    TOK_LSHIFT,     /* << */
    TOK_AMP,        /* & */
    TOK_PIPE,       /* | */
    TOK_CARET,      /* ^ */
    TOK_ARROW,      /* -> */
    TOK_DOLLAR_LBRACE, /* ${ */
    TOK_HASH,       /* # (注释) */
    TOK_NEWLINE,    /* 换行 */
    TOK_INDENT,     /* 缩进增加 */
    TOK_DEDENT,     /* 缩进减少 */

    /* 内存地址引用形式: 0xNN_name 或 name 后接 : 指令 */
    TOK_ADDR_REF
} TokenType;

typedef struct {
    TokenType type;
    const char* lexeme;   /* 指向源文本的起始(非拷贝) */
    int length;           /* lexeme 长度 */
    long ival;            /* 整数值 */
    double fval;          /* 浮点值 */
    int line;             /* 行号(1基) */
    int col;              /* 列号(1基) */
} Token;

/* 判断 token 是否是类型关键字 */
int token_is_type(TokenType t);

/* 获取 token 类型名称 (调试用) */
const char* token_type_name(TokenType t);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_TOKEN_H */
