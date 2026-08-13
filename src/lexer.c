/*
 * xiaomo - 词法分析器实现
 */
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---------- 关键字查找 ---------- */
typedef struct { const char* word; TokenType type; } Keyword;

static const Keyword KEYWORDS[] = {
    {"void",    TOK_VOID},
    {"const",   TOK_CONST},
    {"fn",      TOK_FN},
    {"func",    TOK_FUNC},
    {"if",      TOK_IF},
    {"else",    TOK_ELSE},
    {"while",   TOK_WHILE},
    {"for",     TOK_FOR},
    {"return",  TOK_RETURN},
    {"null",    TOK_NULL},
    {"try",     TOK_TRY},
    {"catch",   TOK_CATCH},
    {"switch",  TOK_SWITCH},
    {"case",    TOK_CASE},
    {"default", TOK_DEFAULT},
    {"break",   TOK_BREAK},
    {"continue",TOK_CONTINUE},
    {"class",   TOK_CLASS},
    {"extends", TOK_EXTENDS},
    {"this",    TOK_THIS},
    {"super",   TOK_SUPER},
    {"print",   TOK_PRINT},
    {"true",    TOK_TRUE},
    {"false",   TOK_FALSE},
    /* 类型 */
    {"int",     TOK_TYPE_INT},
    {"str",     TOK_TYPE_STR},
    {"float",   TOK_TYPE_FLOAT},
    {"bool",    TOK_TYPE_BOOL},
    {"bytes",   TOK_TYPE_BYTES},
    {"uint8",   TOK_TYPE_UINT8},
    {"uint16",  TOK_TYPE_UINT16},
    {"uint32",  TOK_TYPE_UINT32},
    {"uint64",  TOK_TYPE_UINT64},
    {"int8",    TOK_TYPE_INT8},
    {"int16",   TOK_TYPE_INT16},
    {"int32",   TOK_TYPE_INT32},
    {"int64",   TOK_TYPE_INT64},
    /* 指令 */
    {"jmp",     TOK_INST_JMP},
    {"jnz",     TOK_INST_JNZ},
    {"jz",      TOK_INST_JZ},
    {"je",      TOK_INST_JE},
    {"jne",     TOK_INST_JNE},
    {"jg",      TOK_INST_JG},
    {"jl",      TOK_INST_JL},
    {"mov",     TOK_INST_MOV},
    {"dnp",     TOK_INST_DNP},
    {"push",    TOK_INST_PUSH},
    {"pop",     TOK_INST_POP},
    {"add",     TOK_INST_ADD},
    {"sub",     TOK_INST_SUB},
    {"mul",     TOK_INST_MUL},
    {"div",     TOK_INST_DIV},
    {"and",     TOK_INST_AND},
    {"or",      TOK_INST_OR},
    {"xor",     TOK_INST_XOR},
    {"not",     TOK_INST_NOT},
    {"call",    TOK_INST_CALL},
    {"ret",     TOK_INST_RET},
    {"nop",     TOK_INST_NOP},
};

static const int KEYWORD_COUNT = sizeof(KEYWORDS)/sizeof(KEYWORDS[0]);

static TokenType lookup_keyword(const char* word, int len) {
    for (int i = 0; i < KEYWORD_COUNT; i++) {
        if ((int)strlen(KEYWORDS[i].word) == len &&
            strncmp(KEYWORDS[i].word, word, len) == 0) {
            return KEYWORDS[i].type;
        }
    }
    return TOK_IDENT;
}

void lexer_init(Lexer* lx, const char* src) {
    lx->src = src;
    lx->pos = 0;
    lx->len = (int)strlen(src);
    lx->line = 1;
    lx->col = 1;
    lx->error_count = 0;
    lx->error_msg[0] = '\0';
}

static void lexer_error(Lexer* lx, const char* msg) {
    if (lx->error_count == 0) {
        snprintf(lx->error_msg, sizeof(lx->error_msg),
                 "词法错误 @%d:%d: %s", lx->line, lx->col, msg);
    }
    lx->error_count++;
}

static char peek(const Lexer* lx) {
    if (lx->pos >= lx->len) return '\0';
    return lx->src[lx->pos];
}
static char peek2(const Lexer* lx) {
    if (lx->pos + 1 >= lx->len) return '\0';
    return lx->src[lx->pos + 1];
}
static char advance(Lexer* lx) {
    if (lx->pos >= lx->len) return '\0';
    char c = lx->src[lx->pos++];
    if (c == '\n') { lx->line++; lx->col = 1; }
    else lx->col++;
    return c;
}

static int is_hex_digit(char c) {
    return isdigit((unsigned char)c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static void add_token(TokenList* list, Token t) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 64 : list->capacity * 2;
        list->items = (Token*)realloc(list->items, sizeof(Token) * list->capacity);
    }
    list->items[list->count++] = t;
}

static Token make_token(Lexer* lx, TokenType type, const char* start, int len) {
    Token t;
    memset(&t, 0, sizeof(t));
    t.type = type;
    t.lexeme = start;
    t.length = len;
    t.line = lx->line;
    t.col = lx->col - len;
    if (t.col < 1) t.col = 1;
    return t;
}

static void lex_number(Lexer* lx, TokenList* list, const char* start, int start_col) {
    /* 支持 0x 十六进制 和 十进制 */
    if (lx->pos + 1 < lx->len && peek(lx) == 'x') {
        /* 十六进制: 0xNN */
        advance(lx); /* consume 'x' */
        long val = 0;
        int digits = 0;
        while (is_hex_digit(peek(lx))) {
            char c = advance(lx);
            int d = isdigit((unsigned char)c) ? (c - '0') : (tolower((unsigned char)c) - 'a' + 10);
            val = val * 16 + d;
            digits++;
        }
        Token t = make_token(lx, TOK_INT, start, (int)(lx->src + lx->pos - start));
        t.ival = val;
        t.col = start_col;
        add_token(list, t);
        (void)digits;
    } else {
        /* 十进制，可能是浮点 */
        int is_float = 0;
        while (isdigit((unsigned char)peek(lx))) advance(lx);
        if (peek(lx) == '.' && isdigit((unsigned char)peek2(lx))) {
            is_float = 1;
            advance(lx); /* . */
            while (isdigit((unsigned char)peek(lx))) advance(lx);
        }
        if (is_float) {
            char buf[64];
            int len = (int)(lx->src + lx->pos - start);
            if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, start, len); buf[len] = '\0';
            Token t = make_token(lx, TOK_FLOAT, start, len);
            t.fval = atof(buf);
            t.col = start_col;
            add_token(list, t);
        } else {
            char buf[64];
            int len = (int)(lx->src + lx->pos - start);
            if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, start, len); buf[len] = '\0';
            Token t = make_token(lx, TOK_INT, start, len);
            t.ival = atol(buf);
            t.col = start_col;
            add_token(list, t);
        }
    }
}

TokenList lexer_tokenize(Lexer* lx) {
    TokenList list = {0};
    /* 缩进栈 (Python 风格 INDENT/DEDENT) */
    int indent_stack[64];
    int indent_top = 0;
    indent_stack[0] = 0;
    int at_line_start = 1;   /* 当前行尚未出现实质 token */
    int indent_level = 0;    /* 当前行累积缩进空格数 */
    int line_has_token = 0;  /* 当前行是否已有实质 token (决定是否 emit NEWLINE) */

    while (lx->pos < lx->len) {
        char c = peek(lx);

        /* 行首缩进: 统计空格 */
        if (at_line_start && (c == ' ' || c == '\t')) {
            if (c == '\t') { lexer_error(lx, "不支持 tab 缩进, 请用空格"); }
            advance(lx); indent_level++; continue;
        }
        /* 行首其他空白 (CR/VT/FF) */
        if (at_line_start && (c == '\r' || c == '\v' || c == '\f')) {
            advance(lx); continue;
        }
        /* 换行: 结算当前行 */
        if (c == '\n') {
            advance(lx);
            if (line_has_token) {
                Token nl = make_token(lx, TOK_NEWLINE, lx->src + lx->pos - 1, 0);
                nl.col = 1;
                add_token(&list, nl);
            }
            at_line_start = 1;
            indent_level = 0;
            line_has_token = 0;
            continue;
        }
        /* 注释 # 到行尾 (不影响缩进栈) */
        if (c == '#') {
            while (lx->pos < lx->len && peek(lx) != '\n') advance(lx);
            continue;
        }
        /* 行首第一个实质 token: 根据缩进 emit INDENT/DEDENT */
        if (at_line_start) {
            at_line_start = 0;
            if (indent_level > indent_stack[indent_top]) {
                if (indent_top >= 63) { lexer_error(lx, "缩进嵌套过深"); }
                else {
                    indent_stack[++indent_top] = indent_level;
                    Token in = make_token(lx, TOK_INDENT, lx->src + lx->pos - indent_level, 0);
                    in.col = 1;
                    add_token(&list, in);
                }
            } else {
                while (indent_level < indent_stack[indent_top]) {
                    indent_top--;
                    Token de = make_token(lx, TOK_DEDENT, lx->src + lx->pos, 0);
                    de.col = 1;
                    add_token(&list, de);
                }
            }
        }
        line_has_token = 1;

        /* 行中空白: 跳过 */
        if (c == ' ' || c == '\t') {
            advance(lx);
            continue;
        }
        /* 字符串 */
        if (c == '"' || c == '\'') {
            char quote = advance(lx);
            const char* start = lx->src + lx->pos;
            int start_col = lx->col;
            while (lx->pos < lx->len && peek(lx) != quote) {
                if (peek(lx) == '\\') advance(lx);
                advance(lx);
            }
            int len = (int)(lx->src + lx->pos - start);
            if (lx->pos >= lx->len) {
                lexer_error(lx, "未闭合字符串");
            } else {
                advance(lx); /* consume quote */
            }
            Token t = make_token(lx, TOK_STR, start, len);
            t.col = start_col;
            add_token(&list, t);
            continue;
        }
        /* 数字 */
        if (isdigit((unsigned char)c)) {
            const char* start = lx->src + lx->pos;
            int start_col = lx->col;
            lex_number(lx, &list, start, start_col);
            continue;
        }
        /* 标识符 / 关键字 / 地址引用 */
        if (isalpha((unsigned char)c) || c == '_') {
            const char* start = lx->src + lx->pos;
            int start_col = lx->col;
            while (lx->pos < lx->len && (isalnum((unsigned char)peek(lx)) || peek(lx) == '_' || peek(lx) == '.')) {
                advance(lx);
            }
            int len = (int)(lx->src + lx->pos - start);
            TokenType type = lookup_keyword(start, len);
            Token t = make_token(lx, type, start, len);
            t.col = start_col;
            add_token(&list, t);
            continue;
        }

        /* 运算符 */
        const char* start = lx->src + lx->pos;
        switch (c) {
        case ':':
            advance(lx); add_token(&list, make_token(lx, TOK_COLON, start, 1)); continue;
        case ';':
            advance(lx); add_token(&list, make_token(lx, TOK_SEMICOLON, start, 1)); continue;
        case ',':
            advance(lx); add_token(&list, make_token(lx, TOK_COMMA, start, 1)); continue;
        case '.':
            advance(lx); add_token(&list, make_token(lx, TOK_DOT, start, 1)); continue;
        case '(':
            advance(lx); add_token(&list, make_token(lx, TOK_LPAREN, start, 1)); continue;
        case ')':
            advance(lx); add_token(&list, make_token(lx, TOK_RPAREN, start, 1)); continue;
        case '{':
            advance(lx); add_token(&list, make_token(lx, TOK_LBRACE, start, 1)); continue;
        case '}':
            advance(lx); add_token(&list, make_token(lx, TOK_RBRACE, start, 1)); continue;
        case '[':
            advance(lx); add_token(&list, make_token(lx, TOK_LBRACKET, start, 1)); continue;
        case ']':
            advance(lx); add_token(&list, make_token(lx, TOK_RBRACKET, start, 1)); continue;
        case '+':
            advance(lx); add_token(&list, make_token(lx, TOK_PLUS, start, 1)); continue;
        case '-':
            if (peek2(lx) == '>') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_ARROW, start, 2)); }
            else { advance(lx); add_token(&list, make_token(lx, TOK_MINUS, start, 1)); }
            continue;
        case '*':
            advance(lx); add_token(&list, make_token(lx, TOK_STAR, start, 1)); continue;
        case '/':
            advance(lx); add_token(&list, make_token(lx, TOK_SLASH, start, 1)); continue;
        case '%':
            advance(lx); add_token(&list, make_token(lx, TOK_PERCENT, start, 1)); continue;
        case '=':
            if (peek2(lx) == '=') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_EQ, start, 2)); }
            else { advance(lx); add_token(&list, make_token(lx, TOK_ASSIGN, start, 1)); }
            continue;
        case '>':
            if (peek2(lx) == '=') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_GE, start, 2)); }
            else if (peek2(lx) == '>') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_RSHIFT, start, 2)); }
            else { advance(lx); add_token(&list, make_token(lx, TOK_GT, start, 1)); }
            continue;
        case '<':
            if (peek2(lx) == '=') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_LE, start, 2)); }
            else if (peek2(lx) == '<') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_LSHIFT, start, 2)); }
            else { advance(lx); add_token(&list, make_token(lx, TOK_LT, start, 1)); }
            continue;
        case '!':
            if (peek2(lx) == '=') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_NEQ, start, 2)); }
            else { advance(lx); add_token(&list, make_token(lx, TOK_NOT, start, 1)); }
            continue;
        case '&':
            if (peek2(lx) == '&') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_AND_AND, start, 2)); }
            else { advance(lx); add_token(&list, make_token(lx, TOK_AMP, start, 1)); }
            continue;
        case '|':
            if (peek2(lx) == '|') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_OR_OR, start, 2)); }
            else { advance(lx); add_token(&list, make_token(lx, TOK_PIPE, start, 1)); }
            continue;
        case '^':
            advance(lx); add_token(&list, make_token(lx, TOK_CARET, start, 1)); continue;
        case '$':
            if (peek2(lx) == '{') { advance(lx); advance(lx); add_token(&list, make_token(lx, TOK_DOLLAR_LBRACE, start, 2)); }
            else { advance(lx); add_token(&list, make_token(lx, TOK_IDENT, start, 1)); }
            continue;
        default:
            lexer_error(lx, "无法识别的字符");
            advance(lx);
            continue;
        }
    }
    /* 文件结束: 收尾所有未闭合的缩进 (emit DEDENT) */
    while (indent_top > 0) {
        indent_top--;
        Token de = make_token(lx, TOK_DEDENT, lx->src + lx->pos, 0);
        de.col = 1;
        add_token(&list, de);
    }
    /* EOF */
    Token eof;
    memset(&eof, 0, sizeof(eof));
    eof.type = TOK_EOF;
    eof.line = lx->line;
    eof.col = lx->col;
    add_token(&list, eof);
    return list;
}

void token_list_free(TokenList* list) {
    if (list->items) free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

const char* token_type_name(TokenType t) {
    switch (t) {
    case TOK_EOF: return "EOF";
    case TOK_IDENT: return "IDENT";
    case TOK_INT: return "INT";
    case TOK_STR: return "STR";
    case TOK_FLOAT: return "FLOAT";
    case TOK_VOID: return "void";
    case TOK_CONST: return "const";
    case TOK_FN: return "fn";
    case TOK_IF: return "if";
    case TOK_ELSE: return "else";
    case TOK_WHILE: return "while";
    case TOK_RETURN: return "return";
    case TOK_PRINT: return "print";
    case TOK_RSHIFT: return ">>";
    case TOK_COLON: return ":";
    case TOK_NEWLINE: return "NL";
    case TOK_INDENT: return "INDENT";
    case TOK_DEDENT: return "DEDENT";
    case TOK_SEMICOLON: return ";";
    case TOK_COMMA: return ",";
    case TOK_LBRACE: return "{";
    case TOK_RBRACE: return "}";
    case TOK_LPAREN: return "(";
    case TOK_RPAREN: return ")";
    case TOK_LBRACKET: return "[";
    case TOK_RBRACKET: return "]";
    case TOK_ASSIGN: return "=";
    case TOK_PLUS: return "+";
    case TOK_MINUS: return "-";
    case TOK_STAR: return "*";
    case TOK_SLASH: return "/";
    case TOK_GT: return ">";
    case TOK_LT: return "<";
    case TOK_DOLLAR_LBRACE: return "${";
    case TOK_TYPE_INT: return "int";
    case TOK_TYPE_STR: return "str";
    case TOK_TYPE_FLOAT: return "float";
    case TOK_TYPE_BOOL: return "bool";
    case TOK_TYPE_BYTES: return "bytes";
    case TOK_TYPE_UINT8: return "uint8";
    case TOK_TYPE_UINT16: return "uint16";
    case TOK_TYPE_UINT32: return "uint32";
    case TOK_TYPE_UINT64: return "uint64";
    case TOK_TYPE_INT8: return "int8";
    case TOK_TYPE_INT16: return "int16";
    case TOK_TYPE_INT32: return "int32";
    case TOK_TYPE_INT64: return "int64";
    default: return "?";
    }
}

int token_is_type(TokenType t) {
    return t >= TOK_TYPE_INT && t <= TOK_TYPE_INT64;
}

void token_list_dump(const TokenList* list) {
    for (int i = 0; i < list->count; i++) {
        Token t = list->items[i];
        printf("%3d: %-10s '%.*s'", i, token_type_name(t.type), t.length, t.lexeme ? t.lexeme : "");
        if (t.type == TOK_INT) printf("  (val=%ld)", t.ival);
        if (t.type == TOK_FLOAT) printf("  (val=%g)", t.fval);
        printf("\n");
    }
}
