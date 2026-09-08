# Canvas 管线: 图像混合模式 (Multiply / Screen / Overlay)
# 用法: ./xiaomo run examples/canvas/blend_modes.mo

>> print >> "=== Blend Modes ==="

# ---- 两张 3x3 RGB 图像 (归一化 0~1) ----
void imgA : int = tensor([
    [[0.2, 0.3, 0.4], [0.5, 0.6, 0.7], [0.8, 0.9, 1.0]],
    [[0.1, 0.2, 0.3], [0.4, 0.5, 0.6], [0.7, 0.8, 0.9]],
    [[0.0, 0.1, 0.2], [0.3, 0.4, 0.5], [0.6, 0.7, 0.8]]
])

void imgB : int = tensor([
    [[0.5, 0.5, 0.5], [0.5, 0.5, 0.5], [0.5, 0.5, 0.5]],
    [[0.3, 0.3, 0.3], [0.3, 0.3, 0.3], [0.3, 0.3, 0.3]],
    [[0.8, 0.8, 0.8], [0.8, 0.8, 0.8], [0.8, 0.8, 0.8]]
])

# ---- Multiply: dst = A * B ----
void mul_blend : int = nd_mul(${imgA}, ${imgB})

# ---- Screen: dst = 1 - (1-A)*(1-B) ----
void one : int = 1.0
void one_minus_A : int = nd_sub(${one}, ${imgA})
void one_minus_B : int = nd_sub(${one}, ${imgB})
void screen_inv : int = nd_mul(${one_minus_A}, ${one_minus_B})
void screen_blend : int = nd_sub(${one}, ${screen_inv})

# ---- Overlay: ----
# if A < 0.5: dst = 2*A*B
# else: dst = 1 - 2*(1-A)*(1-B)
# 用 sigmoid 做平滑过渡
void A_half : int = nd_sub(${imgA}, 0.5)
void sA : int = sigmoid_n(nd_mul(${A_half}, 10))  # A>0.5 → 1

# overlay_low = 2*A*B
void two_A : int = nd_mul(${imgA}, 2.0)
void overlay_low : int = nd_mul(${two_A}, ${imgB})

# overlay_high = 1 - 2*(1-A)*(1-B)
void two_invA : int = nd_mul(${one_minus_A}, 2.0)
void overlay_high : int = nd_sub(${one}, nd_mul(${two_invA}, ${one_minus_B}))

# blend = overlay_low*(1-sA) + overlay_high*sA
void inv_sA : int = nd_sub(${one}, ${sA})
void low_part : int = nd_mul(${overlay_low}, ${inv_sA})
void high_part : int = nd_mul(${overlay_high}, ${sA})
void overlay_blend : int = nd_add(${low_part}, ${high_part})

# ---- 打印 ----
void print_A : int = tensor_to_array(${imgA})
void print_B : int = tensor_to_array(${imgB})
void print_mul : int = tensor_to_array(${mul_blend})
void print_scr : int = tensor_to_array(${screen_blend})
void print_ovr : int = tensor_to_array(${overlay_blend})

>> print >> "Image A (3x3 RGB, normalized 0-1):"
>> print >> ${print_A}
>> print >> "Image B (uniform):"
>> print >> ${print_B}
>> print >> "  Multiply blend (A * B):"
>> print >> ${print_mul}
>> print >> "  Screen blend (1-(1-A)*(1-B)):"
>> print >> ${print_scr}
>> print >> "  Overlay blend (soft threshold):"
>> print >> ${print_ovr}

>> print >> "=== Blend Modes PASSED ==="
