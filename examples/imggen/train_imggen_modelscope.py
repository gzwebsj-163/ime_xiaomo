#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
train_imggen_modelscope.py — 魔塔(ModelScope) Notebook：MLP 关键字→图片 int8 量化训练

功能（方案 R：隐式表示 + 超采样）：
  1. 从 imggen_config 程序化生成每个关键字的 128×128 目标图
  2. 训练条件 MLP： (onehot(关键字), u, v) -> RGB   （学习 坐标→颜色 的连续映射）
  3. int8 对称量化全部权重，导出 imggen_quantized.json（int8 + scale）
  4. 批量矩阵乘上采样到 1024×1024，重建每个关键字的图
  5. 对比 float32 vs int8 生成图的质量（PSNR）并输出 PNG

用法（魔塔 Notebook / 任意带 numpy 的环境）:
  python train_imggen_modelscope.py --keywords 山,海,星空 --size 1024 --bits 8 --train 96

说明：脚本优先用 PyTorch(可 GPU)，无 torch 时自动降级为 NumPy(CPU)。
  128×128 训练在 CPU 上几十秒~几分钟即可，1024 推理用向量化运算很快。
"""
import argparse
import json
import os
import time

import numpy as np

import imggen_config as cfg
import imggen_quantize as qz

try:
    import torch
    _HAS_TORCH = True
except Exception:
    _HAS_TORCH = False

try:
    from PIL import Image
    _HAS_PIL = True
except Exception:
    _HAS_PIL = False

SEED = 42


# ======================================================================
# 数据：为每个关键字生成 (训练坐标, 目标RGB)
# ======================================================================
def build_dataset(keywords, size):
    """
    返回:
      X_dict: { kw: (N, INPUT) }   INPUT = C(onehot) + 2(uv)
      Y_dict: { kw: (N, 3) }       目标 RGB (0..1)
    """
    C = len(keywords)
    kw2idx = {k: i for i, k in enumerate(keywords)}
    X_dict, Y_dict = {}, {}
    g = np.mgrid[0:size, 0:size]
    u = g[1] / (size - 1)
    v = 1.0 - g[0] / (size - 1)
    uv = np.stack([u.ravel(), v.ravel()], axis=1)     # (H*W, 2)
    for kw in keywords:
        rgb = cfg.build_target_grid(kw, cfg.SHAPES.get(kw, "ridge"), size)
        rgb = rgb.reshape(-1, 3)                      # (H*W, 3)
        onehot = np.zeros((rgb.shape[0], C))
        onehot[:, kw2idx[kw]] = 1.0
        X_dict[kw] = np.concatenate([onehot, uv], axis=1)
        Y_dict[kw] = rgb
    return X_dict, Y_dict, C, kw2idx


# ======================================================================
# MLP（纯 numpy 参数表示，便于统一 float/int8 前向）
# ======================================================================
def init_mlp(C, hidden, seed=SEED):
    rng = np.random.RandomState(seed)
    # 输入 = onehot(C) + u + v
    n_in = C + 2
    W1 = rng.randn(n_in, hidden) * np.sqrt(2.0 / n_in)
    b1 = np.zeros(hidden)
    W2 = rng.randn(hidden, 3) * np.sqrt(2.0 / hidden)
    # 输出用 sigmoid -> [0,1]
    return {"W1": W1, "b1": b1, "W2": W2, "b2": np.zeros(3)}


def forward(params, X):
    """X: (N, C+2). 返回 (N,3) 0..1。"""
    z1 = X @ params["W1"] + params["b1"]
    h = np.tanh(z1)
    z2 = h @ params["W2"] + params["b2"]
    return 1.0 / (1.0 + np.exp(-z2))     # sigmoid


def train_numpy(params, X_all, Y_all, epochs, lr):
    """批量全梯度下降（累计所有关键字样本）。"""
    X = np.concatenate(list(X_all.values()), axis=0)
    Y = np.concatenate(list(Y_all.values()), axis=0)
    N = X.shape[0]
    mom = {k: np.zeros_like(v) for k, v in params.items()}
    report = []
    for ep in range(epochs):
        # 前向
        z1 = X @ params["W1"] + params["b1"]
        h = np.tanh(z1)
        z2 = h @ params["W2"] + params["b2"]
        out = 1.0 / (1.0 + np.exp(-z2))
        # MSE 损失
        dout = 2.0 * (out - Y) / N
        dz2 = dout * out * (1 - out)
        dW2 = h.T @ dz2
        db2 = dz2.sum(axis=0)
        dh = dz2 @ params["W2"].T
        dz1 = dh * (1 - h * h)
        dW1 = X.T @ dz1
        db1 = dz1.sum(axis=0)
        # SGD + 动量
        for k in ("W1", "b1", "W2", "b2"):
            g = {"W1": dW1, "b1": db1, "W2": dW2, "b2": db2}[k]
            mom[k] = 0.9 * mom[k] + lr * g
            params[k] -= mom[k]
        loss = float(np.mean((out - Y) ** 2))
        report.append(loss)
        if ep % max(1, epochs // 5) == 0 or ep == epochs - 1:
            print(f"  [epoch {ep+1}/{epochs}] loss={loss:.6f}")
    return params, report


def train_torch(keywords, X_dict, Y_dict, C, hidden, epochs, lr, device):
    """PyTorch GPU 训练，返回 numpy 参数。"""
    X = np.concatenate(list(X_dict.values()), axis=0)
    Y = np.concatenate(list(Y_dict.values()), axis=0)
    Xt = torch.from_numpy(X).float().to(device)
    Yt = torch.from_numpy(Y).float().to(device)
    n_in = X.shape[1]
    torch.manual_seed(SEED)
    model = torch.nn.Sequential(
        torch.nn.Linear(n_in, hidden), torch.nn.Tanh(),
        torch.nn.Linear(hidden, 3), torch.nn.Sigmoid(),
    ).to(device)
    opt = torch.optim.SGD(model.parameters(), lr=lr, momentum=0.9)
    for ep in range(epochs):
        opt.zero_grad()
        out = model(Xt)
        loss = torch.mean((out - Yt) ** 2)
        loss.backward()
        opt.step()
        if ep % max(1, epochs // 5) == 0 or ep == epochs - 1:
            print(f"  [epoch {ep+1}/{epochs}] loss={loss.item():.6f}")
    # 取出参数
    sd = model.state_dict()
    return {
        "W1": sd["0.weight"].detach().cpu().numpy().T,
        "b1": sd["0.bias"].detach().cpu().numpy(),
        "W2": sd["3.weight"].detach().cpu().numpy().T,
        "b2": sd["3.bias"].detach().cpu().numpy(),
    }


# ======================================================================
# 推理到任意分辨率（批量向量化，GPU 或 numpy）
# ======================================================================
def render(params, kw, kw2idx, C, size, device=None):
    """对一个关键字渲染 size×size 图片。返回 (H,W,3) float 0..1。"""
    g = np.mgrid[0:size, 0:size]
    u = g[1] / (size - 1)
    v = 1.0 - g[0] / (size - 1)
    N = size * size
    uv = np.stack([u.ravel(), v.ravel()], axis=1)
    onehot = np.zeros((N, C))
    onehot[:, kw2idx[kw]] = 1.0
    X = np.concatenate([onehot, uv], axis=1)
    if _HAS_TORCH and device is not None:
        from torch import tensor as T
        Xt = T(X).float().to(device)
        # 用 numpy 前向比较麻烦，直接走 float 计算后回 CPU
        # 这里统一走 numpy 前向（渲染阶段对性能不敏感，1024 也秒级）
        pass
    out = forward(params, X)
    return out.reshape(size, size, 3)


def quantize_and_render(params, C, kw2idx, keywords, size):
    """对 params 做 int8 量化，返回 (int8 生成图 dict, 量化权重 dict, 误差 dict)。"""
    q_state = qz.quantize_state_dict(params, bits=BITS)
    dq_params = {}
    for k, item in q_state.items():
        qarr = np.array(item["data"], dtype=np.float64).reshape(item["shape"])
        dq_params[k] = qarr * item["scale"]
    imgs_q = {}
    for kw in keywords:
        imgs_q[kw] = render(dq_params, kw, kw2idx, C, size)
    return imgs_q, q_state


# ======================================================================
def save_png(img, path):
    """img: (H,W,3) float 0..1 -> PNG"""
    arr = np.clip(img, 0, 1) * 255.0
    arr = np.clip(np.round(arr), 0, 255).astype(np.uint8)
    if _HAS_PIL:
        Image.fromarray(arr, "RGB").save(path)
        return True
    # 无 PIL 时写 PPM（兼容）
    with open(path.replace(".png", ".ppm"), "wb") as f:
        H, W, _ = arr.shape
        f.write(f"P6\n{W} {H}\n255\n".encode())
        f.write(arr.tobytes())
    return False


def main():
    global BITS
    ap = argparse.ArgumentParser()
    ap.add_argument("--keywords", default="山,海,星空,网格,火焰",
                    help="逗号分隔的关键字（可任意自定义）")
    ap.add_argument("--size", type=int, default=cfg.EXPORT_SIZE,
                    help="导出图片分辨率（>=1024 即可），默认 1024")
    ap.add_argument("--bits", type=int, default=8, help="量化位宽，默认 8 (int8)")
    ap.add_argument("--train", type=int, default=180, help="训练轮数")
    ap.add_argument("--hidden", type=int, default=64, help="隐层宽度")
    ap.add_argument("--lr", type=float, default=0.6, help="学习率")
    ap.add_argument("--outdir", default="output", help="输出目录")
    args = ap.parse_args()

    global BITS
    BITS = args.bits
    keywords = [k.strip() for k in args.keywords.split(",") if k.strip()]
    assert len(keywords) >= 1, "至少一个关键字"
    os.makedirs(args.outdir, exist_ok=True)

    # 训练分辨率
    TR = cfg.TRAIN_SIZE
    print(f"[config] 关键字={keywords}  训练分辨率={TR}×{TR}  导出={args.size}×{args.size}  "
          f"位宽={args.bits}bit  隐层={args.hidden}  轮数={args.train}")
    print(f"[env] torch={_HAS_TORCH}, cpu_only={not _HAS_TORCH or not torch.cuda.is_available()}")

    # 1. 数据
    X_dict, Y_dict, C, kw2idx = build_dataset(keywords, TR)

    # 2. 训练
    t0 = time.time()
    if _HAS_TORCH and torch.cuda.is_available():
        print("[train] 使用 PyTorch + CUDA GPU")
        device = torch.device("cuda")
        params = train_torch(keywords, X_dict, Y_dict, C, args.hidden,
                             args.train, args.lr, device)
    else:
        print("[train] 使用 NumPy (CPU，128×128 仍很快)")
        params = init_mlp(C, args.hidden)
        params, _loss_hist = train_numpy(params, X_dict, Y_dict, args.train, args.lr)
    print(f"[train] 完成，耗时 {time.time()-t0:.1f}s")

    # 3. float32 渲染到目标分辨率
    print(f"[render] float32 生成 {args.size}×{args.size}...")
    imgs_f = {kw: render(params, kw, kw2idx, C, args.size) for kw in keywords}

    # 4. int8 量化 + 渲染
    print(f"[quant] int8 量化权重并重新生成...")
    imgs_q, q_state = quantize_and_render(params, C, kw2idx, keywords, args.size)

    # 5. 保存图 + 量化权重 + PSNR
    qpath = os.path.join(args.outdir, "imggen_quantized.json")
    qz.save_quantized(qpath, q_state,
                      meta={"keywords": keywords, "size": args.size,
                            "bits": args.bits, "hidden": args.hidden,
                            "train_res": TR})
    print(f"[save] 量化权重 -> {qpath}")

    print("\n=== 每类 1024×1024 生成结果 vs int8 (PSNR) ===")
    for kw in keywords:
        save_png(imgs_f[kw], os.path.join(args.outdir, f"{kw}_float32.png"))
        save_png(imgs_q[kw], os.path.join(args.outdir, f"{kw}_int8.png"))
        p = qz.psnr(imgs_f[kw], imgs_q[kw])
        print(f"  {kw:<6} PSNR(int8 vs float32) = {p if p==float('inf') else round(p,2)} dB")
    print(f"\n[out] 图片已保存到 {args.outdir}/  （每关键字 *_float32.png / *_int8.png）")


if __name__ == "__main__":
    main()
