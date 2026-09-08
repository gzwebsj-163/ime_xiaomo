#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
真实视频 → BC 字节码(.kbc) 彩色视频推理编排器
==========================================
升级自 make_video_bc.py (灰度): 抽彩色 RGB 帧, BC 差分用三通道独立差分求和
t = |dR|+|dG|+|dB| - 3*THR, 能抓住灰度版会漏掉的彩色物体运动 (如红瓤西瓜
在绿皮/桌面背景上移动). 可视化用彩色原帧做底图叠加轨迹.

链路: 真实 mp4 → ffmpeg 抽彩色帧(PPM/P6) → 生成 R/G/B 三数组 .mo
      → ./xiaomo mo2kbc -o .kbc → 内核执行 → 解析质心 → 彩色底图叠加 → mp4
用法: /usr/local/bin/python3 make_video_bc_color.py <video.mp4>
      [--frames 30] [--w 64] [--h 113] [--thr 24] [--mp4 out.mp4]
"""
import argparse, os, re, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"
FFMPEG = "/opt/local/bin/ffmpeg"
MO     = os.path.join(BASE, "video_bc_color_seq.mo")
KBC    = os.path.join(BASE, "video_bc_color_seq.kbc")
OUT    = os.path.join(BASE, "video_bc_color_result.txt")
FRMDIR = os.path.join(BASE, "vframes_bc_color")
VIZDIR = os.path.join(BASE, "vframes_bc_color_viz")


def extract_frames(video, n, w, h):
    """ffmpeg 均匀抽 n 帧 → 彩色缩放 → PPM(P6) 存 FRMDIR, 返回 list[np.ndarray HxWx3 int."""
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
        frames.append(np.asarray(img, dtype=int))          # HxWx3
    print(f"抽帧完成: {len(frames)} 帧彩色 {frames[0].shape[1]}x{frames[0].shape[0]}  (源时长 {sec:.1f}s)")
    return frames


def flat_array(name, arr):
    """一维数组字面量 (行优先展平)."""
    flat = ",".join(str(int(v)) for v in arr.flatten())
    return f"void {name} : int = [{flat}]"


def build_mo(frames, thr):
    """纯标量+数组版彩色推理 .mo: R/G/B 三数组, 逐帧三通道差分求和→阈值→列/行投影→质心.
    变量数固定(~23) < 40 寄存器池上限, 帧数/分辨率任意加大不超限."""
    n = len(frames); h, w, _ = frames[0].shape
    wh = w * h
    t3 = 3 * thr
    L = []
    L.append(f"# auto-gen make_video_bc_color.py: {n}帧 {w}x{h} 彩色三通道差分+运动质心 (thr={thr})")
    L.append(f'>> print >> "=== VIDEO BC COLOR ({n} frames, thr={thr}) ==="')
    L.append(f"void W : int = {w}")
    L.append(f"void H : int = {h}")
    L.append(f"void WH : int = {wh}")
    L.append(f"void T3 : int = {t3}")
    # R/G/B 三个大数组 (跨行字面量, 每行 ~400 元素)
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
    # 投影/质心工作数组
    L.append(f"void col : int = [{'0,'*(w-1)}0]")
    L.append(f"void row : int = [{'0,'*(h-1)}0]")
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
        bc = i * wh        # 当前帧基址
        bp = (i - 1) * wh  # 前一帧基址
        # 清零投影
        L.append("k = 0")
        L.append("while ${k} < ${W}:")
        L.append("    col[${k}] = 0")
        L.append("    k = ${k} + 1")
        L.append("k2 = 0")
        L.append("while ${k2} < ${H}:")
        L.append("    row[${k2}] = 0")
        L.append("    k2 = ${k2} + 1")
        # 三通道差分求和 + 阈值 + 投影
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
        L.append(f'>> print >> "CX[{i}]: " >> ${{nx}} >> " DX[{i}]: " >> ${{dx_}}')
        L.append(f'>> print >> "CY[{i}]: " >> ${{ny}} >> " DY[{i}]: " >> ${{dy_}}')
    L.append('>> print >> "=== VIDEO BC COLOR DONE ===="')
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
    """解析 CX[i]: 分子 / DX[i]: 分母 → 质心列表 (i>=1)."""
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
    return xs, ys


def render_video_color(frames, xs, ys, upscale=10, fps=5, out_mp4="video_bc_color.mp4"):
    """把运动质心轨迹叠加到彩色原帧 → 放大 → PNG → ffmpeg 合成 mp4."""
    h, w, _ = frames[0].shape
    VW, VH = w * upscale, h * upscale
    os.makedirs(VIZDIR, exist_ok=True)
    for f in os.listdir(VIZDIR):
        os.remove(os.path.join(VIZDIR, f))
    trail = []
    n = len(xs)
    for i in range(n):
        img = Image.fromarray(np.clip(frames[i], 0, 255).astype(np.uint8), mode="RGB")
        img = img.resize((VW, VH), Image.NEAREST)
        d = ImageDraw.Draw(img)
        if xs[i] is not None:
            cxp, cyp = int(round(xs[i] * upscale)), int(round(ys[i] * upscale))
            trail.append((cxp, cyp))
            for j, (tx, ty) in enumerate(trail[:-1]):
                d.ellipse([tx-1, ty-1, tx+1, ty+1], fill=(255, 200, 0))
            r = 6
            d.line([cxp-r, cyp, cxp+r, cyp], fill=(255, 0, 0), width=2)
            d.line([cxp, cyp-r, cxp, cyp+r], fill=(255, 0, 0), width=2)
            d.ellipse([cxp-2, cyp-2, cxp+2, cyp+2], fill=(255, 0, 0))
        d.text((8, 8), f"BC color infer frame {i}", fill=(0, 255, 255))
        img.save(os.path.join(VIZDIR, f"v{i:03d}.png"))
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
    ap.add_argument("--up", type=int, default=10, help="可视化放大倍数")
    ap.add_argument("--fps", type=float, default=5.0)
    ap.add_argument("--mp4", type=str, default="video_bc_color.mp4")
    a = ap.parse_args()

    frames = extract_frames(a.video, a.frames, a.w, a.h)
    build_mo(frames, a.thr)
    r = run_bc()
    if r.returncode != 0:
        print("mo2kbc 失败, 原始输出:"); print(open(OUT).read()[:3000]); sys.exit(1)
    xs, ys = parse()
    n = len(frames)
    print(f"BC 彩色推理完成: 解析到 {n-1} 帧运动质心")
    if len(xs) != n-1:
        print("⚠️ 质心数不匹配, 原始输出见 video_bc_color_result.txt")
        print(open(OUT).read()[:2000]); sys.exit(1)
    print("\n差分帧    彩色运动X   彩色运动Y  (BC 字节码三通道推理):")
    for i in range(len(xs)):
        if xs[i] is not None:
            print(f"  d{i+1:3d}   {xs[i]:8.2f}  {ys[i]:8.2f}")
    mx = np.mean([v for v in xs if v is not None]); my = np.mean([v for v in ys if v is not None])
    print(f"\n运动主体质心: 平均 ({mx:.2f}, {my:.2f})")
    out = render_video_color(frames, xs, ys, upscale=a.up, fps=a.fps, out_mp4=a.mp4)
    print(f"\n=== BC 彩色字节码视频推理链路 PASS ===")
    print(f"  .mo   : {MO}")
    print(f"  .kbc  : {KBC}")
    print(f"  mp4   : {out}")
    print(f"  viz   : {VIZDIR}/v*.png")


if __name__ == "__main__":
    main()
