#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
干净人像 3D 模型重建 (投票核心 → 浮雕立体 → marching cubes)
================================================================
针对"时间累积重影 + 背景残留"两大难看根因:
  1. 逐层去背景: 每帧去掉小连通域(背景杂物)
  2. 自动选物体: 多帧按质心对齐后**时间投票** —— 时有时无的手/西瓜/背景杂物
     投票低自动消失, 持续稳定的"人物核心"被保留(识别率提升)
  3. 去重影: 质心对齐消除镜头/人物位移导致的层间错位
  4. 立体化: 投票核心做距离变换 → 浮雕式厚度剖面(中心厚边缘薄)
  5. 网格: marching cubes + 顶点色=多帧颜色平均(真实纹理)
只输出 3D 模型, 不生成视频.

用法: /usr/local/bin/python3 build_person_clean.py [--thr 0.40] [--thick 10]
输出: person_clean.obj (带顶点色, Blender 可开) + 静态预览 png
"""
import argparse, math, os, sys
import numpy as np
from scipy import ndimage
from PIL import Image, ImageDraw

BASE   = os.path.dirname(os.path.abspath(__file__))
NPZ    = os.path.join(BASE, "video_bc3d_texture_vol.npz")
OUTOBJ = os.path.join(BASE, "person_clean.obj")
Hpad, Wpad = 140, 96


def align_core(vol, tex, bad=(0, 13, 14, 15, 24, 28, 29), min_area=20, mode="head"):
    """选好帧 → 去小连通域 → 对齐(质心/头部) → 投票 + 最亮帧颜色.
    返回 (votes, bright, good): 投票计数, 最亮贡献帧颜色, 选用帧."""
    T, H, W = vol.shape
    good = [t for t in range(T) if vol[t].any() and t not in bad]
    votes = np.zeros((Hpad, Wpad), dtype=np.float64)
    bright = np.zeros((Hpad, Wpad, 3), dtype=np.uint8)
    blum = np.zeros((Hpad, Wpad), dtype=float)   # 当前最亮
    for t in good:
        m = vol[t].copy()
        lab, nn = ndimage.label(m, structure=np.ones((3, 3)))
        if nn:
            sizes = ndimage.sum(m, lab, range(1, nn + 1))
            m = np.isin(lab, np.nonzero(sizes >= min_area)[0] + 1)
        ys, xs = np.nonzero(m)
        if len(ys) == 0:
            continue
        cy, cx = ys.mean(), xs.mean()
        if mode == "head":
            # 头部区域 = 上部 35% 高度, 对齐其质心 → 脸居中清晰
            ytop, ybot = ys.min(), ys.max()
            hy = int(ytop + (ybot - ytop) * 0.35)
            hys, hxs = np.nonzero(m[:hy + 1])
            if len(hys):
                cy, cx = hys.mean(), hxs.mean()
        dy, dx = int(round(Hpad / 2 - cy)), int(round(Wpad / 2 - cx))
        y0s, y1s = max(0, dy), min(Hpad, dy + H)
        x0s, x1s = max(0, dx), min(Wpad, dx + W)
        y0m, y1m = y0s - dy, y1s - dy
        x0m, x1m = x0s - dx, x1s - dx
        canvas = np.zeros((Hpad, Wpad), dtype=bool)
        canvas[y0s:y1s, x0s:x1s] = m[y0m:y1m, x0m:x1m]
        votes += canvas
        # 最亮帧颜色
        sub = tex[t][y0m:y1m, x0m:x1m]
        lum = sub.astype(float).sum(axis=-1)
        cand_b = np.zeros((Hpad, Wpad, 3), dtype=np.uint8)
        cand_b[y0s:y1s, x0s:x1s] = sub
        cand_l = np.zeros((Hpad, Wpad), dtype=float)
        cand_l[y0s:y1s, x0s:x1s] = lum
        upd = canvas & (cand_l > blum)
        if upd.any():
            bright[upd] = cand_b[upd]
            blum[upd] = cand_l[upd]
    return votes, bright, good


def relief_volume(core, color, max_thick=10):
    """投票核心 → 浮雕 3D 体素 + 颜色体素.
    距离变换决定厚度: 中心(远离边缘)厚, 边缘薄 → 凸透镜式立体感.
    返回 (vol, col): (Tz,H,W) bool, (Tz,H,W,3) uint8."""
    H, W = core.shape
    dt = ndimage.distance_transform_edt(core)          # 到背景距离
    dmax = max(dt.max(), 1)
    thick = np.zeros_like(core, dtype=int)
    thick[core] = np.clip(np.round(dt[core] / dmax * (max_thick - 1)) + 1, 1, max_thick)
    Tz = max_thick
    vol = np.zeros((Tz, H, W), dtype=bool)
    col = np.zeros((Tz, H, W, 3), dtype=np.uint8)
    yy, xx = np.nonzero(core)
    for y, x in zip(yy, xx):
        vol[:thick[y, x], y, x] = True
        col[:thick[y, x], y, x] = color[y, x]
    return vol, col


def mesh_obj(vol, col, path, smooth_iters=3, smooth_lambda=0.6):
    """marching cubes → 拉普拉斯平滑 → obj(带顶点色). 返回 (nvert,nface)."""
    from skimage import measure
    if int(vol.sum()) < 3:
        return None
    verts, faces, _, _ = measure.marching_cubes(vol, level=0.5)
    vi = np.clip(np.round(verts).astype(int), 0, np.array(vol.shape) - 1)
    cols = col[vi[:, 0], vi[:, 1], vi[:, 2]]
    # ---- 拉普拉斯平滑 (保持形状, 去像素块锯齿) ----
    if smooth_iters > 0:
        adj = [[] for _ in range(len(verts))]
        for f in faces:
            a, b, c = int(f[0]), int(f[1]), int(f[2])
            adj[a].append(b); adj[a].append(c)
            adj[b].append(a); adj[b].append(c)
            adj[c].append(a); adj[c].append(b)
        for _ in range(smooth_iters):
            nv = verts.copy()
            for i in range(len(verts)):
                nb = adj[i]
                if nb:
                    nv[i] = (1 - smooth_lambda) * verts[i] + \
                            smooth_lambda * np.mean(verts[nb], axis=0)
            verts = nv
    with open(path, "w") as f:
        f.write("# person_clean.obj  vote-core relief mesh (smoothed)\n")
        for v, c in zip(verts, cols):
            f.write(f"v {v[0]:.2f} {v[1]:.2f} {v[2]:.2f} "
                    f"{c[0]/255:.3f} {c[1]/255:.3f} {c[2]/255:.3f}\n")
        for ft in faces:
            f.write(f"f {ft[0]+1} {ft[1]+1} {ft[2]+1}\n")
    return len(verts), len(faces)


def render_previews(vol, col, outprefix):
    """静态多角度渲染 (仅 PNG, 不合成视频)."""
    from PIL import Image as _I, ImageDraw as _D
    Tz, H, W = vol.shape
    max_dim = max(Tz, H, W)
    VW = int(max_dim * 9) // 2 * 2
    VH = int(max_dim * 10) // 2 * 2
    off = (VW / 2.0, VH / 2.0 + max_dim * 0.15)
    scale = (VW * 0.62) / max_dim
    c = (W / 2.0, H / 2.0, Tz / 2.0)

    def proj(pts, va, ea):
        x = pts[:, 0] - c[0]; y = pts[:, 1] - c[1]; z = pts[:, 2] - c[2]
        ca, sa = math.cos(va), math.sin(va)
        ce, se = math.cos(ea), math.sin(ea)
        xr = ca * x + sa * z
        zr = -sa * x + ca * z
        yr = ce * y - se * zr
        zr2 = se * y + ce * zr
        return np.stack([off[0] + xr * scale, off[1] - yr * scale * 0.85, zr2], 1)

    zz, yy, xx = np.nonzero(vol)
    coords = np.stack([xx, yy, zz], 1).astype(float)
    cols = col[zz, yy, xx].astype(float)
    r = max(1, int(scale * 0.6))
    for deg in (0, 90, 180, 270):
        va = math.radians(-25 + deg)
        ea = math.radians(26.0)
        scr = proj(coords, va, ea)
        order = np.argsort(-scr[:, 2])
        canvas = np.full((VH, VW, 3), (18, 20, 34), dtype=np.uint8)
        dmin, dmax = scr[:, 2].min(), scr[:, 2].max()
        drng = max(dmax - dmin, 1e-6)
        for idx in order:
            sx, sy, dep = scr[idx]
            shade = 1.0 - 0.14 * (dep - dmin) / drng   # 轻微深度明暗, 保持明亮
            colv = np.clip(cols[idx] * shade, 0, 255).astype(np.uint8)
            x0, x1 = int(sx - r), int(sx + r) + 1
            y0, y1 = int(sy - r), int(sy + r) + 1
            if x1 <= 0 or y1 <= 0 or x0 >= VW or y0 >= VH:
                continue
            canvas[max(y0, 0):min(y1, VH), max(x0, 0):min(x1, VW)] = colv
        img = _I.fromarray(canvas, "RGB")
        d = _D.Draw(img, "RGBA")
        d.text((8, 8), f"person_clean  {deg}°  (vote-core relief mesh)",
               fill=(150, 225, 255))
        d.text((8, VH - 22), f"voxels={len(zz)}  T={Tz} H={H} W={W}  real colors",
               fill=(215, 215, 215))
        img.save(f"{outprefix}_{deg:03d}.png")
    return [f"{outprefix}_{d:03d}.png" for d in (0, 90, 180, 270)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--thr", type=float, default=0.40, help="投票阈值(高=更保守)")
    ap.add_argument("--thick", type=int, default=10, help="浮雕最大厚度(体素)")
    ap.add_argument("--no-sym", action="store_true", help="关闭对称化(默认开启)")
    a = ap.parse_args()

    d = np.load(NPZ, allow_pickle=True)
    vol, tex = d["vol"], d["tex"]
    T, H, W = vol.shape
    print(f"输入时空体素 {vol.shape}, 占用 {int(vol.sum())}")

    print("[1/4] 逐帧去背景(小连通域) + 头部对齐 + 时间投票 ...")
    votes, bright, good = align_core(vol, tex)
    n = len(good)
    print(f"  选用 {n} 帧, 投票 max={votes.max():.2f}")
    core = (votes / n) >= a.thr          # 比例阈值
    if not a.no_sym:
        # 对称化: 绕核心质心竖轴镜像, 头居中/形态和谐
        ys, xs = np.nonzero(core)
        cx = int(round(xs.mean()))
        fcore = np.zeros_like(core)
        Ww = core.shape[1]
        for xx in range(Ww):
            mx = 2 * cx - xx
            if 0 <= mx < Ww:
                fcore[:, xx] |= core[:, mx]
        core = core | fcore
        print(f"  → 对称化后 核心(2D): {int(core.sum())} 像素")
    print(f"  → 投票核心(2D): {int(core.sum())} 像素 (thr={a.thr})")

    print("[2/4] 最亮帧真实颜色 ...")
    color = bright.copy()

    print("[3/4] 浮雕立体化 + marching cubes 网格 ...")
    v, col = relief_volume(core, color, a.thick)
    print(f"  → 3D 体素 {v.shape}, 占用 {int(v.sum())}")
    r = mesh_obj(v, col, OUTOBJ)
    print(f"  → {OUTOBJ}  {r[0]}顶点/{r[1]}三角  "
          f"{os.path.getsize(OUTOBJ)/1e6:.2f}MB")

    print("[4/4] 静态渲染预览 ...")
    pre = render_previews(v, col, os.path.join(BASE, "person_clean_preview"))
    for p in pre:
        print(f"  → {p}")
    print("=== person_clean.obj 完成 (仅 3D 模型, 无视频) ===")


if __name__ == "__main__":
    main()
