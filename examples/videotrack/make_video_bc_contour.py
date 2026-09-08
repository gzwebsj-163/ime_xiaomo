#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
真实视频 → BC 字节码(.kbc) 彩色推理 + 运动物体外形勾勒 + 真实动作轨迹
===============================================================
升级自 make_video_bc_color.py (质心点): 在 BC 内核里, 除了三通道差分求和→
阈值→行列投影→质心, 还额外记录**每行运动跨度** (该行最左/最右运动 x),
逐帧输出 col_min[y]/col_max[y]. 宿主端用行跨度重建运动掩码 →
scipy.ndimage 连通域标记 → 提取运动物体外形轮廓(bounding box + 边缘描边)
+ 质心路径 + 历史轮廓带, 叠加到彩色原帧, 得到"勾勒外形 + 真实动作"的可视化.

链路: 真实 mp4 → ffmpeg 抽彩色帧(PPM) → 生成 R/G/B 三数组 .mo
      → ./xiaomo mo2kbc -o .kbc → 内核执行 → 解析每行跨度+质心
      → 宿主重建掩码/连通域/轮廓 → 彩色底图叠加 → mp4

用法: /usr/local/bin/python3 make_video_bc_contour.py <video.mp4>
      [--frames 30] [--w 64] [--h 113] [--thr 24] [--mp4 out.mp4]
      [--area 12] 连通域最小面积(像素), 过滤噪点
"""
import argparse, os, re, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"
FFMPEG = "/opt/local/bin/ffmpeg"
MO     = os.path.join(BASE, "video_bc_contour_seq.mo")
KBC    = os.path.join(BASE, "video_bc_contour_seq.kbc")
OUT    = os.path.join(BASE, "video_bc_contour_result.txt")
FRMDIR = os.path.join(BASE, "vframes_bc_contour")
VIZDIR = os.path.join(BASE, "vframes_bc_contour_viz")


def extract_frames(video, n, w, h):
    """ffmpeg 均匀抽 n 帧 → 彩色缩放 → PPM(P6) 存 FRMDIR, 返回 list[np.ndarray HxWx3 int]."""
    os.makedirs(FRMDIR, exist_ok=True)
    for f in os.listdir(FRMDIR):
        os.remove(os.path.join(FRMDIR, f))
    dur = subprocess.run([FFMPEG, "-i", video, "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    m = re.search(r"Duration:\s*(\d+):(\d+):(\d+\.\d+)", dur)
    sec = int(m.group(1))*3600 + int(m.group(2))*60 + float(m.group(3)) if m else 5.0
    fps = n / max(sec, 1e-6)
    r = subprocess.run([
        FFMPEG, "-y", "-i", video,
        "-vf", f"fps={fps:.4f},scale={w}:{h}",
        "-frames:v", str(n),
        os.path.join(FRMDIR, "f%03d.ppm")],
        capture_output=True, text=True)
    if r.returncode != 0:
        print("ffmpeg 抽帧失败:"); print(r.stderr[-2000:]); sys.exit(1)
    files = sorted(os.listdir(FRMDIR))
    frames = []
    for fn in files:
        img = Image.open(os.path.join(FRMDIR, fn)).convert("RGB")
        frames.append(np.asarray(img, dtype=int))
    print(f"抽帧完成: {len(frames)} 帧彩色 {frames[0].shape[1]}x{frames[0].shape[0]}  (源时长 {sec:.1f}s)")
    return frames


def build_mo(frames, thr):
    """纯标量+数组版彩色推理 .mo: R/G/B 三数组差分求和→阈值→行列投影→质心
    + 每行运动跨度(col_min/col_max). 变量数固定(~28) < 40."""
    n = len(frames); h, w, _ = frames[0].shape
    wh = w * h
    t3 = 3 * thr
    L = []
    L.append(f"# auto-gen make_video_bc_contour.py: {n}帧 {w}x{h} 彩色三通道差分+外形轮廓 (thr={thr})")
    L.append(f'>> print >> "=== VIDEO BC CONTOUR ({n} frames, thr={thr}) ==="')
    L.append(f"void W : int = {w}")
    L.append(f"void H : int = {h}")
    L.append(f"void WH : int = {wh}")
    L.append(f"void T3 : int = {t3}")
    L.append(f"void NEG : int = 0 - 1")
    # R/G/B 三个大数组 (跨行字面量)
    for ch, name in ((0, "framesR"), (1, "framesG"), (2, "framesB")):
        L.append(f"void {name} : int = [")
        buf, lines_flat = [], []
        for img in frames:
            for v in img[:, :, ch].flatten():
                buf.append(str(int(v)))
                if len(buf) >= 400:
                    lines_flat.append(", ".join(buf)); buf = []
        if buf:
            lines_flat.append(", ".join(buf))
        for li, chunk in enumerate(lines_flat):
            suffix = "," if li < len(lines_flat) - 1 else ""
            L.append("    " + chunk + suffix)
        L.append("]")
    # 工作数组/变量
    L.append(f"void col : int = [{'0,'*(w-1)}0]")
    L.append(f"void row : int = [{'0,'*(h-1)}0]")
    L.append(f"void cmin : int = [{'0,'*(h-1)}0]")
    L.append(f"void cmax : int = [{'0,'*(h-1)}0]")
    L.append("void y : int = 0")
    L.append("void x : int = 0")
    L.append("void k : int = 0")
    L.append("void k2 : int = 0")
    L.append("void k3 : int = 0")
    L.append("void k4 : int = 0")
    L.append("void t : int = 0")
    L.append("void t_r : int = 0")
    L.append("void t_g : int = 0")
    L.append("void t_b : int = 0")
    L.append("void nx : int = 0")
    L.append("void dx_ : int = 0")
    L.append("void ny : int = 0")
    L.append("void dy_ : int = 0")
    # 逐帧对差分 (帧 i 与 i-1)
    for i in range(1, n):
        bc = i * wh
        bp = (i - 1) * wh
        # 清零投影 + 每行跨度初始化 (cmin=W 表示无运动, cmax=NEG)
        L.append("k = 0")
        L.append("while ${k} < ${W}:")
        L.append("    col[${k}] = 0")
        L.append("    k = ${k} + 1")
        L.append("k2 = 0")
        L.append("while ${k2} < ${H}:")
        L.append("    row[${k2}] = 0")
        L.append("    cmin[${k2}] = ${W}")
        L.append("    cmax[${k2}] = ${NEG}")
        L.append("    k2 = ${k2} + 1")
        # 三通道差分求和 + 阈值 + 投影 + 每行跨度
        L.append("y = 0")
        L.append("while ${y} < ${H}:")
        L.append("    x = 0")
        L.append("    while ${x} < ${W}:")
        L.append(f"        t_r = framesR[{bc}+${{y}}*${{W}}+${{x}}] - framesR[{bp}+${{y}}*${{W}}+${{x}}]")
        L.append(f"        t_g = framesG[{bc}+${{y}}*${{W}}+${{x}}] - framesG[{bp}+${{y}}*${{W}}+${{x}}]")
        L.append(f"        t_b = framesB[{bc}+${{y}}*${{W}}+${{x}}] - framesB[{bp}+${{y}}*${{W}}+${{x}}]")
        L.append("        if ${t_r} < 0:")
        L.append("            t_r = 0 - ${t_r}")
        L.append("        if ${t_g} < 0:")
        L.append("            t_g = 0 - ${t_g}")
        L.append("        if ${t_b} < 0:")
        L.append("            t_b = 0 - ${t_b}")
        L.append("        t = ${t_r} + ${t_g} + ${t_b}")
        L.append("        t = ${t} - ${T3}")
        L.append("        if ${t} > 0:")
        L.append("            col[${x}] = col[${x}] + ${t}")
        L.append("            row[${y}] = row[${y}] + ${t}")
        L.append("            if ${x} < cmin[${y}]:")
        L.append("                cmin[${y}] = ${x}")
        L.append("            if ${x} > cmax[${y}]:")
        L.append("                cmax[${y}] = ${x}")
        L.append("        x = ${x} + 1")
        L.append("    y = ${y} + 1")
        # 质心: cx = Σ x*col[x] / Σ col[x], cy = Σ y*row[y] / Σ row[y]
        L.append("nx = 0")
        L.append("dx_ = 0")
        L.append("k3 = 0")
        L.append("while ${k3} < ${W}:")
        L.append("    nx = ${nx} + ${k3} * col[${k3}]")
        L.append("    dx_ = ${dx_} + col[${k3}]")
        L.append("    k3 = ${k3} + 1")
        L.append("ny = 0")
        L.append("dy_ = 0")
        L.append("k4 = 0")
        L.append("while ${k4} < ${H}:")
        L.append("    ny = ${ny} + ${k4} * row[${k4}]")
        L.append("    dy_ = ${dy_} + row[${k4}]")
        L.append("    k4 = ${k4} + 1")
        # 输出质心 + 每行跨度 (供宿主重建掩码/勾勒外形)
        L.append(f'>> print >> "CX[{i}]: " >> ${{nx}} >> " DX[{i}]: " >> ${{dx_}}')
        L.append(f'>> print >> "CY[{i}]: " >> ${{ny}} >> " DY[{i}]: " >> ${{dy_}}')
        L.append("k = 0")
        L.append("while ${k} < ${H}:")
        L.append(f'    >> print >> "RNG[{i}]: " >> ${{k}} >> " " >> cmin[${{k}}] >> " " >> cmax[${{k}}]')
        L.append("    k = ${k} + 1")
    L.append('>> print >> "=== VIDEO BC CONTOUR DONE ===="')
    with open(MO, "w") as fp:
        fp.write("\n".join(L) + "\n")
    return MO


def run_bc():
    """mo2kbc 编译 .mo → 序列化 .kbc + 内核执行, 返回 stdout."""
    r = subprocess.run([os.path.join(XIAOMO, "xiaomo"), "mo2kbc", "-o", KBC, MO],
                       cwd=XIAOMO, capture_output=True, text=True, timeout=900)
    with open(OUT, "w") as fp:
        fp.write(r.stdout)
        fp.write("\n# ---- STDERR ----\n")
        fp.write(r.stderr)
    return r


def parse():
    """解析质心 + 每行运动跨度 RNG[i]: y lo hi → (xs, ys, rows) rows=[(y,lo,hi)...] per i."""
    txt = open(OUT).read()
    nx = {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"CX\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt)}
    dx = {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"DX\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt)}
    ny = {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"CY\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt)}
    dy = {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"DY\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt)}
    xs, ys = [], []
    for i in sorted(nx):
        if dx.get(i, 0) > 0:
            xs.append(nx[i] / dx[i])
            ys.append(ny.get(i, 0) / dy.get(i, 0) if dy.get(i, 0) else 0.0)
        else:
            xs.append(None); ys.append(None)
    # RNG[i]: y lo hi  (每行跨度)
    rng = {}
    for m in re.finditer(r"RNG\[(\d+)\]:\s*\n?\s*(\d+)\s+(-?\d+)\s+(-?\d+)", txt):
        rng[(int(m.group(1)), int(m.group(2)))] = (int(m.group(3)), int(m.group(4)))
    return xs, ys, rng


def rebuild_masks(rng, n, h, w):
    """用每行跨度重建运动掩码列表 (diff i 对应帧 i 与 i-1, i=1..n-1)."""
    masks = []
    for i in range(1, n):
        m = np.zeros((h, w), dtype=bool)
        for y in range(h):
            if (i, y) in rng:
                lo, hi = rng[(i, y)]
                if lo <= hi:
                    m[y, max(0, lo):min(w, hi + 1)] = True
        masks.append(m)
    return masks


def outline_contours(mask):
    """对运动掩码做连通域标记, 返回 [(bbox(y0,y1,x0,x1), area, edge_mask), ...] 按面积降序."""
    if not mask.any():
        return []
    lab, n = ndimage.label(mask, structure=np.ones((3, 3), dtype=bool))
    sizes = ndimage.sum(mask, lab, range(1, n + 1)).astype(int)
    slices = ndimage.find_objects(lab)
    eroded = ndimage.binary_erosion(mask, structure=np.ones((3, 3), dtype=bool))
    edge = mask & ~eroded
    out = []
    for idx, (sl, size) in enumerate(zip(slices, sizes), start=1):
        if sl is None:
            continue
        y0, y1 = sl[0].start, sl[0].stop
        x0, x1 = sl[1].start, sl[1].stop
        out.append(((y0, y1, x0, x1), int(size), (edge & (lab == idx))))
    out.sort(key=lambda t: -t[1])
    return out


def render_contour(frames, xs, ys, masks, upscale=10, fps=5,
                   out_mp4="video_bc_contour.mp4", area=12, trail_len=8):
    """彩色底图 + 勾勒运动物体外形(轮廓描边+bbox) + 主质心 + 历史轨迹 + 历史轮廓带."""
    h, w = masks[0].shape
    VW, VH = w * upscale, h * upscale
    os.makedirs(VIZDIR, exist_ok=True)
    for f in os.listdir(VIZDIR):
        os.remove(os.path.join(VIZDIR, f))
    trail = []            # 主物体质心轨迹 (vx, vy)
    hist_edges = []       # 历史轮廓描边缓冲 (缩放坐标集合)
    N = len(masks)
    for i in range(N):
        img = Image.fromarray(np.clip(frames[i + 1], 0, 255).astype(np.uint8), mode="RGB")
        img = img.resize((VW, VH), Image.NEAREST)
        d = ImageDraw.Draw(img, "RGBA")
        # 历史轮廓带 (前几帧的轮廓淡色描边 → 展示真实动作路径)
        for j, edge_mask in enumerate(hist_edges):
            a = 60 + 40 * j // max(len(hist_edges), 1)
            for (yy, xx) in zip(*np.where(edge_mask)):
                d.rectangle([xx*upscale, yy*upscale, xx*upscale+upscale-1, yy*upscale+upscale-1],
                            fill=(0, 255, 255, a))
        # 当前帧轮廓: 连通域
        blobs = outline_contours(masks[i])
        cur_edge = np.zeros_like(masks[i])
        cx_cur = cy_cur = None
        for bi, (bbox, size, edge_mask) in enumerate(blobs):
            if size < area:
                continue
            y0, y1, x0, x1 = bbox
            # 轮廓描边 (放大块)
            col = (255, 0, 0) if bi == 0 else (255, 160, 0)
            for (yy, xx) in zip(*np.where(edge_mask)):
                d.rectangle([xx*upscale, yy*upscale, xx*upscale+upscale-1, yy*upscale+upscale-1],
                            fill=col + (255,))
            # bbox 矩形
            d.rectangle([x0*upscale, y0*upscale, x1*upscale-1, y1*upscale-1], outline=col, width=2)
            if bi == 0:
                cur_edge |= edge_mask
                cx_cur = (x0 + x1) / 2.0 * upscale
                cy_cur = (y0 + y1) / 2.0 * upscale
        # 主物体质心 + 轨迹
        if cx_cur is not None:
            if xs[i] is not None and len(trail) > 0:
                # 用 BC 质心 (跨尺度一致), 轨迹连线
                px, py = trail[-1]
                d.line([px, py, xs[i]*upscale, ys[i]*upscale], fill=(255, 255, 0), width=3)
            trail.append((xs[i]*upscale, ys[i]*upscale))
            trail = trail[-trail_len:]
            # 历史轨迹点
            for j, (tx, ty) in enumerate(trail[:-1]):
                d.ellipse([tx-3, ty-3, tx+3, ty+3], fill=(255, 255, 0))
            # 当前质心十字
            cxp, cyp = int(round(xs[i]*upscale)), int(round(ys[i]*upscale))
            r = 7
            d.line([cxp-r, cyp, cxp+r, cyp], fill=(255, 0, 255), width=2)
            d.line([cxp, cyp-r, cxp, cyp+r], fill=(255, 0, 255), width=2)
            # 方向箭头 (真实动作: 从上一个质心指向当前)
            if len(trail) >= 2:
                x0_, y0_ = trail[-2]
                x1_, y1_ = trail[-1]
                vx, vy = x1_-x0_, y1_-y0_
                vl = (vx*vx+vy*vy) ** 0.5
                if vl > 2:
                    ux, uy = vx/vl, vy/vl
                    al = 16
                    d.line([x1_, y1_, x1_-ux*al+uy*al*0.5, y1_-uy*al-ux*al*0.5], fill=(0, 255, 0), width=2)
                    d.line([x1_, y1_, x1_-ux*al-uy*al*0.5, y1_-uy*al+ux*al*0.5], fill=(0, 255, 0), width=2)
        d.text((8, 8), f"BC contour infer frame {i+1}", fill=(0, 255, 255))
        d.text((8, VH-24), f"blobs={sum(1 for b in blobs if b[1]>=area)} center=({xs[i]*upscale/upscale:.1f},{ys[i]*upscale/upscale:.1f})" if xs[i] is not None else "no motion",
               fill=(255, 255, 0))
        img.save(os.path.join(VIZDIR, f"v{i:03d}.png"))
        hist_edges.append(cur_edge)
        hist_edges = hist_edges[-trail_len:]
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
    ap.add_argument("video", help="真实视频路径 (mp4/mov...)")
    ap.add_argument("--frames", type=int, default=30)
    ap.add_argument("--w", type=int, default=64)
    ap.add_argument("--h", type=int, default=113)
    ap.add_argument("--thr", type=float, default=24.0)
    ap.add_argument("--area", type=int, default=12, help="连通域最小面积(像素), 过滤噪点")
    ap.add_argument("--up", type=int, default=10, help="可视化放大倍数")
    ap.add_argument("--fps", type=float, default=5.0)
    ap.add_argument("--mp4", type=str, default="video_bc_contour.mp4")
    a = ap.parse_args()

    frames = extract_frames(a.video, a.frames, a.w, a.h)
    build_mo(frames, a.thr)
    r = run_bc()
    if r.returncode != 0:
        print("mo2kbc 失败, 原始输出:"); print(open(OUT).read()[:3000]); sys.exit(1)
    n = len(frames)
    xs, ys, rng = parse()
    if len(xs) != n - 1:
        print("⚠️ 质心数不匹配, 原始输出见 video_bc_contour_result.txt")
        print(open(OUT).read()[:2000]); sys.exit(1)
    masks = rebuild_masks(rng, n, a.h, a.w)
    print(f"BC 轮廓推理完成: {n-1} 差分帧, 运动掩码重建 {len(masks)} 帧")
    # 汇总每帧连通域数/主 bbox
    total_blobs = 0
    for i, m in enumerate(masks):
        blobs = outline_contours(m)
        big = [b for b in blobs if b[1] >= a.area]
        total_blobs += len(big)
        if big:
            (y0, y1, x0, x1), size, _ = big[0]
            if xs[i] is not None:
                print(f"  d{i+1:3d} 主物体 bbox=({x0},{y0})-({x1},{y1}) 面积={size}  质心=({xs[i]:.1f},{ys[i]:.1f}) 域数={len(big)}")
        else:
            print(f"  d{i+1:3d} 无显著运动")
    print(f"\n≥{a.area}px 连通域总数: {total_blobs}")
    out = render_contour(frames, xs, ys, masks, upscale=a.up, fps=a.fps, out_mp4=a.mp4, area=a.area)
    print(f"\n=== BC 彩色轮廓勾勒视频推理链路 PASS ===")
    print(f"  .mo   : {MO}")
    print(f"  .kbc  : {KBC}")
    print(f"  mp4   : {out}")
    print(f"  viz   : {VIZDIR}/v*.png")


if __name__ == "__main__":
    main()
