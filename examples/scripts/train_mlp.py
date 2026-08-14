#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xiaomo "造模型" 演示脚本: 训练一个真实 MLP 分类器并导出权重 JSON,
供 xiaomo 内核 (load_weights) 加载并复现前向推理。

架构与 examples/mlp_loaded.mo 完全一致:
  输入 x[2] -> 隐层 h[4](tanh) -> 输出 y[2](softmax)
  h = tanh(x·W1 + b1)
  y = softmax(h·W2 + b2)

产出:
  examples/mlp_trained_weights.json   (训练好的真实权重)
  examples/mlp_trained.mo            (xiaomo 加载该权重的推理脚本)
  examples/mlp_trained.py            (Python 参考实现, 用于对照)
"""
import json
import os
import numpy as np

np.random.seed(42)

# ---------- 造一份可分的数据集 (2 类, 每个样本 2 维特征) ----------
# 类0 聚集在左下, 类1 聚集在右上, 加入噪声使其线性不可分 (需隐层非线性)
n_per = 200
cls0 = np.random.randn(n_per, 2) * 0.6 + np.array([-1.2, -1.2])
cls1 = np.random.randn(n_per, 2) * 0.6 + np.array([ 1.2,  1.2])
X = np.vstack([cls0, cls1])
y = np.array([0] * n_per + [1] * n_per)

# 标准化 (保存均值/方差, 以便新样本按同样方式预处理)
mu = X.mean(axis=0); sd = X.std(axis=0)
X = (X - mu) / sd

# ---------- 训练 MLP (手动反向传播 SGD) ----------
n_in, n_hid, n_out = 2, 4, 2
lr = 1.0
W1 = np.random.randn(n_in, n_hid) * 0.5
b1 = np.zeros(n_hid)
W2 = np.random.randn(n_hid, n_out) * 0.5
b2 = np.zeros(n_out)

def softmax(a):
    e = np.exp(a - a.max(axis=1, keepdims=True))
    return e / e.sum(axis=1, keepdims=True)

for epoch in range(2000):
    z1 = X @ W1 + b1
    h = np.tanh(z1)
    z2 = h @ W2 + b2
    p = softmax(z2)
    # 交叉熵损失梯度 (p - onehot)
    Y = np.eye(n_out)[y]
    dz2 = p - Y
    dW2 = h.T @ dz2
    db2 = dz2.sum(axis=0)
    dh = dz2 @ W2.T
    dz1 = dh * (1 - h * h)          # tanh 导数
    dW1 = X.T @ dz1
    db1 = dz1.sum(axis=0)
    W2 -= lr * dW2 / len(X)
    b2 -= lr * db2 / len(X)
    W1 -= lr * dW1 / len(X)
    b1 -= lr * db1 / len(X)

# 训练精度
H = np.tanh(X @ W1 + b1)
preds = np.argmax(softmax(H @ W2 + b2), axis=1)
acc = (preds == y).mean()
print(f"[train] 准确率 = {acc*100:.1f}%")

# ---------- 导出权重为 JSON (xiaomo 可直接加载) ----------
def tolist(a):
    return [float(x) for x in np.asarray(a).ravel().tolist()]

# 同时导出标准化参数, 使 xiaomo 推理脚本能做与训练时相同的前处理
# (bias_add 只做加法, 故直接存 -mu 便于 x_std = bias_add(x, -mu) 后再乘 inv_sd)
inv_sd = 1.0 / sd
weights = {
    "W1": [tolist(W1[i]) for i in range(n_in)],   # 2x4
    "b1": tolist(b1),                              # 4
    "W2": [tolist(W2[i]) for i in range(n_hid)],   # 4x2
    "b2": tolist(b2),                              # 2
    "neg_mu":  tolist(-mu),
    "inv_sd": tolist(inv_sd),
}
print(f"[param] neg_mu={[round(v,6) for v in -mu]}, inv_sd={[round(v,6) for v in inv_sd]}")
out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "mlp_trained_weights.json")
out_path = os.path.normpath(out_path)
with open(out_path, "w") as f:
    json.dump(weights, f, indent=2)
print(f"[save] 权重已导出 -> {out_path}")

# ---------- Python 参考: 对给定样本复现前向推理用于对照 ----------
def forward(x):
    x = (np.asarray(x, dtype=float) - mu) / sd    # 同标准化
    h = np.tanh(x @ W1 + b1)
    p = softmax(np.atleast_2d(h @ W2 + b2))[0]
    return h, p

for sx in [[1.0, 0.5], [-0.8, 0.2], [0.0, 0.0]]:
    h, p = forward(sx)
    print(f"[test] x={sx} -> h={[round(v,6) for v in np.asarray(h).ravel()]}, "
          f"softmax={[round(float(v),6) for v in p]}, class={int(np.argmax(p))}")
