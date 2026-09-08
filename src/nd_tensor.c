/*
 * nd_tensor.c — ND Tensor 引擎实现
 *
 * 纯 C, 无外部依赖。flat buffer + shape 表示, 行主序。
 * 支持: 张量缩并(tensordot)/广播/逐元素/归约/形状操作/卷积/颜色空间
 */
#include "nd_tensor.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdio.h>
#include <stdarg.h>

/* ========== 本地辅助函数 (独立于 vm.c 的 static) ========== */

static int arr_append_local(MoArray* a, MoValue v) {
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

/* ========== 内部工具 ========== */

static int* shape_dup(const int* s, int n) {
    int* d = (int*)malloc(sizeof(int) * (size_t)(n ? n : 1));
    if (d && n) memcpy(d, s, sizeof(int) * (size_t)n);
    return d;
}

static int prod(const int* s, int n) {
    int p = 1;
    for (int i = 0; i < n; i++) p *= s[i];
    return p;
}

/* 安全 max/min */
static int imax(int a, int b) { return a > b ? a : b; }
static int imin(int a, int b) { return a < b ? a : b; }

/* 工具: 将 flat 索引转为多维索引 (行主序/前向遍历) */
static void flat_to_idx(int flat, const int* shape, int ndim, int* out_idx) {
    int remainder = flat;
    for (int i = 0; i < ndim; i++) {
        int stride = 1;
        for (int j = i + 1; j < ndim; j++) stride *= shape[j];
        out_idx[i] = stride > 0 ? remainder / stride : 0;
        remainder = stride > 0 ? remainder % stride : 0;
    }
}

/* 工具: 将多维索引转为 flat 索引 (行主序) */
static int idx_to_flat(const int* idx, const int* shape, int ndim) {
    int flat = 0;
    for (int i = 0; i < ndim; i++) {
        int stride = 1;
        for (int j = i + 1; j < ndim; j++) stride *= shape[j];
        flat += idx[i] * stride;
    }
    return flat;
}

/* ========== 创建与销毁 ========== */

NdTensor* nd_create(const int* shape, int ndim, const double* data) {
    if (ndim < 0) return NULL;
    NdTensor* t = (NdTensor*)calloc(1, sizeof(NdTensor));
    if (!t) return NULL;
    int sz = ndim > 0 ? prod(shape, ndim) : 1;
    t->shape = ndim > 0 ? shape_dup(shape, ndim) : NULL;
    t->ndim = ndim;
    t->size = sz;
    t->data = (double*)malloc(sizeof(double) * (size_t)(sz ? sz : 1));
    if (data) {
        memcpy(t->data, data, sizeof(double) * (size_t)sz);
    } else if (sz > 0) {
        memset(t->data, 0, sizeof(double) * (size_t)sz);
    }
    return t;
}

NdTensor* nd_zeros(const int* shape, int ndim) {
    return nd_create(shape, ndim, NULL);
}

NdTensor* nd_ones(const int* shape, int ndim) {
    NdTensor* t = nd_create(shape, ndim, NULL);
    if (t) for (int i = 0; i < t->size; i++) t->data[i] = 1.0;
    return t;
}

NdTensor* nd_scalar(double val) {
    NdTensor* t = nd_create(NULL, 0, &val);
    return t;
}

/* 辅助: 递归探测嵌套数组的维度数 */
static int _probe_ndim(const MoValue* v) {
    if (!v || v->type != VAL_ARRAY || !v->arr || v->arr->count == 0) return 0;
    MoValue first = v->arr->items[0];
    if (first.type == VAL_ARRAY && first.arr && first.arr->count > 0)
        return 1 + _probe_ndim(&first);
    return 1;
}

/* 辅助: 递归探测形状 (ndim已知, 填充shape数组) */
static void _probe_shape(const MoValue* v, int* shape, int depth) {
    if (!v || v->type != VAL_ARRAY || !v->arr || depth < 0) return;
    shape[0] = v->arr->count;
    if (depth > 0 && v->arr->count > 0) {
        MoValue first = v->arr->items[0];
        if (first.type == VAL_ARRAY && first.arr)
            _probe_shape(&first, shape + 1, depth - 1);
    }
}

/* 辅助: 递归填充数据, idx为当前维度索引数组 */
static void _fill_from_movalue(const MoValue* v, NdTensor* t, const int* shape,
                              int depth, int* idx, int dim) {
    if (!v || !t || !v->arr) return;
    int count = v->arr->count;
    int expected = shape[dim];
    for (int i = 0; i < count && i < expected; i++) {
        idx[dim] = i;
        MoValue child = v->arr->items[i];
        if (dim == depth - 1) {
            /* 最内层: child 是标量 (VAL_FLOAT/VAL_INT) */
            if (child.type == VAL_FLOAT || child.type == VAL_INT) {
                int flat = idx_to_flat(idx, shape, depth);
                if (flat >= 0 && flat < t->size)
                    t->data[flat] = child.fval;
            }
        } else if (child.type == VAL_ARRAY && child.arr) {
            _fill_from_movalue(&child, t, shape, depth, idx, dim + 1);
        }
    }
}

/* 从嵌套 MoValue(VAL_ARRAY) 递归转换 (支持任意维度) */
NdTensor* nd_from_movalue(const MoValue* v) {
    if (!v || v->type != VAL_ARRAY || !v->arr || v->arr->count == 0) return NULL;
    int ndim = _probe_ndim(v);
    if (ndim < 1) return NULL;
    int* shape = (int*)calloc(sizeof(int), (size_t)ndim);
    if (!shape) return NULL;
    _probe_shape(v, shape, ndim - 1);
    NdTensor* t = nd_create(shape, ndim, NULL);
    if (!t) { free(shape); return NULL; }
    int* idx = (int*)calloc(sizeof(int), (size_t)ndim);
    if (!idx) { nd_free(t); free(shape); return NULL; }
    _fill_from_movalue(v, t, shape, ndim, idx, 0);
    free(idx);
    free(shape);
    return t;
}

/* 辅助: 递归构建嵌套 MoValue(VAL_ARRAY), 从 idx[dim] 开始填充 */
static void _to_movalue_rec(const NdTensor* t, MoValue* parent, int* idx, int dim) {
    if (dim == t->ndim - 1) {
        /* 最内层: 创建标量数组 (最后一个维度的每个元素是 float) */
        int n = t->shape[dim];
        for (int j = 0; j < n; j++) {
            idx[dim] = j;
            int flat = idx_to_flat(idx, t->shape, t->ndim);
            MoValue e; memset(&e,0,sizeof(e));
            e.type = VAL_FLOAT; e.fval = (flat >= 0 && flat < t->size) ? t->data[flat] : 0.0;
            arr_append_local(parent->arr, e);
        }
    } else {
        /* 非最内层: 创建子数组, 递归 */
        int n = t->shape[dim];
        for (int j = 0; j < n; j++) {
            idx[dim] = j;
            MoValue child; memset(&child,0,sizeof(child));
            child.type = VAL_ARRAY; child.arr = (MoArray*)calloc(1,sizeof(MoArray));
            _to_movalue_rec(t, &child, idx, dim + 1);
            arr_append_local(parent->arr, child);
        }
    }
}

/* 转为 MoValue(VAL_ARRAY) — 递归支持任意维度 */
MoValue nd_to_movalue(const NdTensor* t) {
    if (!t) { MoValue x; memset(&x,0,sizeof(x)); x.type=VAL_NULL; return x; }
    if (t->ndim == 0) {
        /* 标量 -> 直接 float */
        MoValue x; memset(&x,0,sizeof(x));
        x.type = VAL_FLOAT; x.fval = t->data ? t->data[0] : 0.0;
        return x;
    }
    MoValue v; memset(&v,0,sizeof(v));
    v.type = VAL_ARRAY; v.arr = (MoArray*)calloc(1,sizeof(MoArray));
    int* idx = (int*)calloc(sizeof(int), (size_t)t->ndim);
    if (idx) { _to_movalue_rec(t, &v, idx, 0); free(idx); }
    return v;
}

NdTensor* nd_clone(const NdTensor* t) {
    if (!t) return NULL;
    return nd_create(t->shape, t->ndim, t->data);
}

void nd_free(NdTensor* t) {
    if (!t) return;
    if (t->data) free(t->data);
    if (t->shape) free(t->shape);
    free(t);
}

/* ========== 查询 ========== */

int nd_size(const NdTensor* t) { return t ? t->size : 0; }
int nd_dim(const NdTensor* t, int i) {
    if (!t || i < 0 || i >= t->ndim) return -1;
    if (t->ndim == 0) return 1;  /* 标量视为 1 */
    return t->shape[i];
}

/* 扁平索引计算 */
int nd_offset(const NdTensor* t, const int* indices) {
    if (!t || t->ndim == 0) return 0;
    int off = 0, stride = 1;
    for (int i = t->ndim - 1; i >= 0; i--) {
        int idx = indices[i];
        if (idx < 0) idx += t->shape[i];  /* 负索引 */
        if (idx < 0 || idx >= t->shape[i]) return -1;
        off += idx * stride;
        stride *= t->shape[i];
    }
    return off;
}

double nd_get(const NdTensor* t, const int* indices) {
    if (!t || !t->data) return 0.0;
    int off = nd_offset(t, indices);
    return (off >= 0 && off < t->size) ? t->data[off] : 0.0;
}

void nd_set(NdTensor* t, const int* indices, double val) {
    if (!t || !t->data) return;
    int off = nd_offset(t, indices);
    if (off >= 0 && off < t->size) t->data[off] = val;
}

double nd_get_flat(const NdTensor* t, int flat_idx) {
    if (!t || !t->data || flat_idx < 0 || flat_idx >= t->size) return 0.0;
    return t->data[flat_idx];
}

void nd_set_flat(NdTensor* t, int flat_idx, double val) {
    if (!t || !t->data || flat_idx < 0 || flat_idx >= t->size) return;
    t->data[flat_idx] = val;
}

/* ========== 广播 ========== */

int nd_broadcastable(const int* sha, int nda, const int* shb, int ndb) {
    int maxd = imax(nda, ndb);
    for (int i = 1; i <= maxd; i++) {
        int da = i <= nda ? sha[nda - i] : 1;
        int db = i <= ndb ? shb[ndb - i] : 1;
        if (da != 1 && db != 1 && da != db) return 0;
    }
    return 1;
}

int nd_broadcast_shape(const NdTensor* a, const NdTensor* b, int* out_shape) {
    if (!a || !b) return 0;
    int maxd = imax(a->ndim, b->ndim);
    for (int i = 0; i < maxd; i++) {
        int da = i < maxd - a->ndim ? 1 : a->shape[i - (maxd - a->ndim)];
        int db = i < maxd - b->ndim ? 1 : b->shape[i - (maxd - b->ndim)];
        out_shape[i] = imax(da, db);
    }
    return maxd;
}

/* 将索引从广播形状映射回原始形状 */
static int map_broadcast_idx(const int* idx, int maxd,
                              const int* shape, int ndim) {
    /* 倒着算偏移 */
    int off = 0, stride = 1;
    for (int i = maxd - 1; i >= 0; i--) {
        int si = i < maxd - ndim ? 1 : shape[i - (maxd - ndim)];
        int ii = (si == 1) ? 0 : idx[i];
        off += ii * stride;
        stride *= si;
    }
    return off;
}

NdTensor* nd_broadcast_to(const NdTensor* t, const int* target_shape, int target_ndim) {
    if (!t) return NULL;
    if (t->ndim == target_ndim) {
        int same = 1;
        for (int i = 0; i < target_ndim; i++) if (t->shape[i] != target_shape[i]) { same = 0; break; }
        if (same) return nd_clone(t);
    }
    /* 检查广播可行性 */
    if (!nd_broadcastable(t->shape, t->ndim, target_shape, target_ndim)) return NULL;
    NdTensor* r = nd_zeros(target_shape, target_ndim);
    int maxd = target_ndim;
    /* 填充每个目标索引 */
    int* idx = (int*)calloc(sizeof(int), (size_t)(maxd ? maxd : 1));
    for (int flat = 0; flat < r->size; flat++) {
        flat_to_idx(flat, target_shape, maxd, idx);
        int src_off = map_broadcast_idx(idx, maxd, t->shape, t->ndim);
        r->data[flat] = t->data[src_off];
    }
    free(idx);
    return r;
}

/* ========== 逐元素迭代器 ========== */

/* 对两个广播后的张量做逐元素二元运算 */
typedef double (*binary_op_t)(double a, double b);
typedef double (*unary_op_t)(double a);

static NdTensor* nd_binary_op(const NdTensor* A, const NdTensor* B, binary_op_t op) {
    if (!A || !B) return NULL;
    int out_ndim = imax(A->ndim, B->ndim);
    int* out_shape = (int*)calloc(sizeof(int), (size_t)(out_ndim ? out_ndim : 1));
    out_ndim = nd_broadcast_shape(A, B, out_shape);
    NdTensor* R = nd_zeros(out_shape, out_ndim);
    free(out_shape);
    if (!R || R->size == 0) { free(out_shape); return R; }

    /* 广播 A 和 B 到目标形状, 然后逐元素 */
    NdTensor* brA = nd_broadcast_to(A, R->shape, R->ndim);
    NdTensor* brB = nd_broadcast_to(B, R->shape, R->ndim);
    if (!brA || !brB) { nd_free(R); nd_free(brA); nd_free(brB); return NULL; }
    for (int i = 0; i < R->size; i++) R->data[i] = op(brA->data[i], brB->data[i]);
    nd_free(brA); nd_free(brB);
    return R;
}

static NdTensor* nd_unary_op(const NdTensor* X, unary_op_t op) {
    if (!X) return NULL;
    NdTensor* R = nd_clone(X);
    if (!R) return NULL;
    for (int i = 0; i < R->size; i++) R->data[i] = op(R->data[i]);
    return R;
}

/* 二元运算函数 */
static double op_add(double a, double b) { return a + b; }
static double op_sub(double a, double b) { return a - b; }
static double op_mul(double a, double b) { return a * b; }
static double op_div(double a, double b) { return b != 0.0 ? a / b : 0.0; }
static double op_pow(double a, double b) { return pow(a, b); }

/* 一元运算函数 */
static double op_relu(double x) { return x > 0 ? x : 0.0; }
static double op_tanh(double x) { return tanh(x); }
static double op_sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }
static double op_exp(double x) { return exp(x); }
static double op_log(double x) { return x > 0 ? log(x) : -1e30; }
static double op_abs(double x) { return fabs(x); }
static double op_neg(double x) { return -x; }
static double op_sqrt(double x) { return x >= 0 ? sqrt(x) : 0.0; }

/* ========== 核心算子 ========== */

NdTensor* nd_add(const NdTensor* A, const NdTensor* B) { return nd_binary_op(A, B, op_add); }
NdTensor* nd_sub(const NdTensor* A, const NdTensor* B) { return nd_binary_op(A, B, op_sub); }
NdTensor* nd_mul(const NdTensor* A, const NdTensor* B) { return nd_binary_op(A, B, op_mul); }
NdTensor* nd_div(const NdTensor* A, const NdTensor* B) { return nd_binary_op(A, B, op_div); }
NdTensor* nd_pow(const NdTensor* A, const NdTensor* B) { return nd_binary_op(A, B, op_pow); }

NdTensor* nd_relu(const NdTensor* X) { return nd_unary_op(X, op_relu); }
NdTensor* nd_tanh(const NdTensor* X) { return nd_unary_op(X, op_tanh); }
NdTensor* nd_sigmoid(const NdTensor* X) { return nd_unary_op(X, op_sigmoid); }
NdTensor* nd_exp(const NdTensor* X) { return nd_unary_op(X, op_exp); }
NdTensor* nd_log(const NdTensor* X) { return nd_unary_op(X, op_log); }
NdTensor* nd_abs(const NdTensor* X) { return nd_unary_op(X, op_abs); }
NdTensor* nd_neg(const NdTensor* X) { return nd_unary_op(X, op_neg); }
NdTensor* nd_sqrt(const NdTensor* X) { return nd_unary_op(X, op_sqrt); }

/* ========== 张量缩并 (tensordot) ========== */

NdTensor* nd_tensordot(const NdTensor* A, const NdTensor* B,
                        const int* axes_a, const int* axes_b, int n_axes) {
    if (!A || !B || n_axes <= 0) return NULL;
    
    int nda = A->ndim, ndb = B->ndim;
    
    /* 确定缩并后的形状 */
    int na_free = nda - n_axes;
    int nb_free = ndb - n_axes;
    int out_ndim = na_free + nb_free;
    int* out_shape = (int*)calloc(sizeof(int), (size_t)(out_ndim ? out_ndim : 1));
    
    /* 标记哪些轴被缩并 */
    char* a_contracted = (char*)calloc(nda ? (size_t)nda : 1, 1);
    char* b_contracted = (char*)calloc(ndb ? (size_t)ndb : 1, 1);
    for (int k = 0; k < n_axes; k++) {
        if (axes_a[k] >= 0 && axes_a[k] < nda) a_contracted[axes_a[k]] = 1;
        if (axes_b[k] >= 0 && axes_b[k] < ndb) b_contracted[axes_b[k]] = 1;
    }
    
    int out_pos = 0;
    for (int i = 0; i < nda; i++) if (!a_contracted[i]) out_shape[out_pos++] = A->shape[i];
    for (int i = 0; i < ndb; i++) if (!b_contracted[i]) out_shape[out_pos++] = B->shape[i];
    
    /* 计算缩并总尺寸 */
    int contract_size = 1;
    for (int k = 0; k < n_axes; k++) contract_size *= A->shape[axes_a[k]];
    
    NdTensor* R = nd_zeros(out_shape, out_ndim);
    if (!R || R->size == 0) { free(out_shape); free(a_contracted); free(b_contracted); return R; }
    
    /* 三轮循环: 对 A 的自由轴 × B 的自由轴 × 缩并轴 */
    /* 构造索引数组 */
    int* idx_a = (int*)calloc(sizeof(int), (size_t)(nda ? nda : 1));
    int* idx_b = (int*)calloc(sizeof(int), (size_t)(ndb ? ndb : 1));
    int* idx_out = (int*)calloc(sizeof(int), (size_t)(out_ndim ? out_ndim : 1));
    
    /* 遍历输出所有元素 */
    for (int flat_out = 0; flat_out < R->size; flat_out++) {
        /* 将 flat_out 转为多维索引 idx_out */
        int tmp = flat_out;
        for (int i = out_ndim - 1; i >= 0; i--) {
            int stride = 1;
            for (int j = i + 1; j < out_ndim; j++) stride *= out_shape[j];
            idx_out[i] = stride > 0 ? tmp / stride : 0;
            tmp = stride > 0 ? tmp % stride : 0;
        }
        
        /* 将 idx_out 映射到 idx_a 和 idx_b 的自由轴部分 */
        int out_pos_a = 0, out_pos_b = na_free;
        for (int i = 0; i < nda; i++) {
            if (!a_contracted[i]) idx_a[i] = idx_out[out_pos_a++];
            /* 缩并轴后面循环遍历 */
        }
        for (int i = 0; i < ndb; i++) {
            if (!b_contracted[i]) idx_b[i] = idx_out[out_pos_b++];
        }
        
        /* 缩并求和 */
        double sum = 0.0;
        for (int k_flat = 0; k_flat < contract_size; k_flat++) {
            /* 将 k_flat 映射到各缩并轴 */
            int tmpk = k_flat;
            for (int k = n_axes - 1; k >= 0; k--) {
                int stride = 1;
                for (int kk = k + 1; kk < n_axes; kk++) stride *= A->shape[axes_a[kk]];
                int ak = axes_a[k];
                int bk = axes_b[k];
                int idx_val = stride > 0 ? tmpk / stride : 0;
                tmpk = stride > 0 ? tmpk % stride : 0;
                if (ak >= 0 && ak < nda) idx_a[ak] = idx_val;
                if (bk >= 0 && bk < ndb) idx_b[bk] = idx_val;
            }
            
            int off_a = idx_to_flat(idx_a, A->shape, nda);
            int off_b = idx_to_flat(idx_b, B->shape, ndb);
            sum += A->data[off_a] * B->data[off_b];
        }
        R->data[flat_out] = sum;
    }
    
    free(idx_a); free(idx_b); free(idx_out);
    free(a_contracted); free(b_contracted);
    free(out_shape);
    return R;
}

/* 矩阵乘法: A(...,m,k) × B(...,k,n) = C(...,m,n) */
NdTensor* nd_matmul(const NdTensor* A, const NdTensor* B) {
    if (!A || !B || A->ndim < 1 || B->ndim < 1) return NULL;
    int m = A->shape[A->ndim - 2];
    int k = A->shape[A->ndim - 1];
    int k2 = B->ndim >= 2 ? B->shape[B->ndim - 2] : B->shape[0];
    int n = B->shape[B->ndim - 1];
    if (k != k2) return NULL;
    
    /* 批处理维度广播 */
    int batch_ndim = imax(A->ndim - 2, B->ndim - 2);
    batch_ndim = imax(0, batch_ndim);
    int* batch_shape = (int*)calloc(sizeof(int), (size_t)(batch_ndim ? batch_ndim : 1));
    for (int i = 0; i < batch_ndim; i++) {
        int da = i < batch_ndim - (A->ndim - 2) ? 1 : A->shape[i - (batch_ndim - (A->ndim - 2))];
        int db = i < batch_ndim - (B->ndim - 2) ? 1 : B->shape[i - (batch_ndim - (B->ndim - 2))];
        batch_shape[i] = imax(da, db);
    }
    
    int out_ndim = batch_ndim + 2;
    int* out_shape = (int*)calloc(sizeof(int), (size_t)(out_ndim ? out_ndim : 1));
    for (int i = 0; i < batch_ndim; i++) out_shape[i] = batch_shape[i];
    out_shape[out_ndim - 2] = m;
    out_shape[out_ndim - 1] = n;
    
    NdTensor* R = nd_zeros(out_shape, out_ndim);
    if (!R || R->size == 0) { free(batch_shape); free(out_shape); return R; }
    
    /* 批处理矩阵乘: 对每个 batch 做 2D 矩阵乘 */
    int batch_size = R->size / (m * n);
    if (batch_size <= 1) batch_size = 1;
    
    /* 对每一批 */
    int* idx_a = (int*)calloc(sizeof(int), (size_t)(A->ndim ? A->ndim : 1));
    int* idx_b = (int*)calloc(sizeof(int), (size_t)(B->ndim ? B->ndim : 1));
    
    for (int b = 0; b < batch_size; b++) {
        /* 将 batch 索引映射到 A 和 B 的批维度 */
        int tmpb = b;
        for (int i = batch_ndim - 1; i >= 0; i--) {
            int stride = 1;
            for (int j = i + 1; j < batch_ndim; j++) stride *= batch_shape[j];
            int bi = stride > 0 ? tmpb / stride : 0;
            tmpb = stride > 0 ? tmpb % stride : 0;
            
            int a_idx = i < batch_ndim - (A->ndim - 2) ? 0 : i - (batch_ndim - (A->ndim - 2));
            if (a_idx >= 0 && a_idx < A->ndim - 2) {
                int as = A->shape[a_idx];
                idx_a[a_idx] = (as == 1) ? 0 : bi;
            }
            int b_idx = i < batch_ndim - (B->ndim - 2) ? 0 : i - (batch_ndim - (B->ndim - 2));
            if (b_idx >= 0 && b_idx < B->ndim - 2) {
                int bs = B->shape[b_idx];
                idx_b[b_idx] = (bs == 1) ? 0 : bi;
            }
        }
        
        /* 对这一批做 2D 矩阵乘: C[m,n] = A[m,k] × B[k,n] */
        double* c_block = &R->data[b * m * n];
        
        for (int i = 0; i < m; i++) {
            idx_a[A->ndim - 2] = i;
            for (int j = 0; j < n; j++) {
                idx_b[B->ndim - 1] = j;
                double sum = 0.0;
                for (int kk = 0; kk < k; kk++) {
                    idx_a[A->ndim - 1] = kk;
                    idx_b[B->ndim - 2] = kk;
                    int off_a = idx_to_flat(idx_a, A->shape, A->ndim);
                    int off_b = idx_to_flat(idx_b, B->shape, B->ndim);
                    sum += A->data[off_a] * B->data[off_b];
                }
                c_block[i * n + j] = sum;
            }
        }
    }
    
    free(idx_a); free(idx_b);
    free(batch_shape); free(out_shape);
    return R;
}

/* ========== 归约运算 ========== */

/* 对轴求和 */
static NdTensor* nd_reduce(const NdTensor* X, int axis, 
                            double (*reduce_init)(), 
                            double (*reduce_op)(double, double),
                            int keep_axis) {
    if (!X) return NULL;
    int target_axis = axis;
    if (target_axis < 0) target_axis = X->ndim - 1;
    if (target_axis < 0 || target_axis >= X->ndim) return nd_scalar(reduce_init());
    
    int out_ndim = keep_axis ? X->ndim : imax(1, X->ndim - 1);
    int* out_shape = (int*)calloc(sizeof(int), (size_t)(out_ndim ? out_ndim : 1));
    int out_pos = 0;
    for (int i = 0; i < X->ndim; i++) {
        if (i == target_axis) {
            if (keep_axis) out_shape[out_pos++] = 1;
        } else {
            out_shape[out_pos++] = X->shape[i];
        }
    }
    if (out_pos == 0) { out_shape[0] = 1; out_ndim = 1; }
    
    NdTensor* R = nd_zeros(out_shape, out_ndim);
    if (!R || R->size == 0) { free(out_shape); return R; }
    
    int ax_size = X->shape[target_axis];
    /* 对输出每个元素遍历缩并轴 */
    int* idx = (int*)calloc(sizeof(int), (size_t)(X->ndim ? X->ndim : 1));
    int* out_idx = (int*)calloc(sizeof(int), (size_t)(out_ndim ? out_ndim : 1));
    
    for (int flat_out = 0; flat_out < R->size; flat_out++) {
        /* 将 flat_out 转为多维索引 */
        int tmp = flat_out;
        for (int i = out_ndim - 1; i >= 0; i--) {
            int stride = 1;
            for (int j = i + 1; j < out_ndim; j++) stride *= out_shape[j];
            out_idx[i] = stride > 0 ? tmp / stride : 0;
            tmp = stride > 0 ? tmp % stride : 0;
        }
        
        /* 映射到输入索引 */
        int pos = 0;
        for (int i = 0; i < X->ndim; i++) {
            if (i == target_axis) continue;
            idx[i] = (pos < out_ndim) ? out_idx[pos++] : 0;
        }
        
        double val = reduce_init();
        for (int k = 0; k < ax_size; k++) {
            idx[target_axis] = k;
            int off = 0, stride = 1;
            for (int ii = X->ndim - 1; ii >= 0; ii--) { off += idx[ii] * stride; stride *= X->shape[ii]; }
            val = reduce_op(val, X->data[off]);
        }
        R->data[flat_out] = val;
    }
    
    free(idx); free(out_idx);
    free(out_shape);
    return R;
}

static double reduce_sum_init() { return 0.0; }
static double reduce_sum_op(double a, double b) { return a + b; }
static double reduce_max_init() { return -1e30; }
static double reduce_max_op(double a, double b) { return a > b ? a : b; }
static double reduce_min_init() { return 1e30; }
static double reduce_min_op(double a, double b) { return a < b ? a : b; }

NdTensor* nd_sum(const NdTensor* X, int axis) {
    if (axis == -1) {
        /* 全局求和 */
        double s = 0.0;
        for (int i = 0; i < X->size; i++) s += X->data[i];
        return nd_scalar(s);
    }
    return nd_reduce(X, axis, reduce_sum_init, reduce_sum_op, 0);
}
NdTensor* nd_mean(const NdTensor* X, int axis) {
    if (axis == -1) {
        double s = 0.0;
        for (int i = 0; i < X->size; i++) s += X->data[i];
        return nd_scalar(X->size > 0 ? s / X->size : 0.0);
    }
    NdTensor* s = nd_reduce(X, axis, reduce_sum_init, reduce_sum_op, 0);
    if (s && s->size > 0) {
        double n = (double)X->shape[axis];
        for (int i = 0; i < s->size; i++) s->data[i] /= n;
    }
    return s;
}
NdTensor* nd_max(const NdTensor* X, int axis) {
    if (axis == -1) {
        double m = -1e30;
        for (int i = 0; i < X->size; i++) if (X->data[i] > m) m = X->data[i];
        return nd_scalar(m);
    }
    return nd_reduce(X, axis, reduce_max_init, reduce_max_op, 0);
}
NdTensor* nd_min(const NdTensor* X, int axis) {
    if (axis == -1) {
        double m = 1e30;
        for (int i = 0; i < X->size; i++) if (X->data[i] < m) m = X->data[i];
        return nd_scalar(m);
    }
    return nd_reduce(X, axis, reduce_min_init, reduce_min_op, 0);
}

/* softmax 沿指定轴 */
NdTensor* nd_softmax(const NdTensor* X, int axis) {
    if (!X || X->size == 0) return NULL;
    int ax = axis < 0 ? X->ndim - 1 : axis;
    if (ax < 0 || ax >= X->ndim) return nd_clone(X);
    
    NdTensor* R = nd_clone(X);
    if (!R) return NULL;
    
    int ndim = X->ndim;
    int ax_size = X->shape[ax];
    int outer_size = X->size / ax_size;
    
    /* 用 flat_to_idx/idx_to_flat 做 softmax */
    int* idx = (int*)malloc(sizeof(int) * (size_t)ndim);
    
    /* 对每个 outer 子空间做 softmax */
    for (int outer = 0; outer < outer_size; outer++) {
        /* 将 outer 映射到各非-ax 维度的索引 */
        int remainder = outer;
        for (int i = 0; i < ndim; i++) {
            if (i == ax) continue;
            int stride = 1;
            for (int j = i + 1; j < ndim; j++) {
                if (j == ax) continue;
                stride *= X->shape[j];
            }
            idx[i] = stride > 0 ? remainder / stride : 0;
            remainder = stride > 0 ? remainder % stride : 0;
        }
        
        /* 找最大值 */
        double maxv = -1e30;
        for (int k = 0; k < ax_size; k++) {
            idx[ax] = k;
            int flat = idx_to_flat(idx, X->shape, ndim);
            if (X->data[flat] > maxv) maxv = X->data[flat];
        }
        
        /* exp(x-max) + sum */
        double sum = 0.0;
        for (int k = 0; k < ax_size; k++) {
            idx[ax] = k;
            int flat = idx_to_flat(idx, X->shape, ndim);
            double e = exp(X->data[flat] - maxv);
            R->data[flat] = e;
            sum += e;
        }
        
        /* 归一化 */
        if (sum > 0) {
            for (int k = 0; k < ax_size; k++) {
                idx[ax] = k;
                int flat = idx_to_flat(idx, X->shape, ndim);
                R->data[flat] /= sum;
            }
        }
    }
    
    free(idx);
    return R;
}

/* ========== 形状操作 ========== */

NdTensor* nd_reshape(const NdTensor* t, const int* new_shape, int new_ndim) {
    if (!t) return NULL;
    /* 处理 -1 自动推断 */
    int* resolved = (int*)calloc(sizeof(int), (size_t)(new_ndim ? new_ndim : 1));
    int auto_idx = -1, total = 1;
    for (int i = 0; i < new_ndim; i++) {
        if (new_shape[i] == -1) { auto_idx = i; }
        else { resolved[i] = new_shape[i]; total *= new_shape[i]; }
    }
    if (auto_idx >= 0) {
        if (total == 0) { free(resolved); return NULL; }
        resolved[auto_idx] = t->size / total;
        if (resolved[auto_idx] * total != t->size) { free(resolved); return NULL; }
    }
    int new_sz = prod(resolved, new_ndim);
    if (new_sz != t->size) { free(resolved); return NULL; }
    
    NdTensor* r = nd_create(resolved, new_ndim, t->data);
    free(resolved);
    return r;
}

NdTensor* nd_transpose(const NdTensor* t, const int* perm) {
    if (!t || t->ndim == 0) return nd_clone(t);
    int n = t->ndim;
    int* p = (int*)malloc(sizeof(int) * (size_t)n);
    if (perm) {
        for (int i = 0; i < n; i++) p[i] = perm[i];
    } else {
        for (int i = 0; i < n; i++) p[i] = n - 1 - i;
    }
    
    int* new_shape = (int*)malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) new_shape[i] = t->shape[p[i]];
    
    NdTensor* r = nd_zeros(new_shape, n);
    if (!r) { free(p); free(new_shape); return NULL; }
    
    /* 搬运数据 */
    int* idx_src = (int*)calloc(sizeof(int), (size_t)(n ? n : 1));
    int* idx_dst = (int*)calloc(sizeof(int), (size_t)(n ? n : 1));
    
    for (int flat = 0; flat < t->size; flat++) {
        flat_to_idx(flat, t->shape, n, idx_src);
        for (int i = 0; i < n; i++) idx_dst[i] = idx_src[p[i]];
        int off_dst = idx_to_flat(idx_dst, new_shape, n);
        r->data[off_dst] = t->data[flat];
    }
    
    free(idx_src); free(idx_dst);
    free(p); free(new_shape);
    return r;
}

/* 切片 */
NdTensor* nd_slice(const NdTensor* t, const int* starts, const int* ends, const int* steps) {
    if (!t || t->ndim == 0) return nd_clone(t);
    int n = t->ndim;
    
    /* 计算切片后的形状 */
    int* new_shape = (int*)malloc(sizeof(int) * (size_t)n);
    for (int i = 0; i < n; i++) {
        int s = starts ? starts[i] : 0;
        int e = ends ? ends[i] : t->shape[i];
        int st = steps ? steps[i] : 1;
        if (s < 0) s += t->shape[i];
        if (e < 0) e += t->shape[i];
        s = imax(0, imin(s, t->shape[i]));
        e = imax(0, imin(e, t->shape[i]));
        st = imax(1, st);
        new_shape[i] = (e - s + st - 1) / st;
    }
    
    NdTensor* r = nd_zeros(new_shape, n);
    if (!r) { free(new_shape); return NULL; }
    
    int* idx_src = (int*)calloc(sizeof(int), (size_t)(n ? n : 1));
    int* idx_dst = (int*)calloc(sizeof(int), (size_t)(n ? n : 1));
    
    for (int flat = 0; flat < r->size; flat++) {
        flat_to_idx(flat, r->shape, n, idx_dst);
        for (int i = 0; i < n; i++) {
            int s = starts ? starts[i] : 0;
            int st = steps ? steps[i] : 1;
            if (s < 0) s += t->shape[i];
            s = imax(0, imin(s, t->shape[i]));
            idx_src[i] = s + idx_dst[i] * st;
        }
        int off_src = idx_to_flat(idx_src, t->shape, n);
        r->data[flat] = t->data[off_src];
    }
    
    free(idx_src); free(idx_dst);
    free(new_shape);
    return r;
}

/* 拼接 */
NdTensor* nd_concat(const NdTensor** tensors, int n, int axis) {
    if (!tensors || n < 1) return NULL;
    const NdTensor* first = tensors[0];
    int ndim = first->ndim;
    int ax = axis < 0 ? ndim - 1 : axis;
    if (ax < 0 || ax >= ndim) return NULL;
    
    /* 检查形状兼容性 */
    int total_ax_size = 0;
    for (int i = 0; i < n; i++) {
        if (tensors[i]->ndim != ndim) return NULL;
        for (int j = 0; j < ndim; j++) {
            if (j != ax && tensors[i]->shape[j] != first->shape[j]) return NULL;
        }
        total_ax_size += tensors[i]->shape[ax];
    }
    
    int* new_shape = shape_dup(first->shape, ndim);
    new_shape[ax] = total_ax_size;
    
    NdTensor* r = nd_zeros(new_shape, ndim);
    if (!r) { free(new_shape); return NULL; }
    
    int offset = 0;
    for (int ti = 0; ti < n; ti++) {
        const NdTensor* src = tensors[ti];
        int ax_sz = src->shape[ax];
        
        /* 逐元素复制 */
        int* idx = (int*)calloc(sizeof(int), (size_t)(ndim ? ndim : 1));
        for (int flat = 0; flat < src->size; flat++) {
            flat_to_idx(flat, src->shape, ndim, idx);
            int* dst_idx = (int*)calloc(sizeof(int), (size_t)ndim);
            for (int i = 0; i < ndim; i++) {
                dst_idx[i] = (i == ax) ? idx[i] + offset : idx[i];
            }
            r->data[idx_to_flat(dst_idx, new_shape, ndim)] = src->data[flat];
            free(dst_idx);
        }
        free(idx);
        offset += ax_sz;
    }
    
    free(new_shape);
    return r;
}

/* ========== 卷积 ========== */

NdTensor* nd_im2col(const NdTensor* input,
                     int kh, int kw, int ph, int pw, int sh, int sw) {
    if (!input || input->ndim != 4) return NULL;
    int N = input->shape[0], C = input->shape[1];
    int H = input->shape[2], W = input->shape[3];
    
    int OH = (H + 2 * ph - kh) / sh + 1;
    int OW = (W + 2 * pw - kw) / sw + 1;
    if (OH <= 0 || OW <= 0) return NULL;
    
    int shape[2] = {N * OH * OW, C * kh * kw};
    NdTensor* col = nd_zeros(shape, 2);
    if (!col) return NULL;
    
    int out_hw = OH * OW;
    int filter_size = C * kh * kw;
    
    for (int n = 0; n < N; n++) {
        for (int oh = 0; oh < OH; oh++) {
            for (int ow = 0; ow < OW; ow++) {
                int row_idx = (n * out_hw + oh * OW + ow) * filter_size;
                for (int c = 0; c < C; c++) {
                    for (int khr = 0; khr < kh; khr++) {
                        for (int khc = 0; khc < kw; khc++) {
                            int h = oh * sh + khr - ph;
                            int w = ow * sw + khc - pw;
                            int col_idx = row_idx + (c * kh + khr) * kw + khc;
                            if (h >= 0 && h < H && w >= 0 && w < W) {
                                int in_idx = ((n * C + c) * H + h) * W + w;
                                col->data[col_idx] = input->data[in_idx];
                            }
                        }
                    }
                }
            }
        }
    }
    return col;
}

NdTensor* nd_conv2d(const NdTensor* input, const NdTensor* kernel,
                     int pad_h, int pad_w, int stride_h, int stride_w) {
    if (!input || !kernel || input->ndim != 4 || kernel->ndim != 4) return NULL;
    int N = input->shape[0], C = input->shape[1];
    int H = input->shape[2], W = input->shape[3];
    int O = kernel->shape[0];  /* 输出通道 */
    int KC = kernel->shape[1]; /* 输入通道 (应等于 C) */
    int KH = kernel->shape[2], KW = kernel->shape[3];
    if (KC != C) return NULL;
    
    int OH = (H + 2 * pad_h - KH) / stride_h + 1;
    int OW = (W + 2 * pad_w - KW) / stride_w + 1;
    if (OH <= 0 || OW <= 0) return NULL;
    
    /* im2col */
    NdTensor* col = nd_im2col(input, KH, KW, pad_h, pad_w, stride_h, stride_w);
    if (!col) return NULL;
    
    /* kernel 展平为 (O, C*KH*KW) */
    int kflat_shape[2] = {O, C * KH * KW};
    NdTensor* kflat = nd_reshape(kernel, kflat_shape, 2);
    if (!kflat) { nd_free(col); return NULL; }
    
    /* matmul: col(N*OH*OW, C*KH*KW) × kflat(O, C*KH*KW)^T = output(N*OH*OW, O) */
    NdTensor* kflat_t = nd_transpose(kflat, NULL);
    NdTensor* out_flat = nd_matmul(col, kflat_t);
    
    nd_free(kflat); nd_free(kflat_t); nd_free(col);
    if (!out_flat) return NULL;
    
    /* reshape back to (N, O, OH, OW) */
    int out_shape[4] = {N, O, OH, OW};
    NdTensor* output = nd_reshape(out_flat, out_shape, 4);
    nd_free(out_flat);
    return output;
}

NdTensor* nd_maxpool2d(const NdTensor* input,
                        int kh, int kw, int ph, int pw, int sh, int sw) {
    if (!input || (input->ndim != 4 && input->ndim != 2)) return NULL;
    int N, C, H, W;
    if (input->ndim == 2) {
        N = 1; C = 1;
        H = input->shape[0]; W = input->shape[1];
    } else {
        N = input->shape[0]; C = input->shape[1];
        H = input->shape[2]; W = input->shape[3];
    }
    
    int OH = (H + 2 * ph - kh) / sh + 1;
    int OW = (W + 2 * pw - kw) / sw + 1;
    if (OH <= 0 || OW <= 0) return NULL;
    
    int ndim2 = (input->ndim == 2) ? 2 : 4;
    int out_shape4[4] = {N, C, OH, OW};
    NdTensor* out = nd_zeros(out_shape4, 4);
    if (!out) return NULL;
    
    for (int n = 0; n < N; n++)
        for (int c = 0; c < C; c++)
            for (int oh = 0; oh < OH; oh++)
                for (int ow = 0; ow < OW; ow++) {
                    double maxv = -1e30;
                    for (int khr = 0; khr < kh; khr++)
                        for (int khc = 0; khc < kw; khc++) {
                            int h = oh * sh + khr - ph;
                            int w = ow * sw + khc - pw;
                            if (h >= 0 && h < H && w >= 0 && w < W) {
                                double v = input->data[((n * C + c) * H + h) * W + w];
                                if (v > maxv) maxv = v;
                            }
                        }
                    out->data[((n * C + c) * OH + oh) * OW + ow] = maxv;
                }
    if (ndim2 == 2) {
        /* 2D input → flatten 4D to 2D: reshape (OH, OW) */
        int s2[2] = {OH, OW};
        NdTensor* out2 = nd_reshape(out, s2, 2);
        nd_free(out);
        return out2;
    }
    return out;
}

NdTensor* nd_upsample_nearest(const NdTensor* t, int scale_h, int scale_w) {
    if (!t || scale_h < 1 || scale_w < 1) return NULL;
    int ndim = t->ndim;
    if (ndim != 4 && ndim != 2) return NULL;
    
    int N, C, H, W;
    if (ndim == 2) {
        N = 1; C = 1;
        H = t->shape[0]; W = t->shape[1];
    } else {
        N = t->shape[0]; C = t->shape[1];
        H = t->shape[2]; W = t->shape[3];
    }
    
    int OH = H * scale_h;
    int OW = W * scale_w;
    int out_shape4[4] = {N, C, OH, OW};
    NdTensor* out = nd_zeros(out_shape4, 4);
    if (!out) return NULL;
    
    for (int n = 0; n < N; n++)
        for (int c = 0; c < C; c++)
            for (int oh = 0; oh < OH; oh++)
                for (int ow = 0; ow < OW; ow++) {
                    int ih = oh / scale_h;
                    int iw = ow / scale_w;
                    double v = t->data[((n * C + c) * H + ih) * W + iw];
                    out->data[((n * C + c) * OH + oh) * OW + ow] = v;
                }
    
    if (ndim == 2) {
        int s2[2] = {OH, OW};
        NdTensor* out2 = nd_reshape(out, s2, 2);
        nd_free(out);
        return out2;
    }
    return out;
}

/* ========== 颜色空间 ========== */

NdTensor* nd_rgb2hsv(const NdTensor* rgb) {
    if (!rgb || rgb->ndim < 1 || rgb->shape[rgb->ndim - 1] != 3) return NULL;
    NdTensor* hsv = nd_clone(rgb);
    if (!hsv) return NULL;
    
    int n_pixels = rgb->size / 3;
    for (int i = 0; i < n_pixels; i++) {
        double r = rgb->data[i * 3 + 0];
        double g = rgb->data[i * 3 + 1];
        double b = rgb->data[i * 3 + 2];
        
        double mx = imax(imax(r, g), b);
        double mn = imin(imin(r, g), b);
        double diff = mx - mn;
        
        double h = 0.0, s = 0.0, v = mx;
        
        if (diff > 1e-10) {
            s = diff / mx;
            if (mx == r) h = 60.0 * fmod((g - b) / diff, 6.0);
            else if (mx == g) h = 60.0 * ((b - r) / diff + 2.0);
            else h = 60.0 * ((r - g) / diff + 4.0);
            if (h < 0) h += 360.0;
        }
        
        hsv->data[i * 3 + 0] = h / 360.0;  /* 归一化到 [0,1] */
        hsv->data[i * 3 + 1] = s;
        hsv->data[i * 3 + 2] = v;
    }
    return hsv;
}

NdTensor* nd_hsv2rgb(const NdTensor* hsv) {
    if (!hsv || hsv->ndim < 1 || hsv->shape[hsv->ndim - 1] != 3) return NULL;
    NdTensor* rgb = nd_clone(hsv);
    if (!rgb) return NULL;
    
    int n_pixels = hsv->size / 3;
    for (int i = 0; i < n_pixels; i++) {
        double h = hsv->data[i * 3 + 0] * 360.0;
        double s = hsv->data[i * 3 + 1];
        double v = hsv->data[i * 3 + 2];
        
        if (s < 1e-10) {
            /* 灰度 */
            rgb->data[i * 3 + 0] = v;
            rgb->data[i * 3 + 1] = v;
            rgb->data[i * 3 + 2] = v;
            continue;
        }
        
        h = fmod(h, 360.0);
        if (h < 0) h += 360.0;
        int hi = (int)(h / 60.0) % 6;
        double f = h / 60.0 - hi;
        double p = v * (1.0 - s);
        double q = v * (1.0 - f * s);
        double t = v * (1.0 - (1.0 - f) * s);
        
        switch (hi) {
            case 0: rgb->data[i*3+0]=v; rgb->data[i*3+1]=t; rgb->data[i*3+2]=p; break;
            case 1: rgb->data[i*3+0]=q; rgb->data[i*3+1]=v; rgb->data[i*3+2]=p; break;
            case 2: rgb->data[i*3+0]=p; rgb->data[i*3+1]=v; rgb->data[i*3+2]=t; break;
            case 3: rgb->data[i*3+0]=p; rgb->data[i*3+1]=q; rgb->data[i*3+2]=v; break;
            case 4: rgb->data[i*3+0]=t; rgb->data[i*3+1]=p; rgb->data[i*3+2]=v; break;
            case 5: rgb->data[i*3+0]=v; rgb->data[i*3+1]=p; rgb->data[i*3+2]=q; break;
        }
    }
    return rgb;
}

NdTensor* nd_grayscale(const NdTensor* rgb) {
    if (!rgb || rgb->ndim < 1 || rgb->shape[rgb->ndim - 1] != 3) return NULL;
    int out_ndim = rgb->ndim;
    int* out_shape = shape_dup(rgb->shape, rgb->ndim);
    out_shape[out_ndim - 1] = 1;
    
    NdTensor* gray = nd_zeros(out_shape, out_ndim);
    free(out_shape);
    if (!gray) return NULL;
    
    int n_pixels = rgb->size / 3;
    for (int i = 0; i < n_pixels; i++) {
        double r = rgb->data[i * 3 + 0];
        double g = rgb->data[i * 3 + 1];
        double b = rgb->data[i * 3 + 2];
        gray->data[i] = 0.299 * r + 0.587 * g + 0.114 * b;
    }
    return gray;
}

/* ========== 填充 ========== */

NdTensor* nd_pad(const NdTensor* t, int axis, int pad_before, int pad_after) {
    if (!t || axis < 0 || axis >= t->ndim) return nd_clone(t);
    if (pad_before == 0 && pad_after == 0) return nd_clone(t);

    int ndim = t->ndim;
    int* new_shape = (int*)malloc(sizeof(int) * (size_t)ndim);
    for (int i = 0; i < ndim; i++) new_shape[i] = t->shape[i];
    new_shape[axis] += pad_before + pad_after;

    NdTensor* r = nd_zeros(new_shape, ndim);
    free(new_shape);
    if (!r) return NULL;

    int* idx = (int*)malloc(sizeof(int) * (size_t)ndim);
    int* dst_idx = (int*)malloc(sizeof(int) * (size_t)ndim);
    for (int flat = 0; flat < t->size; flat++) {
        flat_to_idx(flat, t->shape, ndim, idx);
        for (int i = 0; i < ndim; i++) {
            dst_idx[i] = (i == axis) ? idx[i] + pad_before : idx[i];
        }
        r->data[idx_to_flat(dst_idx, r->shape, ndim)] = t->data[flat];
    }
    free(idx);
    free(dst_idx);

    return r;
}

/* ========== 3D 梯度 ========== */

NdTensor* nd_gradient3d(const NdTensor* field) {
    if (!field) return NULL;
    /* 接受 3D [D,H,W] 或 4D [1,D,H,W]; 输出 [3,D,H,W] */
    int ndim = field->ndim;
    int D, H, W;
    if (ndim == 3) {
        D = field->shape[0]; H = field->shape[1]; W = field->shape[2];
    } else if (ndim == 4 && field->shape[0] == 1) {
        D = field->shape[1]; H = field->shape[2]; W = field->shape[3];
    } else return NULL;
    
    int shape[4] = {3, D, H, W};
    NdTensor* grad = nd_zeros(shape, 4);
    if (!grad) return NULL;
    
    for (int d = 0; d < D; d++) {
        for (int h = 0; h < H; h++) {
            for (int w = 0; w < W; w++) {
                int idx0[3], idx1[3], out_idx[4];
                
                /* Gx (沿 W 方向) */
                int w0 = w > 0 ? w-1 : w+1;
                int w1 = w < W-1 ? w+1 : w-1;
                idx0[0]=d; idx0[1]=h; idx0[2]=w0;
                idx1[0]=d; idx1[1]=h; idx1[2]=w1;
                double gx = (nd_get_flat(field, nd_offset(field, idx1)) -
                             nd_get_flat(field, nd_offset(field, idx0))) / 2.0;
                
                /* Gy (沿 H 方向) */
                int h0 = h > 0 ? h-1 : h+1;
                int h1 = h < H-1 ? h+1 : h-1;
                idx0[0]=d; idx0[1]=h0; idx0[2]=w;
                idx1[0]=d; idx1[1]=h1; idx1[2]=w;
                double gy = (nd_get_flat(field, nd_offset(field, idx1)) -
                             nd_get_flat(field, nd_offset(field, idx0))) / 2.0;
                
                /* Gz (沿 D 方向) */
                int d0 = d > 0 ? d-1 : d+1;
                int d1 = d < D-1 ? d+1 : d-1;
                idx0[0]=d0; idx0[1]=h; idx0[2]=w;
                idx1[0]=d1; idx1[1]=h; idx1[2]=w;
                double gz = (nd_get_flat(field, nd_offset(field, idx1)) -
                             nd_get_flat(field, nd_offset(field, idx0))) / 2.0;
                
                out_idx[0]=0; out_idx[1]=d; out_idx[2]=h; out_idx[3]=w;
                nd_set_flat(grad, nd_offset(grad, out_idx), gx);
                out_idx[0]=1;
                nd_set_flat(grad, nd_offset(grad, out_idx), gy);
                out_idx[0]=2;
                nd_set_flat(grad, nd_offset(grad, out_idx), gz);
            }
        }
    }
    return grad;
}

/* ========== Marching Cubes ========== */

/* 标准 MC 查找表 */
static const int mc_edgeTable[256] = {
    0x0  , 0x109, 0x203, 0x30a, 0x406, 0x50f, 0x605, 0x70c,
    0x80c, 0x905, 0xa0f, 0xb06, 0xc0a, 0xd03, 0xe09, 0xf00,
    0x190, 0x99 , 0x393, 0x29a, 0x596, 0x49f, 0x795, 0x69c,
    0x99c, 0x895, 0xb9f, 0xa96, 0xd9a, 0xc93, 0xf99, 0xe90,
    0x230, 0x339, 0x33 , 0x13a, 0x636, 0x73f, 0x435, 0x53c,
    0xa3c, 0xb35, 0x83f, 0x936, 0xe3a, 0xf33, 0xc39, 0xd30,
    0x3a0, 0x2a9, 0x1a3, 0xaa , 0x7a6, 0x6af, 0x5a5, 0x4ac,
    0xbac, 0xaa5, 0x9af, 0x8a6, 0xfaa, 0xea3, 0xda9, 0xca0,
    0x460, 0x569, 0x663, 0x76a, 0x66 , 0x16f, 0x265, 0x36c,
    0xc6c, 0xd65, 0xe6f, 0xf66, 0x86a, 0x963, 0xa69, 0xb60,
    0x5f0, 0x4f9, 0x7f3, 0x6fa, 0x1f6, 0xff , 0x3f5, 0x2fc,
    0xdfc, 0xcf5, 0xfff, 0xef6, 0x9fa, 0x8f3, 0xbf9, 0xaf0,
    0x650, 0x759, 0x453, 0x55a, 0x256, 0x35f, 0x55 , 0x15c,
    0xe5c, 0xf55, 0xc5f, 0xd56, 0xa5a, 0xb53, 0x859, 0x950,
    0x7c0, 0x6c9, 0x5c3, 0x4ca, 0x3c6, 0x2cf, 0x1c5, 0xcc ,
    0xfcc, 0xec5, 0xdcf, 0xcc6, 0xbca, 0xac3, 0x9c9, 0x8c0,
    0x8c0, 0x9c9, 0xac3, 0xbca, 0xcc6, 0xdcf, 0xec5, 0xfcc,
    0xcc , 0x1c5, 0x2cf, 0x3c6, 0x4ca, 0x5c3, 0x6c9, 0x7c0,
    0x950, 0x859, 0xb53, 0xa5a, 0xd56, 0xc5f, 0xf55, 0xe5c,
    0x15c, 0x55 , 0x35f, 0x256, 0x55a, 0x453, 0x759, 0x650,
    0xaf0, 0xbf9, 0x8f3, 0x9fa, 0xef6, 0xfff, 0xcf5, 0xdfc,
    0x2fc, 0x3f5, 0xff , 0x1f6, 0x6fa, 0x7f3, 0x4f9, 0x5f0,
    0xb60, 0xa69, 0x963, 0x86a, 0xf66, 0xe6f, 0xd65, 0xc6c,
    0x36c, 0x265, 0x16f, 0x66 , 0x76a, 0x663, 0x569, 0x460,
    0xca0, 0xda9, 0xea3, 0xfaa, 0x8a6, 0x9af, 0xaa5, 0xbac,
    0x4ac, 0x5a5, 0x6af, 0x7a6, 0xaa , 0x1a3, 0x2a9, 0x3a0,
    0xd30, 0xc39, 0xf33, 0xe3a, 0x936, 0x83f, 0xb35, 0xa3c,
    0x53c, 0x435, 0x73f, 0x636, 0x13a, 0x33 , 0x339, 0x230,
    0xe90, 0xf99, 0xc93, 0xd9a, 0xa96, 0xb9f, 0x895, 0x99c,
    0x69c, 0x795, 0x49f, 0x596, 0x29a, 0x393, 0x99 , 0x190,
    0xf00, 0xe09, 0xd03, 0xc0a, 0xb06, 0xa0f, 0x905, 0x80c,
    0x70c, 0x605, 0x50f, 0x406, 0x30a, 0x203, 0x109, 0x0
};

/* 三角形查找表: 每个配置对应一组边索引 (每3个一组, -1 结尾) */
static const int mc_triTable[256][16] = {
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 8, 3, 9, 8, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 1, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 2, 10, 0, 2, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 8, 3, 2, 10, 8, 10, 9, 8, -1, -1, -1, -1, -1, -1, -1},
    {3, 11, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 11, 2, 8, 11, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 11, 2, 1, 9, 11, 9, 8, 11, -1, -1, -1, -1, -1, -1, -1},
    {3, 10, 1, 11, 10, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 10, 1, 0, 8, 10, 8, 11, 10, -1, -1, -1, -1, -1, -1, -1},
    {3, 9, 0, 3, 11, 9, 11, 10, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 8, 10, 10, 8, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 3, 0, 7, 3, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 1, 9, 4, 7, 1, 7, 3, 1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 4, 7, 3, 0, 4, 1, 2, 10, -1, -1, -1, -1, -1, -1, -1},
    {9, 2, 10, 9, 0, 2, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1},
    {2, 10, 9, 2, 9, 7, 2, 7, 3, 7, 9, 4, -1, -1, -1, -1},
    {8, 4, 7, 3, 11, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 4, 7, 11, 2, 4, 2, 0, 4, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 1, 8, 4, 7, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1},
    {4, 7, 11, 9, 4, 11, 9, 11, 2, 9, 2, 1, -1, -1, -1, -1},
    {3, 10, 1, 3, 11, 10, 7, 8, 4, -1, -1, -1, -1, -1, -1, -1},
    {1, 11, 10, 1, 4, 11, 1, 0, 4, 7, 11, 4, -1, -1, -1, -1},
    {4, 7, 8, 9, 0, 11, 9, 11, 10, 11, 0, 3, -1, -1, -1, -1},
    {4, 7, 11, 4, 11, 9, 9, 11, 10, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, 0, 8, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 5, 4, 1, 5, 0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 5, 4, 8, 3, 5, 3, 1, 5, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 9, 5, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 1, 2, 10, 4, 9, 5, -1, -1, -1, -1, -1, -1, -1},
    {5, 2, 10, 5, 4, 2, 4, 0, 2, -1, -1, -1, -1, -1, -1, -1},
    {2, 10, 5, 3, 2, 5, 3, 5, 4, 3, 4, 8, -1, -1, -1, -1},
    {9, 5, 4, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 11, 2, 0, 8, 11, 4, 9, 5, -1, -1, -1, -1, -1, -1, -1},
    {0, 5, 4, 0, 1, 5, 2, 3, 11, -1, -1, -1, -1, -1, -1, -1},
    {2, 1, 5, 2, 5, 8, 2, 8, 11, 4, 8, 5, -1, -1, -1, -1},
    {10, 3, 11, 10, 1, 3, 9, 5, 4, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 5, 0, 8, 1, 8, 10, 1, 8, 11, 10, -1, -1, -1, -1},
    {5, 4, 0, 5, 0, 11, 5, 11, 10, 11, 0, 3, -1, -1, -1, -1},
    {5, 4, 8, 5, 8, 10, 10, 8, 11, -1, -1, -1, -1, -1, -1, -1},
    {9, 7, 8, 5, 7, 9, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 3, 0, 9, 5, 3, 5, 7, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 7, 8, 0, 1, 7, 1, 5, 7, -1, -1, -1, -1, -1, -1, -1},
    {1, 5, 3, 3, 5, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 7, 8, 9, 5, 7, 10, 1, 2, -1, -1, -1, -1, -1, -1, -1},
    {10, 1, 2, 9, 5, 0, 5, 3, 0, 5, 7, 3, -1, -1, -1, -1},
    {8, 0, 2, 8, 2, 5, 8, 5, 7, 10, 5, 2, -1, -1, -1, -1},
    {2, 10, 5, 2, 5, 3, 3, 5, 7, -1, -1, -1, -1, -1, -1, -1},
    {7, 9, 5, 7, 8, 9, 3, 11, 2, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 7, 9, 7, 2, 9, 2, 0, 2, 7, 11, -1, -1, -1, -1},
    {2, 3, 11, 0, 1, 8, 1, 7, 8, 1, 5, 7, -1, -1, -1, -1},
    {11, 2, 1, 11, 1, 7, 7, 1, 5, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 8, 8, 5, 7, 10, 1, 3, 10, 3, 11, -1, -1, -1, -1},
    {5, 7, 0, 5, 0, 9, 7, 11, 0, 1, 0, 10, 11, 10, 0, -1},
    {11, 10, 0, 11, 0, 3, 10, 5, 0, 8, 0, 7, 5, 7, 0, -1},
    {11, 10, 5, 7, 11, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {10, 6, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 1, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 8, 3, 1, 9, 8, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 5, 2, 6, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 5, 1, 2, 6, 3, 0, 8, -1, -1, -1, -1, -1, -1, -1},
    {9, 6, 5, 9, 0, 6, 0, 2, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 9, 8, 5, 8, 2, 5, 2, 6, 3, 2, 8, -1, -1, -1, -1},
    {2, 3, 11, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 0, 8, 11, 2, 0, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 2, 3, 11, 5, 10, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 1, 9, 2, 9, 11, 2, 9, 8, 11, -1, -1, -1, -1},
    {6, 3, 11, 6, 5, 3, 5, 1, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 11, 0, 11, 5, 0, 5, 1, 5, 11, 6, -1, -1, -1, -1},
    {3, 11, 6, 0, 3, 6, 0, 6, 5, 0, 5, 9, -1, -1, -1, -1},
    {6, 5, 9, 6, 9, 11, 11, 9, 8, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 4, 7, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 3, 0, 4, 7, 3, 6, 5, 10, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 5, 10, 6, 8, 4, 7, -1, -1, -1, -1, -1, -1, -1},
    {10, 6, 5, 1, 9, 7, 1, 7, 3, 7, 9, 4, -1, -1, -1, -1},
    {6, 1, 2, 6, 5, 1, 4, 7, 8, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 5, 5, 2, 6, 3, 0, 4, 3, 4, 7, -1, -1, -1, -1},
    {8, 4, 7, 9, 0, 5, 0, 6, 5, 0, 2, 6, -1, -1, -1, -1},
    {7, 3, 9, 7, 9, 4, 3, 2, 9, 5, 9, 6, 2, 6, 9, -1},
    {3, 11, 2, 7, 8, 4, 10, 6, 5, -1, -1, -1, -1, -1, -1, -1},
    {5, 10, 6, 4, 7, 2, 4, 2, 0, 2, 7, 11, -1, -1, -1, -1},
    {0, 1, 9, 4, 7, 8, 2, 3, 11, 5, 10, 6, -1, -1, -1, -1},
    {9, 2, 1, 9, 11, 2, 9, 4, 11, 7, 11, 4, 5, 10, 6, -1},
    {8, 4, 7, 3, 11, 5, 3, 5, 1, 5, 11, 6, -1, -1, -1, -1},
    {5, 1, 11, 5, 11, 6, 1, 0, 11, 7, 11, 4, 0, 4, 11, -1},
    {0, 5, 9, 0, 6, 5, 0, 3, 6, 11, 6, 3, 8, 4, 7, -1},
    {6, 5, 9, 6, 9, 11, 4, 7, 9, 7, 11, 9, -1, -1, -1, -1},
    {10, 4, 9, 6, 4, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 10, 6, 4, 9, 10, 0, 8, 3, -1, -1, -1, -1, -1, -1, -1},
    {10, 0, 1, 10, 6, 0, 6, 4, 0, -1, -1, -1, -1, -1, -1, -1},
    {8, 3, 1, 8, 1, 6, 8, 6, 4, 6, 1, 10, -1, -1, -1, -1},
    {1, 4, 9, 1, 2, 4, 2, 6, 4, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 1, 2, 9, 2, 4, 9, 2, 6, 4, -1, -1, -1, -1},
    {0, 2, 4, 4, 2, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 3, 2, 8, 2, 4, 4, 2, 6, -1, -1, -1, -1, -1, -1, -1},
    {10, 4, 9, 10, 6, 4, 11, 2, 3, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 2, 2, 8, 11, 4, 9, 10, 4, 10, 6, -1, -1, -1, -1},
    {3, 11, 2, 0, 1, 6, 0, 6, 4, 6, 1, 10, -1, -1, -1, -1},
    {6, 4, 1, 6, 1, 10, 4, 8, 1, 2, 1, 11, 8, 11, 1, -1},
    {9, 6, 4, 9, 3, 6, 9, 1, 3, 11, 6, 3, -1, -1, -1, -1},
    {8, 11, 1, 8, 1, 0, 11, 6, 1, 9, 1, 4, 6, 4, 1, -1},
    {3, 11, 6, 3, 6, 0, 0, 6, 4, -1, -1, -1, -1, -1, -1, -1},
    {6, 4, 8, 11, 6, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 10, 6, 7, 8, 10, 8, 9, 10, -1, -1, -1, -1, -1, -1, -1},
    {0, 7, 3, 0, 10, 7, 0, 9, 10, 6, 7, 10, -1, -1, -1, -1},
    {10, 6, 7, 1, 10, 7, 1, 7, 8, 1, 8, 0, -1, -1, -1, -1},
    {10, 6, 7, 10, 7, 1, 1, 7, 3, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 6, 1, 6, 8, 1, 8, 9, 8, 6, 7, -1, -1, -1, -1},
    {2, 6, 9, 2, 9, 1, 6, 7, 9, 0, 9, 3, 7, 3, 9, -1},
    {7, 8, 0, 7, 0, 6, 6, 0, 2, -1, -1, -1, -1, -1, -1, -1},
    {7, 3, 2, 6, 7, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 11, 10, 6, 8, 10, 8, 9, 8, 6, 7, -1, -1, -1, -1},
    {2, 0, 7, 2, 7, 11, 0, 9, 7, 6, 7, 10, 9, 10, 7, -1},
    {1, 8, 0, 1, 7, 8, 1, 10, 7, 6, 7, 10, 2, 3, 11, -1},
    {11, 2, 1, 11, 1, 7, 10, 6, 1, 6, 7, 1, -1, -1, -1, -1},
    {8, 9, 6, 8, 6, 7, 9, 1, 6, 11, 6, 3, 1, 3, 6, -1},
    {0, 9, 1, 11, 6, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 8, 0, 7, 0, 6, 3, 11, 0, 11, 6, 0, -1, -1, -1, -1},
    {7, 11, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 8, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 9, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 1, 9, 8, 3, 1, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1},
    {10, 1, 2, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 3, 0, 8, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1},
    {2, 9, 0, 2, 10, 9, 6, 11, 7, -1, -1, -1, -1, -1, -1, -1},
    {6, 11, 7, 2, 10, 3, 10, 8, 3, 10, 9, 8, -1, -1, -1, -1},
    {7, 2, 3, 6, 2, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {7, 0, 8, 7, 6, 0, 6, 2, 0, -1, -1, -1, -1, -1, -1, -1},
    {2, 7, 6, 2, 3, 7, 0, 1, 9, -1, -1, -1, -1, -1, -1, -1},
    {1, 6, 2, 1, 8, 6, 1, 9, 8, 8, 7, 6, -1, -1, -1, -1},
    {10, 7, 6, 10, 1, 7, 1, 3, 7, -1, -1, -1, -1, -1, -1, -1},
    {10, 7, 6, 1, 7, 10, 1, 8, 7, 1, 0, 8, -1, -1, -1, -1},
    {0, 3, 7, 0, 7, 10, 0, 10, 9, 6, 10, 7, -1, -1, -1, -1},
    {7, 6, 10, 7, 10, 8, 8, 10, 9, -1, -1, -1, -1, -1, -1, -1},
    {6, 8, 4, 11, 8, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 6, 11, 3, 0, 6, 0, 4, 6, -1, -1, -1, -1, -1, -1, -1},
    {8, 6, 11, 8, 4, 6, 9, 0, 1, -1, -1, -1, -1, -1, -1, -1},
    {9, 4, 6, 9, 6, 3, 9, 3, 1, 11, 3, 6, -1, -1, -1, -1},
    {6, 8, 4, 6, 11, 8, 2, 10, 1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 10, 3, 0, 11, 0, 6, 11, 0, 4, 6, -1, -1, -1, -1},
    {4, 11, 8, 4, 6, 11, 0, 2, 9, 2, 10, 9, -1, -1, -1, -1},
    {10, 9, 3, 10, 3, 2, 9, 4, 3, 11, 3, 6, 4, 6, 3, -1},
    {8, 2, 3, 8, 4, 2, 4, 6, 2, -1, -1, -1, -1, -1, -1, -1},
    {0, 4, 2, 4, 6, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 9, 0, 2, 3, 4, 2, 4, 6, 4, 3, 8, -1, -1, -1, -1},
    {1, 9, 4, 1, 4, 2, 2, 4, 6, -1, -1, -1, -1, -1, -1, -1},
    {8, 4, 6, 8, 6, 1, 8, 1, 3, 10, 1, 6, -1, -1, -1, -1},
    {1, 0, 4, 1, 4, 10, 10, 4, 6, -1, -1, -1, -1, -1, -1, -1},
    {4, 6, 9, 4, 9, 8, 6, 10, 9, 0, 3, 9, 3, 8, 9, -1},
    {9, 4, 6, 9, 6, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 5, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 4, 9, 5, 11, 7, 6, -1, -1, -1, -1, -1, -1, -1},
    {5, 0, 1, 5, 4, 0, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1},
    {11, 7, 6, 8, 3, 4, 3, 5, 4, 3, 1, 5, -1, -1, -1, -1},
    {9, 5, 4, 10, 1, 2, 7, 6, 11, -1, -1, -1, -1, -1, -1, -1},
    {6, 11, 7, 1, 2, 10, 0, 8, 3, 4, 9, 5, -1, -1, -1, -1},
    {7, 6, 11, 5, 4, 10, 4, 2, 10, 4, 0, 2, -1, -1, -1, -1},
    {3, 4, 8, 3, 5, 4, 3, 2, 5, 10, 5, 2, 11, 7, 6, -1},
    {7, 2, 3, 7, 6, 2, 5, 4, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 5, 4, 0, 8, 6, 0, 6, 2, 6, 8, 7, -1, -1, -1, -1},
    {3, 6, 2, 3, 7, 6, 1, 5, 0, 5, 4, 0, -1, -1, -1, -1},
    {6, 2, 8, 6, 8, 7, 2, 1, 8, 4, 8, 5, 1, 5, 8, -1},
    {9, 5, 4, 10, 1, 6, 1, 7, 6, 1, 3, 7, -1, -1, -1, -1},
    {1, 6, 10, 1, 7, 6, 1, 0, 7, 8, 7, 0, 9, 5, 4, -1},
    {4, 0, 10, 4, 10, 5, 0, 3, 10, 6, 10, 7, 3, 7, 10, -1},
    {4, 8, 7, 4, 7, 5, 5, 7, 10, -1, -1, -1, -1, -1, -1, -1},
    {9, 8, 5, 5, 8, 6, 6, 8, 11, -1, -1, -1, -1, -1, -1, -1},
    {5, 0, 9, 5, 6, 0, 6, 11, 0, 11, 3, 0, -1, -1, -1, -1},
    {0, 1, 8, 8, 1, 5, 8, 5, 6, 8, 6, 11, -1, -1, -1, -1},
    {1, 5, 6, 1, 6, 3, 3, 6, 11, -1, -1, -1, -1, -1, -1, -1},
    {10, 1, 2, 6, 11, 8, 6, 8, 5, 8, 9, 5, -1, -1, -1, -1},
    {0, 9, 1, 11, 3, 10, 11, 10, 6, 10, 3, 2, -1, -1, -1, -1},
    {0, 2, 8, 8, 2, 5, 8, 5, 6, 8, 6, 11, 10, 5, 2, -1},
    {11, 3, 2, 11, 2, 6, 10, 5, 2, 1, 5, 10, -1, -1, -1, -1},
    {9, 8, 5, 8, 6, 5, 2, 3, 4, 2, 4, 6, 4, 3, 8, -1},
    {0, 9, 5, 0, 5, 2, 2, 5, 6, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 8, 8, 1, 5, 2, 3, 6, 3, 8, 6, 5, 6, 8, -1},
    {1, 5, 6, 2, 1, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 3, 6, 1, 6, 10, 3, 8, 6, 5, 6, 9, 8, 9, 6, -1},
    {10, 1, 0, 10, 0, 6, 9, 5, 0, 5, 6, 0, -1, -1, -1, -1},
    {0, 3, 8, 0, 8, 5, 10, 5, 0, 5, 6, 10, -1, -1, -1, -1},
    {10, 5, 6, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 5, 10, 7, 5, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {11, 5, 10, 11, 7, 5, 8, 3, 0, -1, -1, -1, -1, -1, -1, -1},
    {5, 11, 7, 5, 10, 11, 1, 9, 0, -1, -1, -1, -1, -1, -1, -1},
    {10, 7, 5, 10, 11, 7, 9, 8, 1, 8, 3, 1, -1, -1, -1, -1},
    {11, 1, 2, 11, 7, 1, 7, 5, 1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 1, 2, 7, 1, 7, 5, 7, 2, 11, -1, -1, -1, -1},
    {9, 7, 5, 9, 2, 7, 9, 0, 2, 2, 11, 7, -1, -1, -1, -1},
    {7, 5, 2, 7, 2, 11, 5, 9, 2, 3, 2, 8, 9, 8, 2, -1},
    {2, 5, 10, 2, 3, 5, 3, 7, 5, -1, -1, -1, -1, -1, -1, -1},
    {8, 2, 0, 8, 5, 2, 8, 7, 5, 10, 2, 5, -1, -1, -1, -1},
    {9, 0, 1, 5, 10, 3, 5, 3, 7, 3, 10, 2, -1, -1, -1, -1},
    {9, 8, 2, 9, 2, 1, 8, 7, 2, 10, 2, 5, 7, 5, 2, -1},
    {1, 3, 5, 3, 7, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 7, 0, 7, 1, 1, 7, 5, -1, -1, -1, -1, -1, -1, -1},
    {9, 0, 3, 9, 3, 5, 5, 3, 7, -1, -1, -1, -1, -1, -1, -1},
    {9, 8, 7, 5, 9, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {5, 8, 4, 5, 10, 8, 10, 11, 8, -1, -1, -1, -1, -1, -1, -1},
    {5, 0, 4, 5, 11, 0, 5, 10, 11, 11, 3, 0, -1, -1, -1, -1},
    {0, 1, 9, 8, 4, 10, 8, 10, 11, 10, 4, 5, -1, -1, -1, -1},
    {10, 11, 4, 10, 4, 5, 11, 3, 4, 9, 4, 1, 3, 1, 4, -1},
    {2, 5, 1, 2, 8, 5, 2, 11, 8, 4, 5, 8, -1, -1, -1, -1},
    {0, 4, 11, 0, 11, 3, 4, 5, 11, 2, 11, 1, 5, 1, 11, -1},
    {0, 2, 5, 0, 5, 9, 2, 11, 5, 4, 5, 8, 11, 8, 5, -1},
    {9, 4, 5, 2, 11, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 5, 10, 3, 5, 2, 3, 4, 5, 3, 8, 4, -1, -1, -1, -1},
    {5, 10, 2, 5, 2, 4, 4, 2, 0, -1, -1, -1, -1, -1, -1, -1},
    {3, 10, 2, 3, 5, 10, 3, 8, 5, 4, 5, 8, 0, 1, 9, -1},
    {5, 10, 2, 5, 2, 4, 1, 9, 2, 9, 4, 2, -1, -1, -1, -1},
    {8, 4, 5, 8, 5, 3, 3, 5, 1, -1, -1, -1, -1, -1, -1, -1},
    {0, 4, 5, 1, 0, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {8, 4, 5, 8, 5, 3, 9, 0, 5, 0, 3, 5, -1, -1, -1, -1},
    {9, 4, 5, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 11, 7, 4, 9, 11, 9, 10, 11, -1, -1, -1, -1, -1, -1, -1},
    {0, 8, 3, 4, 9, 7, 9, 11, 7, 9, 10, 11, -1, -1, -1, -1},
    {1, 10, 11, 1, 11, 4, 1, 4, 0, 7, 4, 11, -1, -1, -1, -1},
    {3, 1, 4, 3, 4, 8, 1, 10, 4, 7, 4, 11, 10, 11, 4, -1},
    {4, 11, 7, 9, 11, 4, 9, 2, 11, 9, 1, 2, -1, -1, -1, -1},
    {9, 7, 4, 9, 11, 7, 9, 1, 11, 2, 11, 1, 0, 8, 3, -1},
    {11, 7, 4, 11, 4, 2, 2, 4, 0, -1, -1, -1, -1, -1, -1, -1},
    {11, 7, 4, 11, 4, 2, 8, 3, 4, 3, 2, 4, -1, -1, -1, -1},
    {2, 9, 10, 2, 7, 9, 2, 3, 7, 7, 4, 9, -1, -1, -1, -1},
    {9, 10, 7, 9, 7, 4, 10, 2, 7, 8, 7, 0, 2, 0, 7, -1},
    {3, 7, 10, 3, 10, 2, 7, 4, 10, 1, 10, 0, 4, 0, 10, -1},
    {1, 10, 2, 8, 7, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 1, 4, 1, 7, 7, 1, 3, -1, -1, -1, -1, -1, -1, -1},
    {4, 9, 1, 4, 1, 7, 0, 8, 1, 8, 7, 1, -1, -1, -1, -1},
    {4, 0, 3, 7, 4, 3, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {4, 8, 7, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {9, 10, 8, 10, 11, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 9, 3, 9, 11, 11, 9, 10, -1, -1, -1, -1, -1, -1, -1},
    {0, 1, 10, 0, 10, 8, 8, 10, 11, -1, -1, -1, -1, -1, -1, -1},
    {3, 1, 10, 11, 3, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 2, 11, 1, 11, 9, 9, 11, 8, -1, -1, -1, -1, -1, -1, -1},
    {3, 0, 9, 3, 9, 11, 1, 2, 9, 2, 11, 9, -1, -1, -1, -1},
    {0, 2, 11, 8, 0, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {3, 2, 11, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 8, 2, 8, 10, 10, 8, 9, -1, -1, -1, -1, -1, -1, -1},
    {9, 10, 2, 0, 9, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {2, 3, 8, 2, 8, 10, 0, 1, 8, 1, 10, 8, -1, -1, -1, -1},
    {1, 10, 2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {1, 3, 8, 9, 1, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 9, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {0, 3, 8, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1},
    {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}
};

NdTensor* nd_marching_cubes(const NdTensor* field, double iso) {
    if (!field) return NULL;
    int ndim = field->ndim;
    int D, H, W;
    if (ndim == 3) {
        D = field->shape[0]; H = field->shape[1]; W = field->shape[2];
    } else if (ndim == 4 && field->shape[0] == 1) {
        D = field->shape[1]; H = field->shape[2]; W = field->shape[3];
    } else return NULL;
    
    if (D < 2 || H < 2 || W < 2) return NULL;
    
    /* 立方体顶点相对偏移 (x,y,z) */
    static const int cube_offsets[8][3] = {
        {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0},
        {0,0,1}, {1,0,1}, {1,1,1}, {0,1,1}
    };
    /* 每条边连接的顶点对 */
    static const int edge_verts[12][2] = {
        {0,1}, {1,2}, {2,3}, {3,0},
        {4,5}, {5,6}, {6,7}, {7,4},
        {0,4}, {1,5}, {2,6}, {3,7}
    };
    
    /* 第一遍: 统计三角形数 */
    int total_tris = 0;
    for (int d = 0; d < D-1; d++) {
        for (int h = 0; h < H-1; h++) {
            for (int w = 0; w < W-1; w++) {
                double vals[8];
                int cube_index = 0;
                for (int i = 0; i < 8; i++) {
                    int idx[3] = {d + cube_offsets[i][2],
                                  h + cube_offsets[i][1],
                                  w + cube_offsets[i][0]};
                    vals[i] = nd_get_flat(field, nd_offset(field, idx));
                    if (vals[i] < iso) cube_index |= (1 << i);
                }
                int edges = mc_edgeTable[cube_index];
                if (edges == 0) continue;
                /* 统计该立方体的三角形数 */
                const int* tri = mc_triTable[cube_index];
                int nt = 0;
                for (int j = 0; tri[j] != -1; j++) nt++;
                total_tris += nt / 3;
            }
        }
    }
    
    if (total_tris == 0) {
        /* 返回空张量 */
        int shape[2] = {0, 3};
        return nd_zeros(shape, 2);
    }
    
    /* 第二遍: 填充三角形 */
    int out_shape[2] = {total_tris * 3, 3};
    NdTensor* mesh = nd_zeros(out_shape, 2);
    if (!mesh) return NULL;
    
    int out_idx = 0;
    for (int d = 0; d < D-1; d++) {
        for (int h = 0; h < H-1; h++) {
            for (int w = 0; w < W-1; w++) {
                double vals[8];
                int cube_index = 0;
                for (int i = 0; i < 8; i++) {
                    int idx[3] = {d + cube_offsets[i][2],
                                  h + cube_offsets[i][1],
                                  w + cube_offsets[i][0]};
                    vals[i] = nd_get_flat(field, nd_offset(field, idx));
                    if (vals[i] < iso) cube_index |= (1 << i);
                }
                int edges = mc_edgeTable[cube_index];
                if (edges == 0) continue;
                
                /* 计算每条边的插值顶点 (若边被切割) */
                double vert_list[12][3];
                for (int e = 0; e < 12; e++) {
                    if (edges & (1 << e)) {
                        int v0 = edge_verts[e][0], v1 = edge_verts[e][1];
                        double val0 = vals[v0], val1 = vals[v1];
                        double t = (iso - val0) / (val1 - val0);
                        if (t < 0) t = 0;
                        if (t > 1) t = 1;
                        /* 世界坐标 */
                        int p0[3] = {w + cube_offsets[v0][0],
                                     h + cube_offsets[v0][1],
                                     d + cube_offsets[v0][2]};
                        int p1[3] = {w + cube_offsets[v1][0],
                                     h + cube_offsets[v1][1],
                                     d + cube_offsets[v1][2]};
                        for (int c = 0; c < 3; c++) {
                            vert_list[e][c] = p0[c] + t * (p1[c] - p0[c]);
                        }
                    }
                }
                
                /* 输出三角形 */
                const int* tri = mc_triTable[cube_index];
                for (int j = 0; tri[j] != -1; j++) {
                    int edge_idx = tri[j];
                    if (edge_idx >= 0 && edge_idx < 12) {
                        for (int c = 0; c < 3; c++) {
                            nd_set_flat(mesh, out_idx * 3 + c, vert_list[edge_idx][c]);
                        }
                        out_idx++;
                    }
                }
            }
        }
    }
    
    return mesh;
}

/* ========== 场生成辅助 ========== */

NdTensor* nd_sphere_field(int D, int H, int W, double radius,
                           double cx, double cy, double cz) {
    if (D <= 0 || H <= 0 || W <= 0) return NULL;
    int shape[3] = {D, H, W};
    NdTensor* field = nd_zeros(shape, 3);
    if (!field) return NULL;
    for (int d = 0; d < D; d++) {
        for (int h = 0; h < H; h++) {
            for (int w = 0; w < W; w++) {
                double dx = (double)w - cx;
                double dy = (double)h - cy;
                double dz = (double)d - cz;
                double dist = sqrt(dx*dx + dy*dy + dz*dz) - radius;
                int idx[3] = {d, h, w};
                nd_set(field, idx, dist);
            }
        }
    }
    return field;
}

/* 椭球体 SDF: 半径 rx/ry/rz 不同, 模拟石头形状 */
NdTensor* nd_ellipsoid_field(int D, int H, int W,
                             double rx, double ry, double rz,
                             double cx, double cy, double cz) {
    if (D <= 0 || H <= 0 || W <= 0) return NULL;
    int shape[3] = {D, H, W};
    NdTensor* field = nd_zeros(shape, 3);
    if (!field) return NULL;
    for (int d = 0; d < D; d++) {
        for (int h = 0; h < H; h++) {
            for (int w = 0; w < W; w++) {
                double dx = ((double)w - cx) / rx;
                double dy = ((double)h - cy) / ry;
                double dz = ((double)d - cz) / rz;
                /* 椭球 SDF = (||(p-c)/r|| - 1) * min(r) 近似 */
                double dist = sqrt(dx*dx + dy*dy + dz*dz) - 1.0;
                double min_r = rx;
                if (ry < min_r) min_r = ry;
                if (rz < min_r) min_r = rz;
                /* 用最小半径缩放让距离更自然 */
                int idx[3] = {d, h, w};
                nd_set(field, idx, dist * min_r);
            }
        }
    }
    return field;
}

/* ========== 网格渲染 (三角形光栅化) ========== */

/* 辅助: clamp int */
static int iclamp(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

NdTensor* nd_rasterize_triangles(const NdTensor* verts_2d, const NdTensor* triangles,
                                  int width, int height) {
    if (!verts_2d || width <= 0 || height <= 0) return NULL;
    if (verts_2d->ndim != 2 || verts_2d->shape[1] != 2) return NULL;
    
    int nverts = verts_2d->shape[0];
    int ntris = 0;
    
    if (triangles) {
        /* 用索引表: [T, 3] */
        if (triangles->ndim != 2 || triangles->shape[1] != 3) return NULL;
        ntris = triangles->shape[0];
    } else {
        /* 每 3 个连续顶点一个三角形 */
        if (nverts < 3 || nverts % 3 != 0) return NULL;
        ntris = nverts / 3;
    }
    
    /* 创建输出图像 [H, W] */
    int img_shape[2] = {height, width};
    NdTensor* img = nd_zeros(img_shape, 2);
    if (!img) return NULL;
    
    /* 逐三角形光栅化 */
    for (int t = 0; t < ntris; t++) {
        int i0, i1, i2;
        if (triangles) {
            i0 = (int)nd_get_flat(triangles, t * 3 + 0);
            i1 = (int)nd_get_flat(triangles, t * 3 + 1);
            i2 = (int)nd_get_flat(triangles, t * 3 + 2);
        } else {
            i0 = t * 3 + 0;
            i1 = t * 3 + 1;
            i2 = t * 3 + 2;
        }
        if (i0 < 0 || i0 >= nverts || i1 < 0 || i1 >= nverts || i2 < 0 || i2 >= nverts)
            continue;
        
        /* 读取顶点 (screen coords: x,y) */
        double x0 = nd_get_flat(verts_2d, i0 * 2 + 0);
        double y0 = nd_get_flat(verts_2d, i0 * 2 + 1);
        double x1 = nd_get_flat(verts_2d, i1 * 2 + 0);
        double y1 = nd_get_flat(verts_2d, i1 * 2 + 1);
        double x2 = nd_get_flat(verts_2d, i2 * 2 + 0);
        double y2 = nd_get_flat(verts_2d, i2 * 2 + 1);
        
        /* 包围盒 (裁剪到图像范围) */
        int min_x = iclamp((int)floor(fmin(x0, fmin(x1, x2))), 0, width - 1);
        int max_x = iclamp((int)ceil(fmax(x0, fmax(x1, x2))), 0, width - 1);
        int min_y = iclamp((int)floor(fmin(y0, fmin(y1, y2))), 0, height - 1);
        int max_y = iclamp((int)ceil(fmax(y0, fmax(y1, y2))), 0, height - 1);
        
        if (min_x > max_x || min_y > max_y) continue;
        
        /* 叉积符号 确定 winding */
        double area2 = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
        if (area2 == 0) continue; /* 退化三角形 */
        
        /* 边缘函数法 (winding-independent) */
        for (int py = min_y; py <= max_y; py++) {
            double fy = (double)py + 0.5; /* 像素中心 */
            for (int px = min_x; px <= max_x; px++) {
                double fx = (double)px + 0.5;
                
                double e0 = (x1 - x0) * (fy - y0) - (y1 - y0) * (fx - x0);
                double e1 = (x2 - x1) * (fy - y1) - (y2 - y1) * (fx - x1);
                double e2 = (x0 - x2) * (fy - y2) - (y0 - y2) * (fx - x2);
                
                /* 同侧测试 (允许零值) */
                if ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                    /* 设置像素为白色 (1.0) */
                    int img_idx[2] = {py, px};
                    nd_set(img, img_idx, 1.0);
                }
            }
        }
    }
    
    return img;
}

/* 线框光栅化: 绘制三角形边缘 (1px 宽线条) */
NdTensor* nd_rasterize_wireframe(const NdTensor* verts_2d, const NdTensor* triangles,
                                  int width, int height) {
    if (!verts_2d || width <= 0 || height <= 0) return NULL;
    if (verts_2d->ndim != 2 || verts_2d->shape[1] != 2) return NULL;
    
    int nverts = verts_2d->shape[0];
    int ntris = 0;
    
    if (triangles) {
        if (triangles->ndim != 2 || triangles->shape[1] != 3) return NULL;
        ntris = triangles->shape[0];
    } else {
        if (nverts < 3 || nverts % 3 != 0) return NULL;
        ntris = nverts / 3;
    }
    
    int img_shape[2] = {height, width};
    NdTensor* img = nd_zeros(img_shape, 2);
    if (!img) return NULL;
    
    /* Bresenham 画线辅助 */
    /* 在图像 (py,px) 画点 (值为 1.0) */
    for (int t = 0; t < ntris; t++) {
        int i0, i1, i2;
        if (triangles) {
            i0 = (int)nd_get_flat(triangles, t * 3 + 0);
            i1 = (int)nd_get_flat(triangles, t * 3 + 1);
            i2 = (int)nd_get_flat(triangles, t * 3 + 2);
        } else {
            i0 = t * 3 + 0;
            i1 = t * 3 + 1;
            i2 = t * 3 + 2;
        }
        if (i0 < 0 || i0 >= nverts || i1 < 0 || i1 >= nverts || i2 < 0 || i2 >= nverts)
            continue;
        
        double x0 = nd_get_flat(verts_2d, i0 * 2 + 0);
        double y0 = nd_get_flat(verts_2d, i0 * 2 + 1);
        double x1 = nd_get_flat(verts_2d, i1 * 2 + 0);
        double y1 = nd_get_flat(verts_2d, i1 * 2 + 1);
        double x2 = nd_get_flat(verts_2d, i2 * 2 + 0);
        double y2 = nd_get_flat(verts_2d, i2 * 2 + 1);
        
        /* 画三条边: 0-1, 1-2, 2-0 */
        double edges[3][4] = {{x0,y0,x1,y1}, {x1,y1,x2,y2}, {x2,y2,x0,y0}};
        for (int e = 0; e < 3; e++) {
            int xa = (int)round(edges[e][0]), ya = (int)round(edges[e][1]);
            int xb = (int)round(edges[e][2]), yb = (int)round(edges[e][3]);
            
            /* Bresenham 直线算法 */
            int dx = abs(xb - xa), sx = xa < xb ? 1 : -1;
            int dy = -abs(yb - ya), sy = ya < yb ? 1 : -1;
            int err = dx + dy;  /* 注意 dy 为负 */
            
            while (1) {
                if (xa >= 0 && xa < width && ya >= 0 && ya < height) {
                    int idx[2] = {ya, xa};
                    nd_set(img, idx, 1.0);
                }
                if (xa == xb && ya == yb) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; xa += sx; }
                if (e2 <= dx) { err += dx; ya += sy; }
            }
        }
    }
    
    return img;
}

/* ========== 调试 ========== */

void nd_print_info(const NdTensor* t, const char* label) {
    if (!t) { printf("%s: NULL\n", label ? label : "tensor"); return; }
    printf("%s: %d-dim [", label ? label : "tensor", t->ndim);
    for (int i = 0; i < t->ndim; i++) printf("%s%d", i ? "," : "", t->shape[i]);
    printf("], size=%d", t->size);
    if (t->data && t->size > 0) printf(", [0]=%.4g", t->data[0]);
    printf("\n");
}

void nd_print_data(const NdTensor* t, const char* label) {
    if (!t) { printf("%s: NULL\n", label ? label : "tensor"); return; }
    printf("%s (%d-dim, size=%d): ", label ? label : "", t->ndim, t->size);
    int show = t->size < 64 ? t->size : 64;
    printf("[");
    for (int i = 0; i < show; i++) printf("%s%.4g", i ? ", " : "", t->data[i]);
    if (show < t->size) printf(", ...");
    printf("]\n");
}

/* ========== 光线追踪渲染 (追光) ========== */

static double vec3_len(double x, double y, double z) {
    return sqrt(x*x + y*y + z*z);
}
static void vec3_norm(double* x, double* y, double* z) {
    double len = vec3_len(*x, *y, *z);
    if (len > 1e-12) { *x /= len; *y /= len; *z /= len; }
}

/* 球体 SDF: distance = length(p - center) - radius */
static double sphere_sdf(double px, double py, double pz,
                         double cx, double cy, double cz, double r) {
    double dx = px - cx, dy = py - cy, dz = pz - cz;
    return sqrt(dx*dx + dy*dy + dz*dz) - r;
}

/* 中心差分法计算球体 SDF 梯度 (法线) */
static void sphere_normal(double px, double py, double pz,
                          double cx, double cy, double cz, double r,
                          double* nx, double* ny, double* nz) {
    double eps = 1e-4;
    double dx = sphere_sdf(px + eps, py, pz, cx, cy, cz, r)
              - sphere_sdf(px - eps, py, pz, cx, cy, cz, r);
    double dy = sphere_sdf(px, py + eps, pz, cx, cy, cz, r)
              - sphere_sdf(px, py - eps, pz, cx, cy, cz, r);
    double dz = sphere_sdf(px, py, pz + eps, cx, cy, cz, r)
              - sphere_sdf(px, py, pz - eps, cx, cy, cz, r);
    *nx = dx; *ny = dy; *nz = dz;
    vec3_norm(nx, ny, nz);
}

/*
 * nd_render_raytrace: 光线追踪渲染 3D 球体
 *
 * 参数: [W, H, cx, cy, cz, r, cam_x, cam_y, cam_z, light_x, light_y, light_z, ambient, diffuse]
 * 输出: [H, W] 灰度图像 (0~1)
 */
NdTensor* nd_render_raytrace(int W, int H,
                             double cx, double cy, double cz, double r,
                             double cam_x, double cam_y, double cam_z,
                             double light_x, double light_y, double light_z,
                             double ambient, double diffuse_k) {
    if (W <= 0 || H <= 0) return NULL;

    int shape[2] = {H, W};
    NdTensor* img = nd_zeros(shape, 2);
    if (!img) return NULL;

    /* 归一化光照方向 */
    vec3_norm(&light_x, &light_y, &light_z);

    /* 相机看向原点 */
    double target_x = 0, target_y = 0, target_z = 0;
    double up_x = 0, up_y = 1, up_z = 0;

    /* 计算相机坐标系 (lookat) */
    double fwd_x = target_x - cam_x;
    double fwd_y = target_y - cam_y;
    double fwd_z = target_z - cam_z;
    vec3_norm(&fwd_x, &fwd_y, &fwd_z);

    /* right = fwd × up */
    double right_x = fwd_y * up_z - fwd_z * up_y;
    double right_y = fwd_z * up_x - fwd_x * up_z;
    double right_z = fwd_x * up_y - fwd_y * up_x;
    vec3_norm(&right_x, &right_y, &right_z);

    /* real_up = right × fwd */
    double real_up_x = right_y * fwd_z - right_z * fwd_y;
    double real_up_y = right_z * fwd_x - right_x * fwd_z;
    double real_up_z = right_x * fwd_y - right_y * fwd_x;

    double focal_len = vec3_len(cam_x, cam_y, cam_z);
    double vp_h = 2.0 * tan(45.0 * M_PI / 180.0 / 2.0) * focal_len; /* 45° FOV */
    double vp_w = vp_h * (double)W / (double)H;

    int max_steps = 64;
    double hit_eps = 1e-3;
    double max_dist = 50.0;

    for (int py = 0; py < H; py++) {
        for (int px = 0; px < W; px++) {
            /* 像素映射到视平面 */
            double ndc_x = ((double)px + 0.5) / (double)W * 2.0 - 1.0;
            double ndc_y = ((double)py + 0.5) / (double)H * 2.0 - 1.0;
            double sx = ndc_x * vp_w / 2.0;
            double sy = ndc_y * vp_h / 2.0;

            /* 视平面点: 在相机前方 focal_len 处 */
            double ppx = cam_x + fwd_x * focal_len + right_x * sx + real_up_x * sy;
            double ppy = cam_y + fwd_y * focal_len + right_y * sx + real_up_y * sy;
            double ppz = cam_z + fwd_z * focal_len + right_z * sx + real_up_z * sy;

            /* 光线方向 = 像素点 - 相机原点 */
            double dir_x = ppx - cam_x;
            double dir_y = ppy - cam_y;
            double dir_z = ppz - cam_z;
            vec3_norm(&dir_x, &dir_y, &dir_z);

            /* 光线步进 */
            double t = 0;
            int hit = 0;
            for (int step = 0; step < max_steps; step++) {
                double rpx = cam_x + dir_x * t;
                double rpy = cam_y + dir_y * t;
                double rpz = cam_z + dir_z * t;

                double d = sphere_sdf(rpx, rpy, rpz, cx, cy, cz, r);
                if (d < hit_eps) { hit = 1; break; }
                t += d;
                if (t > max_dist) break;
            }

            double pixel_val = ambient;
            if (hit) {
                double hx = cam_x + dir_x * t;
                double hy = cam_y + dir_y * t;
                double hz = cam_z + dir_z * t;
                double nx, ny, nz;
                sphere_normal(hx, hy, hz, cx, cy, cz, r, &nx, &ny, &nz);
                double diff = nx * light_x + ny * light_y + nz * light_z;
                if (diff < 0) diff = 0;
                pixel_val = ambient + diffuse_k * diff;
                if (pixel_val > 1.0) pixel_val = 1.0;
            }

            int idx[2] = {py, px};
            nd_set(img, idx, pixel_val);
        }
    }

    return img;
}