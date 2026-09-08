#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
线条跟踪视频推理编排器 v2 (BC 内核 = xiaomo VM 张量管线)
=======================================================
v2 修复:
  1) 推出阈值化抗噪: colt = relu(col - thrvec) (nd_relu+nd_sub 纯张量算)
     去掉背景/噪声偏置对质心的污染 (v1 背景=8+噪声σ=8 全被加权)
  2) 生成侧线条固定 2 列宽 (int(cx), int(cx)+1) → 消除 1~2 列跳变的质心抖动
  3) 速度自适应: cx ∈ [0.2w, 0.75w] 保证线条不出画布 (v1 出画布质心被截断拉回)

流程:
  1) 生成 N 帧"视频"(带噪声/背景/移动线条) -> 自动生成 .mo
  2) 跑 ./xiaomo run -> 每帧 VM 列投影+阈值化+质心定位线条 x
  3) 解析输出 -> 与真值对比(误差/线性速度) -> 写 PGM 可视化

运动预测 (--predict):
  pred[i] = cx[i-1] + alpha * (cx[i-1] - cx[i-2])   (加权线性外推)
    alpha=1.0 -> 朴素线性外推 2*prev - prev2 (瞬时速度全外推, 噪声放大 2x)
    alpha<1.0 -> 指数平滑/阻尼外推 (只外推部分速度, 降噪但牺牲高速跟随)
  帧 i>=2 起预测; 精度由 --alpha 权衡.

用: /usr/local/bin/python3 make_line_track.py [--frames 16] [--w 32] [--h 16] [--noise 8] [--bg 8] [--thr 20] [--no-thr] [--blob --predict --alpha 1.0]
"""
import argparse, os, re, subprocess, sys

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"          # VM 根
MO     = os.path.join(BASE, "line_track_seq.mo")
OUT    = os.path.join(BASE, "track_result.txt")

def gen_frames(n, w, h, noise, bg, width=2, diag=False, blob=False):
    """生成 n 帧灰度(0-255):
    vert:  固定 width 列竖直线条匀速右移 (竖直跟踪)
    diag:  45°斜线带 (宽width) 沿对角线匀速移动 (细长结构, 演示质心法边界)
    blob:  3x3 紧凑方块匀速 2D 移动 (紧凑目标 2D 跟踪, 质心法有效)
    cx/cy 限制在画布内 [0.2, 0.8]."""
    import numpy as np
    frames, truth = [], []
    x0, x1 = w*0.20, w*0.75
    y0, y1 = h*0.20, h*0.75
    vx = (x1 - x0) / max(1, n-1)
    vy = (y1 - y0) / max(1, n-1)
    for t in range(n):
        cx, cy = x0 + vx*t, y0 + vy*t
        truth.append((cx, cy))
        img = np.full((h, w), bg)
        if blob:
            # 3x3 紧凑方块
            for y in range(h):
                for x in range(w):
                    if abs(x-cx) <= 1 and abs(y-cy) <= 1:
                        img[y, x] = 220.0
        elif not diag:
            x_base = int(round(cx))
            for k in range(width):
                x = x_base + k - (width//2)
                if 0 <= x < w:
                    img[:, x] = 220.0
        else:
            # 45°斜线带: 每行 y 对应 x = cx + (y-cy), 沿斜线放 width 个像素
            for y in range(h):
                xc = cx + (y - cy)
                x_base = int(round(xc))
                for k in range(width):
                    x = x_base + k - (width//2)
                    if 0 <= x < w:
                        img[y, x] = 220.0
        img += np.random.normal(0, noise, (h, w))
        img = np.clip(img, 0, 255)
        frames.append(img)
    return frames, truth

def mo_array(name, img, w):
    rows = []
    for row in img:
        rows.append("[" + ",".join(f"{v:.1f}" for v in row) + "]")
    return f"void {name} : int = tensor([\n  " + ",\n  ".join(rows) + "\n])"

def build_mo(frames, truth, thr, diag=False, blob=False, predict=False, alpha=1.0):
    h, w = frames[0].shape
    n = len(frames)
    L = []
    L.append(f"# auto-gen by make_line_track.py v2: {n}帧 {w}x{h}, 帧级阈值化+投影+质心线条跟踪 (thr={thr}, diag={diag}, blob={blob}, predict={predict}, alpha={alpha})")
    L.append(f">> print >> \"=== LINE TRACK SEQ ({n} frames, thr={thr}, diag={diag}, blob={blob}) ===\"")
    # 列索引向量 + 行索引向量
    L.append(f"void xidx : int = tensor([{','.join(map(str, range(w)))}])")
    L.append(f"void yidx : int = tensor([{','.join(map(str, range(h)))}])")
    if thr > 0:
        # 帧级阈值张量 (h,w) 全 thr —— 所有帧共用; 像素级阈值化(非列级!)
        rows = []
        for y in range(h):
            rows.append("[" + ",".join([str(thr)]*w) + "]")
        L.append("void thrframe : int = tensor([\n  " + ",\n  ".join(rows) + "\n])")
    for i, img in enumerate(frames):
        L.append(mo_array(f"f{i}", img, w))
    # 每帧质心 (展开, 无循环): 帧级阈值化 -> 列/行投影 -> x/y 质心
    H2 = h // 2
    for i in range(n):
        if thr > 0:
            # 像素级阈值化: 背景8/噪声 -> 0, 线条220 -> 200 (纯张量算子)
            L.append(f"void ft{i} : int = nd_relu(nd_sub(${{f{i}}}, ${{thrframe}}))")
            L.append(f"void col{i} : int = nd_sum(${{ft{i}}}, 0)")
            if diag:
                # 上下半切分 (斜线2D+斜率): 上半 [0,H2)x[0,W), 下半 [H2,H)x[0,W)
                L.append(f"void ftt{i} : int = nd_slice(${{ft{i}}}, [0,0], [{H2},{w}], [1,1])")
                L.append(f"void ftb{i} : int = nd_slice(${{ft{i}}}, [{H2},0], [{h},{w}], [1,1])")
                L.append(f"void colt{i} : int = nd_sum(${{ftt{i}}}, 0)")
                L.append(f"void colb{i} : int = nd_sum(${{ftb{i}}}, 0)")
        else:
            L.append(f"void col{i} : int = nd_sum(${{f{i}}}, 0)")
            if diag:
                L.append(f"void ftt{i} : int = nd_slice(${{f{i}}}, [0,0], [{H2},{w}], [1,1])")
                L.append(f"void ftb{i} : int = nd_slice(${{f{i}}}, [{H2},0], [{h},{w}], [1,1])")
                L.append(f"void colt{i} : int = nd_sum(${{ftt{i}}}, 0)")
                L.append(f"void colb{i} : int = nd_sum(${{ftb{i}}}, 0)")
        L.append(f"void num{i} : int = nd_sum(nd_mul(${{col{i}}}, ${{xidx}}), -1)")
        L.append(f"void den{i} : int = nd_sum(${{col{i}}}, -1)")
        L.append(f"void cx{i}  : int = nd_div(${{num{i}}}, ${{den{i}}})")
        if diag:
            # 上半/下半 x 质心 + 质量
            L.append(f"void numt{i} : int = nd_sum(nd_mul(${{colt{i}}}, ${{xidx}}), -1)")
            L.append(f"void dent{i} : int = nd_sum(${{colt{i}}}, -1)")
            L.append(f"void cxt{i} : int = nd_div(${{numt{i}}}, ${{dent{i}}})")
            L.append(f"void numb{i} : int = nd_sum(nd_mul(${{colb{i}}}, ${{xidx}}), -1)")
            L.append(f"void denb{i} : int = nd_sum(${{colb{i}}}, -1)")
            L.append(f"void cxb{i} : int = nd_div(${{numb{i}}}, ${{denb{i}}})")
            # 中心 y = H2/2 * (denT + 3*denB)/(denT+denB)  (上下半质量线性插值)
            # 注: 旧内核 scale 与 ND 引擎不互通; 数字字面量=0-dim标量(避免[1]张量)
            L.append(f"void denall{i} : int = nd_add(${{dent{i}}}, ${{denb{i}}})")
            L.append(f"void denb3{i} : int = nd_mul(${{denb{i}}}, 3)")
            L.append(f"void numy{i} : int = nd_add(${{dent{i}}}, ${{denb3{i}}})")
            L.append(f"void cy{i} : int = nd_mul(nd_div(${{numy{i}}}, ${{denall{i}}}), {H2/2})")
            # 斜率 = (cxb - cxt) / H2  (px/row)
            L.append(f"void sl{i} : int = nd_div(nd_sub(${{cxb{i}}}, ${{cxt{i}}}), {H2})")
            L.append(f">> print >> \"CX[{i}]:\" >> ${{cx{i}}} >> \" CY[{i}]:\" >> ${{cy{i}}} >> \" SL[{i}]:\" >> ${{sl{i}}} >> \" GT:({truth[i][0]:.2f},{truth[i][1]:.2f})\"")
        elif blob:
            # 紧凑目标: 行投影 -> y 质心 (x 质心已由 col 求得)
            L.append(f"void row{i} : int = nd_sum(${{ft{i}}}, 1)")
            L.append(f"void numy{i} : int = nd_sum(nd_mul(${{row{i}}}, ${{yidx}}), -1)")
            L.append(f"void deny{i} : int = nd_sum(${{row{i}}}, -1)")
            L.append(f"void cy{i} : int = nd_div(${{numy{i}}}, ${{deny{i}}})")
            if predict and i >= 2:
                # 加权线性外推: pred[i] = prev + alpha*(prev - prev2)
                #   -> (1+alpha)*prev - alpha*prev2  (纯张量标量运算)
                #   alpha=1.0 朴素外推(瞬时速度全外推, 噪声*2)
                #   alpha<1.0 阻尼外推(部分速度, 降噪牺牲高速跟随)
                L.append(f"void dpx{i} : int = nd_sub(${{cx{i-1}}}, ${{cx{i-2}}})")
                L.append(f"void apx{i} : int = nd_mul(${{dpx{i}}}, {alpha})")
                L.append(f"void px{i}  : int = nd_add(${{cx{i-1}}}, ${{apx{i}}})")
                L.append(f"void dpy{i} : int = nd_sub(${{cy{i-1}}}, ${{cy{i-2}}})")
                L.append(f"void apy{i} : int = nd_mul(${{dpy{i}}}, {alpha})")
                L.append(f"void py{i}  : int = nd_add(${{cy{i-1}}}, ${{apy{i}}})")
                L.append(f">> print >> \"CX[{i}]:\" >> ${{cx{i}}} >> \" CY[{i}]:\" >> ${{cy{i}}} >> \" PX[{i}]:\" >> ${{px{i}}} >> \" PY[{i}]:\" >> ${{py{i}}} >> \" GT:({truth[i][0]:.2f},{truth[i][1]:.2f})\"")
            else:
                L.append(f">> print >> \"CX[{i}]:\" >> ${{cx{i}}} >> \" CY[{i}]:\" >> ${{cy{i}}} >> \" GT:({truth[i][0]:.2f},{truth[i][1]:.2f})\"")
        else:
            L.append(f">> print >> \"CX[{i}]:\" >> ${{cx{i}}} >> \" GT:{truth[i]:.2f}\"")
    L.append('>> print >> "=== LINE TRACK SEQ DONE ===\"')
    with open(MO, "w") as fp:
        fp.write("\n".join(L) + "\n")

def run_vm():
    r = subprocess.run([os.path.join(XIAOMO, "xiaomo"), "run", MO],
                       cwd=XIAOMO, capture_output=True, text=True, timeout=600)
    with open(OUT, "w") as fp:
        fp.write(r.stdout)
    return r

def parse(truth, diag=False):
    """xiaomo print 会把每个 token 换行输出 (CX[i]:\n数值\n  GT:..), 需读全文跨行匹配.
    diag: 同时解析 CY[i] 和 SL[i](斜率 px/row)."""
    xs, ys, sls = [], [], []
    txt = open(OUT).read()
    for m in re.finditer(r"CX\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
        xs.append(float(m.group(2)))
    if diag:
        for m in re.finditer(r"CY\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
            ys.append(float(m.group(2)))
        for m in re.finditer(r"SL\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
            sls.append(float(m.group(2)))
    return (xs, ys, sls) if diag else xs

def parse_predict():
    """解析 PX[i]/PY[i] 线性外推预测值 (blob+predict 模式, 帧 i>=2)."""
    px, py = [], []
    txt = open(OUT).read()
    for m in re.finditer(r"PX\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
        px.append((int(m.group(1)), float(m.group(2))))
    for m in re.finditer(r"PY\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)", txt):
        py.append((int(m.group(1)), float(m.group(2))))
    return px, py

def write_pgm(frames, xs, truth, path):
    import numpy as np
    h, w = frames[0].shape
    canvas = frames[-1].astype(float).copy()
    n = len(xs)
    for t in range(n):
        xo = int(round(xs[t]))
        yo = int(round(t * (h-1) / max(1, n-1)))
        for dy in range(-1, 2):
            for dx in range(-1, 2):
                yy, xx = yo+dy, xo+dx
                if 0 <= xx < w and 0 <= yy < h:
                    canvas[yy, xx] = 255.0
    # 真值轨迹(绿色通道127 叠加白点下方)
    with open(path, "wb") as fp:
        fp.write(f"P5\n{w} {h}\n255\n".encode())
        fp.write(np.clip(canvas,0,255).astype(np.uint8).tobytes())

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frames", type=int, default=16)
    ap.add_argument("--w", type=int, default=32)
    ap.add_argument("--h", type=int, default=16)
    ap.add_argument("--noise", type=float, default=8.0)
    ap.add_argument("--bg", type=float, default=8.0)
    ap.add_argument("--thr", type=float, default=20.0, help="阈值化阈值(0=关闭)")
    ap.add_argument("--no-thr", action="store_true", help="关闭阈值化(对照)")
    ap.add_argument("--diag", action="store_true", help="45°斜线2D跟踪(输出x+y质心)")
    ap.add_argument("--blob", action="store_true", help="3x3紧凑方块2D跟踪(输出x+y质心)")
    ap.add_argument("--predict", action="store_true", help="线性外推运动预测(用前两帧质心预测下一帧, 验证轨迹预测)")
    ap.add_argument("--alpha", type=float, default=1.0, help="预测外推权重: 1.0=朴素线性外推(噪声放大), <1.0=阻尼/指数平滑(降噪牺牲高速跟随)")
    a = ap.parse_args()
    if a.no_thr: a.thr = 0.0
    if a.diag and a.blob:
        print("--diag 与 --blob 互斥"); sys.exit(1)
    mode2d = a.diag or a.blob

    frames, truth = gen_frames(a.frames, a.w, a.h, a.noise, a.bg, diag=a.diag, blob=a.blob)
    build_mo(frames, truth, a.thr, diag=a.diag, blob=a.blob, predict=a.predict, alpha=a.alpha)
    print(f"已生成 {a.frames} 帧 {a.w}x{a.h} 移动线条序列 (thr={a.thr}, diag={a.diag}, blob={a.blob}, predict={a.predict}, alpha={a.alpha}) → {MO}")

    r = run_vm()
    if r.returncode != 0:
        print("VM 运行失败:\n", r.stdout[-3000:], r.stderr[-2000:])
        sys.exit(1)

    parsed = parse(truth, diag=mode2d)
    if mode2d:
        xs, ys, sls = parsed
        check = xs if len(xs) <= len(ys) else ys
    else:
        xs, ys, sls, check = parsed, None, None, parsed
    print(f"VM 输出解析: 检测到 {len(check)} 帧质心 (共 {a.frames} 帧)")
    if len(check) != a.frames:
        print("⚠️ 质心数不匹配, 原始输出见 track_result.txt")
        print(open(OUT).read()[:1500])
        sys.exit(1)

    import numpy as np
    if mode2d:
        tx = np.array([t[0] for t in truth]); ty = np.array([t[1] for t in truth])
        ex = np.abs(np.array(xs) - tx); ey = np.abs(np.array(ys) - ty)
        tag = "斜线2D" if a.diag else "紧凑blob 2D"
        print(f"\n帧   真值x    检测x    err_x   真值y    检测y    err_y  " + ("检测斜率(真值≈1)" if a.diag else ""))
        for i in range(a.frames):
            if a.diag:
                print(f"{i:3d}  {tx[i]:7.2f}  {xs[i]:9.2f}  {ex[i]:6.2f}  {ty[i]:7.2f}  {ys[i]:9.2f}  {ey[i]:6.2f}  {sls[i]:8.2f}")
            else:
                print(f"{i:3d}  {tx[i]:7.2f}  {xs[i]:9.2f}  {ex[i]:6.2f}  {ty[i]:7.2f}  {ys[i]:9.2f}  {ey[i]:6.2f}")
        print(f"\nX: 平均误差 {ex.mean():.3f}px | 最大 {ex.max():.3f}px | 速度拟合 {np.polyfit(range(a.frames), xs, 1)[0]:.3f} (真值 {np.polyfit(range(a.frames), tx, 1)[0]:.3f})")
        print(f"Y: 平均误差 {ey.mean():.3f}px | 最大 {ey.max():.3f}px | 速度拟合 {np.polyfit(range(a.frames), ys, 1)[0]:.3f} (真值 {np.polyfit(range(a.frames), ty, 1)[0]:.3f})")
        if a.diag:
            print(f"斜率: 平均 {np.mean(sls):.3f} px/row (真值≈1.0) | 波动 σ={np.std(sls):.3f}")
        ok = ex.mean() < 1.0 and ey.mean() < 1.0
        print(f"\n=== {tag}跟踪 PASS ===" if ok else "\n⚠️ 误差偏大, 需检查")
        if a.predict and a.blob:
            px, py = parse_predict()
            # 预测帧 i>=2 起; 对比真值 tx[i]/ty[i]
            npx = len(px)
            if npx == a.frames - 2:
                pxs = np.array([v for _, v in px]); pys = np.array([v for _, v in py])
                idx = np.array([i for i, _ in px])
                pex = np.abs(pxs - tx[idx]); pey = np.abs(pys - ty[idx])
                print(f"\n帧   真值x    预测x   err_px   真值y    预测y   err_py   (线性外推)")
                for k in range(npx):
                    i = idx[k]
                    print(f"{i:3d}  {tx[i]:7.2f}  {pxs[k]:8.2f}  {pex[k]:6.2f}  {ty[i]:7.2f}  {pys[k]:8.2f}  {pey[k]:6.2f}")
                print(f"\n预测误差: X 平均 {pex.mean():.3f}px | 最大 {pex.max():.3f}px | Y 平均 {pey.mean():.3f}px | 最大 {pey.max():.3f}px")
                pok = pex.mean() < 1.0 and pey.mean() < 1.0
                print("=== 运动预测(线性外推) PASS ===" if pok else "\n⚠️ 预测误差偏大")
            else:
                print(f"⚠️ 预测点数 {npx} != 帧数-2 ({a.frames-2})")
    else:
        err = np.abs(np.array(xs) - np.array(truth))
        print(f"\n帧   真值x    检测x     误差")
        for i in range(a.frames):
            print(f"{i:3d}  {truth[i]:7.2f}  {xs[i]:9.2f}  {err[i]:6.2f}")
        print(f"\n平均误差: {err.mean():.3f} px | 最大误差: {err.max():.3f} px")
        print(f"轨迹速度(线性拟合斜率): {np.polyfit(range(a.frames), xs, 1)[0]:.3f} px/帧 (真值 {(a.w*0.75-a.w*0.2)/max(1,a.frames-1):.3f})")
        print("\n=== 线条跟踪视频推理链路 PASS ===" if err.mean() < 1.0 else "\n⚠️ 误差偏大, 需检查")

    write_pgm(frames, xs, truth if not a.diag else [t[0] for t in truth], os.path.join(BASE, "track_viz.pgm"))
    print(f"可视化已写: {BASE}/track_viz.pgm")

if __name__ == "__main__":
    main()
