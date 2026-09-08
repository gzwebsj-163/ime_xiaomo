#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
真实视频 → BC 内核(xiaomo VM) 视频推理编排器 v2
================================================
链路: 真实视频文件
  -> ffmpeg 抽帧(灰度 PGM) + 缩放
  -> 自动生成 .mo (帧张量 + 帧间差分 + 阈值化 + 行列投影 + 运动质心)
  -> ./xiaomo run 逐帧差分推理
  -> 解析轨迹 -> PGM 可视化(最后帧叠轨迹)

运动检测原理: 相邻帧差分 dt = relu(|f[t]-f[t-1]| - thr),
  差分帧列投影->x质心, 行投影->y质心 = 运动区域中心(视频中移动主体)

运动预测 (--predict): pred[i] = cx[i-1] + alpha*(cx[i-1]-cx[i-2])
  (加权线性外推, 帧 i>=2 起; 用已检测质心预测下一帧位置)
  真实视频无真值 -> 预测误差 = |pred[i] - 实际检测cx[i]| (轨迹可预测性/平滑度)
  alpha=1.0 朴素外推 / alpha=0.5 阻尼外推(降尾部风险)

用: /usr/local/bin/python3 make_video_track.py <video.mp4> [--frames 8] [--w 64] [--h 36] [--thr 24] [--predict] [--alpha 0.5]
"""
import argparse, os, re, subprocess, sys
import numpy as np

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"
FFMPEG = "/opt/local/bin/ffmpeg"
MO     = os.path.join(BASE, "video_track_seq.mo")
OUT    = os.path.join(BASE, "video_track_result.txt")
FRMDIR = os.path.join(BASE, "vframes")

def extract_frames(video, n, w, h):
    """ffmpeg 抽 n 帧 → 灰度缩放 → PGM(P5) 存 FRMDIR, 返回帧数组 list[h][w]."""
    os.makedirs(FRMDIR, exist_ok=True)
    for f in os.listdir(FRMDIR):
        os.remove(os.path.join(FRMDIR, f))
    # 均匀抽 n 帧: fps 由时长动态定, 避免首尾黑帧
    import subprocess as sp
    dur = sp.run([FFMPEG, "-i", video, "-f", "null", "-"],
                 capture_output=True, text=True).stderr
    m = re.search(r"Duration:\s*(\d+):(\d+):(\d+\.\d+)", dur)
    sec = int(m.group(1))*3600 + int(m.group(2))*60 + float(m.group(3)) if m else 5.0
    fps = n / max(sec, 1e-6)   # n/sec 保证抽满 n 帧(首帧含在内)
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
            header = fp.readline().strip()          # P5
            dim    = fp.readline().strip().split()  # W H
            W, H = int(dim[0]), int(dim[1])
            mx    = int(fp.readline().strip())       # 255
            data  = np.frombuffer(fp.read(), dtype=np.uint8).reshape(H, W)
        frames.append(data.astype(float))
    print(f"抽帧完成: {len(frames)} 帧 {W}x{H}  (源时长 {sec:.1f}s, fps={fps:.3f})")
    return frames

def mo_array(name, img, w):
    rows = []
    for row in img:
        rows.append("[" + ",".join(f"{v:.1f}" for v in row) + "]")
    return f"void {name} : int = tensor([\n  " + ",\n  ".join(rows) + "\n])"

def build_mo(frames, thr, predict=False, alpha=0.5):
    n = len(frames); h, w = frames[0].shape
    L = [f"# auto-gen by make_video_track.py v2: 真实视频 {n}帧 {w}x{h} 帧间差分运动跟踪 (thr={thr}, predict={predict}, alpha={alpha})"]
    L.append(f'>> print >> "=== VIDEO TRACK ({n} frames, thr={thr}) ==="')
    L.append(f"void xidx : int = tensor([{','.join(map(str, range(w)))}])")
    L.append(f"void yidx : int = tensor([{','.join(map(str, range(h)))}])")
    rows = []
    for y in range(h):
        rows.append("[" + ",".join([str(thr)]*w) + "]")
    L.append("void thrframe : int = tensor([\n  " + ",\n  ".join(rows) + "\n])")
    for i, img in enumerate(frames):
        L.append(mo_array(f"f{i}", img, w))
    # 帧间差分(从帧1起): dt = relu(|f[i]-f[i-1]| - thr) -> 列/行投影->运动质心
    for i in range(1, n):
        L.append(f"void d{i} : int = nd_abs(nd_sub(${{f{i}}}, ${{f{i-1}}}))")
        L.append(f"void dt{i} : int = nd_relu(nd_sub(${{d{i}}}, ${{thrframe}}))")
        L.append(f"void col{i} : int = nd_sum(${{dt{i}}}, 0)")
        L.append(f"void row{i} : int = nd_sum(${{dt{i}}}, 1)")
        L.append(f"void nx{i} : int = nd_sum(nd_mul(${{col{i}}}, ${{xidx}}), -1)")
        L.append(f"void dx{i} : int = nd_sum(${{col{i}}}, -1)")
        L.append(f"void cx{i} : int = nd_div(${{nx{i}}}, ${{dx{i}}})")
        L.append(f"void ny{i} : int = nd_sum(nd_mul(${{row{i}}}, ${{yidx}}), -1)")
        L.append(f"void dy{i} : int = nd_sum(${{row{i}}}, -1)")
        L.append(f"void cy{i} : int = nd_div(${{ny{i}}}, ${{dy{i}}})")
        if predict and i >= 3:
            # 加权线性外推: pred[i] = prev + alpha*(prev - prev2) (纯张量标量运算)
            # 注意: 差分质心自 cx[1] 起(cx[0]不存在), 故预测需 cx[i-1],cx[i-2] 均存在 -> i>=3
            L.append(f"void dpx{i} : int = nd_sub(${{cx{i-1}}}, ${{cx{i-2}}})")
            L.append(f"void apx{i} : int = nd_mul(${{dpx{i}}}, {alpha})")
            L.append(f"void px{i}  : int = nd_add(${{cx{i-1}}}, ${{apx{i}}})")
            L.append(f"void dpy{i} : int = nd_sub(${{cy{i-1}}}, ${{cy{i-2}}})")
            L.append(f"void apy{i} : int = nd_mul(${{dpy{i}}}, {alpha})")
            L.append(f"void py{i}  : int = nd_add(${{cy{i-1}}}, ${{apy{i}}})")
            L.append(f'>> print >> "DX[{i}]:" >> ${{cx{i}}} >> " DY[{i}]:" >> ${{cy{i}}} >> " PX[{i}]:" >> ${{px{i}}} >> " PY[{i}]:" >> ${{py{i}}}')
        else:
            L.append(f'>> print >> "DX[{i}]:" >> ${{cx{i}}} >> " DY[{i}]:" >> ${{cy{i}}}')
    L.append('>> print >> "=== VIDEO TRACK DONE ==="')
    with open(MO, "w") as fp:
        fp.write("\n".join(L) + "\n")

def run_vm():
    r = subprocess.run([os.path.join(XIAOMO, "xiaomo"), "run", MO],
                       cwd=XIAOMO, capture_output=True, text=True, timeout=600)
    with open(OUT, "w") as fp:
        fp.write(r.stdout)
    return r

def parse():
    xs, ys = [], []
    txt = open(OUT).read()
    for m in re.finditer(r"DX\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
        xs.append(float(m.group(2)))
    for m in re.finditer(r"DY\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
        ys.append(float(m.group(2)))
    return xs, ys

def parse_predict():
    """解析 PX[i]/PY[i] 运动预测 (i>=2). 返回 (帧号, 预测值) 列表."""
    px, py = [], []
    txt = open(OUT).read()
    for m in re.finditer(r"PX\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
        px.append((int(m.group(1)), float(m.group(2))))
    for m in re.finditer(r"PY\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
        py.append((int(m.group(1)), float(m.group(2))))
    return px, py

def write_pgm(frames, xs, ys, path):
    h, w = frames[0].shape
    canvas = frames[-1].astype(float).copy()
    n = len(xs)
    for t in range(n):
        xo = int(round(xs[t])); yo = int(round(ys[t]))
        for dy in range(-1, 2):
            for dx in range(-2, 3):
                yy, xx = yo+dy, xo+dx
                if 0 <= xx < w and 0 <= yy < h:
                    canvas[yy, xx] = 255.0
    with open(path, "wb") as fp:
        fp.write(f"P5\n{w} {h}\n255\n".encode())
        fp.write(np.clip(canvas,0,255).astype(np.uint8).tobytes())

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video", help="真实视频路径 (mp4/mov...)")
    ap.add_argument("--frames", type=int, default=8)
    ap.add_argument("--w", type=int, default=64)
    ap.add_argument("--h", type=int, default=36)
    ap.add_argument("--thr", type=float, default=24.0, help="差分阈值(越高越抗噪/只留大运动)")
    ap.add_argument("--predict", action="store_true", help="运动预测: 用前两帧差分质心线性外推下一帧 (输出 PX[i]/PY[i])")
    ap.add_argument("--alpha", type=float, default=0.5, help="预测外推权重: 1.0=朴素(噪声放大), 0.5=阻尼(降尾部风险, 默认)")
    ap.add_argument("--viz", type=str, default=os.path.join(BASE, "video_track_viz.pgm"))
    a = ap.parse_args()

    frames = extract_frames(a.video, a.frames, a.w, a.h)
    build_mo(frames, a.thr, predict=a.predict, alpha=a.alpha)
    r = run_vm()
    if r.returncode != 0:
        print("VM 运行失败:\n", r.stdout[-3000:], r.stderr[-2000:]); sys.exit(1)
    xs, ys = parse()
    n = len(frames)
    print(f"VM 解析: 检测到 {len(xs)} 帧运动质心 (差分帧 {n-1})")
    if len(xs) != n-1:
        print("⚠️ 差分帧数不匹配, 原始输出见 video_track_result.txt"); print(open(OUT).read()[:2000]); sys.exit(1)
    print("\n差分帧  运动X   运动Y (VM 检测):")
    for i in range(len(xs)):
        print(f"  d{i+1:3d}   {xs[i]:8.2f}  {ys[i]:8.2f}")
    mx, my = np.mean(xs), np.mean(ys)
    # 位移统计: 轨迹跨度 + 匀速性
    spanx = np.max(xs) - np.min(xs); spany = np.max(ys) - np.min(ys)
    print(f"\n运动主体质心: 平均 ({mx:.2f}, {my:.2f}) | 轨迹跨度 X={spanx:.1f}px Y={spany:.1f}px")
    # 运动预测评估: 预测误差 = |pred[i] - 实际检测 cx[i]/cy[i]| (真实视频无真值, 自洽性)
    if a.predict:
        px, py = parse_predict()
        if len(px) == n-3 and len(py) == n-3:   # 帧 i=2..n-1 共 n-2 个差分质心, i>=2 起预测 → n-1 起共 n-3
            idx  = np.array([i for i, _ in px])
            pxs  = np.array([v for _, v in px]); pys = np.array([v for _, v in py])
            # 差分帧 d[i] 对应 VM 质心 cx[i]; 预测帧号 i 与差分序号 i-1 对应
            actx = np.array([xs[i-1] for i in idx]); acty = np.array([ys[i-1] for i in idx])
            pex = np.abs(pxs - actx); pey = np.abs(pys - acty)
            print(f"\n预测帧  实测X   预测X   err_px  实测Y   预测Y   err_py  (alpha={a.alpha})")
            for k in range(len(idx)):
                print(f"  {idx[k]:4d}  {actx[k]:7.2f}  {pxs[k]:7.2f}  {pex[k]:6.2f}  {acty[k]:7.2f}  {pys[k]:7.2f}  {pey[k]:6.2f}")
            print(f"\n预测误差: X 平均 {pex.mean():.3f}px | 最大 {pex.max():.3f}px | Y 平均 {pey.mean():.3f}px | 最大 {pey.max():.3f}px")
            print(f"轨迹可预测性: X 平均 {pex.mean()/max(spanx,1e-6)*100:.1f}% 轨迹跨度 | Y 平均 {pey.mean()/max(spany,1e-6)*100:.1f}% 轨迹跨度")
        else:
            print(f"⚠️ 预测点数 {len(px)}x{len(py)} != 期望 {n-3}, 跳过预测评估")
    print(f"\n=== 真实视频→BC内核 视频推理链路 PASS (帧间差分+运动质心 {len(xs)} 帧{'+运动预测' if a.predict else ''}) ===")
    write_pgm(frames, xs, ys, a.viz)
    print(f"可视化已写: {a.viz} (最后帧叠白点轨迹)")

if __name__ == "__main__":
    main()
