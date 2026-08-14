/*
 * xiaomo - 张量推理内核实现
 * 提供原生 matmul / bias_add / relu / tanh / sigmoid / softmax / mul / transpose。
 * 矩阵表示: MoValue(VAL_ARRAY) of rows, 每行是一维 VAL_ARRAY of VAL_FLOAT。
 * 纯 C, 不依赖 vm.c 的 static helper, 直接操作公开 MoValue/MoArray 结构。
 */
#include "tensor.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

/* ---- 内部小工具 ---- */

static MoValue mk_float(double v) {
    MoValue x; memset(&x, 0, sizeof(x));
    x.type = VAL_FLOAT; x.fval = v; x.ival = (long)v;
    return x;
}

/* 新建空一维数组 */
static MoArray* new_array(void) {
    MoArray* a = (MoArray*)calloc(1, sizeof(MoArray));
    return a;
}

/* 往 MoArray 追加一个值 (返回 0 成功) */
static int arr_push(MoArray* a, MoValue v) {
    if (!a) return -1;
    if (a->count >= a->capacity) {
        int nc = a->capacity ? a->capacity * 2 : 8;
        MoValue* ni = (MoValue*)realloc(a->items, sizeof(MoValue) * (size_t)nc);
        if (!ni) return -1;
        a->items = ni; a->capacity = nc;
    }
    a->items[a->count++] = v;
    return 0;
}

/* 本地 null 值 */
static MoValue xm_null(void) {
    MoValue x; memset(&x, 0, sizeof(x)); x.type = VAL_NULL; return x;
}

/* 深度克隆一个 MoValue (用于逐元素算子原地克隆) */
static MoValue xm_clone(const MoValue* v) {
    MoValue r; memset(&r, 0, sizeof(r));
    if (!v) return xm_null();
    if (v->type == VAL_ARRAY && v->arr) {
        r.type = VAL_ARRAY;
        r.arr = new_array();
        for (int i = 0; i < v->arr->count; i++) {
            MoValue e = xm_clone(&v->arr->items[i]);
            arr_push(r.arr, e);
        }
        return r;
    }
    if (v->type == VAL_STR) {
        r.type = VAL_STR;
        r.sval = v->sval ? strdup(v->sval) : NULL;
        return r;
    }
    memcpy(&r, v, sizeof(MoValue));
    return r;
}

/* 创建 n 元素二维矩阵 (空) */
static MoValue empty_mat(int rows) {
    MoValue m; memset(&m, 0, sizeof(m));
    m.type = VAL_ARRAY;
    m.arr = new_array();
    for (int r = 0; r < rows; r++) {
        MoValue row; memset(&row, 0, sizeof(row));
        row.type = VAL_ARRAY; row.arr = new_array();
        arr_push(m.arr, row);
    }
    return m;
}

/* ---- 对外 API ---- */

MoValue xm_vec_new(int length, const double* data) {
    MoValue v; memset(&v, 0, sizeof(v));
    v.type = VAL_ARRAY; v.arr = new_array();
    for (int i = 0; i < length; i++) {
        arr_push(v.arr, mk_float(data ? data[i] : 0.0));
    }
    return v;
}

MoValue xm_mat_new(int rows, int cols, const double* data) {
    MoValue m = empty_mat(rows);
    for (int r = 0; r < rows; r++) {
        MoValue* row = &m.arr->items[r];
        row->arr = new_array();
        for (int c = 0; c < cols; c++) {
            double val = data ? data[r * cols + c] : 0.0;
            arr_push(row->arr, mk_float(val));
        }
    }
    return m;
}

MoValue xm_mat_zeros(int rows, int cols) {
    return xm_mat_new(rows, cols, NULL);
}

MoValue xm_mat_eye(int n) {
    MoValue m = xm_mat_zeros(n, n);
    for (int i = 0; i < n; i++) xm_mat_set(&m, i, i, 1.0);
    return m;
}

int xm_mat_shape(const MoValue* v, int* rows, int* cols) {
    if (!v || v->type != VAL_ARRAY || !v->arr) { *rows = 0; *cols = 0; return 0; }
    int r = v->arr->count;
    if (r == 0) { *rows = 0; *cols = 0; return 1; }
    /* 判断是一维还是二维 */
    MoValue first = v->arr->items[0];
    if (first.type == VAL_ARRAY && first.arr) {
        *rows = r;
        *cols = first.arr->count;
    } else {
        /* 一维向量 -> 1×r 行向量 */
        *rows = 1;
        *cols = r;
    }
    return 1;
}

int xm_mat_get(const MoValue* v, int r, int c, double* out) {
    if (!v || v->type != VAL_ARRAY || !v->arr) return -1;
    if (r < 0 || r >= v->arr->count) return -1;
    MoValue row = v->arr->items[r];
    if (row.type == VAL_ARRAY && row.arr) {
        if (c < 0 || c >= row.arr->count) return -1;
        MoValue e = row.arr->items[c];
        *out = (e.type == VAL_FLOAT || e.type == VAL_INT) ? e.fval : 0.0;
        return 0;
    } else {
        /* 扁平标量向量 (一维): 视为 1×n, 行 r 必须为 0, 列 c 索引到 items */
        if (r != 0) return -1;
        if (c < 0 || c >= v->arr->count) return -1;
        MoValue e = v->arr->items[c];
        *out = (e.type == VAL_FLOAT || e.type == VAL_INT) ? e.fval : 0.0;
        return 0;
    }
}

int xm_mat_set(MoValue* v, int r, int c, double val) {
    if (!v || v->type != VAL_ARRAY || !v->arr) return -1;
    if (r < 0 || r >= v->arr->count) return -1;
    MoValue* row = &v->arr->items[r];
    if (row->type == VAL_ARRAY && row->arr) {
        if (c < 0 || c >= row->arr->count) return -1;
        row->arr->items[c] = mk_float(val);
        return 0;
    }
    /* 扁平标量向量: 视为 1×n, 行 r 必须为 0 */
    if (r != 0) return -1;
    if (c < 0 || c >= v->arr->count) return -1;
    v->arr->items[c] = mk_float(val);
    return 0;
}

void xm_mat_to_flat(const MoValue* v, double* buf) {
    int r, c, rows, cols;
    if (!xm_mat_shape(v, &rows, &cols)) return;
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
            xm_mat_get(v, r, c, &buf[r * cols + c]);
}

/* 判断是否为标量 (INT/FLOAT) */
static int is_scalar(const MoValue* v) {
    return v && (v->type == VAL_INT || v->type == VAL_FLOAT);
}
static double scalar_val(const MoValue* v) { return v->fval; }

/* ---- 算子 ---- */

MoValue xm_matmul(const MoValue* A, const MoValue* B) {
    int ma, ka, mb, kb;
    if (!xm_mat_shape(A, &ma, &ka) || !xm_mat_shape(B, &mb, &kb)) return xm_null();
    if (ka != mb) return xm_null();  /* 维度不匹配 */
    MoValue C = xm_mat_zeros(ma, kb);
    double *fa = (double*)malloc(sizeof(double) * (size_t)(ma * ka ? ma * ka : 1));
    double *fb = (double*)malloc(sizeof(double) * (size_t)(mb * kb ? mb * kb : 1));
    xm_mat_to_flat(A, fa); xm_mat_to_flat(B, fb);
    for (int i = 0; i < ma; i++) {
        for (int j = 0; j < kb; j++) {
            double s = 0;
            for (int k = 0; k < ka; k++)
                s += fa[i * ka + k] * fb[k * kb + j];
            xm_mat_set(&C, i, j, s);
        }
    }
    free(fa); free(fb);
    return C;
}

MoValue xm_bias_add(const MoValue* X, const MoValue* b) {
    int r, c, br, bc;
    if (!xm_mat_shape(X, &r, &c)) return xm_null();
    MoValue Y = xm_clone(X);
    if (is_scalar(b)) {
        double bias = scalar_val(b);
        for (int i = 0; i < r; i++)
            for (int j = 0; j < c; j++) {
                double v; xm_mat_get(&Y, i, j, &v); xm_mat_set(&Y, i, j, v + bias);
            }
        return Y;
    }
    if (!xm_mat_shape(b, &br, &bc)) return Y;
    double* fb = (double*)malloc(sizeof(double) * (size_t)(br * bc ? br * bc : 1));
    xm_mat_to_flat(b, fb);
    for (int i = 0; i < r; i++) {
        for (int j = 0; j < c; j++) {
            double bv = 0.0;
            /* bias 向量为 1×c 或 c 向量 */
            if (br == 1 && bc == c) bv = fb[j];
            else if (br == 1 && bc == 1) bv = fb[0];
            else if (br == c && bc == 1) bv = fb[i]; /* 偏置按列 */
            double v; xm_mat_get(&Y, i, j, &v); xm_mat_set(&Y, i, j, v + bv);
        }
    }
    free(fb);
    return Y;
}

MoValue xm_relu(const MoValue* X) {
    MoValue Y = xm_clone(X);
    int r, c; if (!xm_mat_shape(&Y, &r, &c)) return Y;
    for (int i = 0; i < r; i++)
        for (int j = 0; j < c; j++) {
            double v; xm_mat_get(&Y, i, j, &v);
            xm_mat_set(&Y, i, j, v > 0 ? v : 0.0);
        }
    return Y;
}

MoValue xm_tanh(const MoValue* X) {
    MoValue Y = xm_clone(X);
    int r, c; if (!xm_mat_shape(&Y, &r, &c)) return Y;
    for (int i = 0; i < r; i++)
        for (int j = 0; j < c; j++) {
            double v; xm_mat_get(&Y, i, j, &v);
            xm_mat_set(&Y, i, j, tanh(v));
        }
    return Y;
}

MoValue xm_sigmoid(const MoValue* X) {
    MoValue Y = xm_clone(X);
    int r, c; if (!xm_mat_shape(&Y, &r, &c)) return Y;
    for (int i = 0; i < r; i++)
        for (int j = 0; j < c; j++) {
            double v; xm_mat_get(&Y, i, j, &v);
            xm_mat_set(&Y, i, j, 1.0 / (1.0 + exp(-v)));
        }
    return Y;
}

MoValue xm_softmax(const MoValue* X) {
    int r, c;
    if (!xm_mat_shape(X, &r, &c)) return xm_null();
    MoValue Y = xm_mat_zeros(r, c);
    double* row = (double*)malloc(sizeof(double) * (size_t)(c ? c : 1));
    for (int i = 0; i < r; i++) {
        double m = -1e30;
        for (int j = 0; j < c; j++) { xm_mat_get(X, i, j, &row[j]); if (row[j] > m) m = row[j]; }
        double s = 0;
        for (int j = 0; j < c; j++) { row[j] = exp(row[j] - m); s += row[j]; }
        for (int j = 0; j < c; j++) xm_mat_set(&Y, i, j, row[j] / s);
    }
    free(row);
    return Y;
}

MoValue xm_mul(const MoValue* A, const MoValue* B) {
    int ra, ca, rb, cb;
    if (!xm_mat_shape(A, &ra, &ca) || !xm_mat_shape(B, &rb, &cb)) return xm_null();
    if (ra != rb || ca != cb) return xm_null();
    MoValue Y = xm_mat_zeros(ra, ca);
    for (int i = 0; i < ra; i++)
        for (int j = 0; j < ca; j++) {
            double a, b; xm_mat_get(A, i, j, &a); xm_mat_get(B, i, j, &b);
            xm_mat_set(&Y, i, j, a * b);
        }
    return Y;
}

MoValue xm_transpose(const MoValue* A) {
    int r, c;
    if (!xm_mat_shape(A, &r, &c)) return xm_null();
    MoValue T = xm_mat_zeros(c, r);
    for (int i = 0; i < r; i++)
        for (int j = 0; j < c; j++) {
            double v; xm_mat_get(A, i, j, &v); xm_mat_set(&T, j, i, v);
        }
    return T;
}

/* 逐元素相减 A - B (形状必须相同) */
MoValue xm_sub(const MoValue* A, const MoValue* B) {
    int ra, ca, rb, cb;
    if (!xm_mat_shape(A, &ra, &ca) || !xm_mat_shape(B, &rb, &cb)) return xm_null();
    if (ra != rb || ca != cb) return xm_null();
    MoValue Y = xm_mat_zeros(ra, ca);
    for (int i = 0; i < ra; i++)
        for (int j = 0; j < ca; j++) {
            double a, b; xm_mat_get(A, i, j, &a); xm_mat_get(B, i, j, &b);
            xm_mat_set(&Y, i, j, a - b);
        }
    return Y;
}

/* 逐元素数乘 A * k (学习率/缩放) */
MoValue xm_scale(const MoValue* A, double k) {
    int r, c;
    if (!xm_mat_shape(A, &r, &c)) return xm_null();
    MoValue Y = xm_mat_zeros(r, c);
    for (int i = 0; i < r; i++)
        for (int j = 0; j < c; j++) {
            double a; xm_mat_get(A, i, j, &a); xm_mat_set(&Y, i, j, a * k);
        }
    return Y;
}

/* 标量损失值 (VAL_FLOAT) */
static MoValue mk_scalar_loss(double v) {
    MoValue x; memset(&x, 0, sizeof(x));
    x.type = VAL_FLOAT; x.fval = v; x.ival = (long)v;
    return x;
}

/* 均方误差标量: mean((A-B)^2) */
MoValue xm_loss_mse(const MoValue* A, const MoValue* B) {
    int ra, ca, rb, cb;
    if (!xm_mat_shape(A, &ra, &ca) || !xm_mat_shape(B, &rb, &cb)) return xm_null();
    if (ra != rb || ca != cb) return xm_null();
    double sum = 0; int n = ra * ca;
    for (int i = 0; i < ra; i++)
        for (int j = 0; j < ca; j++) {
            double a, b; xm_mat_get(A, i, j, &a); xm_mat_get(B, i, j, &b);
            double d = a - b; sum += d * d;
        }
    return mk_scalar_loss(n ? sum / n : 0.0);
}

/* 交叉熵标量: -mean( sum_c y_true_c * log(y_pred_c + eps) ) */
MoValue xm_loss_crossentropy(const MoValue* A, const MoValue* B) {
    int ra, ca, rb, cb;
    if (!xm_mat_shape(A, &ra, &ca) || !xm_mat_shape(B, &rb, &cb)) return xm_null();
    if (ra != rb || ca != cb) return xm_null();
    const double eps = 1e-12;
    double sum = 0; int n = ra * ca;
    for (int i = 0; i < ra; i++)
        for (int j = 0; j < ca; j++) {
            double yt, yp; xm_mat_get(A, i, j, &yt); xm_mat_get(B, i, j, &yp);
            if (yt > 0.0) sum += yt * log(yp + eps);
        }
    return mk_scalar_loss(n ? -sum / n : 0.0);
}

/* tanh 导数: (1 - h^2) 逐元素 */
MoValue xm_grad_tanh(const MoValue* H) {
    int r, c;
    if (!xm_mat_shape(H, &r, &c)) return xm_null();
    MoValue Y = xm_mat_zeros(r, c);
    for (int i = 0; i < r; i++)
        for (int j = 0; j < c; j++) {
            double h; xm_mat_get(H, i, j, &h);
            xm_mat_set(&Y, i, j, 1.0 - h * h);
        }
    return Y;
}

/* MSE 梯度: (2/len)(A - B) 逐元素 */
MoValue xm_grad_mse(const MoValue* A, const MoValue* B) {
    int ra, ca, rb, cb;
    if (!xm_mat_shape(A, &ra, &ca) || !xm_mat_shape(B, &rb, &cb)) return xm_null();
    if (ra != rb || ca != cb) return xm_null();
    MoValue Y = xm_mat_zeros(ra, ca);
    double k = (ra * ca) ? 2.0 / (double)(ra * ca) : 0.0;
    for (int i = 0; i < ra; i++)
        for (int j = 0; j < ca; j++) {
            double a, b; xm_mat_get(A, i, j, &a); xm_mat_get(B, i, j, &b);
            xm_mat_set(&Y, i, j, k * (a - b));
        }
    return Y;
}

void xm_mat_print(const MoValue* v, char* buf, int buflen) {
    if (!v || buflen <= 0) return;
    int r, c; xm_mat_shape(v, &r, &c);
    int off = 0;
    off += snprintf(buf + off, buflen - off, "[");
    for (int i = 0; i < r && off < buflen - 1; i++) {
        if (i > 0) off += snprintf(buf + off, buflen - off, "; ");
        off += snprintf(buf + off, buflen - off, "[");
        for (int j = 0; j < c && off < buflen - 1; j++) {
            double vv; xm_mat_get(v, i, j, &vv);
            if (j > 0) off += snprintf(buf + off, buflen - off, ", ");
            off += snprintf(buf + off, buflen - off, "%.4g", vv);
        }
        off += snprintf(buf + off, buflen - off, "]");
    }
    if (off < buflen - 1) snprintf(buf + off, buflen - off, "]");
}
