#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
αβ 滤波器 (匀速模型卡尔曼等价物) —— 检测+跟踪+预测三段统一闭环
================================================================
对上一轮线性外推的升级:
  旧: pred[i] = cx[i-1] + alpha*(cx[i-1]-cx[i-2])   (用带噪原始检测外推, 误差传播)
  新: αβ滤波器  状态(位置F + 速度V) 预测-更新闭环:
      预测: P{i} = F{i-1} + V{i-1}                  (用修正态外推, 非带噪原始值!)
      更新: R{i} = z{i} - P{i}                      (残差)
            F{i} = P{i} + alpha * R{i}              (位置修正)
            V{i} = V{i-1} + beta  * R{i}            (速度修正)
      beta = alpha^2 / (2-alpha)  (临界阻尼, 匀速最优)
  输出: 检测z / 滤波态F / 预测态P 三组 vs 真值对比 + alpha 扫参 + 旧线性外推基线对照.
  帧 i>=1 起即可预测 (只需前一帧状态, 比旧的 i>=2 少一帧滞后).

用法: /usr/local/bin/python3 make_ab_filter.py [--frames 16] [--w 48] [--h 32]
    [--noise 8] [--bg 8] [--thr 20] [--alphas 0.3,0.5,0.7,0.9] [--seed 42]

⚠️ .mo print 字符串字面量必须 \"...\" 首尾都转义(豆子词法), 用 EQ=chr(92)+'\"'
   显式构造, 避免各层转义累计出错.
"""
import argparse, os, re, subprocess, sys
import numpy as np

from make_line_track import gen_frames, mo_array

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"          # VM 根
MO     = os.path.join(BASE, "ab_filter_seq.mo")
OUT    = os.path.join(BASE, "ab_filter_result.txt")

EQ = '"'            # .mo print 字符串字面量用裸双引号 (原版 make_line_track.py 实测格式)


def build_ab_mo(frames, thr, alpha):
    """生成 .mo: 帧级阈值化 + 列/行投影质心(z) + αβ滤波(F/V/P)."""
    h, w = frames[0].shape
    n = len(frames)
    beta = alpha * alpha / (2 - alpha)
    L = []
    L.append("# auto-gen by make_ab_filter.py: %d帧 %dx%d blob 2D + αβ滤波 (alpha=%s, beta=%.4f)" % (n, w, h, alpha, beta))
    L.append(">> print >> " + EQ + "=== AB FILTER SEQ (%d frames, alpha=%s, beta=%.4f) ===" % (n, alpha, beta) + EQ)
    L.append("void xidx : int = tensor([" + ",".join(map(str, range(w))) + "])")
    L.append("void yidx : int = tensor([" + ",".join(map(str, range(h))) + "])")
    rows = []
    for y in range(h):
        rows.append("[" + ",".join([str(thr)] * w) + "]")
    L.append("void thrframe : int = tensor([\n  " + ",\n  ".join(rows) + "\n])")
    for i, img in enumerate(frames):
        L.append(mo_array("f%d" % i, img, w))
    for i in range(n):
        L.append("void ft%d : int = nd_relu(nd_sub(${f%d}, ${thrframe}))" % (i, i))
        L.append("void col%d : int = nd_sum(${ft%d}, 0)" % (i, i))
        L.append("void row%d : int = nd_sum(${ft%d}, 1)" % (i, i))
        L.append("void num%d : int = nd_sum(nd_mul(${col%d}, ${xidx}), -1)" % (i, i))
        L.append("void den%d : int = nd_sum(${col%d}, -1)" % (i, i))
        L.append("void zx%d : int = nd_div(${num%d}, ${den%d})" % (i, i, i))
        L.append("void numy%d : int = nd_sum(nd_mul(${row%d}, ${yidx}), -1)" % (i, i))
        L.append("void deny%d : int = nd_sum(${row%d}, -1)" % (i, i))
        L.append("void zy%d : int = nd_div(${numy%d}, ${deny%d})" % (i, i, i))
        L.append(">> print >> " + EQ + "ZX[%d]:" % i + EQ + " >> ${zx%d} >> " % i + EQ + " ZY[%d]:" % i + EQ + " >> ${zy%d} >> " % i + EQ + EQ)
    L.append("void Fx0 : int = ${zx0}")
    L.append("void Fy0 : int = ${zy0}")
    L.append("void Vx0 : int = nd_sub(${zx0}, ${zx0})")
    L.append("void Vy0 : int = nd_sub(${zy0}, ${zy0})")
    for i in range(1, n):
        L.append("void Px%d : int = nd_add(${Fx%d}, ${Vx%d})" % (i, i-1, i-1))
        L.append("void Py%d : int = nd_add(${Fy%d}, ${Vy%d})" % (i, i-1, i-1))
        L.append("void Rx%d : int = nd_sub(${zx%d}, ${Px%d})" % (i, i, i))
        L.append("void Ry%d : int = nd_sub(${zy%d}, ${Py%d})" % (i, i, i))
        L.append("void aRx%d : int = nd_mul(${Rx%d}, %s)" % (i, i, alpha))
        L.append("void aRy%d : int = nd_mul(${Ry%d}, %s)" % (i, i, alpha))
        L.append("void Fx%d : int = nd_add(${Px%d}, ${aRx%d})" % (i, i, i))
        L.append("void Fy%d : int = nd_add(${Py%d}, ${aRy%d})" % (i, i, i))
        L.append("void bRx%d : int = nd_mul(${Rx%d}, %s)" % (i, i, beta))
        L.append("void bRy%d : int = nd_mul(${Ry%d}, %s)" % (i, i, beta))
        L.append("void Vx%d : int = nd_add(${Vx%d}, ${bRx%d})" % (i, i-1, i))
        L.append("void Vy%d : int = nd_add(${Vy%d}, ${bRy%d})" % (i, i-1, i))
        L.append(">> print >> " + EQ + "PX[%d]:" % i + EQ + " >> ${Px%d} >> " % i + EQ + " PY[%d]:" % i + EQ + " >> ${Py%d} >> " % i + EQ + " FX[%d]:" % i + EQ + " >> ${Fx%d} >> " % i + EQ + " FY[%d]:" % i + EQ + " >> ${Fy%d} >> " % i + EQ + EQ)
    L.append(">> print >> " + EQ + "=== AB FILTER DONE ===" + EQ)
    with open(MO, "w") as fp:
        fp.write("\n".join(L) + "\n")


def run_vm():
    r = subprocess.run([os.path.join(XIAOMO, "xiaomo"), "run", MO],
                       cwd=XIAOMO, capture_output=True, text=True, timeout=600)
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
    return (grab("ZX"), grab("ZY"), grab("PX"), grab("PY"), grab("FX"), grab("FY"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--w", type=int, default=48)
    ap.add_argument("--h", type=int, default=32)
    ap.add_argument("--noise", type=float, default=8.0)
    ap.add_argument("--bg", type=float, default=8.0)
    ap.add_argument("--thr", type=float, default=20.0)
    ap.add_argument("--alphas", default="0.3,0.5,0.7,0.9")
    ap.add_argument("--seed", type=int, default=42)
    a = ap.parse_args()
    alphas = [float(x) for x in a.alphas.split(",")]

    np.random.seed(a.seed)
    frames, truth = gen_frames(a.frames, a.w, a.h, a.noise, a.bg, blob=True)
    tx = np.array([t[0] for t in truth]); ty = np.array([t[1] for t in truth])
    print("αβ 滤波扫参: %d帧 %dx%d blob2D noise=%s thr=%s seed=%s alphas=%s" % (a.frames, a.w, a.h, a.noise, a.thr, a.seed, alphas))

    summary = []
    zx_all = zy_all = None
    for alpha in alphas:
        build_ab_mo(frames, a.thr, alpha)
        r = run_vm()
        if r.returncode != 0:
            print("VM 运行失败 (alpha=%s):\n%s\n%s" % (alpha, r.stdout[-3000:], r.stderr[-2000:]))
            sys.exit(1)
        zx, zy, px, py, fx, fy = parse_all()
        if zx_all is None:
            zx_all, zy_all = zx, zy
        idx = np.array(sorted(fx.keys()))
        fxe = np.abs(np.array([fx[i] for i in idx]) - tx[idx])
        fye = np.abs(np.array([fy[i] for i in idx]) - ty[idx])
        pxe = np.abs(np.array([px[i] for i in idx]) - tx[idx])
        pye = np.abs(np.array([py[i] for i in idx]) - ty[idx])
        truth_vx = (tx[-1] - tx[0]) / max(1, a.frames - 1)
        truth_vy = (ty[-1] - ty[0]) / max(1, a.frames - 1)
        vest_vx = fx[max(idx)] - fx[max(idx) - 1]
        vest_vy = fy[max(idx)] - fy[max(idx) - 1]
        summary.append(dict(alpha=alpha, fxm=fxe.mean(), fxm_=fxe.max(),
                            fym=fye.mean(), fym_=fye.max(),
                            pxm=pxe.mean(), pxm_=pxe.max(),
                            pym=pye.mean(), pym_=pye.max(),
                            vest_vx=vest_vx, vest_vy=vest_vy,
                            truth_vx=truth_vx, truth_vy=truth_vy))
        print("\n--- alpha=%s (beta=%.4f) ---" % (alpha, alpha*alpha/(2-alpha)))
        print("  滤波态F: X 平均 %.3fpx / 最大 %.3fpx | Y 平均 %.3fpx / 最大 %.3fpx" % (fxe.mean(), fxe.max(), fye.mean(), fye.max()))
        print("  预测态P: X 平均 %.3fpx / 最大 %.3fpx | Y 平均 %.3fpx / 最大 %.3fpx" % (pxe.mean(), pxe.max(), pye.mean(), pye.max()))
        print("  末帧速度(滤波): X %.3f / Y %.3f px/帧 (真值 %.3f/%.3f)" % (vest_vx, vest_vy, truth_vx, truth_vy))

    zi = np.array(sorted(zx_all.keys()))
    ze = np.abs(np.array([zx_all[i] for i in zi]) - tx[zi])
    zey = np.abs(np.array([zy_all[i] for i in zi]) - ty[zi])
    oi = np.array([i for i in zi if i >= 2])
    opx = np.array([zx_all[i-1] + (zx_all[i-1] - zx_all[i-2]) for i in oi])
    opy = np.array([zy_all[i-1] + (zy_all[i-1] - zy_all[i-2]) for i in oi])
    opex = np.abs(opx - tx[oi]); opey = np.abs(opy - ty[oi])

    print("\n\n========== 汇总对比 (X 轴; Y 轴趋势一致) ==========")
    print("原始检测 z   : 平均 %.3fpx / 最大 %.3fpx" % (ze.mean(), ze.max()))
    print("旧线性外推   : 平均 %.3fpx / 最大 %.3fpx (帧 %d~%d)" % (opex.mean(), opex.max(), oi.min(), oi.max()))
    print("%6s | %9s %9s | %9s %9s | %7s" % ("alpha", "滤波F均值", "滤波F最大", "预测P均值", "预测P最大", "末帧Vx"))
    for s in summary:
        print("%6.2f | %9.3f %9.3f | %9.3f %9.3f | %7.3f" % (s["alpha"], s["fxm"], s["fxm_"], s["pxm"], s["pxm_"], s["vest_vx"]))
    best = min(summary, key=lambda s: s["fxm"])
    print("\n最佳滤波 alpha=%s: 平均 %.3fpx vs 原始 %.3fpx (降低 %.1f%%)" % (best["alpha"], best["fxm"], ze.mean(), (1-best["fxm"]/ze.mean())*100))
    print("=== αβ 滤波器 (匀速卡尔曼等价物) 三段闭环 PASS ===" if best["fxm"] < ze.mean() else "⚠️ 滤波未优于原始检测, 需检查")


if __name__ == "__main__":
    main()
