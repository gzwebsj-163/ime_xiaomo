# xiaomo 训练算子验证: 单样本反向传播, 与 numpy 参考逐位对照
# 用法: ./xiaomo run examples/train_step_test.mo
# npm 参考 (seed=7, 见 examples/scripts/train_step_ref.py):
#   h = [0.570344, -0.228354, 0.016186, -0.230695]
#   p = [0.509340, 0.490660]
#   loss_ce = 0.712003
#   dz2 = p - y = [0.509340, -0.509340]
#   dW2 = h.T @ dz2 = [0.290499, -0.290499, -0.116310, 0.116310, ...]
#   gth  = 1 - h^2 = [0.674708, 0.947854, 0.999738, 0.946780]

# ---- 输入与真值 (单样本, one-hot) ----
void x : int = [1.0, 0.5]
void y : int = [0.0, 1.0]

# ---- 权重 (numpy seed=7 初始值) ----
void W1 : int = [
    [0.845263, -0.232969, 0.016410, 0.203758],
    [-0.394462, 0.001033, -0.000445, -0.877362]
]
void b1 : int = [0.0, 0.0, 0.0, 0.0]
void W2 : int = [
    [0.508829, 0.300249],
    [-0.312714, -0.085774],
    [0.252650, -0.130678],
    [-0.121375, -0.726621]
]
void b2 : int = [0.0, 0.0]

# ---- 前向 ----
void z1 : int = matmul(${x}, ${W1})
void a1 : int = bias_add(${z1}, ${b1})
void h  : int = tanh(${a1})
void z2 : int = matmul(${h}, ${W2})
void a2 : int = bias_add(${z2}, ${b2})
void p  : int = softmax(${a2})

# ---- 损失 ----
void loss : int = loss_crossentropy(${y}, ${p})

# ---- 反向传播 (交叉熵 -> dz2 = p - y) ----
void dz2 : int = diff(${p}, ${y})

# dW2 = h^T @ dz2  (注意 h 是 1x4 行向量, 需转置成 4x1)
void hT   : int = transpose(${h})
void dW2  : int = matmul(${hT}, ${dz2})

# 隐藏层梯度: dh = dz2 @ W2^T ; dz1 = dh * (1-h^2)
void W2T   : int = transpose(${W2})
void dh    : int = matmul(${dz2}, ${W2T})
void gth   : int = grad_tanh(${h})       # 1 - h^2
void dz1   : int = hadamard(${dh}, ${gth})
void xT    : int = transpose(${x})
void dW1   : int = matmul(${xT}, ${dz1})

# ---- 输出 ----
>> print >> "=== 训练算子单步验证 ==="
>> print >> "h     = " >> ${h}
>> print >> "p     = " >> ${p}
>> print >> "loss  = " >> ${loss}
>> print >> "dz2   = " >> ${dz2}
>> print >> "gth   = " >> ${gth}
>> print >> "dW2   = " >> ${dW2}
>> print >> "dW1   = " >> ${dW1}
