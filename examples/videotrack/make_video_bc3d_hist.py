#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
历史轮廓线 → 3D 物体推理 (时空体素 Space-Time Volume)
================================================================
用 2D BC 轮廓推理里"每一帧的历史轮廓线"(运动掩码) 当作 3D 物体的一个
时间切片: 把整个视频的轮廓沿时间轴堆叠成一块 3D 体素 (T×H×W),
再用旋转视角渲染成一个可视化的 3D 物体 —— 历史轮廓线在 3D 里立体呈现,
绕 Y 轴 360° 回转可见其完整形状与动作扫掠.

链路: 复用 make_video_bc_contour.py 的 真实视频→BC .kbc→RNG 行跨度→掩码
      → 掩码沿时间轴堆叠 = 时空体素
      → 裁剪/闭运算 → 彩虹时间着色 + 深度明暗 + 质心脊柱 → 旋转渲染 mp4

用法: /usr/local/bin/python3 make_video_bc3d_hist.py <video.mp4>
      [--frames 30] [--w 64] [--h 113] [--thr 60] [--mp4 video_bc3d_hist.mp4]
"""
import argparse, math, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

BASE   = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE)
import make_video_bc_contour as M          # 复用 2D 轮廓管线

FFMPEG = M.FFMPEG
VIZDIR = os.path.join(BASE, "vframes_bc3d_hist")
OUT3D = os.path.join(BASE, "video_bc3d_hist_vol.npz")


def build_volume(masks):
    """掩码列表(每帧轮廓) → 沿时间轴堆叠 → 裁剪到联合 bbox → 轻闭运算.
    vol[z, y, x]: z=时间(帧), y=画面纵向, x=画面横向. 返回 vol, (z0,y0,x0)."""
    vol = np.stack(masks, axis=0).astype(bool)          # (T,H,W)
    nz, ny, nx = np.nonzero(vol)
    if len(nz) == 0:
        raise RuntimeError("无任何运动掩码, 无法构建时空体素")
    z0, z1 = nz.min(), nz.max() + 1
    y0, y1 = ny.min(), ny.max() + 1
    x0, x1 = nx.min(), nx.max() + 1
    v = vol[z0:z1, y0:y1, x0:x1]
    # 每帧 2D 闭运算填洞 + 时间轴闭运算连接断裂切片(形成连通实体)
    for z in range(v.shape[0]):
        v[z] = ndimage.binary_closing(v[z], structure=np.ones((3, 3)))
    if v.shape[0] > 1:
        v = ndimage.binary_closing(v, structure=np.ones((3, 3, 3)))
    return v, (z0, y0, x0)


def project(v, c, scale, view_ang, elev_ang, off):
    """等距投影: 模型坐标 → 屏幕. v=世界坐标, c=中心, 绕Y旋转视角."""
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


def rainbow(t, n):
    """时间索引 → RGB 彩虹色 (t=0 红 → 中 绿 → 末 蓝紫)."""
    hue = 0.0 + 0.8 * (t / max(n - 1, 1))
    # HSV → RGB (hue 0..1)
    h = hue % 1.0
    i = int(h * 6)
    f = h * 6 - i
    p, q, tt = 0.0, 1.0 * (1 - f), 1.0 * (1 - (1 - f))
    rgb = [(1, tt, p), (q, 1, p), (p, 1, tt),
           (p, q, 1), (tt, p, 1), (1, p, q)][int(i) % 6]
    return tuple(int(round(c * 255)) for c in rgb)


def render_hist3d(vol, spine, out_mp4, fps=14, step_deg=5.0, elev=24.0):
    """旋转视角渲染时空体素: numpy 画布合成(画家算法) + PIL 只画网格/文字."""
    T, H, W = vol.shape
    os.makedirs(VIZDIR, exist_ok=True)
    for f in os.listdir(VIZDIR):
        os.remove(os.path.join(VIZDIR, f))
    max_dim = max(T, H, W)
    VW = int(max_dim * 21) // 2 * 2
    VH = int(max_dim * 17) // 2 * 2
    off = (VW / 2.0, VH / 2.0 + max_dim * 1.5)
    scale = (VW * 0.40) / max_dim
    c = (W / 2.0, H / 2.0, T / 2.0)
    elev_r = math.radians(elev)
    r = max(1, int(scale * 0.55))                 # 方块半宽, 略重叠消除缝隙
    # 6-邻域表面检测: 任一相邻位置为空即为表面
    surf = np.zeros_like(vol, dtype=bool)
    for ax in range(3):
        surf |= (np.roll(vol, 1, axis=ax) == False) | (np.roll(vol, -1, axis=ax) == False)
    surf &= vol
    # 占用体素坐标 (x,y,z) + 属性
    zz, yy, xx = np.nonzero(vol)                   # 时间, 高, 宽
    occ_coords = np.stack([xx, yy, zz], axis=1).astype(float)
    occ_surf = surf[zz, yy, xx]
    occ_color = np.array([rainbow(int(z), T) for z in zz], dtype=np.uint8)  # (N,3)
    n_occ = len(occ_coords)
    n_frames = int(360 / step_deg)
    for fi in range(n_frames):
        view_ang = math.radians(-25 + fi * step_deg)
        scr = project_screen(occ_coords, c, scale, view_ang, elev_r, off)   # (N,3)
        order = np.argsort(-scr[:, 2])            # 远→近
        canvas = np.full((VH, VW, 3), (12, 14, 26), dtype=np.uint8)
        # 深度归一化明暗
        dmin, dmax = scr[:, 2].min(), scr[:, 2].max()
        drng = max(dmax - dmin, 1e-6)
        for idx in order:
            sx, sy, dep = scr[idx]
            col = occ_color[idx].astype(float)
            fr = 1.0 if occ_surf[idx] else 0.62
            shade = 1.0 - 0.30 * (dep - dmin) / drng
            col = np.clip(col * fr * shade, 0, 255).astype(np.uint8)
            x0 = int(sx - r); x1 = int(sx + r) + 1
            y0 = int(sy - r); y1 = int(sy + r) + 1
            if x1 <= 0 or y1 <= 0 or x0 >= VW or y0 >= VH:
                continue
            x0 = max(x0, 0); y0 = max(y0, 0)
            x1 = min(x1, VW); y1 = min(y1, VH)
            canvas[y0:y1, x0:x1] = col            # 画家算法: 近者覆盖远者
        img = Image.fromarray(canvas, "RGB")
        d = ImageDraw.Draw(img, "RGBA")
        # 底座参考网格 (y=0 平面)
        for gx in range(0, W + 1, max(1, W // 5)):
            a = project((gx, 0, 0), c, scale, view_ang, elev_r, off)
            b = project((gx, 0, T - 1), c, scale, view_ang, elev_r, off)
            d.line([a[:2], b[:2]], fill=(70, 84, 120), width=1)
        for gz in range(0, T + 1, max(1, T // 5)):
            a = project((0, 0, gz), c, scale, view_ang, elev_r, off)
            b = project((W - 1, 0, gz), c, scale, view_ang, elev_r, off)
            d.line([a[:2], b[:2]], fill=(70, 84, 120), width=1)
        # 质心脊柱 (黄色折线)
        if spine:
            pts = [project((x, y, z), c, scale, view_ang, elev_r, off) for (x, y, z) in spine]
            for j in range(1, len(pts)):
                d.line([pts[j - 1][:2], pts[j][:2]], fill=(255, 230, 40), width=3)
            p = project(spine[-1], c, scale, view_ang, elev_r, off)
            rr = int(r * 2.2)
            d.line([p[0] - rr, p[1], p[0] + rr, p[1]], fill=(255, 0, 255), width=2)
            d.line([p[0], p[1] - rr, p[0], p[1] + rr], fill=(255, 0, 255), width=2)
        d.text((8, 8), f"Space-Time Volume from BC contour history  frame {fi+1}/{n_frames}",
               fill=(120, 220, 255))
        d.text((8, VH - 24),
               f"voxels={n_occ}  T={T} H={H} W={W}  color=time t  (red=start→blue=end)",
               fill=(200, 200, 200))
        img.save(os.path.join(VIZDIR, f"v{fi:03d}.png"))
    out = os.path.join(BASE, out_mp4)
    r = subprocess.run([
        FFMPEG, "-y", "-framerate", str(fps),
        "-i", os.path.join(VIZDIR, "v%03d.png"),
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "23", out],
        capture_output=True, text=True)
    if r.returncode != 0:
        print("mp4 合成失败:"); print(r.stderr[-2000:]); sys.exit(1)
    return out


def project_screen(coords, c, scale, view_ang, elev_ang, off):
    """向量化投影 (N,3) → (N,3) [sx, sy, depth]."""
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--w", type=int, default=64)
    ap.add_argument("--h", type=int, default=113)
    ap.add_argument("--thr", type=float, default=60.0, help="差分阈值(高→只留核心强运动)")
    ap.add_argument("--area", type=int, default=12)
    ap.add_argument("--fps", type=float, default=14.0)
    ap.add_argument("--step", type=float, default=5.0, help="视角旋转步长(度)")
    ap.add_argument("--mp4", type=str, default="video_bc3d_hist.mp4")
    ap.add_argument("--render-only", action="store_true",
                    help="复用已保存的时空体素+脊柱直接渲染(跳过BC推理)")
    a = ap.parse_args()

    if a.render_only:
        if not os.path.exists(OUT3D):
            print(f"无 {OUT3D}, 请先完整跑一次"); sys.exit(1)
        data = np.load(OUT3D, allow_pickle=True)
        vol = data["vol"]
        spine = data["spine"]
        T, H, W = vol.shape
        print(f"[render-only] 载入时空体素 {T}(时间)×{H}(高)×{W}(宽), "
              f"占用 {int(vol.sum())} voxel, 脊柱 {len(spine)} 点")
        out = render_hist3d(vol, spine, a.mp4, fps=a.fps, step_deg=a.step)
        print(f"  → {out} ({os.path.getsize(out)/1e6:.2f}MB)")
        print("=== 历史轮廓线 → 3D 物体推理 (render-only) PASS ===")
        sys.exit(0)

    print(f"[1/5] 抽帧 + BC 轮廓推理 (thr={a.thr}) ...")
    frames = M.extract_frames(a.video, a.frames, a.w, a.h)
    M.build_mo(frames, a.thr)
    r = M.run_bc()
    if r.returncode != 0:
        print("mo2kbc 失败:"); print(open(M.OUT).read()[:3000]); sys.exit(1)
    n = len(frames)
    xs, ys, rng = M.parse()
    if len(xs) != n - 1:
        print("⚠️ 质心数不匹配"); sys.exit(1)
    masks = M.rebuild_masks(rng, n, a.h, a.w)
    print(f"  → {n-1} 差分帧, {len(masks)} 帧运动掩码 (历史轮廓线)")

    print("[2/5] 历史轮廓线沿时间轴堆叠 → 时空体素 ...")
    vol, (z0, y0, x0) = build_volume(masks)
    T, H, W = vol.shape
    print(f"  → 时空体素 {T}(时间)×{H}(高)×{W}(宽), 占用 {int(vol.sum())} voxel"
          f"  ({vol.sum()/(T*H*W)*100:.1f}%)\n  裁剪偏移 (z0,y0,x0)=({z0},{y0},{x0})")

    print("[3/5] 质心脊柱 (每帧 BC 质心 → 3D 轨迹) ...")
    spine = []
    for p in range(n - 1):
        if xs[p] is not None and ys[p] is not None:
            spine.append((xs[p] - x0, ys[p] - y0, p - z0))
    np.savez(OUT3D, vol=vol, spine=np.asarray(spine, dtype=float))
    print(f"  → 脊柱 {len(spine)} 点, 已存 {OUT3D}")

    print("[4/5] 旋转视角渲染 3D ...")
    out = render_hist3d(vol, spine, a.mp4, fps=a.fps, step_deg=a.step)
    print(f"  → {out} ({os.path.getsize(out)/1e6:.2f}MB)")

    print("[5/5] 汇总")
    print(f"  thr={a.thr}  时间切片数 T={T}  3D 体积 {T*H*W} voxel")
    print(f"  彩虹着色=时间轴 (红→绿→蓝紫 = 视频开始→结束)")
    print(f"  黄色折线=物体质心 3D 轨迹 (真实动作)")
    print("=== 历史轮廓线 → 3D 物体推理 PASS ===")
    print(f"  vol : {OUT3D}")
    print(f"  mp4 : {os.path.join(BASE, a.mp4)}")
    print(f"  viz : {VIZDIR}/v*.png")


if __name__ == "__main__":
    main()
