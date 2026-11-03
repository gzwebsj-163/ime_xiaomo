/* xiaomo 嵌入式实现：与 src/main.c 的 cmd_run 等价，但不依赖文件系统之外的任何宿主功能。 */
#include "xiaomo_embed.h"
#include "parser.h"
#include "vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static char* xmo_read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1u << 16;
    size_t n = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) { fclose(f); return NULL; }

    for (;;) {
        if (n == cap - 1) {
            if (cap > SIZE_MAX / 2) { free(buf); fclose(f); return NULL; }
            size_t next_cap = cap * 2;
            char* next = (char*)realloc(buf, next_cap);
            if (!next) { free(buf); fclose(f); return NULL; }
            buf = next;
            cap = next_cap;
        }
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (r == 0) break;
    }

    if (ferror(f)) { free(buf); fclose(f); return NULL; }
    fclose(f);
    buf[n] = '\0';
    return buf;
}

int xmo_run_source(const char* src, XmoResult* res) {
    if (!res) return -1;
    memset(res, 0, sizeof(*res));
    if (!src) {
        res->error = 1;
        snprintf(res->error_msg, sizeof(res->error_msg), "源码为空");
        return 1;
    }
    char errbuf[1024] = "";
    AstNode* program = xiaomo_parse_source(src, errbuf, sizeof(errbuf));
    if (!program) {
        res->error = 2;
        snprintf(res->error_msg, sizeof(res->error_msg), "解析失败: %s", errbuf);
        return 2;
    }
    VM vm;
    vm_init(&vm);
    int rc = vm_run(&vm, program);
    if (rc != 0) {
        res->error = 3;
        if (vm.error_count > 0 && vm.error_msg[0] != '\0') {
            snprintf(res->error_msg, sizeof(res->error_msg), "执行错误: %s", vm.error_msg);
        } else {
            snprintf(res->error_msg, sizeof(res->error_msg), "执行失败(rc=%d)", rc);
        }
    }
    int cnt = vm_output_count(&vm);
    if (cnt > 0) {
        size_t len = 1;
        for (int i = 0; i < cnt; i++) len += strlen(vm_output(&vm, i)) + 1;
        char* out = (char*)malloc(len);
        if (out) {
            size_t p = 0;
            for (int i = 0; i < cnt; i++) {
                const char* line = vm_output(&vm, i);
                size_t l = strlen(line);
                memcpy(out + p, line, l);
                p += l;
                out[p++] = '\n';
            }
            out[p] = '\0';
            res->output = out;
        } else {
            res->error = 3;
            snprintf(res->error_msg, sizeof(res->error_msg), "输出内存不足");
        }
    }
    vm_free(&vm);
    ast_free(program);
    return res->error;
}

int xmo_run_file(const char* path, XmoResult* res) {
    if (!res) return -1;
    memset(res, 0, sizeof(*res));
    char* src = xmo_read_all(path);
    if (!src) {
        res->error = 1;
        snprintf(res->error_msg, sizeof(res->error_msg), "无法读取文件: %s", path);
        return 1;
    }
    int rc = xmo_run_source(src, res);
    free(src);
    return rc;
}

void xmo_result_free(XmoResult* res) {
    if (!res) return;
    free(res->output);
    res->output = NULL;
}

int xmo_exec(const char* path, char** out, char* errbuf, int errlen) {
    XmoResult res;
    int rc = xmo_run_file(path, &res);
    if (out) *out = res.output;      /* 所有权转移给调用方 */
    else { free(res.output); res.output = NULL; }
    if (errbuf && errlen > 0) {
        errbuf[0] = '\0';
        if (rc != 0) snprintf(errbuf, (size_t)errlen, "%s", res.error_msg);
    }
    res.output = NULL;
    xmo_result_free(&res);
    return rc;
}

void xmo_free(char* p) { free(p); }
