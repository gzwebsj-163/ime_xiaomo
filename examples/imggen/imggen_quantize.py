#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
imggen_quantize.py — int8 对称量化工具（可独立复用）

对称量化：对每个张量 T，计算
    scale = max(|T|) / 127
    T_int8 = round(T / scale)   （clip 到 [-127, 127]）
反量化：
    T_hat  = T_int8 * scale

API:
  quantize_tensor(T)      -> (T_int8_np, scale_float)
  dequantize_tensor(q, s) -> float ndarray
  psnr(a, b)              -> 峰值信噪比 (dB)，用于图像质量对比
  save_quantized(path, weights, scales)
  load_quantized(path)
"""
import json
import numpy as np


def quantize_tensor(T, bits=8):
    """对称 int8 量化: 返回 (quantized_int_list, scale)"""
    T = np.asarray(T, dtype=np.float64)
    amax = float(np.max(np.abs(T))) if T.size else 0.0
    if amax < 1e-30:                    # 全零张量
        return np.zeros(T.shape, dtype=np.int8), 1.0
    qmax = (2 ** (bits - 1)) - 1        # int8 -> 127
    scale = amax / qmax
    q = np.clip(np.round(T / scale), -qmax, qmax).astype(np.int8)
    return q, scale


def dequantize_tensor(q, scale):
    return np.asarray(q, dtype=np.float64) * scale


def psnr(a, b, data_range=1.0):
    """图像峰值信噪比: a,b 同形状 float, 范围 [0,1]"""
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    mse = float(np.mean((a - b) ** 2))
    if mse == 0:
        return float("inf")
    return 10.0 * np.log10((data_range ** 2) / mse)


def quantize_state_dict(state_dict, bits=8):
    """对 {权重名: ndarray} 逐个量化: 返回 {name: {"data": [...], "scale": s}}"""
    out = {}
    for k, T in state_dict.items():
        q, s = quantize_tensor(T, bits)
        out[k] = {"data": q.ravel().tolist(), "shape": list(T.shape),
                  "scale": s, "bits": bits}
    return out


def save_quantized(path, state_dict_quant, meta=None):
    payload = {"meta": meta or {}, "weights": state_dict_quant}
    with open(path, "w") as f:
        json.dump(payload, f, indent=2)
    return path


def load_quantized(path):
    with open(path, "r") as f:
        payload = json.load(f)
    return payload
