/*
 * xiaomo - AST 实现
 */
#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

AstNode* ast_new(NodeType type, int line, int col) {
    AstNode* n = (AstNode*)calloc(1, sizeof(AstNode));
    if (!n) return NULL;
    n->type = type;
    n->line = line;
    n->col = col;
    n->mtype = TYPE_UNKNOWN;
    return n;
}

void ast_set_text(AstNode* n, const char* text) {
    if (n->text) free(n->text);
    n->text = text ? strdup(text) : NULL;
}

/* 按指定长度复制文本 (token lexeme 不是 NUL 结尾, 必须按 length 复制) */
void ast_set_text_len(AstNode* n, const char* text, int len) {
    if (n->text) free(n->text);
    if (text && len > 0) {
        n->text = (char*)malloc(len + 1);
        if (n->text) {
            memcpy(n->text, text, len);
            n->text[len] = '\0';
        }
    } else {
        n->text = NULL;
    }
}

void node_list_add(NodeList* list, AstNode* node) {
    if (list->count >= list->capacity) {
        list->capacity = list->capacity == 0 ? 8 : list->capacity * 2;
        list->items = (AstNode**)realloc(list->items, sizeof(AstNode*) * list->capacity);
    }
    list->items[list->count++] = node;
}

void node_list_free(NodeList* list) {
    for (int i = 0; i < list->count; i++) ast_free(list->items[i]);
    if (list->items) free(list->items);
    list->items = NULL;
    list->count = list->capacity = 0;
}

void ast_free(AstNode* node) {
    if (!node) return;
    if (node->text) free(node->text);
    ast_free(node->left);
    ast_free(node->right);
    ast_free(node->cond);
    ast_free(node->then_block);
    ast_free(node->else_block);
    node_list_free(&node->body);
    node_list_free(&node->args);
    free(node);
}

static const char* node_type_name(NodeType t) {
    switch (t) {
    case NODE_PROGRAM: return "Program";
    case NODE_VAR_DECL: return "VarDecl";
    case NODE_CONST_DECL: return "ConstDecl";
    case NODE_ASSIGN: return "Assign";
    case NODE_PRINT: return "Print";
    case NODE_IF: return "If";
    case NODE_WHILE: return "While";
    case NODE_FOR: return "For";
    case NODE_FN_DECL: return "FnDecl";
    case NODE_CALL: return "Call";
    case NODE_EXPR: return "Expr";
    case NODE_INSTRUCTION: return "Instruction";
    case NODE_DATA_DECL: return "DataDecl";
    case NODE_BLOCK: return "Block";
    case NODE_TEMPLATE_REF: return "TemplateRef";
    case NODE_BINOP: return "BinOp";
    case NODE_LITERAL: return "Literal";
    case NODE_INDEX: return "Index";
    case NODE_ARRAY_LIT: return "ArrayLit";
    case NODE_INDEX_ACCESS: return "IndexAccess";
    case NODE_RETURN: return "Return";
    case NODE_BREAK: return "Break";
    case NODE_CONTINUE: return "Continue";
    default: return "?";
    }
}

void ast_dump(const AstNode* node, int depth) {
    if (!node) return;
    for (int i = 0; i < depth; i++) printf("  ");
    printf("%s", node_type_name(node->type));
    if (node->text) printf(" '%s'", node->text);
    if (node->type == NODE_LITERAL) {
        if (node->mtype == TYPE_STR) printf(" str=\"%s\"", node->text ? node->text : "");
        else printf(" val=%ld", node->ival);
    }
    if (node->type == NODE_BINOP) printf(" op=%s", token_type_name((TokenType)node->inst));
    printf("\n");
    ast_dump(node->left, depth + 1);
    ast_dump(node->right, depth + 1);
    ast_dump(node->cond, depth + 1);
    ast_dump(node->then_block, depth + 1);
    ast_dump(node->else_block, depth + 1);
    for (int i = 0; i < node->body.count; i++) ast_dump(node->body.items[i], depth + 1);
    for (int i = 0; i < node->args.count; i++) ast_dump(node->args.items[i], depth + 1);
}
