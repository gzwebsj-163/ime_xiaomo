/*
 * xiaomo - 词法分析器
 * 将 .mo 源码转换为 Token 序列
 */
#ifndef XIAOMO_LEXER_H
#define XIAOMO_LEXER_H

#include "token.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* src;
    int pos;
    int len;
    int line;
    int col;
    int error_count;
    char error_msg[512];
} Lexer;

/* Token 动态数组 */
typedef struct {
    Token* items;
    int count;
    int capacity;
} TokenList;

/* 初始化词法分析器 */
void lexer_init(Lexer* lx, const char* src);

/* 生成全部 token */
TokenList lexer_tokenize(Lexer* lx);

/* 释放 token 列表 */
void token_list_free(TokenList* list);

/* 调试: 打印 token 序列 */
void token_list_dump(const TokenList* list);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_LEXER_H */
