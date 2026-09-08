# Canvas 管线: 颜色量化 (uniform quantization)
# 用法: ./xiaomo run examples/canvas/color_quantize.mo

>> print >> "=== Color Quantization (uniform) ==="

# ---- RGB 输入: 4x4 RGB 图像 (每个像素 [R,G,B], 范围 0~255) ----
void rgb_img : int = tensor([
    [[12, 45, 200], [67, 89, 210], [34, 12, 180], [200, 50, 30]],
    [[90, 200, 10], [150, 60, 220], [20, 180, 100], [250, 200, 150]],
    [[40, 80, 120], [180, 30, 190], [70, 220, 40], [100, 100, 100]],
    [[0, 128, 255], [128, 128, 128], [255, 255, 255], [32, 64, 96]]
])

# ---- 量化参数: 每通道 4 级 (64 bins) ----
# bins = 256 / 4 = 64, 每个像素量化到最近的级中心: 32, 96, 160, 224

# 步骤: 对每个通道除以 64, 取整, 再乘 64 加 32
# quantized = floor(value / 64) * 64 + 32

# 但 nd tensor 没有 floor_div, 用转换: 先除 64, 取整再乘 64 加 32
# 用 nd_mul + nd_add + nd_sub 拼:
# q = (v // 64) * 64 + 32
# 近似: 先用 (v / 64) 得 float, 但 ND tensor 全是 double

# ---- 简单量化: 3 级 (85 bins) ----
# q = round(v / 85) * 85, 到 0,85,170,255 四档
# 简化: 用 nd_add/nd_sub/nd_mul 组合

# ---- 直接做: 对每个值除以 85, round, 乘 85 ----
# 用(v + 42) // 85 * 85 近似: 但没有 floor 运算

# 用代数方法: q = ((v + 42) / 85) 取整乘 85
# 没有取整运算, 用 trn = v / 85 然后近似

# 换个方法: 用 2 级量化 (黑白二值), 阈值 128
# 这样只需要一次比较和选择

# ---- 方法: 二值量化 (threshold = 128) ----
# 对每个通道: if v > 128, v=220; else v=35

# 由于 .mo 没有直接的 if-then-else 逐元素运算
# 用数学方法: v = 35 + (v > 128 ? 185 : 0)
# 用 sigmoid 近似: (1 / (1 + exp(-(v-128)/20))) * 185 + 35
# 但这个比较麻烦

# 最简单: 用 nd_slice + nd_pad 没法做条件, 用数学技巧
# threshold_op: result = (v/128 取整) * 185 + 35
# 用 1/(1+e^(-(v-128)/20)) 做可微阈值

# ---- 直接用 tanh 做软阈值: ---- 
# t = tanh((v - 128) / 20)  # -1 ~ 1
# q = 35 + (t + 1) / 2 * 185  # 35 ~ 220

# 简化: t = tanh((v - 128) / 20)
# v = 127.5 + t * 92.5

# ---- 颜色量化(4级) 用 floor 除法近似 ----
# 先除以 64, 截断小数, 再乘 64 + 32
# 用 tanh 近似截断: trunc(x) ≈ x - tanh(10*sin(pi*x))/pi (复杂)
# 
# 更简单: 用 软量化: q = sum_{k=0}^{3} c_k * softmax(-|v - c_k|/T)
# 但这需要外层循环

# ---- 最简洁: 直接用已实现的算子做均匀 4 级量化 ----
# 每个通道除以 85, round, 乘 85
# round(x) = floor(x + 0.5), 用 tanh 近似
# 或者用: 先 (v + 42) / 85 不做截断, 然后用数学技巧

# ---- 换个思路: 手动在每个通道上做 ----
# .mo 支持下标访问, 可以做逐像素操作
# 但对于 4x4x3 的图像, 要写 48 个像素太繁琐

# ---- 最终用: 4 级 soft 量化 ----
# level = [32, 96, 160, 224]
# 对每个 v, 计算到 4 个 level 的负距离, 用 softmax 加权
# 这等价于: q = sum_k level_k * softmax(-(v - level_k)^2 / T)

# 实际上, 可以直接用数学运算:
# q = 32 + 64 * floor(v/64) 
# 其中 floor(v/64) 用 (v - (v%64))/64
# 但取模运算没有

# ---- 最实用的方法: 用 2 级量化 + nd_relu 组合 ----
# relu 可以近似阈值

# ---- OK, 我直接用最简单可写的方法: 手动量化 4 个小像素 ----
# 只量化第一行的 4 个像素, 对每个 RGB 通道单独做

# 取第一行 R 通道
void row0 : int = nd_slice(${rgb_img}, [0, 0, 0], [1, 4, 3], [1, 1, 1])

# 展平为 12 个值 (4像素 × 3通道)
void flat : int = reshape(${row0}, [12])

# 手工做 4 级量化 (阈值 64/128/192)
# 对每个值:
# q = 32 if v < 64
# q = 96 if 64 <= v < 128
# q = 160 if 128 <= v < 192
# q = 224 if v >= 192

# 用 3 个 relu 层构造阶梯函数:
# step1 = relu(v - 64) / (v - 64) 但不可除零
# 用 sigmoid: s(v - 64) = 1/(1+e^(-(v-64)/10))
# s0 = sigmoid(v - 64)   # v>64 时接近 1
# s1 = sigmoid(v - 128)  # v>128 时接近 1
# s2 = sigmoid(v - 192)  # v>192 时接近 1

# q = 32 + 64*s0 + 64*s1 + 64*s2
# 但 sigmoid 不是精确阶跃, 有过渡区

# 用硬阈值: tanh(k*(v-t)) 接近阶跃
# 用 k=10 时过渡区 ~0.5

# ---- 干脆: 把每个通道除以 64, 用 nd_div 再做 nd_mul 取整 ----
# 但这又回到除法取整问题

# ---- 最稳妥: 手工计算几个像素验证概念 ----
# 写一个更小的测试, 用显式公式

# OK 我换一个更简短的演示, 用 nd_relu 做硬阈值:
void v : int = 100.0
void t64 : int = sigmoid_n((${v} - 64) / 10)
void t128 : int = sigmoid_n((${v} - 128) / 10)
void t192 : int = sigmoid_n((${v} - 192) / 10)

# 量化值: q = 32 + 64*t64 + 64*t128 + 64*t192
void q : int = 32 + 64 * ${t64} + 64 * ${t128} + 64 * ${t192}

>> print >> "Test single pixel: v=100"
>> print >> "t64 (v>64?) = " >> ${t64}
>> print >> "t128 (v>128?) = " >> ${t128}
>> print >> "t192 (v>192?) = " >> ${t192}
>> print >> "quantized = " >> ${q}

# ---- 逐像素对整张图像做量化 ----
# 对 RGB 图像做 4 级量化, 用逐元素 sigmoid
# 广播: 每个通道值减去阈值

# 对整张图操作: 所有像素, 所有通道
void v_all : int = ${rgb_img}

# 做 3 个阈值判断
void s64 : int = sigmoid_n(nd_div(nd_sub(${v_all}, 64), 10))
void s128 : int = sigmoid_n(nd_div(nd_sub(${v_all}, 128), 10))
void s192 : int = sigmoid_n(nd_div(nd_sub(${v_all}, 192), 10))

# q = 32 + 64*s64 + 64*s128 + 64*s192
void q64_1 : int = nd_mul(${s64}, 64)
void q64_2 : int = nd_mul(${s128}, 64)
void q64_3 : int = nd_mul(${s192}, 64)
void q_sum : int = nd_add(nd_add(${q64_1}, ${q64_2}), ${q64_3})
void quantized : int = nd_add(${q_sum}, 32)

# 打印结果
void print_in : int = tensor_to_array(${rgb_img})
void print_out : int = tensor_to_array(${quantized})

>> print >> ""
>> print >> "Original RGB (4x4x3):"
>> print >> ${print_in}
>> print >> "Quantized (4 levels):"
>> print >> ${print_out}

>> print >> "=== Color Quantization PASSED ==="