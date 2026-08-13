/*
 * xiaomo - 语法分析器实现
 * 依据 lang_spec.md (XScript v1.0)
 *
 * 支持语句:
 *   void name : type = value           变量声明
 *   const name : type = addr : value   常量声明
 *   addr : inst = op : extra           内存指令
 *   >> print >> msg                    输出
 *   >> uint32 : data >> uint16 : data  数据声明
 *   if cond: ... else: ...             条件
 *   while cond: ...                    循环
 *   fn name(params) { ... }            函数
 *   name = expr                        赋值
 *   return expr                        返回
 */
#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const Token* cur(Parser* p) {
    if (p->pos >= p->tokens.count) return &p->tokens.items[p->tokens.count - 1];
    return &p->tokens.items[p->pos];
}
static const Token* peek(Parser* p, int k) {
    int i = p->pos + k;
    if (i >= p->tokens.count) return &p->tokens.items[p->tokens.count - 1];
    return &p->tokens.items[i];
}
static void advance(Parser* p) {
    if (p->pos < p->tokens.count) p->pos++;
}
static int check(Parser* p, TokenType t) {
    return cur(p)->type == t;
}
static int match(Parser* p, TokenType t) {
    if (check(p, t)) { advance(p); return 1; }
    return 0;
}
static void parse_error(Parser* p, const char* msg) {
    if (p->error_count == 0) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "语法错误 @%d:%d: %s (token=%s)",
                 cur(p)->line, cur(p)->col, msg,
                 token_type_name(cur(p)->type));
    }
    p->error_count++;
}

/* 前瞻声明 */
static AstNode* parse_statement(Parser* p);
static AstNode* parse_expr(Parser* p);
static AstNode* parse_block(Parser* p);

static MoType type_from_token(TokenType t) {
    switch (t) {
    case TOK_TYPE_INT: return TYPE_INT;
    case TOK_TYPE_STR: return TYPE_STR;
    case TOK_TYPE_FLOAT: return TYPE_FLOAT;
    case TOK_TYPE_BOOL: return TYPE_BOOL;
    case TOK_TYPE_BYTES: return TYPE_BYTES;
    case TOK_TYPE_UINT8: return TYPE_UINT8;
    case TOK_TYPE_UINT16: return TYPE_UINT16;
    case TOK_TYPE_UINT32: return TYPE_UINT32;
    case TOK_TYPE_UINT64: return TYPE_UINT64;
    case TOK_TYPE_INT8: return TYPE_INT8;
    case TOK_TYPE_INT16: return TYPE_INT16;
    case TOK_TYPE_INT32: return TYPE_INT32;
    case TOK_TYPE_INT64: return TYPE_INT64;
    default: return TYPE_UNKNOWN;
    }
}

static int is_inst_token(TokenType t) {
    return t >= TOK_INST_JMP && t <= TOK_INST_NOP;
}

/* 解析 `${name}` 模板引用 */
static AstNode* parse_template_ref(Parser* p) {
    const Token* t = cur(p);
    AstNode* n = ast_new(NODE_TEMPLATE_REF, t->line, t->col);
    advance(p); /* consume ${ */
    if (check(p, TOK_INT)) {
        n->ival = cur(p)->ival;
    } else if (check(p, TOK_IDENT)) {
        ast_set_text_len(n, cur(p)->lexeme, cur(p)->length);
    } else {
        parse_error(p, "模板引用内需要标识符或地址");
    }
    advance(p);
    if (!match(p, TOK_RBRACE)) parse_error(p, "模板引用缺少 }");
    return n;
}

/* 解析字面量 */
static AstNode* parse_primary(Parser* p) {
    const Token* t = cur(p);
    if (t->type == TOK_INT) {
        AstNode* n = ast_new(NODE_LITERAL, t->line, t->col);
        n->ival = t->ival;
        n->mtype = TYPE_INT;
        advance(p);
        return n;
    }
    if (t->type == TOK_FLOAT) {
        AstNode* n = ast_new(NODE_LITERAL, t->line, t->col);
        n->fval = t->fval;
        n->ival = (long)t->fval;
        n->mtype = TYPE_FLOAT;
        advance(p);
        return n;
    }
    if (t->type == TOK_STR) {
        AstNode* n = ast_new(NODE_LITERAL, t->line, t->col);
        ast_set_text_len(n, t->lexeme, t->length);
        n->mtype = TYPE_STR;
        advance(p);
        return n;
    }
    if (t->type == TOK_IDENT) {
        AstNode* n = ast_new(NODE_EXPR, t->line, t->col);
        ast_set_text_len(n, t->lexeme, t->length);
        advance(p);
        /* 函数调用 */
        if (check(p, TOK_LPAREN)) {
            AstNode* call = ast_new(NODE_CALL, t->line, t->col);
            ast_set_text(call, n->text);
            ast_free(n);
            advance(p);
            while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
                node_list_add(&call->args, parse_expr(p));
                if (!match(p, TOK_COMMA)) break;
            }
            match(p, TOK_RPAREN);
            return call;
        }
        /* 下标访问 ident[index] (支持链式) */
        if (check(p, TOK_LBRACKET)) {
            AstNode* base = n;
            while (check(p, TOK_LBRACKET)) {
                AstNode* idx = ast_new(NODE_INDEX_ACCESS, t->line, t->col);
                idx->left = base;
                advance(p); /* [ */
                if (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF))
                    idx->right = parse_expr(p);   /* 下标表达式 */
                match(p, TOK_RBRACKET);
                base = idx;
            }
            return base;
        }
        return n;
    }
    if (t->type == TOK_DOLLAR_LBRACE) {
        AstNode* ref = parse_template_ref(p);
        /* 链式下标访问 ${arr}[i][j]... */
        while (check(p, TOK_LBRACKET)) {
            AstNode* idx = ast_new(NODE_INDEX_ACCESS, t->line, t->col);
            idx->left = ref;
            advance(p); /* [ */
            if (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF))
                idx->right = parse_expr(p);
            match(p, TOK_RBRACKET);
            ref = idx;
        }
        return ref;
    }
    if (t->type == TOK_LPAREN) {
        advance(p);
        AstNode* e = parse_expr(p);
        match(p, TOK_RPAREN);
        return e;
    }
    if (t->type == TOK_LBRACKET) {
        /* 数组字面量 [a, b, ...] -> NODE_ARRAY_LIT */
        AstNode* n = ast_new(NODE_ARRAY_LIT, t->line, t->col);
        advance(p);
        while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
            node_list_add(&n->args, parse_expr(p));
            if (!match(p, TOK_COMMA)) break;
        }
        match(p, TOK_RBRACKET);
        return n;
    }
    if (t->type == TOK_TRUE) {
        AstNode* n = ast_new(NODE_LITERAL, t->line, t->col);
        n->ival = 1; n->mtype = TYPE_BOOL; advance(p); return n;
    }
    if (t->type == TOK_FALSE) {
        AstNode* n = ast_new(NODE_LITERAL, t->line, t->col);
        n->ival = 0; n->mtype = TYPE_BOOL; advance(p); return n;
    }
    parse_error(p, "意外的字面量");
    advance(p);
    return ast_new(NODE_EXPR, t->line, t->col);
}

/* 解析一元表达式 */
static AstNode* parse_unary(Parser* p) {
    if (check(p, TOK_MINUS) || check(p, TOK_NOT)) {
        TokenType op = cur(p)->type;
        const Token* t = cur(p);
        advance(p);
        AstNode* operand = parse_unary(p);
        AstNode* n = ast_new(NODE_BINOP, t->line, t->col);
        n->inst = op;
        n->left = operand;
        return n;
    }
    return parse_primary(p);
}

/* 解析乘法/除法 */
static AstNode* parse_mul(Parser* p) {
    AstNode* left = parse_unary(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH) || check(p, TOK_PERCENT)) {
        TokenType op = cur(p)->type;
        const Token* t = cur(p);
        advance(p);
        AstNode* right = parse_unary(p);
        AstNode* n = ast_new(NODE_BINOP, t->line, t->col);
        n->inst = op;
        n->left = left;
        n->right = right;
        left = n;
    }
    return left;
}

/* 解析加法/减法 */
static AstNode* parse_add(Parser* p) {
    AstNode* left = parse_mul(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        TokenType op = cur(p)->type;
        const Token* t = cur(p);
        advance(p);
        AstNode* right = parse_mul(p);
        AstNode* n = ast_new(NODE_BINOP, t->line, t->col);
        n->inst = op;
        n->left = left;
        n->right = right;
        left = n;
    }
    return left;
}

/* 解析比较 */
static AstNode* parse_comparison(Parser* p) {
    AstNode* left = parse_add(p);
    while (check(p, TOK_GT) || check(p, TOK_LT) || check(p, TOK_GE) ||
           check(p, TOK_LE) || check(p, TOK_EQ) || check(p, TOK_NEQ)) {
        TokenType op = cur(p)->type;
        const Token* t = cur(p);
        advance(p);
        AstNode* right = parse_add(p);
        AstNode* n = ast_new(NODE_BINOP, t->line, t->col);
        n->inst = op;
        n->left = left;
        n->right = right;
        left = n;
    }
    return left;
}

/* 解析逻辑与/或 */
static AstNode* parse_logic(Parser* p) {
    AstNode* left = parse_comparison(p);
    while (check(p, TOK_AND_AND) || check(p, TOK_OR_OR)) {
        TokenType op = cur(p)->type;
        const Token* t = cur(p);
        advance(p);
        AstNode* right = parse_comparison(p);
        AstNode* n = ast_new(NODE_BINOP, t->line, t->col);
        n->inst = op;
        n->left = left;
        n->right = right;
        left = n;
    }
    return left;
}

static AstNode* parse_expr(Parser* p) {
    return parse_logic(p);
}

/* 解析语句块 (缩进/冒号块) */
static AstNode* parse_block(Parser* p) {
    const Token* t = cur(p);
    AstNode* block = ast_new(NODE_BLOCK, t->line, t->col);
    /* 块: 花括号 {} 或 冒号+缩进 (INDENT...DEDENT) */
    if (match(p, TOK_LBRACE)) {
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            node_list_add(&block->body, parse_statement(p));
            while (check(p, TOK_NEWLINE)) advance(p);
        }
        match(p, TOK_RBRACE);
    } else if (match(p, TOK_NEWLINE)) {
        /* 缩进块: 期望 INDENT, 收集到 DEDENT */
        if (match(p, TOK_INDENT)) {
            while (!check(p, TOK_DEDENT) && !check(p, TOK_EOF)) {
                node_list_add(&block->body, parse_statement(p));
                while (check(p, TOK_NEWLINE)) advance(p);
            }
            match(p, TOK_DEDENT);
        } else {
            /* 冒号后直接换行但无缩进: 视为空块 */
        }
    } else {
        /* 冒号同行单语句 */
        node_list_add(&block->body, parse_statement(p));
    }
    return block;
}

/* 解析变量声明 void name : type = value */
static AstNode* parse_var_decl(Parser* p) {
    const Token* t = cur(p);
    advance(p); /* void */
    AstNode* n = ast_new(NODE_VAR_DECL, t->line, t->col);
    if (check(p, TOK_IDENT)) {
        ast_set_text_len(n, cur(p)->lexeme, cur(p)->length);
        advance(p);
    } else {
        parse_error(p, "变量声明需要名称");
    }
    if (match(p, TOK_COLON)) {
        if (check(p, TOK_TYPE_INT) || check(p, TOK_TYPE_STR) || check(p, TOK_TYPE_FLOAT) ||
            check(p, TOK_TYPE_BOOL) || check(p, TOK_TYPE_BYTES) ||
            check(p, TOK_TYPE_UINT8) || check(p, TOK_TYPE_UINT16) ||
            check(p, TOK_TYPE_UINT32) || check(p, TOK_TYPE_UINT64) ||
            check(p, TOK_TYPE_INT8) || check(p, TOK_TYPE_INT16) ||
            check(p, TOK_TYPE_INT32) || check(p, TOK_TYPE_INT64)) {
            n->mtype = type_from_token(cur(p)->type);
            advance(p);
        }
    }
    if (match(p, TOK_ASSIGN)) {
        n->left = parse_expr(p);
    }
    return n;
}

/* 解析常量声明 const name : type = addr : value */
static AstNode* parse_const_decl(Parser* p) {
    const Token* t = cur(p);
    advance(p); /* const */
    AstNode* n = ast_new(NODE_CONST_DECL, t->line, t->col);
    if (check(p, TOK_IDENT)) {
        ast_set_text_len(n, cur(p)->lexeme, cur(p)->length);
        advance(p);
    } else {
        parse_error(p, "常量声明需要名称");
    }
    if (match(p, TOK_COLON)) {
        if (token_is_type(cur(p)->type)) {
            n->mtype = type_from_token(cur(p)->type);
            advance(p);
        }
    }
    if (match(p, TOK_ASSIGN)) {
        /* addr : value */
        if (check(p, TOK_INT)) {
            n->ival = cur(p)->ival;  /* addr */
            advance(p);
            if (match(p, TOK_COLON)) {
                if (check(p, TOK_INT)) {
                    n->left = ast_new(NODE_LITERAL, cur(p)->line, cur(p)->col);
                    n->left->ival = cur(p)->ival;
                    n->left->mtype = TYPE_INT;
                    advance(p);
                }
            }
        } else {
            n->left = parse_expr(p);
        }
    }
    return n;
}

/* 解析输出语句 >> print >> msg */
static AstNode* parse_print(Parser* p) {
    const Token* t = cur(p);
    AstNode* n = ast_new(NODE_PRINT, t->line, t->col);
    /* 已消费 >> print >> */
    while (!check(p, TOK_EOF) && !check(p, TOK_NEWLINE)) {
        /* 遇到新的 >> 也处理 (data decl) */
        if (check(p, TOK_RSHIFT)) {
            /* 可能是 data decl 或 指令链接, 简化: 跳过 */
            if (peek(p, 1)->type == TOK_TYPE_UINT32 || peek(p, 1)->type == TOK_TYPE_UINT16 ||
                peek(p, 1)->type == TOK_TYPE_UINT64 || peek(p, 1)->type == TOK_TYPE_UINT8 ||
                peek(p, 1)->type == TOK_TYPE_BYTES) {
                /* 这是数据声明，交给 data decl 处理 */
                break;
            }
            advance(p); /* consume >> */
            continue;
        }
        node_list_add(&n->args, parse_expr(p));
        /* 数组输出 [a, b] */
        if (check(p, TOK_LBRACKET)) {
            AstNode* idx = ast_new(NODE_INDEX, cur(p)->line, cur(p)->col);
            advance(p);
            while (!check(p, TOK_RBRACKET) && !check(p, TOK_EOF)) {
                node_list_add(&idx->args, parse_expr(p));
                if (!match(p, TOK_COMMA)) break;
            }
            match(p, TOK_RBRACKET);
            node_list_add(&n->args, idx);
        }
        break; /* 一行一条 */
    }
    return n;
}

/* 解析内存指令 addr : inst = op : extra */
static AstNode* parse_instruction(Parser* p) {
    const Token* t = cur(p);
    AstNode* n = ast_new(NODE_INSTRUCTION, t->line, t->col);
    /* addr 名称 (0xNN_name 或 name) */
    if (check(p, TOK_IDENT)) {
        ast_set_text_len(n, cur(p)->lexeme, cur(p)->length);
        advance(p);
    }
    if (!match(p, TOK_COLON)) {
        parse_error(p, "指令需要冒号");
    }
    if (is_inst_token(cur(p)->type)) {
        n->inst = cur(p)->type;
        advance(p);
    } else {
        parse_error(p, "无效指令");
    }
    if (match(p, TOK_ASSIGN)) {
        if (check(p, TOK_INT)) {
            n->left = ast_new(NODE_LITERAL, cur(p)->line, cur(p)->col);
            n->left->ival = cur(p)->ival;
            n->left->mtype = TYPE_INT;
            advance(p);
        } else if (check(p, TOK_IDENT)) {
            n->left = ast_new(NODE_EXPR, cur(p)->line, cur(p)->col);
            ast_set_text_len(n->left, cur(p)->lexeme, cur(p)->length);
            advance(p);
        }
    }
    /* 额外指令 : extra (如 : nop) */
    while (match(p, TOK_COLON)) {
        if (is_inst_token(cur(p)->type)) advance(p);
        else if (check(p, TOK_INT)) advance(p);
        else break;
    }
    return n;
}

/* 解析数据声明 >> uint32 : data >> uint16 : data */
static AstNode* parse_data_decl(Parser* p) {
    const Token* t = cur(p);
    AstNode* n = ast_new(NODE_DATA_DECL, t->line, t->col);
    n->mtype = TYPE_BYTES;
    /* 循环处理 >> type : data */
    while (match(p, TOK_RSHIFT)) {
        if (token_is_type(cur(p)->type)) {
            n->mtype = type_from_token(cur(p)->type);
            advance(p);
        }
        if (match(p, TOK_COLON)) {
            /* 收集数据项 */
            while (!check(p, TOK_EOF)) {
                if (check(p, TOK_INT)) {
                    node_list_add(&n->args, ast_new(NODE_LITERAL, cur(p)->line, cur(p)->col));
                    n->args.items[n->args.count-1]->ival = cur(p)->ival;
                    n->args.items[n->args.count-1]->mtype = TYPE_INT;
                    advance(p);
                } else if (check(p, TOK_STR)) {
                    AstNode* lit = ast_new(NODE_LITERAL, cur(p)->line, cur(p)->col);
                    ast_set_text_len(lit, cur(p)->lexeme, cur(p)->length);
                    lit->mtype = TYPE_STR;
                    node_list_add(&n->args, lit);
                    advance(p);
                } else if (check(p, TOK_DOLLAR_LBRACE)) {
                    node_list_add(&n->args, parse_template_ref(p));
                } else {
                    break;
                }
            }
        }
        if (!check(p, TOK_RSHIFT)) break;
    }
    return n;
}

/* 解析 fn 函数声明 */
static AstNode* parse_fn_decl(Parser* p) {
    const Token* t = cur(p);
    advance(p); /* fn */
    AstNode* n = ast_new(NODE_FN_DECL, t->line, t->col);
    if (check(p, TOK_IDENT)) {
        ast_set_text_len(n, cur(p)->lexeme, cur(p)->length);
        advance(p);
    }
    if (match(p, TOK_LPAREN)) {
        while (!check(p, TOK_RPAREN) && !check(p, TOK_EOF)) {
            /* 参数: name 或 name : type */
            if (check(p, TOK_IDENT)) {
                AstNode* param = ast_new(NODE_EXPR, cur(p)->line, cur(p)->col);
                ast_set_text_len(param, cur(p)->lexeme, cur(p)->length);
                advance(p);
                if (match(p, TOK_COLON)) {
                    if (token_is_type(cur(p)->type)) {
                        param->mtype = type_from_token(cur(p)->type);
                        advance(p);
                    }
                }
                node_list_add(&n->args, param);
            } else {
                parse_error(p, "无效函数参数");
                break;
            }
            if (!match(p, TOK_COMMA)) break;
        }
        match(p, TOK_RPAREN);
    }
    if (match(p, TOK_LBRACE)) {
        while (!check(p, TOK_RBRACE) && !check(p, TOK_EOF)) {
            node_list_add(&n->body, parse_statement(p));
            while (check(p, TOK_NEWLINE)) advance(p);
        }
        match(p, TOK_RBRACE);
    } else if (match(p, TOK_COLON)) {
        /* 冒号缩进体: 交给 parse_block (支持 {} 或 冒号+INDENT...DEDENT 多语句) */
        AstNode* body = parse_block(p);
        if (body) {
            /* 转移 body 的子语句到函数体, 避免双重释放 */
            for (int i = 0; i < body->body.count; i++)
                node_list_add(&n->body, body->body.items[i]);
            /* 只释放 NODE_BLOCK 外壳, 不释放已移交的子节点 */
            body->body.items = NULL;
            body->body.count = body->body.capacity = 0;
            free(body);
        }
    } else {
        /* 无显式冒号: 单语句 */
        node_list_add(&n->body, parse_statement(p));
    }
    return n;
}

/* 解析 if 语句 */
static AstNode* parse_if(Parser* p) {
    const Token* t = cur(p);
    advance(p); /* if */
    AstNode* n = ast_new(NODE_IF, t->line, t->col);
    n->cond = parse_expr(p);
    match(p, TOK_COLON);
    n->then_block = parse_block(p);
    if (match(p, TOK_ELSE)) {
        match(p, TOK_COLON);
        n->else_block = parse_block(p);
    }
    return n;
}

/* 解析 while 语句 */
static AstNode* parse_while(Parser* p) {
    const Token* t = cur(p);
    advance(p); /* while */
    AstNode* n = ast_new(NODE_WHILE, t->line, t->col);
    n->cond = parse_expr(p);
    match(p, TOK_COLON);
    n->then_block = parse_block(p);
    return n;
}

/* 解析 for 语句 (简化: for init; cond; step) */
static AstNode* parse_for(Parser* p) {
    const Token* t = cur(p);
    advance(p); /* for */
    AstNode* n = ast_new(NODE_FOR, t->line, t->col);
    /* 简化：for 后的表达式 */
    n->cond = parse_expr(p);
    match(p, TOK_COLON);
    n->then_block = parse_block(p);
    return n;
}

/* 解析 return */
static AstNode* parse_return(Parser* p) {
    const Token* t = cur(p);
    advance(p);
    AstNode* n = ast_new(NODE_RETURN, t->line, t->col);
    if (!check(p, TOK_EOF) && !check(p, TOK_NEWLINE) &&
        !check(p, TOK_RBRACE) && !check(p, TOK_COLON)) {
        n->left = parse_expr(p);
    }
    return n;
}

/* 解析语句 */
static AstNode* parse_statement(Parser* p) {
    const Token* t = cur(p);

    /* 跳过行尾残留 */
    while (check(p, TOK_NEWLINE)) advance(p);

    switch (t->type) {
    case TOK_VOID: return parse_var_decl(p);
    case TOK_CONST: return parse_const_decl(p);
    case TOK_FN: return parse_fn_decl(p);
    case TOK_IF: return parse_if(p);
    case TOK_WHILE: return parse_while(p);
    case TOK_FOR: return parse_for(p);
    case TOK_RETURN: return parse_return(p);
    case TOK_BREAK: {
        AstNode* n = ast_new(NODE_BREAK, t->line, t->col);
        advance(p); return n;
    }
    case TOK_CONTINUE: {
        AstNode* n = ast_new(NODE_CONTINUE, t->line, t->col);
        advance(p); return n;
    }
    case TOK_RSHIFT: {
        /* >> print >> ... 或 >> type : data */
        if (peek(p, 1)->type == TOK_PRINT || peek(p, 1)->type == TOK_IDENT) {
            if (peek(p, 1)->type == TOK_PRINT) {
                advance(p); /* >> */
                advance(p); /* print */
                if (match(p, TOK_RSHIFT)) {
                    return parse_print(p);
                }
                return parse_print(p);
            }
        }
        /* 数据声明 */
        if (peek(p, 1)->type == TOK_TYPE_UINT8 || peek(p, 1)->type == TOK_TYPE_UINT16 ||
            peek(p, 1)->type == TOK_TYPE_UINT32 || peek(p, 1)->type == TOK_TYPE_UINT64 ||
            peek(p, 1)->type == TOK_TYPE_BYTES) {
            return parse_data_decl(p);
        }
        /* 普通 >> 输出 */
        advance(p); /* >> */
        return parse_print(p);
    }
    case TOK_IDENT:
        /* 可能是赋值, 或内存指令, 或输出流 */
        {
            /* name = expr */
            if (peek(p, 1)->type == TOK_ASSIGN) {
                AstNode* n = ast_new(NODE_ASSIGN, t->line, t->col);
                ast_set_text_len(n, t->lexeme, t->length);
                advance(p); /* name */
                advance(p); /* = */
                n->left = parse_expr(p);
                return n;
            }
            /* name[i] = expr 数组元素赋值 */
            if (peek(p, 1)->type == TOK_LBRACKET) {
                /* 先试解析下标访问, 再看是否赋值 */
                AstNode* idx = parse_primary(p); /* 会消费 ident [...] */
                if (check(p, TOK_ASSIGN)) {
                    advance(p); /* = */
                    AstNode* n = ast_new(NODE_ASSIGN, t->line, t->col);
                    ast_set_text(n, "[]@");           /* 标记为下标赋值 */
                    n->left = idx;
                    n->right = parse_expr(p);        /* 存入值 */
                    return n;
                }
                return idx; /* 只是读取 */
            }
            /* name : inst = ... 内存指令 */
            if (peek(p, 1)->type == TOK_COLON && is_inst_token(peek(p, 2)->type)) {
                return parse_instruction(p);
            }
            /* 内存地址指令形式: 0xNN_name : inst ... */
            /* 单独表达式 */
            return parse_expr(p);
        }
    case TOK_INT:
        /* 内存地址引用: 0xNN_name : inst ... 或 0xNN : ... */
        {
            if (peek(p, 1)->type == TOK_COLON && is_inst_token(peek(p, 2)->type)) {
                /* 0xNN : inst */
                AstNode* n = ast_new(NODE_INSTRUCTION, t->line, t->col);
                char buf[32];
                snprintf(buf, sizeof(buf), "0x%lX", t->ival);
                ast_set_text(n, buf);
                advance(p);
                match(p, TOK_COLON);
                if (is_inst_token(cur(p)->type)) {
                    n->inst = cur(p)->type;
                    advance(p);
                }
                if (match(p, TOK_ASSIGN)) {
                    if (check(p, TOK_INT)) {
                        n->left = ast_new(NODE_LITERAL, cur(p)->line, cur(p)->col);
                        n->left->ival = cur(p)->ival;
                        n->left->mtype = TYPE_INT;
                        advance(p);
                    }
                }
                while (match(p, TOK_COLON)) {
                    if (is_inst_token(cur(p)->type)) advance(p);
                    else if (check(p, TOK_INT)) advance(p);
                    else break;
                }
                return n;
            }
            /* 简单表达式语句 */
            return parse_expr(p);
        }
    default:
        return parse_expr(p);
    }
}

AstNode* parser_parse(Parser* p) {
    AstNode* program = ast_new(NODE_PROGRAM, 1, 1);
    while (!check(p, TOK_EOF)) {
        if (check(p, TOK_NEWLINE)) { advance(p); continue; }
        AstNode* stmt = parse_statement(p);
        if (stmt) node_list_add(&program->body, stmt);
        /* 防止死循环: 若没推进则强制前进 */
        if (cur(p)->type != TOK_EOF && p->pos == 0) break;
    }
    return program;
}

AstNode* xiaomo_parse_source(const char* src, char* errbuf, int errlen) {
    Lexer lx;
    lexer_init(&lx, src);
    TokenList tokens = lexer_tokenize(&lx);
    if (lx.error_count > 0) {
        if (errbuf) snprintf(errbuf, errlen, "%s", lx.error_msg);
        token_list_free(&tokens);
        return NULL;
    }
    Parser p;
    memset(&p, 0, sizeof(p));
    p.tokens = tokens;
    p.pos = 0;
    AstNode* program = parser_parse(&p);
    if (p.error_count > 0) {
        if (errbuf) snprintf(errbuf, errlen, "%s", p.error_msg);
        ast_free(program);
        token_list_free(&tokens);
        return NULL;
    }
    token_list_free(&tokens);
    return program;
}
