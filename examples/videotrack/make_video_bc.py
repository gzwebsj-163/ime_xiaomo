#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
真实视频 → BC 字节码(.kbc) 视频推理编排器
==========================================
与 make_video_track.py (解释器直跑 .mo) 不同, 本引擎把整条推理管线
**重写成纯标量+数组版 .mo** (帧间差分/阈值/行列投影/质心全用 while 循环 +
数组元素读写表达, 不用 nd_* 张量算子), 从而:
  .mo → mo2kbc 编译 → video_bc.kbc (二进制字节码) → xiaomo kvm 执行
即真正的 "BC 字节码推理" 路径 (解释器层不再参与推理).

链路:
  真实 mp4 → ffmpeg 抽帧(灰度 PGM, 小分辨率) → 生成纯数组 .mo
  → ./xiaomo mo2kbc -o video_bc.kbc (.mo 编译成 BC 并经内核执行)
  → 解析每帧运动质心 (CX[i]/CY[i])
  → PIL 把轨迹叠加到原视频帧 → ffmpeg PNG→mp4 生成可视化视频

用法: /usr/local/bin/python3 make_video_bc.py <video.mp4>
      [--frames 12] [--w 48] [--h 27] [--thr 24] [--out dir]
"""
import argparse, os, re, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"
FFMPEG = "/opt/local/bin/ffmpeg"
MO     = os.path.join(BASE, "video_bc_seq.mo")
KBC    = os.path.join(BASE, "video_bc_seq.kbc")
OUT    = os.path.join(BASE, "video_bc_result.txt")
FRMDIR = os.path.join(BASE, "vframes_bc")
VIZDIR = os.path.join(BASE, "vframes_bc_viz")


def extract_frames(video, n, w, h):
    """ffmpeg 均匀抽 n 帧 → 灰度缩放 → PGM(P5) 存 FRMDIR, 返回帧数组 list[h][w] (int)."""
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
        "-vf", f"fps={fps:.4f},scale={w}:{h},format=gray",
        "-frames:v", str(n),
        os.path.join(FRMDIR, "f%03d.pgm")],
        capture_output=True, text=True)
    if r.returncode != 0:
        print("ffmpeg 抽帧失败:"); print(r.stderr[-2000:]); sys.exit(1)
    files = sorted(os.listdir(FRMDIR))
    frames = []
    for fn in files:
        p = os.path.join(FRMDIR, fn)
        with open(p, "rb") as fp:
            header = fp.readline().strip()
            dim    = fp.readline().strip().split()
            W, H = int(dim[0]), int(dim[1])
            mx    = int(fp.readline().strip())
            data  = np.frombuffer(fp.read(), dtype=np.uint8).reshape(H, W)
        frames.append(data.astype(int))
    print(f"抽帧完成: {len(frames)} 帧 {W}x{H}  (源时长 {sec:.1f}s)")
    return frames


def flat_array(name, img):
    """一维数组字面量 (行优先展平)."""
    flat = ",".join(str(int(v)) for v in img.flatten())
    return f"void {name} : int = [{flat}]"


def build_mo(frames, thr):
    """纯标量+数组版推理 .mo: 30 帧合并为单个大数组 frames, 逐帧差分→阈值→列/行投影→质心.
    变量数固定(~19) < 40 寄存器池上限, 帧数/分辨率任意加大不超限 (不再每帧一个数组变量)."""
    n = len(frames); h, w = frames[0].shape
    wh = w * h
    L = []
    L.append(f"# auto-gen make_video_bc.py: 真实视频 {n}帧 {w}x{h} 差分+运动质心 (BC字节码推理 thr={thr})")
    L.append(f'>> print >> "=== VIDEO BC ({n} frames, thr={thr}) ==="')
    L.append(f"void W : int = {w}")
    L.append(f"void H : int = {h}")
    L.append(f"void WH : int = {wh}")
    L.append(f"void THR : int = {thr}")
    # frames 大数组 (跨行字面量, 每行 ~400 元素, parser 支持跨行/缩进)
    L.append("void frames : int = [")
    buf, lines_flat = [], []
    for img in frames:
        for v in img.flatten():
            buf.append(str(int(v)))
            if len(buf) >= 400:
                lines_flat.append(", ".join(buf)); buf = []
    if buf:
        lines_flat.append(", ".join(buf))
    for li, chunk in enumerate(lines_flat):
        suffix = "," if li < len(lines_flat) - 1 else ""
        L.append("    " + chunk + suffix)
    L.append("]")
    # 全局工作变量: 差分/投影/质心 累加 (每帧对复用, 差分前清零)
    L.append(f"void col : int = [{'0,'*(w-1)}0]")
    L.append(f"void row : int = [{'0,'*(h-1)}0]")
    L.append("void y : int = 0")
    L.append("void x : int = 0")
    L.append("void t : int = 0")
    L.append("void a : int = 0")
    # 逐帧对差分 (帧 i 与 i-1)
    for i in range(1, n):
        bc = i * wh        # 当前帧在 frames 里的基址
        bp = (i - 1) * wh  # 前一帧基址
        # 清零投影
        L.append("void k : int = 0")
        L.append("while ${k} < ${W}:")
        L.append("    col[${k}] = 0")
        L.append("    void k : int = ${k} + 1")
        L.append("void k2 : int = 0")
        L.append("while ${k2} < ${H}:")
        L.append("    row[${k2}] = 0")
        L.append("    void k2 : int = ${k2} + 1")
        # 差分 + 阈值 + 投影
        L.append("void y : int = 0")
        L.append("while ${y} < ${H}:")
        L.append("    void x : int = 0")
        L.append("    while ${x} < ${W}:")
        L.append(f"        void t : int = frames[{bc}+${{y}}*${{W}}+${{x}}] - frames[{bp}+${{y}}*${{W}}+${{x}}]")
        L.append("        if ${t} < 0:")
        L.append("            t = 0 - ${t}")
        L.append("        t = ${t} - ${THR}")
        L.append("        if ${t} > 0:")
        L.append("            col[${x}] = col[${x}] + ${t}")
        L.append("            row[${y}] = row[${y}] + ${t}")
        L.append("        void x : int = ${x} + 1")
        L.append("    void y : int = ${y} + 1")
        # 质心: cx = Σ x*col[x] / Σ col[x], cy = Σ y*row[y] / Σ row[y]
        L.append("void nx : int = 0")
        L.append("void dx_ : int = 0")
        L.append("void k3 : int = 0")
        L.append("while ${k3} < ${W}:")
        L.append("    nx = ${nx} + ${k3} * col[${k3}]")
        L.append("    dx_ = ${dx_} + col[${k3}]")
        L.append("    void k3 : int = ${k3} + 1")
        L.append("void ny : int = 0")
        L.append("void dy_ : int = 0")
        L.append("void k4 : int = 0")
        L.append("while ${k4} < ${H}:")
        L.append("    ny = ${ny} + ${k4} * row[${k4}]")
        L.append("    dy_ = ${dy_} + row[${k4}]")
        L.append("    void k4 : int = ${k4} + 1")
        L.append(f'>> print >> "CX[{i}]:" >> ${{nx}} >> " DX[{i}]: " >> ${{dx_}}')
        L.append(f'>> print >> "CY[{i}]:" >> ${{ny}} >> " DY[{i}]: " >> ${{dy_}}')
    L.append('>> print >> "=== VIDEO BC DONE ==="')
    with open(MO, "w") as fp:
        fp.write("\n".join(L) + "\n")
    return MO


def run_bc():
    """mo2kbc 编译 .mo → 序列化 .kbc + 内核执行, 返回 stdout."""
    r = subprocess.run([os.path.join(XIAOMO, "xiaomo"), "mo2kbc", "-o", KBC, MO],
                       cwd=XIAOMO, capture_output=True, text=True, timeout=600)
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


def render_video(frames, xs, ys, upscale=10, fps=5, out_mp4="video_bc.mp4"):
    """把运动质心轨迹叠加到原视频帧(PGM) → 放大 → PNG → ffmpeg 合成 mp4.
    质心坐标在 w×h 小图上; upscale 放大到 w*s × h*s."""
    h, w = frames[0].shape
    VW, VH = w * upscale, h * upscale
    os.makedirs(VIZDIR, exist_ok=True)
    for f in os.listdir(VIZDIR):
        os.remove(os.path.join(VIZDIR, f))
    # 轨迹累积: 每帧画历史轨迹 + 当前点
    trail = []
    n = len(xs)
    for i in range(n):
        img = Image.fromarray(np.clip(frames[i], 0, 255).astype(np.uint8), mode="L")
        img = img.resize((VW, VH), Image.NEAREST)
        rgb = img.convert("RGB")
        d = ImageDraw.Draw(rgb)
        if xs[i] is not None:
            cxp, cyp = int(round(xs[i] * upscale)), int(round(ys[i] * upscale))
            trail.append((cxp, cyp))
            # 历史轨迹 (前段点)
            for j, (tx, ty) in enumerate(trail[:-1]):
                d.ellipse([tx-1, ty-1, tx+1, ty+1], fill=(255, 200, 0))
            # 当前点: 红色十字
            r = 6
            d.line([cxp-r, cyp, cxp+r, cyp], fill=(255, 0, 0), width=2)
            d.line([cxp, cyp-r, cxp, cyp+r], fill=(255, 0, 0), width=2)
            d.ellipse([cxp-2, cyp-2, cxp+2, cyp+2], fill=(255, 0, 0))
        # 帧号标注
        d.text((8, 8), f"BC infer frame {i}", fill=(0, 255, 255))
        rgb.save(os.path.join(VIZDIR, f"v{i:03d}.png"))
    # 合成 mp4
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
    ap.add_argument("--frames", type=int, default=12)
    ap.add_argument("--w", type=int, default=48)
    ap.add_argument("--h", type=int, default=27)
    ap.add_argument("--thr", type=float, default=24.0)
    ap.add_argument("--up", type=int, default=10, help="可视化放大倍数")
    ap.add_argument("--fps", type=float, default=5.0)
    ap.add_argument("--mp4", type=str, default="video_bc.mp4")
    a = ap.parse_args()

    frames = extract_frames(a.video, a.frames, a.w, a.h)
    build_mo(frames, a.thr)
    r = run_bc()
    if r.returncode != 0:
        print("mo2kbc 失败, 原始输出:"); print(open(OUT).read()[:3000]); sys.exit(1)
    xs, ys = parse()
    n = len(frames)
    print(f"BC 推理完成: 解析到 {n-1} 帧运动质心")
    if len(xs) != n-1:
        print("⚠️ 质心数不匹配, 原始输出见 video_bc_result.txt")
        print(open(OUT).read()[:2000]); sys.exit(1)
    print("\n差分帧  运动X   运动Y  (BC 字节码推理):")
    for i in range(len(xs)):
        if xs[i] is not None:
            print(f"  d{i+1:3d}   {xs[i]:8.2f}  {ys[i]:8.2f}")
    mx = np.mean([v for v in xs if v is not None]); my = np.mean([v for v in ys if v is not None])
    print(f"\n运动主体质心: 平均 ({mx:.2f}, {my:.2f})")
    out = render_video(frames, xs, ys, upscale=a.up, fps=a.fps, out_mp4=a.mp4)
    print(f"\n=== BC 字节码视频推理链路 PASS ===")
    print(f"  .mo   : {MO}")
    print(f"  .kbc  : {KBC}")
    print(f"  mp4   : {out}")
    print(f"  viz   : {VIZDIR}/v*.png")


if __name__ == "__main__":
    main()
