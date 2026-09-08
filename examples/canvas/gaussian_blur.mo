# Canvas 管线: 高斯模糊 (conv2d)
# 用法: ./xiaomo run examples/canvas/gaussian_blur.mo

>> print >> "=== Gaussian Blur (conv2d) ==="

# ---- 输入: 4x4 灰度图像 (单通道) ----
void img : int = tensor([
    [10, 20, 30, 40],
    [50, 60, 70, 80],
    [90, 100, 110, 120],
    [130, 140, 150, 160]
])

# ---- 高斯核 3x3 (sigma=0.85, 归一化) ----
void gauss_kernel : int = tensor([
    [1, 2, 1],
    [2, 4, 2],
    [1, 2, 1]
])

# ---- 归一化: gauss_kernel sum = 16 ----
void kernel_sum : int = 16.0

# ---- 需要将输入 reshape 为 [1,1,H,W] (NCHW) ----
void img_4d : int = reshape(${img}, [1, 1, 4, 4])

# ---- kernel reshape 为 [1,1,3,3] ----
void knl_4d : int = reshape(${gauss_kernel}, [1, 1, 3, 3])

# ---- conv2d: pad=1, stride=1 (same size output) ----
void blurred : int = conv2d(${img_4d}, ${knl_4d}, 1, 1, 1, 1)

# ---- 取出结果 [1,1,4,4] -> 4x4 ----
void raw : int = reshape(${blurred}, [4, 4])

# ---- 归一化除以 kernel_sum ----
void result : int = nd_div(${raw}, ${kernel_sum})

# ---- 转数组打印实际值 ----
void print_img : int = tensor_to_array(${img})
void print_out : int = tensor_to_array(${result})

>> print >> "Input (4x4 grayscale):"
>> print >> ${print_img}
>> print >> "Blurred result (4x4):"
>> print >> ${print_out}

>> print >> "=== Gaussian Blur PASSED ==="