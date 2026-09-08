#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
imggen_config.py — 关键字 → 目标图案 的隐式定义（程序化生成）

每个关键字对应一个 128×128 的 RGB 目标图，由坐标 (u,v)∈[0,1] 采样得到。
MLP 学习这个映射： (onehot(keyword), u, v) -> RGB
放大到 1024 时，只需对更密的网格采样同一映射即可，天然平滑。
"""
import numpy as np

# ---------- 训练分辨率（低维训练，GPU 高效） ----------
TRAIN_SIZE = 128
# ---------- 推理/导出分辨率（超采样放大到高清） ----------
EXPORT_SIZE = 1024

# ---------- 默认关键字与图案类型 ----------
# 每个关键字一个图案谱：{x 依赖}、{y 依赖}、{半径依赖}、{色板}
PALETTES = {
    # (底, 亮部) 颜色对，用于插值/混合
    "山":      ((0.05, 0.08, 0.12), (0.45, 0.65, 0.85)),   # 夜色山峦蓝
    "海":      ((0.02, 0.28, 0.40), (0.35, 0.85, 0.95)),   # 深海青
    "星空":    ((0.01, 0.01, 0.05), (1.00, 0.95, 0.70)),   # 夜空+星光
    "网格":    ((0.08, 0.08, 0.12), (0.20, 0.90, 0.50)),   # 绿网格
    "火焰":    ((0.10, 0.02, 0.00), (1.00, 0.55, 0.10)),   # 红橙火焰
    "卡通":    ((0.55, 0.80, 0.95), (0.30, 0.75, 0.30)),   # 卡通扁平插画(占位)
}

# 每个关键字使用的“隐式图案函数族”
#   shape_type: 决定 (u,v) 怎么组合成标量特征（用于混合色板）
#   layer:      多层叠加以增加细节
SHAPES = {
    "山":   "ridge",    # 山脊: 多频正弦叠加 -> 高度场
    "海":   "wave",     # 波浪: 行进波
    "星空": "stars",    # 星点: 伪随机散点
    "网格": "grid",     # 网格: 锯齿/棋盘
    "火焰": "flame",    # 火焰: 波形+垂直渐变
    "卡通": "cartoon",  # 卡通扁平插画: 蓝天+太阳+白云+草地
}


def _fade(t):
    return t * t * t * (t * (t * 6 - 15) + 10)   # smoothstep


def make_pattern(shape_type, u, v):
    """u,v: [0,1] 网格(H,W)。返回标量特征图（用于混合色板），范围约 [0,1]。"""
    if shape_type == "ridge":
        # 山脊高度场：多频正弦
        h = (np.sin(u * 8 * np.pi) * 0.5 + 0.5) * 0.4 \
          + (np.sin(u * 23 * np.pi + 1.7) * 0.5 + 0.5) * 0.3 \
          + (np.sin(u * 51 * np.pi + 0.7) * 0.5 + 0.5) * 0.3
        # 高度决定是否“露出”亮部色（越靠山脊越亮）
        return np.clip(h * (1 - v) * 2.2, 0, 1)
    if shape_type == "wave":
        ph = np.sin(u * 12 * np.pi + v * 30) * 0.5 + 0.5
        crest = np.abs(np.cos(u * 20 * np.pi + v * 6)) 
        return np.clip(0.6 * ph + 0.4 * crest, 0, 1) * (1 - v * 0.5)
    if shape_type == "stars":
        # 伪随机星点：高频量化 + 抖动
        ru = np.floor(u * 48)
        rv = np.floor(v * 48)
        seed = (ru * 377.0 + rv * 129.0)
        rnd = np.abs(np.sin(seed) * 43758.5453) % 1.0
        star = (rnd > 0.94).astype(np.float64) * (rnd - 0.94) / 0.06
        # 银河渐变底色
        bg = _fade(np.clip((u - 0.5) * 4 + 0.5, 0, 1)) * (1 - v) * 0.6
        return np.clip(bg + star * 1.2, 0, 1)
    if shape_type == "grid":
        fu = _fade(np.abs((u * 16) % 1 - 0.5) * 4)      # 竖栅栏
        fv = _fade(np.abs((v * 16) % 1 - 0.5) * 4)      # 横栅栏
        m = np.maximum(fu, fv)
        # 加深在线密集处
        return np.clip(m * 1.4, 0, 1)
    if shape_type == "flame":
        w = np.sin(u * 10 * np.pi + v * 15) * 0.5 + 0.5
        body = np.clip(1 - v * 2.0 + 0.3 * w, 0, 1)      # 底部亮、顶部暗
        core = np.clip(1 - ((v - 0.3) / 0.5) ** 2, 0, 1) # 内部高温核
        return np.clip(core * 0.8 + body * 0.4, 0, 1)
    # 默认：平滑渐变
    return (u + v) / 2


def _cartoon_scene(u, v):
    """卡通扁平插画场景：蓝天 + 太阳 + 白云 + 草地。
    u:(H,W) 右增; v:(H,W) 上高下低(顶部=1, 底部=0)。
    返回 (H,W,3) RGB 0..1。"""
    H, W = u.shape
    rgb = np.zeros((H, W, 3))
    # ---- 渐变蓝天 (上部浅蓝, 地平线附近更浅) ----
    sky = np.clip(0.60 + 0.15 * v, 0, 1)          # 顶部亮
    rgb[..., 0] = 0.45 * sky + 0.55               # 蓝分量偏强
    rgb[..., 1] = 0.72 * sky + 0.22
    rgb[..., 2] = 0.95
    # ---- 绿色草地 (下半部): 平滑过渡地平线 ----
    grass_zone = 1.0 / (1.0 + np.exp(-(0.25 - v) * 28))   # v<0.22 草地,sigmoid 平滑
    grass = np.array([0.42, 0.75, 0.30])
    rgb = rgb * (1 - grass_zone[..., None]) + grass[None, None, :] * grass_zone[..., None]
    # ---- 太阳: 右上角圆 + 光晕 (亮黄) ----
    sun_c = np.array([0.78, 0.75])                 # 中心 (u,v)
    dc = np.sqrt((u - sun_c[0]) ** 2 + (v - sun_c[1]) ** 2)
    sun_body = 1.0 / (1.0 + np.exp((dc - 0.07) * 220))    # 实心圆
    sun_glow = 1.0 / (1.0 + np.exp((dc - 0.12) * 160))    # 柔光晕
    sun_col = np.array([1.00, 0.90, 0.35])
    sun_cool = np.array([0.98, 0.80, 0.30])
    rgb = rgb * (1 - sun_glow[..., None]) + sun_cool[None, None, :] * sun_glow[..., None]
    rgb = rgb * (1 - sun_body[..., None]) + sun_col[None, None, :] * sun_body[..., None]
    # ---- 白云: 两朵 (u=0.28,0.72 高处) 椭圆堆积 ----
    for cx, cy, s in [(0.26, 0.72, 0.07), (0.74, 0.78, 0.055)]:
        dc = np.sqrt(((u - cx) / 1.6) ** 2 + ((v - cy)) ** 2)
        cloud = 1.0 / (1.0 + np.exp((dc - s) * 140))
        # 三个叠圆, 撑高白色块
        for dx in (-0.9, 0.0, 0.9):
            dcc = np.sqrt(((u - cx - dx * s * 0.6) / 1.6) ** 2 + ((v - cy) * 0.85) ** 2)
            cloud = np.maximum(cloud, 1.0 / (1.0 + np.exp((dcc - s) * 140)))
        white = np.array([1.0, 1.0, 1.0])
        rgb = rgb * (1 - cloud[..., None]) + white[None, None, :] * cloud[..., None]
    return np.clip(rgb, 0, 1)


def color_for(kw, shape_type, u, v):
    """返回 (H,W,3) RGB 特征图（约 0..1）。"""
    H, W = u.shape
    if shape_type == "cartoon":
        return _cartoon_scene(u, v)
    lo, hi = PALETTES.get(kw, ((0, 0, 0), (1, 1, 1)))
    lo = np.array(lo); hi = np.array(hi)
    feat = make_pattern(shape_type, u, v)                    # (H,W)
    feat3 = feat[..., None]                                   # (H,W,1)
    rgb = lo[None, None, :] * (1 - feat3) + hi[None, None, :] * feat3
    # 额外加一点横向渐变让图更有层次
    tint = (feat + 0.5 * u) / 1.5
    rgb = np.clip(rgb * tint[..., None], 0, 1)
    return rgb


def build_target_grid(kw, shape_type, size):
    """为关键字生成 size×size 的 RGB 目标图。返回 (H,W,3) float 0..1。"""
    g = np.mgrid[0:size, 0:size]
    u = g[1] / (size - 1)
    v = 1.0 - g[0] / (size - 1)     # 让 v 从上到下递减（顶部 v=1）
    return color_for(kw, shape_type, u, v)


def default_keywords():
    return list(PALETTES.keys())
