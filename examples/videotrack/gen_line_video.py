#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成"移动线条"演示视频 —— 与 make_line_track.py 的 gen_frames 同源
================================================
演示 VM 视频推理链路喂进去的输入是什么样的:
  black: 黑底白竖线匀速右移 (最干净)
  noise: 深灰底 + 高斯噪声 + 竖线右移 (演示为什么需要阈值化)
  diag:  白斜线对角移动 (细长结构, 质心法边界)
  blob:  白方块 2D 移动 (紧凑目标, 质心法有效)

用: /usr/local/bin/python3 gen_line_video.py [--fps 30] [--sec 2.5] [--w 640] [--h 360] [--noise 8]
输出: 各模式 *_move.mp4 到本目录
"""
import argparse, glob, os, subprocess
import numpy as np

BASE   = os.path.dirname(os.path.abspath(__file__))
FFMPEG = "/opt/local/bin/ffmpeg"

def gen_frames(mode, n, w, h, noise):
    frames = []
    x0, x1 = w*0.10, w*0.88
    y0, y1 = h*0.15, h*0.72
    for t in range(n):
        frac = t / max(1, n-1)
        cx = x0 + (x1-x0)*frac
        cy = y0 + (y1-y0)*frac
        if mode == "black":
            img = np.zeros((h, w, 3), np.uint8)
        else:
            img = np.full((h, w, 3), 10, np.uint8)  # 深灰底(背景偏置)
        if mode in ("black", "noise"):
            x = int(round(cx))
            img[:, x-3:x+3] = (235, 235, 235)       # 竖线 6px 宽
        elif mode == "diag":
            for yy in range(h):
                xc = cx + (yy - cy)
                xb = int(round(xc))
                xb0 = max(0, xb-4); xb1 = min(w, xb+5)
                img[max(0,yy-1):min(h,yy+2), xb0:xb1] = (235, 235, 235)  # 斜线带
        elif mode == "blob":
            r = 6
            img[max(0,int(round(cy))-r):min(h,int(round(cy))+r),
                max(0,int(round(cx))-r):min(w,int(round(cx))+r)] = (235, 235, 235)
        if noise > 0 and mode != "black":
            img = np.clip(img.astype(np.int16) + np.random.normal(0, noise, img.shape).astype(np.int16), 0, 255).astype(np.uint8)
        frames.append(img)
    return frames

def write_ppm(img, path):
    h, w = img.shape[:2]
    with open(path, "wb") as fp:
        fp.write(f"P6\n{w} {h}\n255\n".encode())
        fp.write(img.tobytes())

def make_video(mode, n, w, h, fps, noise):
    frames = gen_frames(mode, n, w, h, noise)
    d = os.path.join(BASE, "gen_tmp")
    os.makedirs(d, exist_ok=True)
    for f in glob.glob(os.path.join(d, "*.ppm")):
        os.remove(f)
    for i, img in enumerate(frames):
        write_ppm(img, os.path.join(d, f"f{i:04d}.ppm"))
    out = os.path.join(BASE, f"{mode}_move.mp4")
    r = subprocess.run([FFMPEG, "-y", "-framerate", str(fps),
                        "-i", os.path.join(d, "f%04d.ppm"),
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f"[{mode}] ffmpeg 失败:"); print(r.stderr[-1500:]); return
    print(f"[{mode}] {n}帧 {w}x{h} @{fps}fps -> {out} ({os.path.getsize(out)//1024}KB)")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--sec", type=float, default=2.5)
    ap.add_argument("--w", type=int, default=640)
    ap.add_argument("--h", type=int, default=360)
    ap.add_argument("--noise", type=float, default=8.0, help="噪声σ(仅 noise 模式用)")
    ap.add_argument("--modes", default="black,diag,blob,noise", help="逗号分隔: black,diag,blob,noise")
    a = ap.parse_args()
    n = int(round(a.fps * a.sec))
    for m in [s.strip() for s in a.modes.split(",") if s.strip()]:
        make_video(m, n, a.w, a.h, a.fps, a.noise)

if __name__ == "__main__":
    main()
