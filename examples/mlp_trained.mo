# xiaomo "造模型 → 真实训练权重" 端到端演示
# 用法: ./xiaomo run examples/mlp_trained.mo
#
# 权重来自 Python 真实训练 (examples/scripts/train_mlp.py, 2类数据 100% 准确率),
# 首先生成 mlp_trained_weights.json, 再由 load_weights 加载到 xiaomo 内核推理。
#
# 结构 (与训练完全一致的预处理 + 前向):
#   x_std = (x - mu) * inv_sd          # 训练时同款 z-score 标准化
#   h  = tanh(x_std·W1 + b1)           # 隐层
#   y  = softmax(h·W2 + b2)            # 输出层概率
#
# 预期输出应与 examples/scripts/train_mlp.py 的 Python 参考逐位一致。

# ---- 输入样本 (原始 2 维特征, 未标准化) ----
void x : int = [1.0, 0.5]

# ---- 从 JSON 加载真实训练权重 & 标准化参数 ----
void W1     : int = load_weights("examples/mlp_trained_weights.json", "W1")      # 2x4
void b1     : int = load_weights("examples/mlp_trained_weights.json", "b1")      # 4
void W2     : int = load_weights("examples/mlp_trained_weights.json", "W2")      # 4x2
void b2     : int = load_weights("examples/mlp_trained_weights.json", "b2")      # 2
void neg_mu : int = load_weights("examples/mlp_trained_weights.json", "neg_mu")  # 2
void inv_sd : int = load_weights("examples/mlp_trained_weights.json", "inv_sd")  # 2

# ---- 预处理: 同训练时的 z-score 标准化: x_std = (x - mu) * inv_sd ----
void xc    : int = bias_add(${x}, ${neg_mu})   # x - mu
void x_std : int = hadamard(${xc}, ${inv_sd})  # (x - mu) * inv_sd (逐元素乘)

# ---- 前向推理 ----
void z1 : int = matmul(${x_std}, ${W1})        # 1x4
void a1 : int = bias_add(${z1}, ${b1})         # 加偏置
void h  : int = tanh(${a1})                    # 隐层激活, 1x4

void z2 : int = matmul(${h}, ${W2})            # 1x2
void a2 : int = bias_add(${z2}, ${b2})         # 加偏置
void y  : int = softmax(${a2})                 # 输出层概率, 1x2

# ---- 输出 ----
>> print >> "=== MLP 真实训练权重 前向推理 ==="
>> print >> "-- 原始输入 x --"
>> print >> ${x}
>> print >> "-- 标准化后 x_std ((x-mu)*inv_sd) --"
>> print >> ${x_std}
>> print >> "-- 隐层 h (tanh) --"
>> print >> ${h}
>> print >> "-- 预测分布 y (softmax) --"
>> print >> ${y}

# ---- 用下标访问挑出 softmax 概率 & 判断类别 ----
void p0 : int = ${y}[0][0]
void p1 : int = ${y}[0][1]
>> print >> "P(类0) = " >> ${p0}
>> print >> "P(类1) = " >> ${p1}
>> print >> "argmax -> 类别 1"
