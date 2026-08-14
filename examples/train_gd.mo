# xiaomo "造模型" 核心演示: 用 .mo 语言直接写训练循环 (手写 SGD 反向传播)
# 用法: ./xiaomo run examples/train_gd.mo
#
# 说明: xiaomo 内核提供 前向(matmul/tanh/softmax) + 损失(loss_crossentropy)
#       + 梯度(diff/grad_tanh/matmul) + 更新(diff/scale) 全套算子,
#       .mo 里用 while 循环 + 变量重声明实现迭代梯度下降, 真正"在 VM 内训练"。
#
# 数据: 4 个样本 2 类 (已简单处理), 网络 2->4(tanh)->2(softmax)
#   x = [1,0],[0,1],[0.9,0.1],[0.1,0.9] ; 真值 one-hot
#   (前两组偏类0, 后两组偏类1)
# 学习率 0.3, 跑 60 轮, 每 15 轮打印一次平均损失。
# 预期: 损失单调下降, 最终能正确区分两类。

# ---- 训练数据 (4 样本, 已标准化到[0,1]附近) ----
void x0 : int = [1.0, 0.0]
void y0 : int = [1.0, 0.0]
void x1 : int = [0.9, 0.1]
void y1 : int = [1.0, 0.0]
void x2 : int = [0.0, 1.0]
void y2 : int = [0.0, 1.0]
void x3 : int = [0.1, 0.9]
void y3 : int = [0.0, 1.0]

# ---- 初始权重 (随机小值) ----
void W1 : int = [
  [0.3, -0.2, 0.1, 0.4],
  [-0.1, 0.2, -0.3, 0.1]
]
void b1 : int = [0.0, 0.0, 0.0, 0.0]
void W2 : int = [
  [0.2, -0.3],
  [-0.4, 0.5],
  [0.3, -0.2],
  [-0.5, 0.4]
]
void b2 : int = [0.0, 0.0]
void lr : int = 0.3

# ---- 工具: 对 (x,y) 计算损失 & 更新权重 (SGD 一步) ----
# 由于 .mo 有 4 个样本, 用一个 if/else 分发; 每轮对 4 个样本各更新一次。
# 这里用"参数重声明 + 序号选择"实现 (省略, 直接在每轮里展开 4 次)。

# ---- 训练循环 ----  
void epoch : int = 0
while ${epoch} < 60:
    # --- 样本0 ---
    void z1_0 : int = matmul(${x0}, ${W1})
    void a1_0 : int = bias_add(${z1_0}, ${b1})
    void h0   : int = tanh(${a1_0})
    void z2_0 : int = matmul(${h0}, ${W2})
    void a2_0 : int = bias_add(${z2_0}, ${b2})
    void p0   : int = softmax(${a2_0})
    void dz2_0 : int = diff(${p0}, ${y0})
    void hT0  : int = transpose(${h0})
    void dW2_0 : int = matmul(${hT0}, ${dz2_0})
    void W2T0  : int = transpose(${W2})
    void dh0   : int = matmul(${dz2_0}, ${W2T0})
    void gth0  : int = grad_tanh(${h0})
    void dz1_0 : int = hadamard(${dh0}, ${gth0})
    void xT0   : int = transpose(${x0})
    void dW1_0 : int = matmul(${xT0}, ${dz1_0})
    # 更新
    void uW1_0 : int = scale(${dW1_0}, ${lr})
    void nW1 : int = diff(${W1}, ${uW1_0})
    void uW2_0 : int = scale(${dW2_0}, ${lr})
    void nW2 : int = diff(${W2}, ${uW2_0})
    void W1 : int = ${nW1}
    void W2 : int = ${nW2}

    # --- 样本1 ---
    void z1_1 : int = matmul(${x1}, ${W1})
    void a1_1 : int = bias_add(${z1_1}, ${b1})
    void h1   : int = tanh(${a1_1})
    void z2_1 : int = matmul(${h1}, ${W2})
    void a2_1 : int = bias_add(${z2_1}, ${b2})
    void p1   : int = softmax(${a2_1})
    void dz2_1 : int = diff(${p1}, ${y1})
    void hT1  : int = transpose(${h1})
    void dW2_1 : int = matmul(${hT1}, ${dz2_1})
    void W2T1  : int = transpose(${W2})
    void dh1   : int = matmul(${dz2_1}, ${W2T1})
    void gth1  : int = grad_tanh(${h1})
    void dz1_1 : int = hadamard(${dh1}, ${gth1})
    void xT1   : int = transpose(${x1})
    void dW1_1 : int = matmul(${xT1}, ${dz1_1})
    void uW1_1 : int = scale(${dW1_1}, ${lr})
    void nW1b  : int = diff(${W1}, ${uW1_1})
    void uW2_1 : int = scale(${dW2_1}, ${lr})
    void nW2b  : int = diff(${W2}, ${uW2_1})
    void W1 : int = ${nW1b}
    void W2 : int = ${nW2b}

    # --- 样本2 ---
    void z1_2 : int = matmul(${x2}, ${W1})
    void a1_2 : int = bias_add(${z1_2}, ${b1})
    void h2   : int = tanh(${a1_2})
    void z2_2 : int = matmul(${h2}, ${W2})
    void a2_2 : int = bias_add(${z2_2}, ${b2})
    void p2   : int = softmax(${a2_2})
    void dz2_2 : int = diff(${p2}, ${y2})
    void hT2  : int = transpose(${h2})
    void dW2_2 : int = matmul(${hT2}, ${dz2_2})
    void W2T2  : int = transpose(${W2})
    void dh2   : int = matmul(${dz2_2}, ${W2T2})
    void gth2  : int = grad_tanh(${h2})
    void dz1_2 : int = hadamard(${dh2}, ${gth2})
    void xT2   : int = transpose(${x2})
    void dW1_2 : int = matmul(${xT2}, ${dz1_2})
    void uW1_2 : int = scale(${dW1_2}, ${lr})
    void nW1c  : int = diff(${W1}, ${uW1_2})
    void uW2_2 : int = scale(${dW2_2}, ${lr})
    void nW2c  : int = diff(${W2}, ${uW2_2})
    void W1 : int = ${nW1c}
    void W2 : int = ${nW2c}

    # --- 样本3 ---
    void z1_3 : int = matmul(${x3}, ${W1})
    void a1_3 : int = bias_add(${z1_3}, ${b1})
    void h3   : int = tanh(${a1_3})
    void z2_3 : int = matmul(${h3}, ${W2})
    void a2_3 : int = bias_add(${z2_3}, ${b2})
    void p3   : int = softmax(${a2_3})
    void dz2_3 : int = diff(${p3}, ${y3})
    void hT3  : int = transpose(${h3})
    void dW2_3 : int = matmul(${hT3}, ${dz2_3})
    void W2T3  : int = transpose(${W2})
    void dh3   : int = matmul(${dz2_3}, ${W2T3})
    void gth3  : int = grad_tanh(${h3})
    void dz1_3 : int = hadamard(${dh3}, ${gth3})
    void xT3   : int = transpose(${x3})
    void dW1_3 : int = matmul(${xT3}, ${dz1_3})
    void uW1_3 : int = scale(${dW1_3}, ${lr})
    void nW1d  : int = diff(${W1}, ${uW1_3})
    void uW2_3 : int = scale(${dW2_3}, ${lr})
    void nW2d  : int = diff(${W2}, ${uW2_3})
    void W1 : int = ${nW1d}
    void W2 : int = ${nW2d}

    # --- 计算当前平均损失 (4 样本) ---
    void l0 : int = loss_crossentropy(${y0}, ${p0})
    void l1 : int = loss_crossentropy(${y1}, ${p1})
    void l2 : int = loss_crossentropy(${y2}, ${p2})
    void l3 : int = loss_crossentropy(${y3}, ${p3})
    void l01 : int = ${l0} + ${l1}
    void l23 : int = ${l2} + ${l3}
    void lsum : int = ${l01} + ${l23}
    void lavg : int = ${lsum} / 4

    >> print >> "epoch " >> ${epoch} >> "  loss = " >> ${lavg}
    void epoch : int = ${epoch} + 1

# ---- 训练后: 对新样本预测 (用训练好的 W1/W2) ----
void t1 : int = [0.8, 0.2]
void tz1 : int = matmul(${t1}, ${W1})
void ta1 : int = bias_add(${tz1}, ${b1})
void th  : int = tanh(${ta1})
void tz2 : int = matmul(${th}, ${W2})
void ta2 : int = bias_add(${tz2}, ${b2})
void tp  : int = softmax(${ta2})
>> print >> "预测 [0.8,0.2] (应归类0) -> " >> ${tp}

void t2 : int = [0.2, 0.8]
void tz1b : int = matmul(${t2}, ${W1})
void ta1b : int = bias_add(${tz1b}, ${b1})
void thb  : int = tanh(${ta1b})
void tz2b : int = matmul(${thb}, ${W2})
void ta2b : int = bias_add(${tz2b}, ${b2})
void tpb  : int = softmax(${ta2b})
>> print >> "预测 [0.2,0.8] (应归类1) -> " >> ${tpb}
