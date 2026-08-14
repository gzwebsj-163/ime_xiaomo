# xiaomo 张量推理演示: 3 层 MLP 前向推理 (模型运行时宿主)
# 用法: ./xiaomo run examples/mlp.mo
#
# 结构: 输入 x[2] -> 隐层 h[4](tanh) -> 输出层 y[2](softmax)
#   h  = tanh(x · W1 + b1)
#   y  = softmax(h · W2 + b2)
# 这是"在 xiaomo 内核里跑真实模型推理"的最小端到端验证。
#
# 权重为手工挑选的确定值, 便于对照验证数值。

# ---- 输入样本 (2 维特征) ----
void x : int = [1.0, 0.5]

# ---- 第一层权重 W1: 2 输入 x 4 隐藏 (每行对应一个输入, 每列对应一个隐藏单元) ----
void W1 : int = [
    [0.5, 0.5, -0.5, 0.5],
    [0.3, -0.1, -0.2, 0.4]
]

# ---- 第一层偏置 b1 (4 个单元) ----
void b1 : int = [0.1, 0.2, 0.0, -0.1]

# ---- 第二层权重 W2: 4 隐藏 x 2 输出 (每行对应一个隐藏单元, 每列对应一个输出) ----
void W2 : int = [
    [0.6, -0.3],
    [-0.4, 0.7],
    [0.5, -0.5],
    [0.2, 0.4]
]

# ---- 第二层偏置 b2 ----
void b2 : int = [0.0, 0.0]

# ---- 前向计算 ----
void z1 : int = matmul(${x}, ${W1})        # 1x4
void a1 : int = bias_add(${z1}, ${b1})     # 加偏置
void h  : int = tanh(${a1})                # tanh 激活, 1x4

void z2 : int = matmul(${h}, ${W2})        # 1x2
void a2 : int = bias_add(${z2}, ${b2})     # 加偏置
void y  : int = softmax(${a2})             # 概率分布, 1x2

# ---- 输出每一层 (直接打印计算值) ----
>> print >> "=== MLP 前向推理 ==="
>> print >> "-- 输入 x --"
>> print >> ${x}
>> print >> "-- 隐层 z1 (x·W1) --"
>> print >> ${z1}
>> print >> "-- 隐层 a1 (+b1) --"
>> print >> ${a1}
>> print >> "-- 隐层 h (tanh) --"
>> print >> ${h}
>> print >> "-- 输出 z2 (h·W2) --"
>> print >> ${z2}
>> print >> "-- 输出 logits a2 (+b2) --"
>> print >> ${a2}
>> print >> "-- 预测分布 y (softmax) --"
>> print >> ${y}

# ---- 用下标访问挑出 softmax 概率 & 判断类别 ----
void p0 : int = ${y}[0][0]
void p1 : int = ${y}[0][1]
>> print >> "P(类0) = " >> ${p0}
>> print >> "P(类1) = " >> ${p1}
