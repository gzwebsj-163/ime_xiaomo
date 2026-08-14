/*
 * xiaomo - 权重加载器实现
 * 简洁 JSON 子集解析 (只支持数值 + 嵌套数组 + 对象), 用于从权重文件加载参数。
 */
#include "weights.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ---- 极简 JSON 子集解析器 (只处理 对象{}/数组[]/字符串"…"/数值) ---- */

typedef struct {
    const char* p;
    const char* end;
    int err;
} JsonCtx;

static void jskip(JsonCtx* c) {
    while (c->p < c->end && isspace((unsigned char)*c->p)) c->p++;
}

static int jpeek(JsonCtx* c) { return c->p < c->end ? (unsigned char)*c->p : 0; }

/* 解析带引号字符串 -> 复制到 buf, 返回 0 成功 */
static int jstring(JsonCtx* c, char* buf, int buflen) {
    jskip(c);
    if (c->p >= c->end || *c->p != '"') { c->err = 1; return -1; }
    c->p++;
    int n = 0;
    while (c->p < c->end && *c->p != '"') {
        if (*c->p == '\\' && c->p + 1 < c->end) c->p++;
        if (n < buflen - 1) buf[n++] = *c->p;
        c->p++;
    }
    buf[n] = '\0';
    if (c->p >= c->end) { c->err = 1; return -1; }
    c->p++; /* skip closing quote */
    return 0;
}

/* 解析一个数值 -> out */
static int jnumber(JsonCtx* c, double* out) {
    jskip(c);
    char buf[64]; int n = 0;
    while (c->p < c->end && (isdigit((unsigned char)*c->p) || *c->p == '-' ||
                             *c->p == '+' || *c->p == '.' ||
                             *c->p == 'e' || *c->p == 'E')) {
        if (n < 63) buf[n++] = *c->p;
        c->p++;
    }
    buf[n] = '\0';
    if (n == 0) { c->err = 1; return -1; }
    *out = strtod(buf, NULL);
    return 0;
}

/* 递归构造 MoValue 数组: 把当前 JSON 值 (数组或数值) 转为 MoValue */
static MoValue jvalue_to_mo(JsonCtx* c) {
    MoValue v; memset(&v, 0, sizeof(v));
    jskip(c);
    if (jpeek(c) == '[') {
        /* 数组 -> VAL_ARRAY */
        v.type = VAL_ARRAY;
        v.arr = (MoArray*)calloc(1, sizeof(MoArray));
        c->p++; /* '[' */
        jskip(c);
        if (jpeek(c) != ']') {
            for (;;) {
                MoValue e = jvalue_to_mo(c);
                /* 追加到 arr (手动扩容) */
                if (v.arr->count >= v.arr->capacity) {
                    int nc = v.arr->capacity ? v.arr->capacity * 2 : 8;
                    MoValue* ni = (MoValue*)realloc(v.arr->items, sizeof(MoValue) * (size_t)nc);
                    if (!ni) { free(e.arr); c->err = 1; return v; }
                    v.arr->items = ni; v.arr->capacity = nc;
                }
                v.arr->items[v.arr->count++] = e;
                jskip(c);
                if (jpeek(c) == ',') { c->p++; continue; }
                break;
            }
        }
        jskip(c);
        if (jpeek(c) == ']') c->p++; else c->err = 1;
    } else if (jpeek(c) == '"') {
        v.type = VAL_STR;
        char tmp[128]; if (jstring(c, tmp, sizeof(tmp)) != 0) v.type = VAL_NULL;
        else v.sval = strdup(tmp);
    } else {
        double d; if (jnumber(c, &d) != 0) { v.type = VAL_NULL; return v; }
        v.type = VAL_FLOAT; v.fval = d; v.ival = (long)d;
    }
    return v;
}

/* 在对象 { ... } 中查找 key, 返回其值的 MoValue; 找不到指针位置停在值开头返回 NULL */
static int jfind_key(JsonCtx* c, const char* key, MoValue* out) {
    jskip(c);
    if (jpeek(c) != '{') { c->err = 1; return 0; }
    c->p++; /* '{' */
    jskip(c);
    if (jpeek(c) == '}') return 0; /* 空对象 */
    for (;;) {
        jskip(c);
        char kbuf[128];
        if (jstring(c, kbuf, sizeof(kbuf)) != 0) return 0;
        jskip(c);
        if (jpeek(c) != ':') return 0;
        c->p++; /* ':' */
        if (strcmp(kbuf, key) == 0) {
            *out = jvalue_to_mo(c);
            return 1;
        }
        /* 跳过该键对应值 */
        MoValue tmp = jvalue_to_mo(c);
        if (tmp.type != VAL_NULL) {
            if (tmp.type == VAL_STR) free(tmp.sval);
            else if (tmp.arr) free(tmp.arr->items), free(tmp.arr);
        }
        jskip(c);
        if (jpeek(c) == ',') { c->p++; continue; }
        break;
    }
    return 0;
}

int xm_load_weight_from_json(const char* json, const char* key, MoValue* out,
                             char* errbuf, int errbuflen) {
    if (!json || !key || !out) return -1;
    JsonCtx c; c.p = json; c.end = json + strlen(json); c.err = 0;
    MoValue v; memset(&v, 0, sizeof(v));
    if (!jfind_key(&c, key, &v)) {
        if (errbuf) snprintf(errbuf, errbuflen, "权重文件中找不到键: %s", key);
        return -1;
    }
    if (v.type == VAL_NULL) {
        if (errbuf) snprintf(errbuf, errbuflen, "权重 %s 解析失败", key);
        return -1;
    }
    *out = v;
    return 0;
}

int xm_load_weight(const char* path, const char* key, MoValue* out,
                   char* errbuf, int errbuflen) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (errbuf) snprintf(errbuf, errbuflen, "无法打开权重文件: %s", path);
        return -1;
    }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 16 * 1024 * 1024) { fclose(f); if (errbuf) snprintf(errbuf, errbuflen, "权重文件过大或读取失败"); return -1; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); if (errbuf) snprintf(errbuf, errbuflen, "内存不足"); return -1; }
    size_t rd = fread(buf, 1, (size_t)sz, f); buf[rd] = '\0'; fclose(f);

    int rc = xm_load_weight_from_json(buf, key, out, errbuf, errbuflen);
    free(buf);
    return rc;
}
