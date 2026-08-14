# xiaomo "造模型 → 用真实权重" 演示: 从 JSON 权重文件加载参数跑 MLP 前向推理
# 用法: ./xiaomo run examples/mlp_loaded.mo
#
# 与 mlp.mo 的区别: 权重不硬编码在源码里, 而是通过 load_weights() 从外部
# JSON 权重文件 (examples/mlp_weights.json) 读取 —— 这正是"接真实训练/导出
# 权重跑推理"的标准路径。
#
# 结构: 输入 x[2] -> 隐层 h[4](tanh) -> 输出层 y[2](softmax)
#   W1/b1/W2/b2 全部来自 mlp_weights.json
# 预期输出与 mlp.mo 硬编码版逐位一致。

# ---- 输入样本 (2 维特征) ----
void x : int = [1.0, 0.5]

# ---- 从 JSON 权重文件加载各层参数 ----
void W1 : int = load_weights("examples/mlp_weights.json", "W1")   # 2x4
void b1 : int = load_weights("examples/mlp_weights.json", "b1")   # 4
void W2 : int = load_weights("examples/mlp_weights.json", "W2")   # 4x2
void b2 : int = load_weights("examples/mlp_weights.json", "b2")   # 2

# ---- 前向计算 ---- 
void z1 : int = matmul(${x}, ${W1})        # 1x4
void a1 : int = bias_add(${z1}, ${b1})     # 加偏置
void h  : int = tanh(${a1})                # tanh 激活, 1x4

void z2 : int = matmul(${h}, ${W2})        # 1x2
void a2 : int = bias_add(${z2}, ${b2})     # 加偏置
void y  : int = softmax(${a2})             # 概率分布, 1x2

# ---- 输出每一层 ----
>> print >> "=== MLP (权重来自外部 JSON) 前向推理 ==="
>> print >> "-- 输入 x --"
>> print >> ${x}
>> print >> "-- 隐层 z1 (x·W1) --"
>> print >> ${z1}
>> print >> "-- 隐层 a1 (+b1) --"
>> print >> ${a1}
>> print >> "-- 隐层 h (tanh) --"
>> print >> ${h}
>> print >> "-- 输出 logits a2 (+b2) --"
>> print >> ${a2}
>> print >> "-- 预测分布 y (softmax) --"
>> print >> ${y}

# ---- 用下标访问挑出 softmax 概率 & 判断类别 ----
void p0 : int = ${y}[0][0]
void p1 : int = ${y}[0][1]
>> print >> "P(类0) = " >> ${p0}
>> print >> "P(类1) = " >> ${p1}
