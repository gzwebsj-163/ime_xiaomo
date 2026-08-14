/*
 * xiaomo - 张量推理内核 (Tensor Inference Core)
 *
 * 为 xiaomo VM 提供面向模型推理的原生张量算子。
 * 表示约定: 矩阵/向量用 MoValue(VAL_ARRAY) 表示
 *   - 矩阵    : [row1, row2, ...]  每个 row 是一维数组 [c0, c1, ...]
 *   - 行向量  : [e0, e1, ...] (一维数组)
 *   - 列向量  : 视作 n×1 矩阵
 * 元素均为 VAL_FLOAT。
 *
 * 这是"模型运行时宿主"的 C 地基: 只做纯数值计算, 不依赖 vm.c 的 static helper,
 * 通过公开的 MoValue/MoArray 结构自建结果值。
 */
#ifndef XIAOMO_TENSOR_H
#define XIAOMO_TENSOR_H

#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 矩阵/向量 基本工具 (基于公开 MoValue/MoArray) ---- */

/* 创建 1 维向量 (length 个 VAL_FLOAT, 数据复制自 data; data 可为 NULL 则填 0) */
MoValue xm_vec_new(int length, const double* data);
/* 创建 m×n 矩阵 (data 按行主序, 长度 m*n; 可为 NULL 则填 0) */
MoValue xm_mat_new(int rows, int cols, const double* data);
/* 创建 m×n 全零矩阵 */
MoValue xm_mat_zeros(int rows, int cols);
/* 创建 n×n 单位矩阵 */
MoValue xm_mat_eye(int n);

/* 查询: 返回 1 当 v 是合法矩阵; 行列数写入输出参数; 对一维向量返回 cols=1 */
int xm_mat_shape(const MoValue* v, int* rows, int* cols);
/* 元素访问: out = v[r*cols + c], 返回 0 成功 */
int xm_mat_get(const MoValue* v, int r, int c, double* out);
/* 元素写入: 就地修改 v 中的元素 */
int xm_mat_set(MoValue* v, int r, int c, double val);

/* 扁平化读: 按行主序把矩阵元素写到一维 buf (buf 需 >= rows*cols) */
void xm_mat_to_flat(const MoValue* v, double* buf);

/* ---- 模型算子 (返回新 MoValue, 由调用方 value_free) ---- */
/* C = A × B, A:(m×k) B:(k×n) -> C:(m×n) */
MoValue xm_matmul(const MoValue* A, const MoValue* B);
/* Y = X + b, X:(m×n) b:(1×n 或 n 向量, 广播到每行) 或 b:标量 */
MoValue xm_bias_add(const MoValue* X, const MoValue* b);
/* ReLU 逐元素 */
MoValue xm_relu(const MoValue* X);
/* tanh 逐元素 */
MoValue xm_tanh(const MoValue* X);
/* sigmoid 逐元素 */
MoValue xm_sigmoid(const MoValue* X);
/* softmax 沿行 (每行一个分布); 对一维向量则整体 softmax */
MoValue xm_softmax(const MoValue* X);
/* 逐元素相乘 (Hadamard), 形状必须相同 */
MoValue xm_mul(const MoValue* A, const MoValue* B);
/* 逐元素相减: A - B, 形状必须相同 (用于梯度下降权重更新: W - lr*dW) */
MoValue xm_sub(const MoValue* A, const MoValue* B);
/* 逐元素数乘: A * k (标量 k 广播到所有元素; 用于学习率缩放) */
MoValue xm_scale(const MoValue* A, double k);
/* 转置: A:(m×n) -> (n×m) */
MoValue xm_transpose(const MoValue* A);

/* ---- 损失 & 梯度 (训练支线) ---- */
/* 均方误差标量: mean((A-B)^2), A/B 形状相同 */
MoValue xm_loss_mse(const MoValue* A, const MoValue* B);
/* 交叉熵标量: -mean( sum_c y_true_c * log(y_pred_c + eps) ) */
MoValue xm_loss_crossentropy(const MoValue* A, const MoValue* B);
/* tanh 导数: (1 - h^2) 逐元素 */
MoValue xm_grad_tanh(const MoValue* H);
/* MSE 梯度: (2/len)(A - B) 逐元素 */
MoValue xm_grad_mse(const MoValue* A, const MoValue* B);

/* 打印矩阵到 buf (调试/输出用) */
void xm_mat_print(const MoValue* v, char* buf, int buflen);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_TENSOR_H */
