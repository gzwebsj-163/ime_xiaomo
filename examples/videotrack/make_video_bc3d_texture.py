#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
保留人像外观的 3D 推理 (带纹理时空体素 Textured Space-Time Volume)
================================================================
前一版(时空体素)只堆了运动轮廓线, 把物体抽象成了彩虹时间轨迹, 丢失人像外观.
本版升级: 前景分割拿到**完整人像轮廓**(背景差分+形态学+最大连通域填洞),
每帧人像轮廓沿时间轴堆叠成时空体素, 并**保留每帧原始画面颜色作为纹理**,
旋转渲染出"保留人像真实外观"的 3D 物体 (体素颜色=真实肤色/西瓜色/衣物色).

链路: 抽帧(64x113 彩色) → BC .kbc 推理(质心脊柱=历史线条, 复用轮廓管线)
      → 宿主端背景差分分割完整人像 → 轮廓堆叠=时空体素 + 纹理采样
      → 旋转视角渲染(画家算法, 纹理色+深度明暗) → mp4

用法: /usr/local/bin/python3 make_video_bc3d_texture.py <video.mp4> [--thr 120]
      [--frames 30] [--w 64] [--h 113] [--mp4 video_bc3d_texture.mp4]
"""
import argparse, math, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"
FFMPEG = "/opt/local/bin/ffmpeg"
sys.path.insert(0, BASE)
import make_video_bc_contour as M          # 复用 2D 轮廓管线(BC 推理)

VIZDIR = os.path.join(BASE, "vframes_bc3d_texture")
OUT3D  = os.path.join(BASE, "video_bc3d_texture_vol.npz")
PREVIEW_PREFIX = os.path.join(BASE, "tex3d_preview")


def foreground_masks(imgs, thr_bg, morph=(3, 3)):
    """背景差分分割完整人像: 中位数背景 → 颜色距离阈值 → 形态学 → 最大连通域填洞."""
    N, H, W, _ = imgs.shape
    bg = np.median(imgs, axis=0).astype(int)
    diff = np.abs(imgs - bg).sum(axis=-1).astype(int)
    masks = np.zeros((N, H, W), dtype=bool)
    for t in range(N):
        d = diff[t]
        m = (d > thr_bg)
        if morph:
            m = ndimage.binary_closing(m, structure=np.ones(morph))
            m = ndimage.binary_opening(m, structure=np.ones(morph))
        m = ndimage.binary_fill_holes(m)
        lab, nn = ndimage.label(m, structure=np.ones((3, 3)))
        if nn > 0:
            sizes = ndimage.sum(m, lab, range(1, nn + 1))
            m = (lab == (np.argmax(sizes) + 1))
        masks[t] = m
    return masks


def build_volume_texture(imgs, masks):
    """轮廓堆叠 → 裁剪 → 纹理采样. 返回 vol(T,H,W), tex(T,H,W,3), offset(z0,y0,x0)."""
    vol = np.stack(masks, axis=0)                       # (T,H,W) bool
    nz, ny, nx = np.nonzero(vol)
    z0, z1 = nz.min(), nz.max() + 1
    y0, y1 = ny.min(), ny.max() + 1
    x0, x1 = nx.min(), nx.max() + 1
    v = vol[z0:z1, y0:y1, x0:x1]
    # 逐帧 2D 闭运算填洞 + 时间轴闭连接(形成连续体)
    for z in range(v.shape[0]):
        v[z] = ndimage.binary_closing(v[z], structure=np.ones((3, 3)))
    if v.shape[0] > 1:
        v = ndimage.binary_closing(v, structure=np.ones((3, 3, 3)))
    T, H, W = v.shape
    tex = np.zeros((T, H, W, 3), dtype=np.uint8)
    for z in range(T):
        tex[z] = imgs[z0 + z, y0:y1, x0:x1]
    return v, tex, (z0, y0, x0)


def project(v, c, scale, view_ang, elev_ang, off):
    x, y, z = v[0] - c[0], v[1] - c[1], v[2] - c[2]
    ca, sa = math.cos(view_ang), math.sin(view_ang)
    ce, se = math.cos(elev_ang), math.sin(elev_ang)
    xr = ca * x + sa * z
    zr = -sa * x + ca * z
    yr = ce * y - se * zr
    zr2 = se * y + ce * zr
    sx = off[0] + xr * scale
    sy = off[1] - yr * scale * 0.85
    return sx, sy, zr2


def project_screen(coords, c, scale, view_ang, elev_ang, off):
    x = coords[:, 0] - c[0]
    y = coords[:, 1] - c[1]
    z = coords[:, 2] - c[2]
    ca, sa = math.cos(view_ang), math.sin(view_ang)
    ce, se = math.cos(elev_ang), math.sin(elev_ang)
    xr = ca * x + sa * z
    zr = -sa * x + ca * z
    yr = ce * y - se * zr
    zr2 = se * y + ce * zr
    sx = off[0] + xr * scale
    sy = off[1] - yr * scale * 0.85
    return np.stack([sx, sy, zr2], axis=1)


def render_textured(vol, tex, spine, out_mp4, fps=12, step_deg=5.0, elev=26.0,
                    save_previews=True):
    """旋转视角渲染: 体素颜色=原帧纹理(保留人像外观) + 深度明暗."""
    T, H, W = vol.shape
    os.makedirs(VIZDIR, exist_ok=True)
    for f in os.listdir(VIZDIR):
        os.remove(os.path.join(VIZDIR, f))
    max_dim = max(T, H, W)
    VW = int(max_dim * 18) // 2 * 2
    VH = int(max_dim * 15) // 2 * 2
    off = (VW / 2.0, VH / 2.0 + max_dim * 1.2)
    scale = (VW * 0.42) / max_dim
    c = (W / 2.0, H / 2.0, T / 2.0)
    elev_r = math.radians(elev)
    r = max(1, int(scale * 0.55))
    # 表面检测(6邻域)
    surf = np.zeros_like(vol, dtype=bool)
    for ax in range(3):
        surf |= (np.roll(vol, 1, axis=ax) == False) | (np.roll(vol, -1, axis=ax) == False)
    surf &= vol
    zz, yy, xx = np.nonzero(vol)
    occ_coords = np.stack([xx, yy, zz], axis=1).astype(float)
    occ_tex = tex[zz, yy, xx].astype(float)              # (N,3) 真实颜色
    occ_surf = surf[zz, yy, xx]
    n_occ = len(occ_coords)
    n_frames = int(360 / step_deg)
    preview_angles = set(range(0, 360, 90))
    for fi in range(n_frames):
        view_ang = math.radians(-25 + fi * step_deg)
        scr = project_screen(occ_coords, c, scale, view_ang, elev_r, off)
        order = np.argsort(-scr[:, 2])
        canvas = np.full((VH, VW, 3), (16, 18, 30), dtype=np.uint8)
        dmin, dmax = scr[:, 2].min(), scr[:, 2].max()
        drng = max(dmax - dmin, 1e-6)
        for idx in order:
            sx, sy, dep = scr[idx]
            col = occ_tex[idx]
            fr = 1.0 if occ_surf[idx] else 0.80
            shade = 1.0 - 0.28 * (dep - dmin) / drng
            col = np.clip(col * fr * shade, 0, 255).astype(np.uint8)
            x0 = int(sx - r); x1 = int(sx + r) + 1
            y0 = int(sy - r); y1 = int(sy + r) + 1
            if x1 <= 0 or y1 <= 0 or x0 >= VW or y0 >= VH:
                continue
            x0 = max(x0, 0); y0 = max(y0, 0)
            x1 = min(x1, VW); y1 = min(y1, VH)
            canvas[y0:y1, x0:x1] = col
        img = Image.fromarray(canvas, "RGB")
        d = ImageDraw.Draw(img, "RGBA")
        # 底座网格
        for gx in range(0, W + 1, max(1, W // 5)):
            a = project((gx, 0, 0), c, scale, view_ang, elev_r, off)
            b = project((gx, 0, T - 1), c, scale, view_ang, elev_r, off)
            d.line([a[:2], b[:2]], fill=(70, 84, 120), width=1)
        for gz in range(0, T + 1, max(1, T // 5)):
            a = project((0, 0, gz), c, scale, view_ang, elev_r, off)
            b = project((W - 1, 0, gz), c, scale, view_ang, elev_r, off)
            d.line([a[:2], b[:2]], fill=(70, 84, 120), width=1)
        # 质心脊柱(黄色, 历史线条)
        if spine is not None and len(spine):
            pts = [project((x, y, z), c, scale, view_ang, elev_r, off) for (x, y, z) in spine]
            for j in range(1, len(pts)):
                d.line([pts[j - 1][:2], pts[j][:2]], fill=(255, 230, 40), width=2)
        # 文字
        d.text((8, 8), f"Textured 3D from video contour history  frame {fi+1}/{n_frames}",
               fill=(140, 220, 255))
        d.text((8, VH - 24),
               f"voxels={n_occ}  T={T} H={H} W={W}  texture=real frame colors",
               fill=(210, 210, 210))
        img.save(os.path.join(VIZDIR, f"v{fi:03d}.png"))
        if save_previews and fi in preview_angles:
            img.copy().save(f"{PREVIEW_PREFIX}_{fi:03d}.png")
    out = os.path.join(BASE, out_mp4)
    r = subprocess.run([
        FFMPEG, "-y", "-framerate", str(fps),
        "-i", os.path.join(VIZDIR, "v%03d.png"),
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "23", out],
        capture_output=True, text=True)
    if r.returncode != 0:
        print("mp4 合成失败:"); print(r.stderr[-2000:]); sys.exit(1)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--w", type=int, default=64)
    ap.add_argument("--h", type=int, default=113)
    ap.add_argument("--thr", type=float, default=120.0, help="背景差分阈值(高=只留强差异)")
    ap.add_argument("--fps", type=float, default=12.0)
    ap.add_argument("--mp4", type=str, default="video_bc3d_texture.mp4")
    ap.add_argument("--render-only", action="store_true")
    a = ap.parse_args()

    if a.render_only:
        data = np.load(OUT3D, allow_pickle=True)
        vol, tex, spine = data["vol"], data["tex"], data["spine"]
        if len(spine) == 0:
            spine = None
        print(f"[render-only] 载入时空体素 {vol.shape}, 纹理体素 {tex.shape}, 脊柱 {0 if spine is None else len(spine)}")
        out = render_textured(vol, tex, spine, a.mp4, fps=a.fps)
        print(f"  → {out}")
        sys.exit(0)

    print(f"[1/5] 抽帧 (thr={a.thr}) ...")
    frames = M.extract_frames(a.video, a.frames, a.w, a.h)
    imgs = np.stack(frames).astype(int)
    N, H, W, _ = imgs.shape

    print("[2/5] BC 推理 (质心脊柱=历史线条) ...")
    M.build_mo(frames, max(24.0, a.thr * 0.5))
    r = M.run_bc()
    if r.returncode != 0:
        print("mo2kbc 失败:"); print(open(M.OUT).read()[:3000]); sys.exit(1)
    xs, ys, _ = M.parse()
    spine = [(xs[p], ys[p], p) for p in range(N - 1)
             if xs[p] is not None and ys[p] is not None]

    print("[3/5] 前景分割(保留完整人像轮廓) ...")
    masks = foreground_masks(imgs, a.thr)
    frac = [masks[t].mean() * 100 for t in range(N)]
    print(f"  前景占比: min={min(frac):.1f}% max={max(frac):.1f}% avg={np.mean(frac):.1f}%")

    print("[4/5] 轮廓堆叠时空体素 + 纹理采样 ...")
    vol, tex, (z0, y0, x0) = build_volume_texture(imgs, masks)
    T, Hh, Ww = vol.shape
    print(f"  → 时空体素 {T}(时间)×{Hh}(高)×{Ww}(宽)  占用 {int(vol.sum())} voxel")
    spine3 = [(x - x0, y - y0, z - z0) for (x, y, z) in spine]
    np.savez(OUT3D, vol=vol, tex=tex, spine=np.asarray(spine3, dtype=float))

    print("[5/5] 旋转渲染(纹理=真实人像外观) ...")
    out = render_textured(vol, tex, spine3, a.mp4, fps=a.fps)
    print(f"  → {out} ({os.path.getsize(out)/1e6:.2f}MB)")
    print("  → 预览图:", [f"{PREVIEW_PREFIX}_{a:03d}.png" for a in (0, 90, 180, 270)])
    print("=== 保留人像外观的 3D 推理 PASS ===")
    print(f"  vol+tex+spine: {OUT3D}")


if __name__ == "__main__":
    main()
