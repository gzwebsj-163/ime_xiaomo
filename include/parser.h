/*
 * xiaomo - 语法分析器
 * 依据 lang_spec.md 构建 AST
 */
#ifndef XIAOMO_PARSER_H
#define XIAOMO_PARSER_H

#include "token.h"
#include "ast.h"
#include "lexer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    TokenList tokens;
    int pos;
    int error_count;
    char error_msg[1024];
} Parser;

/* 解析 token 序列为 AST 程序 */
AstNode* parser_parse(Parser* p);

/* 从源码解析 (便捷入口) */
AstNode* xiaomo_parse_source(const char* src, char* errbuf, int errlen);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_PARSER_H */
