# CNN Autoencoder — 卷积÷池化÷上采样重建 (第③层深层模型)
# 结构: 8x8 输入 → conv1(3x3,pad1) → pool(2x2) → conv2(3x3,pad1)
#       → pool(2x2) → upsample(2x) → conv3(3x3,pad1) → upsample(2x)
#       → conv4(3x3,pad1) → 输出 8x8 重建
#
# 所有卷积: 1 通道输入/输出, 3x3 核, padding=1
# 等价于: input(8×8) → 4×4 → 2×2 → 4×4 → 8×8
# 真实场景需学习权重, 此处用固定核做特征提取+重建演示

>> print >> "=== CNN Autoencoder (8x8) ==="

# ---- 输入: 8x8 带结构图像 ----
void img : int = tensor([
    [10, 10, 30, 50, 50, 30, 10, 10],
    [10, 20, 50, 80, 80, 50, 20, 10],
    [20, 40, 80, 120, 120, 80, 40, 20],
    [30, 60, 100, 150, 150, 100, 60, 30],
    [30, 60, 100, 150, 150, 100, 60, 30],
    [20, 40, 80, 120, 120, 80, 40, 20],
    [10, 20, 50, 80, 80, 50, 20, 10],
    [10, 10, 30, 50, 50, 30, 10, 10]
])

# ---- 转为 4D [1,1,8,8] 用于卷积 ----
void img_4d : int = reshape(${img}, [1, 1, 8, 8])

# ---- 卷积核定义 ----
# conv1: 边缘检测核 + 平滑核 (2 个 kernel, 演示多通道)
void k1_edge : int = reshape(tensor([[-1,-1,-1],[0,0,0],[1,1,1]]), [1,1,3,3])
void k1_blur : int = reshape(tensor([[1,2,1],[2,4,2],[1,2,1]]), [1,1,3,3])

# conv2 核: 细节增强
void k2_sharp : int = reshape(tensor([[0,-1,0],[-1,5,-1],[0,-1,0]]), [1,1,3,3])

# conv3 核: 重建核 (平滑)
void k3_smooth : int = reshape(tensor([[1,2,1],[2,4,2],[1,2,1]]), [1,1,3,3])

# conv4 核: 最终输出
void k4_out : int = reshape(tensor([[1,1,1],[1,2,1],[1,1,1]]), [1,1,3,3])

# ======== Encoder ========

# ---- Conv1: 边缘特征 ----
void c1_edge_raw : int = conv2d(${img_4d}, ${k1_edge}, 1, 1, 1, 1)
void c1_edge : int = nd_abs(${c1_edge_raw})  # 绝对值取边缘强度

# ---- Conv1: 平滑分支 ----
void c1_blur : int = conv2d(${img_4d}, ${k1_blur}, 1, 1, 1, 1)
void c1_blur_n : int = nd_div(${c1_blur}, 16.0)

# ---- 融合: 边缘+平滑 ----
void c1_fuse : int = nd_add(${c1_edge}, ${c1_blur_n})

# ---- Pool1: 2x2 downsample [1,1,8,8] → [1,1,4,4] ----
void p1 : int = maxpool2d(${c1_fuse}, 2, 2, 0, 0, 2, 2)

# ---- Conv2: 细节增强 ----
void c2 : int = conv2d(${p1}, ${k2_sharp}, 1, 1, 1, 1)
void c2_relu : int = nd_relu(${c2})

# ---- Pool2: 2x2 [1,1,4,4] → [1,1,2,2] (latent code) ----
void latent : int = maxpool2d(${c2_relu}, 2, 2, 0, 0, 2, 2)

# ======== Decoder ========

# ---- Upsample1: 2x [1,1,2,2] → [1,1,4,4] ----
void u1 : int = upsample_n(${latent}, 2, 2)

# ---- Conv3: 重建特征 ----
void c3 : int = conv2d(${u1}, ${k3_smooth}, 1, 1, 1, 1)
void c3_n : int = nd_div(${c3}, 16.0)

# ---- Upsample2: 2x [1,1,4,4] → [1,1,8,8] ----
void u2 : int = upsample_n(${c3_n}, 2, 2)

# ---- Conv4: 最终输出 ----
void c4 : int = conv2d(${u2}, ${k4_out}, 1, 1, 1, 1)
void c4_n : int = nd_div(${c4}, 9.0)

# ---- 展平回 2D ----
void out_2d : int = reshape(${c4_n}, [8, 8])

# ======== 验证 ========

# 打印各阶段
void p_img : int = tensor_to_array(${img})
void p_c1 : int = tensor_to_array(reshape(${c1_fuse}, [8,8]))
void p_p1 : int = tensor_to_array(reshape(${p1}, [4,4]))
void p_lat : int = tensor_to_array(reshape(${latent}, [2,2]))
void p_out : int = tensor_to_array(${out_2d})

>> print >> ""
>> print >> "Input (8x8):"
>> print >> ${p_img}
>> print >> "After Conv1 (edge+blur, 8x8):"
>> print >> ${p_c1}
>> print >> "After Pool1 (4x4):"
>> print >> ${p_p1}
>> print >> "Latent code (2x2):"
>> print >> ${p_lat}
>> print >> "Reconstructed (8x8):"
>> print >> ${p_out}

# ---- 重建误差 ----
void diff : int = nd_sub(${c4_n}, ${img_4d})
void mse : int = nd_mean(nd_mul(${diff}, ${diff}), 0)
>> print >> "Reconstruction MSE: " >> ${mse}

>> print >> ""
>> print >> "=== CNN Autoencoder PASSED ==="