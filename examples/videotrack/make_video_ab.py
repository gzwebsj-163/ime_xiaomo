#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
αβ 滤波移植到真实视频 —— 30 帧真实视频 (64x36, vframes/*.pgm)
================================================================
真实视频无真值, 用「预测 vs 实测检测」自洽性评估 (与 make_video_track --predict 同口径):
  err = |预测P{i} - 实测检测 cx{i}|
对比:
  1. 旧线性外推 pred = cx[i-1] + α(cx[i-1]-cx[i-2])   (i>=3, numpy 从解析出的 cx 复算)
  2. αβ 滤波预测 P{i} = F{i-1} + V{i-1}              (i>=2, VM 内闭环)
  3. αβ 滤波态 F 的平滑度: 帧间跳变 |F{i}-F{i-1}| vs 原始 |cx{i}-cx{i-1}|

用法: /usr/local/bin/python3 make_video_ab.py [--dir vframes] [--thr 24] [--alphas 0.3,0.5,0.7]
"""
import argparse, os, re, subprocess, sys
import numpy as np

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"
MO     = os.path.join(BASE, "video_ab_seq.mo")
OUT    = os.path.join(BASE, "video_ab_result.txt")


def load_pgms(d, maxn=32):
    """按数字序读 vframes/f*.pgm (P5 灰度)."""
    files = []
    for f in sorted(os.listdir(d)):
        m = re.match(r"f(\d+)\.pgm$", f)
        if m:
            files.append((int(m.group(1)), os.path.join(d, f)))
    files.sort()
    frames = []
    for _, p in files[:maxn]:
        with open(p, "rb") as fp:
            head = fp.readline().strip()
            dim  = fp.readline().strip().split()
            W, H = int(dim[0]), int(dim[1])
            mx   = int(fp.readline().strip())
            data = np.frombuffer(fp.read(), dtype=np.uint8).reshape(H, W)
        frames.append(data.astype(float))
    print(f"载入真实视频帧: {len(frames)} 帧 {W}x{H}  (来自 {d}/)")
    return frames


def mo_array(name, img, w):
    rows = []
    for row in img:
        rows.append("[" + ",".join(f"{v:.1f}" for v in row) + "]")
    return f"void {name} : int = tensor([\n  " + ",\n  ".join(rows) + "\n])"


def build_mo(frames, thr, alpha):
    n = len(frames); h, w = frames[0].shape
    beta = alpha * alpha / (2 - alpha)
    L = []
    L.append("# auto-gen by make_video_ab.py: 真实视频 %d帧 %dx%d 差分+αβ滤波 (thr=%s alpha=%s beta=%.4f)" % (n, w, h, thr, alpha, beta))
    L.append('>> print >> "=== VIDEO AB (%d frames, thr=%s, alpha=%s, beta=%.4f) ==="' % (n, thr, alpha, beta))
    L.append("void xidx : int = tensor([" + ",".join(map(str, range(w))) + "])")
    L.append("void yidx : int = tensor([" + ",".join(map(str, range(h))) + "])")
    rows = []
    for y in range(h):
        rows.append("[" + ",".join([str(thr)] * w) + "]")
    L.append("void thrframe : int = tensor([\n  " + ",\n  ".join(rows) + "\n])")
    for i, img in enumerate(frames):
        L.append(mo_array("f%d" % i, img, w))
    # 帧间差分 -> 运动质心 (自 i=1)
    for i in range(1, n):
        L.append("void d%d : int = nd_abs(nd_sub(${f%d}, ${f%d}))" % (i, i, i-1))
        L.append("void dt%d : int = nd_relu(nd_sub(${d%d}, ${thrframe}))" % (i, i))
        L.append("void col%d : int = nd_sum(${dt%d}, 0)" % (i, i))
        L.append("void row%d : int = nd_sum(${dt%d}, 1)" % (i, i))
        L.append("void nx%d : int = nd_sum(nd_mul(${col%d}, ${xidx}), -1)" % (i, i))
        L.append("void dx%d : int = nd_sum(${col%d}, -1)" % (i, i))
        L.append("void cx%d : int = nd_div(${nx%d}, ${dx%d})" % (i, i, i))
        L.append("void ny%d : int = nd_sum(nd_mul(${row%d}, ${yidx}), -1)" % (i, i))
        L.append("void dy%d : int = nd_sum(${row%d}, -1)" % (i, i))
        L.append("void cy%d : int = nd_div(${ny%d}, ${dy%d})" % (i, i, i))
    # αβ 种子: F1 = cx1, V1 = 0 (差分质心自 cx1 起)
    L.append("void Fx1 : int = ${cx1}")
    L.append("void Fy1 : int = ${cy1}")
    L.append("void Vx1 : int = nd_sub(${cx1}, ${cx1})")
    L.append("void Vy1 : int = nd_sub(${cy1}, ${cy1})")
    L.append('>> print >> "DX[1]:" >> ${cx1} >> " DY[1]:" >> ${cy1} >> " FX[1]:" >> ${Fx1} >> " FY[1]:" >> ${Fy1}')
    for i in range(2, n):
        L.append("void Px%d : int = nd_add(${Fx%d}, ${Vx%d})" % (i, i-1, i-1))
        L.append("void Py%d : int = nd_add(${Fy%d}, ${Vy%d})" % (i, i-1, i-1))
        L.append("void Rx%d : int = nd_sub(${cx%d}, ${Px%d})" % (i, i, i))
        L.append("void Ry%d : int = nd_sub(${cy%d}, ${Py%d})" % (i, i, i))
        L.append("void aRx%d : int = nd_mul(${Rx%d}, %s)" % (i, i, alpha))
        L.append("void aRy%d : int = nd_mul(${Ry%d}, %s)" % (i, i, alpha))
        L.append("void Fx%d : int = nd_add(${Px%d}, ${aRx%d})" % (i, i, i))
        L.append("void Fy%d : int = nd_add(${Py%d}, ${aRy%d})" % (i, i, i))
        L.append("void bRx%d : int = nd_mul(${Rx%d}, %s)" % (i, i, beta))
        L.append("void bRy%d : int = nd_mul(${Ry%d}, %s)" % (i, i, beta))
        L.append("void Vx%d : int = nd_add(${Vx%d}, ${bRx%d})" % (i, i-1, i))
        L.append("void Vy%d : int = nd_add(${Vy%d}, ${bRy%d})" % (i, i-1, i))
        L.append('>> print >> "DX[%d]:" >> ${cx%d} >> " DY[%d]:" >> ${cy%d} >> " PX[%d]:" >> ${Px%d} >> " PY[%d]:" >> ${Py%d} >> " FX[%d]:" >> ${Fx%d} >> " FY[%d]:" >> ${Fy%d}' % (i, i, i, i, i, i, i, i, i, i, i, i))
    L.append('>> print >> "=== VIDEO AB DONE ==="')
    with open(MO, "w") as fp:
        fp.write("\n".join(L) + "\n")


def run_vm():
    r = subprocess.run([os.path.join(XIAOMO, "xiaomo"), "run", MO],
                       cwd=XIAOMO, capture_output=True, text=True, timeout=900)
    with open(OUT, "w") as fp:
        fp.write(r.stdout)
    return r


def parse_all():
    txt = open(OUT).read()
    def grab(tag):
        d = {}
        for m in re.finditer(rf"{tag}\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
            d[int(m.group(1))] = float(m.group(2))
        return d
    return (grab("DX"), grab("DY"), grab("PX"), grab("PY"), grab("FX"), grab("FY"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(BASE, "vframes"))
    ap.add_argument("--thr", type=float, default=24.0)
    ap.add_argument("--alphas", default="0.3,0.5,0.7")
    a = ap.parse_args()
    alphas = [float(x) for x in a.alphas.split(",")]

    frames = load_pgms(a.dir)
    n = len(frames)
    cx = cy = None
    summary = []
    for alpha in alphas:
        build_mo(frames, a.thr, alpha)
        r = run_vm()
        if r.returncode != 0:
            print("VM 运行失败 (alpha=%s):\n%s\n%s" % (alpha, r.stdout[-3000:], r.stderr[-2000:]))
            sys.exit(1)
        dx, dy, px, py, fx, fy = parse_all()
        if cx is None:
            cx, cy = dx, dy
        idx = np.array([i for i in sorted(fx.keys()) if i in px and i in py])
        # αβ 预测自洽误差: |P[i]-cx[i]| (i>=2)
        pxe = np.abs(np.array([px[i] for i in idx]) - np.array([cx[i] for i in idx]))
        pye = np.abs(np.array([py[i] for i in idx]) - np.array([cy[i] for i in idx]))
        # αβ 滤波态 vs 原始检测 (平滑偏离)
        fxe = np.abs(np.array([fx[i] for i in idx]) - np.array([cx[i] for i in idx]))
        fye = np.abs(np.array([fy[i] for i in idx]) - np.array([cy[i] for i in idx]))
        # 帧间跳变: 滤波后 vs 原始
        iarr = sorted(fx.keys())
        raw_jump = np.mean([abs(cx[i]-cx[i-1]) for i in iarr if i-1 in cx])
        f_jump   = np.mean([abs(fx[i]-fx[i-1]) for i in iarr if i-1 in fx])
        summary.append(dict(alpha=alpha, pxm=pxe.mean(), pxm_=pxe.max(),
                            pym=pye.mean(), pym_=pye.max(),
                            fxm=fxe.mean(), fym=fye.mean(),
                            raw_jump=raw_jump, f_jump=f_jump))
        print("\n--- alpha=%s (beta=%.4f) ---" % (alpha, alpha*alpha/(2-alpha)))
        print("  αβ预测: X 平均 %.3fpx / 最大 %.3fpx | Y 平均 %.3fpx / 最大 %.3fpx" % (pxe.mean(), pxe.max(), pye.mean(), pye.max()))
        print("  αβ滤波: X 偏离检测 %.3fpx | Y 偏离 %.3fpx | 帧间跳变 %.3f vs 原始 %.3f (平滑 %.0f%%)" % (fxe.mean(), fye.mean(), f_jump, raw_jump, (1-f_jump/raw_jump)*100))

    # 旧线性外推基线 (numpy 复算): pred = cx[i-1] + 0.5*(cx[i-1]-cx[i-2]), i>=3
    ii = np.array([i for i in sorted(cx.keys()) if i >= 3])
    linx = np.array([cx[i-1] + 0.5*(cx[i-1]-cx[i-2]) for i in ii])
    liny = np.array([cy[i-1] + 0.5*(cy[i-1]-cy[i-2]) for i in ii])
    lin_ex = np.abs(linx - np.array([cx[i] for i in ii]))
    lin_ey = np.abs(liny - np.array([cy[i] for i in ii]))

    print("\n\n========== 真实视频 30 帧 预测自洽性对比 ==========")
    print("旧线性外推(α=0.5): X 平均 %.3fpx / 最大 %.3fpx | Y 平均 %.3fpx / 最大 %.3fpx (帧 %d~%d)" % (lin_ex.mean(), lin_ex.max(), lin_ey.mean(), lin_ey.max(), ii.min(), ii.max()))
    print("%6s | %9s %9s | %8s" % ("alpha", "αβ预测X均值", "αβ预测X最大", "帧间跳变平滑%"))
    for s in summary:
        print("%6.2f | %9.3f %9.3f | %8.0f%%" % (s["alpha"], s["pxm"], s["pxm_"], (1-s["f_jump"]/s["raw_jump"])*100))
    best = min(summary, key=lambda s: s["pxm"])
    print("\n最佳 αβ alpha=%s: X 平均 %.3fpx vs 旧线性 %.3fpx (改善 %.1f%%)" % (best["alpha"], best["pxm"], lin_ex.mean(), (1-best["pxm"]/lin_ex.mean())*100))
    print("=== 真实视频 αβ 滤波(预测自洽) PASS ===" if best["pxm"] < lin_ex.mean() else "⚠️ αβ 未优于旧线性, 需检查")


if __name__ == "__main__":
    main()
