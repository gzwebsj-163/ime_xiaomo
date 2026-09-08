/*
 * nd_tensor.h — ND Tensor 引擎
 *
 * 为 xiaomo 提供真正的 N 维张量支持（flat buffer + shape）。
 * 替换嵌套数组的矩阵表示，支持广播、张量缩并、卷积等。
 * 
 * 这是图片算力架构的「地基」——多元和复合元数层。
 */
#ifndef XIAOMO_ND_TENSOR_H
#define XIAOMO_ND_TENSOR_H

#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ND Tensor 数据结构 ---- */

struct NdTensor {
    double* data;      /* 连续 buffer，行主序 */
    int* shape;        /* 各维度大小 */
    int ndim;          /* 维度数 */
    int size;          /* 总元素数 = shape[0]*shape[1]*... */
};

/* ---- 创建与销毁 ---- */

/* 创建 ND 张量 (数据复制自 data; data 为 NULL 则填 0) */
NdTensor* nd_create(const int* shape, int ndim, const double* data);
/* 创建全 0 张量 */
NdTensor* nd_zeros(const int* shape, int ndim);
/* 创建全 1 张量 */
NdTensor* nd_ones(const int* shape, int ndim);
/* 创建标量张量 (0 维) */
NdTensor* nd_scalar(double val);
/* 从 MoValue(VAL_ARRAY) 转换 (递归将嵌套数组转 flat buffer) */
NdTensor* nd_from_movalue(const MoValue* v);
/* 深度克隆 */
NdTensor* nd_clone(const NdTensor* t);
/* 释放 */
void nd_free(NdTensor* t);

/* ---- 查询 ---- */

/* 元素总数 */
int nd_size(const NdTensor* t);
/* 获取维度 i 的大小 */
int nd_dim(const NdTensor* t, int i);
/* 获取元素的线性偏移 (支持负索引，如 -1 表示最后) */
int nd_offset(const NdTensor* t, const int* indices);
/* 获取元素值 */
double nd_get(const NdTensor* t, const int* indices);
/* 设置元素值 */
void nd_set(NdTensor* t, const int* indices, double val);
/* 获取扁平索引的元素 */
double nd_get_flat(const NdTensor* t, int flat_idx);
void nd_set_flat(NdTensor* t, int flat_idx, double val);

/* 转为 MoValue(VAL_ARRAY) 嵌套数组 (用于 .mo 层兼容) */
MoValue nd_to_movalue(const NdTensor* t);

/* ---- 广播 ---- */

/* 检查形状是否可广播 (返回 0 不可, 1 可) */
int nd_broadcastable(const int* sha, int nda, const int* shb, int ndb);
/* 广播后的形状 (输出到 out_shape, 返回 ndim) */
int nd_broadcast_shape(const NdTensor* a, const NdTensor* b, int* out_shape);
/* 将张量广播到目标形状 (返回新张量) */
NdTensor* nd_broadcast_to(const NdTensor* t, const int* target_shape, int target_ndim);

/* ---- 核心算子 (返回新 NdTensor, 调用方 nd_free) ---- */

/* 张量缩并 (广义矩阵乘法): 
 *    C[a,b] = sum_k A[a,k] * B[k,b]
 *    更一般: C[...,i,j] = sum_k A[...,i,k] * B[...,k,j]
 *   axes_a/axes_b 指定缩并的轴
 */
NdTensor* nd_tensordot(const NdTensor* A, const NdTensor* B,
                        const int* axes_a, const int* axes_b, int n_axes);

/* 矩阵乘法: A:(...,m,k) × B:(...,k,n) -> (...,m,n) (带广播) */
NdTensor* nd_matmul(const NdTensor* A, const NdTensor* B);

/* 逐元素运算 (带广播) */
NdTensor* nd_add(const NdTensor* A, const NdTensor* B);
NdTensor* nd_sub(const NdTensor* A, const NdTensor* B);
NdTensor* nd_mul(const NdTensor* A, const NdTensor* B);  /* 逐元素乘 */
NdTensor* nd_div(const NdTensor* A, const NdTensor* B);  /* 逐元素除 */
NdTensor* nd_pow(const NdTensor* A, const NdTensor* B);  /* 逐元素幂 */

/* 一元逐元素运算 */
NdTensor* nd_relu(const NdTensor* X);
NdTensor* nd_tanh(const NdTensor* X);
NdTensor* nd_sigmoid(const NdTensor* X);
NdTensor* nd_exp(const NdTensor* X);
NdTensor* nd_log(const NdTensor* X);
NdTensor* nd_abs(const NdTensor* X);
NdTensor* nd_neg(const NdTensor* X);
NdTensor* nd_sqrt(const NdTensor* X);

/* 归约运算 (沿指定轴) */
NdTensor* nd_sum(const NdTensor* X, int axis);   /* axis=-1 全局求和 */
NdTensor* nd_mean(const NdTensor* X, int axis);
NdTensor* nd_max(const NdTensor* X, int axis);
NdTensor* nd_min(const NdTensor* X, int axis);

/* softmax 沿指定轴 (axis=-1 沿最后一维) */
NdTensor* nd_softmax(const NdTensor* X, int axis);

/* ---- 形状操作 ---- */

/* 重塑 (总元素数必须不变, -1 自动推断) */
NdTensor* nd_reshape(const NdTensor* t, const int* new_shape, int new_ndim);

/* 转置 (perm 指定轴顺序, perm=NULL 表示完全逆转) */
NdTensor* nd_transpose(const NdTensor* t, const int* perm);

/* 切片: t[start[0]:end[0]:step[0], start[1]:end[1]:step[1], ...] */
NdTensor* nd_slice(const NdTensor* t, const int* starts, const int* ends, const int* steps);

/* 拼接: 沿 axis 拼接多个张量 (形状除 axis 外必须相同) */
NdTensor* nd_concat(const NdTensor** tensors, int n, int axis);

/* 填充: 在 axis 维两端各填充 pad 个 0 */
NdTensor* nd_pad(const NdTensor* t, int axis, int pad_before, int pad_after);

/* ---- 卷积相关 ---- */

/* im2col: 将 4D 输入 (N,C,H,W) 展开为 2D 矩阵 (N*OH*OW, C*KH*KW) */
NdTensor* nd_im2col(const NdTensor* input, 
                     int kh, int kw, int ph, int pw, int sh, int sw);

/* 2D 卷积: input(N,C,H,W) @ kernel(O,C,KH,KW) -> output(N,O,OH,OW) */
NdTensor* nd_conv2d(const NdTensor* input, const NdTensor* kernel,
                     int pad_h, int pad_w, int stride_h, int stride_w);

/* 2D 最大池化 */
NdTensor* nd_maxpool2d(const NdTensor* input,
                        int kh, int kw, int ph, int pw, int sh, int sw);

/* ---- 池化与上采样 ---- */

/* 最近邻上采样: 4D (N,C,H,W) 或 2D (H,W), scales 为整数倍 */
NdTensor* nd_upsample_nearest(const NdTensor* t, int scale_h, int scale_w);

/* ---- 图像专用 ---- */

/* 颜色空间转换: RGB <-> HSV (input shape: (...,3)) */
NdTensor* nd_rgb2hsv(const NdTensor* rgb);
NdTensor* nd_hsv2rgb(const NdTensor* hsv);

/* 灰度化: (... ,3) -> (..., 1) */
NdTensor* nd_grayscale(const NdTensor* rgb);

/* ---- 3D 体积分析 ---- */

/* 3D 梯度: 用中心差分计算 3D 标量场梯度
 * input:  3D 张量 [D, H, W] 或 4D [1, D, H, W]
 * return: 4D 张量 [3, D, H, W] (通道 0=Gx, 1=Gy, 2=Gz) */
NdTensor* nd_gradient3d(const NdTensor* field);

/* Marching Cubes: 从 3D 标量场提取等值面网格
 * field:  3D [D, H, W] 或 4D [1, D, H, W] 标量场
 * iso:    等值面阈值
 * return: 张量 [N*3, 3], N=三角形数, 每行一个顶点(x,y,z), 每三行一个三角形 */
NdTensor* nd_marching_cubes(const NdTensor* field, double iso);

/* ---- 场生成辅助 ---- */

/* 生成 3D 球体距离场 (用于测试 MC)
 * D,H,W: 场尺寸
 * radius: 球体半径
 * cx,cy,cz: 球心坐标 (归一化到场索引空间)
 * 返回: [D, H, W] 标量场, 值为到球心的有符号距离 */
NdTensor* nd_sphere_field(int D, int H, int W, double radius,
                           double cx, double cy, double cz);
NdTensor* nd_ellipsoid_field(int D, int H, int W,
                             double rx, double ry, double rz,
                             double cx, double cy, double cz);

/* ---- 网格渲染 ---- */

/* 光栅化 2D 三角形到灰度图像
 * verts_2d: [N, 2] 或 [N*3, 2] 投影后的 2D 顶点坐标
 * triangles: [T, 3] 顶点索引 (若为 NULL, 则每 3 个连续顶点构成一个三角形)
 * width, height: 输出图像尺寸
 * 返回: [H, W] 灰度图像 (0=背景, 1=填充) */
NdTensor* nd_rasterize_triangles(const NdTensor* verts_2d, const NdTensor* triangles,
                                 int width, int height);
NdTensor* nd_rasterize_wireframe(const NdTensor* verts_2d, const NdTensor* triangles,
                                 int width, int height);

/* ---- 光线追踪 ---- */
NdTensor* nd_render_raytrace(int W, int H,
                             double cx, double cy, double cz, double r,
                             double cam_x, double cam_y, double cam_z,
                             double light_x, double light_y, double light_z,
                             double ambient, double diffuse_k);

/* ---- 调试 ---- */

/* 打印张量信息 */
void nd_print_info(const NdTensor* t, const char* label);
/* 打印张量数据 (最多 64 元素) */
void nd_print_data(const NdTensor* t, const char* label);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_ND_TENSOR_H */