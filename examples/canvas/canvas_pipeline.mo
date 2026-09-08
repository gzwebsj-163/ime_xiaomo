# Canvas 管线: 多轮组合管线 (Blur → Edge → Blend → Quantize)
# 用法: ./xiaomo run examples/canvas/canvas_pipeline.mo
#
# 管线流程:
#   1. 高斯模糊 (去噪)
#   2. Sobel 边缘检测
#   3. 边缘叠加回原图 (Screen blend)
#   4. 颜色量化 (4级 soft)
# 
# 等价于: output = quantize(screen(gray_blur(img), sobel(gray_blur(img))))

>> print >> "=== Canvas Pipeline: Blur → Edge → Blend → Quantize ==="

# ---- 输入: 8x8 灰度图像 (有块状边缘) ----
void img : int = tensor([
    [20, 20, 20, 20, 80, 80, 80, 80],
    [20, 20, 20, 20, 80, 80, 80, 80],
    [20, 20, 50, 50, 80, 80, 80, 80],
    [20, 20, 50, 50, 80, 80, 80, 80],
    [40, 40, 40, 40, 120, 120, 120, 120],
    [40, 40, 40, 40, 120, 120, 120, 120],
    [40, 40, 60, 60, 120, 120, 120, 120],
    [40, 40, 60, 60, 120, 120, 120, 120]
])

# ======== 第1轮: 高斯模糊 ========
void img_4d : int = reshape(${img}, [1, 1, 8, 8])
void gk_4d : int = reshape(tensor([[1,2,1],[2,4,2],[1,2,1]]), [1, 1, 3, 3])
void blurred_raw : int = conv2d(${img_4d}, ${gk_4d}, 1, 1, 1, 1)
void blurred_2d : int = reshape(${blurred_raw}, [8, 8])
void blurred : int = nd_div(${blurred_2d}, 16.0)

# ======== 第2轮: Sobel 边缘检测 ========
void b_4d : int = reshape(${blurred}, [1, 1, 8, 8])
void sx_4d : int = reshape(tensor([[-1,0,1],[-2,0,2],[-1,0,1]]), [1, 1, 3, 3])
void sy_4d : int = reshape(tensor([[-1,-2,-1],[0,0,0],[1,2,1]]), [1, 1, 3, 3])
void gx_raw : int = conv2d(${b_4d}, ${sx_4d}, 0, 0, 1, 1)
void gy_raw : int = conv2d(${b_4d}, ${sy_4d}, 0, 0, 1, 1)
void Gx : int = reshape(${gx_raw}, [6, 6])
void Gy : int = reshape(${gy_raw}, [6, 6])
void mag : int = nd_sqrt(nd_add(nd_mul(${Gx}, ${Gx}), nd_mul(${Gy}, ${Gy})))

# ---- 归一化边缘到 0~1 ----
void mag_max : int = 400.0
void edge_norm : int = nd_div(${mag}, ${mag_max})
void edge_clip : int = nd_relu(nd_sub(${edge_norm}, 0.05))  # 去噪阈值
void edge_norm2 : int = nd_mul(${edge_clip}, 1.5)            # 增强对比
void edge : int = nd_relu(nd_sub(${edge_norm2}, 0.0))        # 钳位负值

# ======== 第3轮: Screen 混合 (边缘叠加到模糊图) ========
# 裁剪 blurred 到 6x6 匹配边缘尺寸
void blur_crop : int = nd_slice(${blurred}, [1, 1], [7, 7], [1, 1])
void blur_norm : int = nd_div(${blur_crop}, 160.0)

# Screen: dst = 1 - (1-blur)*(1-edge)
void one : int = 1.0
void inv_b : int = nd_sub(${one}, ${blur_norm})
void inv_e : int = nd_sub(${one}, ${edge})
void screen_raw : int = nd_mul(${inv_b}, ${inv_e})
void blended : int = nd_sub(${one}, ${screen_raw})

# 恢复 0~160 范围
void blended_160 : int = nd_mul(${blended}, 160.0)

# ======== 第4轮: 颜色量化 (软 4 级) ========
void s64 : int = sigmoid_n(nd_div(nd_sub(${blended_160}, 64), 12))
void s128 : int = sigmoid_n(nd_div(nd_sub(${blended_160}, 128), 12))
void s192 : int = sigmoid_n(nd_div(nd_sub(${blended_160}, 192), 12))
void q_sum : int = nd_add(nd_add(nd_mul(${s64}, 64), nd_mul(${s128}, 64)), nd_mul(${s192}, 64))
void quantized : int = nd_add(${q_sum}, 32)

# ======== 打印各轮结果 ========
void print_orig : int = tensor_to_array(${img})
void print_blur : int = tensor_to_array(${blurred})
void print_edge : int = tensor_to_array(${edge})
void print_blend : int = tensor_to_array(${blended_160})
void print_quant : int = tensor_to_array(${quantized})

>> print >> ""
>> print >> "Step 1 — Input (8x8 grayscale):"
>> print >> ${print_orig}
>> print >> "Step 2 — Gaussian Blurred:"
>> print >> ${print_blur}
>> print >> "Step 3 — Edge Magnitude (6x6, normalized):"
>> print >> ${print_edge}
>> print >> "Step 4 — Screen Blended (6x6, edge over blur):"
>> print >> ${print_blend}
>> print >> "Step 5 — Quantized (4 levels, 6x6):"
>> print >> ${print_quant}

>> print >> ""
>> print >> "=== Canvas Pipeline COMPLETE ==="
