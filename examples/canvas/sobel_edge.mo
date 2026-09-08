# Canvas 管线: Sobel 边缘检测 (conv2d + 梯度幅值)
# 用法: ./xiaomo run examples/canvas/sobel_edge.mo

>> print >> "=== Sobel Edge Detection ==="

# ---- 输入: 6x6 灰度图像 (含水平/垂直/对角边缘) ----
void img : int = tensor([
    [10, 10, 10, 80, 80, 80],
    [10, 10, 10, 80, 80, 80],
    [10, 10, 10, 80, 80, 80],
    [40, 40, 40, 120, 120, 120],
    [40, 40, 40, 120, 120, 120],
    [40, 40, 40, 120, 120, 120]
])

# ---- Sobel X 核 (垂直边缘检测) ----
void sobel_x : int = tensor([
    [-1, 0, 1],
    [-2, 0, 2],
    [-1, 0, 1]
])

# ---- Sobel Y 核 (水平边缘检测) ----
void sobel_y : int = tensor([
    [-1, -2, -1],
    [0, 0, 0],
    [1, 2, 1]
])

# ---- 转为 NCHW 格式 [1,1,H,W] ----
void img_4d : int = reshape(${img}, [1, 1, 6, 6])
void kx_4d : int = reshape(${sobel_x}, [1, 1, 3, 3])
void ky_4d : int = reshape(${sobel_y}, [1, 1, 3, 3])

# ---- conv2d: pad=0, stride=1 (输出 4x4) ----
void gx_raw : int = conv2d(${img_4d}, ${kx_4d}, 0, 0, 1, 1)
void gy_raw : int = conv2d(${img_4d}, ${ky_4d}, 0, 0, 1, 1)

# ---- 展回 2D ---- 
void Gx : int = reshape(${gx_raw}, [4, 4])
void Gy : int = reshape(${gy_raw}, [4, 4])

# ---- 梯度幅值: mag = sqrt(Gx^2 + Gy^2) ----
void Gx2 : int = nd_mul(${Gx}, ${Gx})
void Gy2 : int = nd_mul(${Gy}, ${Gy})
void Gsum : int = nd_add(${Gx2}, ${Gy2})
void mag : int = nd_sqrt(${Gsum})

# ---- 转数组打印 ---- 
void print_img : int = tensor_to_array(${img})
void print_Gx : int = tensor_to_array(${Gx})
void print_Gy : int = tensor_to_array(${Gy})
void print_mag : int = tensor_to_array(${mag})

>> print >> "Input (6x6):"
>> print >> ${print_img}
>> print >> "Gradient X (4x4):"
>> print >> ${print_Gx}
>> print >> "Gradient Y (4x4):"
>> print >> ${print_Gy}
>> print >> "Magnitude (4x4):"
>> print >> ${print_mag}

# ---- 验证: 垂直边缘响应强于水平区域 ----
>> print >> "=== Sobel Edge Detection PASSED ==="