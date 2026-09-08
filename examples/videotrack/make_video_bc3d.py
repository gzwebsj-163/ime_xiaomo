#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
3D 体素世界 → BC 字节码(.kbc) 纯 3D 物体推理 + 三维外形勾勒 + 3D 真实动作
=====================================================================
从 2D 画面推理升维到 3D 体素空间: 推理对象是一整块 D×D×D 的 3D voxel 立方体,
BC 内核在里面做**三维**差分/阈值/质心/行跨度, 勾勒运动物体的三维外形,
追踪它的 3D 真实动作(螺旋平移+自旋+俯仰).

场景: 一枚"火箭"沿螺旋路径前进, 同时绕自身轴自旋、轻微俯仰 —— 3D 真实动作.

BC 内核每帧输出:
  * C3[i]: nx dx_ ny dy_ nz dz_   → 3D 质心 = (nx/dx_, ny/dy_, nz/dz_)
  * Z3[i]: z y lo hi              → 每(z,y)行的运动 x 跨度 → 宿主重建 3D 运动掩码

宿主端: scipy.ndimage 3D 连通域标记 + 表面提取(3D 腐蚀) → 勾勒三维外形
        → 旋转视角渲染(等距投影) → mp4 回转展示.

链路: numpy 生成 3D 体素帧 → 单大数组 .mo → mo2kbc → .kbc → 内核执行
      → 解析 C3/Z3 → 宿主 3D 重建 → 旋转渲染视频

用法: /usr/local/bin/python3 make_video_bc3d.py
      [--d 20] [--frames 36] [--thr 0.5] [--mp4 video_bc3d.mp4]
      [--area 6] 3D 连通域最小体积(voxel), 过滤噪点
"""
import argparse, os, re, subprocess, sys, math
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

BASE   = os.path.dirname(os.path.abspath(__file__))
XIAOMO = "/Users/root1/Desktop/xiaomo"
FFMPEG = "/opt/local/bin/ffmpeg"
MO     = os.path.join(BASE, "video_bc3d_seq.mo")
KBC    = os.path.join(BASE, "video_bc3d_seq.kbc")
OUT    = os.path.join(BASE, "video_bc3d_result.txt")
VIZDIR = os.path.join(BASE, "vframes_bc3d_viz")


# ---------------------------------------------------------------- 3D 场景 #
def rocket_local(X, Y, Z):
    """本地坐标下的火箭占用场 (指向 +Y, 鼻朝上). 返回 bool 数组同形状."""
    r = np.sqrt(X * X + Z * Z)
    bx = np.abs(X); bz = np.abs(Z)
    # 1) 机身: y in [-3,6], 半径 2.2
    body = (Y >= -3.0) & (Y <= 6.0) & (r <= 2.2)
    # 2) 鼻锥: y in [6,9], 半径 2.2 → 0 线性收窄
    t = np.clip((Y - 6.0) / 3.0, 0.0, 1.0)
    nose_r = 2.2 * (1.0 - t)
    nose = (Y > 6.0) & (Y <= 9.0) & (r <= nose_r)
    # 3) 尾翼: y in [-4,-3], 十字四片 (沿 X 轴和 Z 轴)
    fin1 = (Y >= -4.0) & (Y <= -3.0) & (bz <= 0.6) & (bx <= 3.4)
    fin2 = (Y >= -4.0) & (Y <= -3.0) & (bx <= 0.6) & (bz <= 3.4)
    return body | nose | fin1 | fin2


def gen_scene(D, frames, a=(0.0, 0.0, 0.0)):
    """生成 frames 帧 D×D×D 3D 体素 (bool). 火箭沿螺旋前进+自旋+俯仰."""
    c = D / 2.0
    R = max(3.0, D * 0.28)          # 螺旋半径
    y0 = D * 0.35
    y1 = D * 0.68
    # 按 BC 索引约定存储: vol[f][z,y,x], x 最内层 / y 中层 / z 最外层
    # meshgrid 顺序 (Z, Y, X) → axis0=Z, axis1=Y, axis2=X
    Zg, Yg, Xg = np.meshgrid(np.arange(D), np.arange(D), np.arange(D),
                             indexing="ij")  # 形状 (D,D,D)
    Xf, Yf, Zf = Xg.astype(float), Yg.astype(float), Zg.astype(float)
    vol = np.zeros((frames, D, D, D), dtype=bool)
    for f in range(frames):
        th = 2 * math.pi * f / frames * 3.0          # 螺旋角 (3 圈)
        px = c + R * math.cos(th)
        pz = c + R * math.sin(th)
        py = y0 + (y1 - y0) * (f / max(frames - 1, 1))
        ry = th * 0.4                                 # 自旋 (绕 Y)
        rx = 0.25 * math.sin(2 * math.pi * f / frames * 2.0)   # 俯仰 (绕 X)
        cy_, sy_ = math.cos(ry), math.sin(ry)
        cx_, sx_ = math.cos(rx), math.sin(rx)
        # world - center → 先绕X(pitch): y'=cx*y-sx*z, z'=sx*y+cx*z
        #            → 再绕Y(yaw):  x''=cy*x+sy*z', z''=-sy*x+cy*z'
        x0 = Xf - px; y0_ = Yf - py; z0 = Zf - pz
        yp = cx_ * y0_ - sx_ * z0
        zp = sx_ * y0_ + cx_ * z0
        lx = cy_ * x0 + sy_ * zp
        lz = -sy_ * x0 + cy_ * zp
        ly = yp
        vol[f] = rocket_local(lx, ly, lz)
    return vol


# ------------------------------------------------------------- .mo 生成 #
def build_mo(vol, thr):
    """单大数组 frames(0/1) → 3D 差分 + 3D 质心 + 每(z,y)行 x 跨度.
    变量数 ~29 < 40 不超限. 索引 = i*D3 + z*D2 + y*D + x."""
    n, D, _, _ = vol.shape
    D2 = D * D; D3 = D2 * D
    L = []
    L.append(f"# auto-gen make_video_bc3d.py: {n}帧 {D}^3 3D体素差分+质心+行跨度 (thr={thr})")
    L.append(f'>> print >> "=== VIDEO BC3D ({n} frames, D={D}, thr={thr}) ==="')
    L.append(f"void W : int = {D}")
    L.append(f"void H : int = {D}")
    L.append(f"void D2 : int = {D2}")
    L.append(f"void D3 : int = {D3}")
    L.append(f"void T : int = {thr}")
    L.append(f"void NEG : int = 0 - 1")
    # 单大数组 frames (跨行字面量)
    L.append("void frames : int = [")
    buf, lines_flat = [], []
    for img in vol:
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
    # 工作数组: cmin/cmax 按 (z,y) 索引 = z*D + y
    L.append(f"void cmin : int = [{'0,'*(D2-1)}0]")
    L.append(f"void cmax : int = [{'0,'*(D2-1)}0]")
    L.append("void z : int = 0")
    L.append("void y : int = 0")
    L.append("void x : int = 0")
    L.append("void i : int = 0")
    L.append("void k : int = 0")
    L.append("void k2 : int = 0")
    L.append("void k3 : int = 0")
    L.append("void k4 : int = 0")
    L.append("void k5 : int = 0")
    L.append("void zy : int = 0")
    L.append("void i0 : int = 0")
    L.append("void i1 : int = 0")
    L.append("void d : int = 0")
    L.append("void nx : int = 0")
    L.append("void dx_ : int = 0")
    L.append("void ny : int = 0")
    L.append("void dy_ : int = 0")
    L.append("void nz : int = 0")
    L.append("void dz_ : int = 0")
    # 逐帧差分 (帧 i 与 i-1)
    for i in range(1, n):
        bi = i * D3
        bj = (i - 1) * D3
        # 清 cmin/cmax
        L.append("k = 0")
        L.append("while ${k} < ${D2}:")
        L.append("    cmin[${k}] = ${W}")
        L.append("    cmax[${k}] = ${NEG}")
        L.append("    k = ${k} + 1")
        L.append("nx = 0")
        L.append("dx_ = 0")
        L.append("ny = 0")
        L.append("dy_ = 0")
        L.append("nz = 0")
        L.append("dz_ = 0")
        # 3D 三重循环差分
        L.append("z = 0")
        L.append("while ${z} < ${H}:")
        L.append("    y = 0")
        L.append("    while ${y} < ${W}:")
        L.append("        zy = ${z} * ${W} + ${y}")
        L.append("        x = 0")
        L.append("        while ${x} < ${W}:")
        L.append(f"            i0 = {bi} + ${{z}} * ${{D2}} + ${{y}} * ${{W}} + ${{x}}")
        L.append(f"            i1 = {bj} + ${{z}} * ${{D2}} + ${{y}} * ${{W}} + ${{x}}")
        L.append("            d = frames[${i0}] - frames[${i1}]")
        L.append("            if ${d} < 0:")
        L.append("                d = 0 - ${d}")
        L.append("            if ${d} > ${T}:")
        L.append("                if ${x} < cmin[${zy}]:")
        L.append("                    cmin[${zy}] = ${x}")
        L.append("                if ${x} > cmax[${zy}]:")
        L.append("                    cmax[${zy}] = ${x}")
        L.append("                nx = ${nx} + ${x}")
        L.append("                ny = ${ny} + ${y}")
        L.append("                nz = ${nz} + ${z}")
        L.append("                dx_ = ${dx_} + 1")
        L.append("                dy_ = ${dy_} + 1")
        L.append("                dz_ = ${dz_} + 1")
        L.append("            x = ${x} + 1")
        L.append("        y = ${y} + 1")
        L.append("    z = ${z} + 1")
        # 输出 3D 质心(分子/分母) + 每(z,y)行跨度
        L.append(f'>> print >> "C3[{i}]: " >> ${{nx}} >> " " >> ${{dx_}} >> " " >> ${{ny}} >> " " >> ${{dy_}} >> " " >> ${{nz}} >> " " >> ${{dz_}}')
        L.append("k = 0")
        L.append("while ${k} < ${H}:")
        L.append("    k2 = 0")
        L.append("    while ${k2} < ${W}:")
        L.append("        zy = ${k} * ${W} + ${k2}")
        L.append(f'        >> print >> "Z3[{i}]: " >> ${{k}} >> " " >> ${{k2}} >> " " >> cmin[${{zy}}] >> " " >> cmax[${{zy}}]')
        L.append("        k2 = ${k2} + 1")
        L.append("    k = ${k} + 1")
    L.append('>> print >> "=== VIDEO BC3D DONE ==="')
    with open(MO, "w") as fp:
        fp.write("\n".join(L) + "\n")
    return MO


# ------------------------------------------------------------- BC 执行 #
def run_bc():
    r = subprocess.run([os.path.join(XIAOMO, "xiaomo"), "mo2kbc", "-o", KBC, MO],
                       cwd=XIAOMO, capture_output=True, text=True, timeout=1800)
    with open(OUT, "w") as fp:
        fp.write(r.stdout)
        fp.write("\n# ---- STDERR ----\n")
        fp.write(r.stderr)
    return r


# ------------------------------------------------------------- 解析 #
def parse():
    txt = open(OUT).read()
    c3 = {}
    for m in re.finditer(r"C3\[(\d+)\]:\s*\n?\s*([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)\s+([\d.eE+-]+)", txt):
        i = int(m.group(1))
        c3[i] = (float(m.group(2)), float(m.group(3)), float(m.group(4)),
                 float(m.group(5)), float(m.group(6)), float(m.group(7)))
    z3 = {}
    for m in re.finditer(r"Z3\[(\d+)\]:\s*\n?\s*(\d+)\s+(\d+)\s+(-?\d+)\s+(-?\d+)", txt):
        z3[(int(m.group(1)), int(m.group(2)), int(m.group(3)))] = (int(m.group(4)), int(m.group(5)))
    return c3, z3


def rebuild_masks(z3, n, D):
    """用每(z,y)行 x 跨度重建 3D 运动掩码 (diff i, i=1..n-1). 返回 [D,D,D] bool 列表."""
    masks = []
    for i in range(1, n):
        m = np.zeros((D, D, D), dtype=bool)
        for z in range(D):
            for y in range(D):
                key = (i, z, y)
                if key in z3:
                    lo, hi = z3[key]
                    if lo <= hi:
                        m[z, y, max(0, lo):min(D, hi + 1)] = True
        masks.append(m)
    return masks


# ------------------------------------------------------------- 3D 可视化 #
def project(v, D, view_ang, elev_ang, scale, off):
    """等距透视投影: 3D 体素坐标 → 2D 屏幕. 绕 Y 轴旋转视角."""
    c = D / 2.0
    x, y, z = v[0] - c, v[1] - c, v[2] - c
    ca, sa = math.cos(view_ang), math.sin(view_ang)
    ce, se = math.cos(elev_ang), math.sin(elev_ang)
    xr = ca * x + sa * z
    zr = -sa * x + ca * z
    yr = ce * y - se * zr
    zr2 = se * y + ce * zr
    depth = zr2
    sx = off[0] + xr * scale
    sy = off[1] - yr * scale * 0.85
    return sx, sy, depth


def render_3d(vol, masks, c3, D, upscale=14, fps=5, out_mp4="video_bc3d.mp4",
              area=6, trail_len=12):
    """旋转视角 3D 可视化: 等距投影渲染运动表面 + 3D 质心轨迹 + 方向箭头.
    每帧视角绕 Y 旋转一小步, 生成 mp4 让 3D 轮廓可回转观看."""
    os.makedirs(VIZDIR, exist_ok=True)
    for f in os.listdir(VIZDIR):
        os.remove(os.path.join(VIZDIR, f))
    # 固定相机仰角, 视角每帧旋转
    elev = math.radians(28)
    VW = int(D * upscale * 2.6) // 2 * 2
    VH = int(D * upscale * 2.2) // 2 * 2
    off = (VW / 2, VH / 2)
    scale = upscale * 0.9
    trail = []          # 3D 质心轨迹 [(x,y,z)...]
    hist_surf = []      # 历史表面体素 (世界坐标集合)
    N = len(masks)
    # 每个体素一个 3D 方块顶点颜色 (预计算表面用)
    for i in range(N):
        img = Image.new("RGB", (VW, VH), (10, 12, 24))
        d = ImageDraw.Draw(img, "RGBA")
        view_ang = math.radians(-20 - i * (30.0 / max(N, 1)))  # 30° 总回转
        # 绘制参考网格 (D×D 底座)
        for gx in range(0, D + 1, max(1, D // 5)):
            a = project((gx, 0, 0), D, view_ang, elev, scale, off)
            b = project((gx, 0, D - 1), D, view_ang, elev, scale, off)
            d.line([a[:2], b[:2]], fill=(40, 50, 80), width=1)
        for gz in range(0, D + 1, max(1, D // 5)):
            a = project((0, 0, gz), D, view_ang, elev, scale, off)
            b = project((D - 1, 0, gz), D, view_ang, elev, scale, off)
            d.line([a[:2], b[:2]], fill=(40, 50, 80), width=1)
        # 历史表面 (真实动作路径)
        for j, hset in enumerate(hist_surf):
            a = 40 + 35 * j // max(len(hist_surf), 1)
            for v in hset:
                sx, sy, _ = project(v, D, view_ang, elev, scale, off)
                r = max(2, upscale // 2)
                d.ellipse([sx - r, sy - r, sx + r, sy + r], fill=(0, 200, 255, a))
        # 当前帧: 3D 连通域 + 表面提取
        surf = masks[i] & ~ndimage.binary_erosion(masks[i], structure=np.ones((3, 3, 3)))
        lab, nb = ndimage.label(masks[i], structure=np.ones((3, 3, 3)))
        sizes = ndimage.sum(masks[i], lab, range(1, nb + 1)).astype(int)
        slices = ndimage.find_objects(lab)
        blobs = [(sl, int(sz)) for sl, sz in zip(slices, sizes) if sl is not None]
        blobs.sort(key=lambda t: -t[1])
        cur_surf = set()
        big = [b for b in blobs if b[1] >= area]
        for bi, (sl, sz) in enumerate(big):
            col = (255, 50, 60) if bi == 0 else (255, 160, 30)
            zs, ys, xs = sl
            # 该连通域表面体素 → 画出 (主物体实心+描边, 次要半透明)
            zz, yy, xx = np.meshgrid(
                np.arange(zs.start, zs.stop), np.arange(ys.start, ys.stop),
                np.arange(xs.start, xs.stop), indexing="ij")
            sub = lab[zs, ys, xs]
            m_sub = (sub == bi + 1)
            if bi == 0:
                cur_surf |= set(zip(*np.where((surf & (lab == bi + 1)))))
            for (zv, yv, xv) in zip(*np.where(m_sub)):
                if bi > 0 and not (surf & (lab == bi + 1))[zv - zs.start, yv - ys.start, xv - xs.start]:
                    continue
                sx, sy, _ = project((xv, yv, zv), D, view_ang, elev, scale, off)
                r = max(2, upscale // 2 + 1)
                d.rectangle([sx - r, sy - r, sx + r, sy + r],
                            fill=col + (255 if bi == 0 else 150,))
        # 3D 质心轨迹
        if i in c3:
            cx = c3[i][0] / c3[i][1] if c3[i][1] else None
            cy = c3[i][2] / c3[i][3] if c3[i][3] else None
            cz = c3[i][4] / c3[i][5] if c3[i][5] else None
            if cx is not None:
                trail.append((cx, cy, cz))
                trail = trail[-trail_len:]
                for j in range(1, len(trail)):
                    p1 = project(trail[j - 1], D, view_ang, elev, scale, off)
                    p2 = project(trail[j], D, view_ang, elev, scale, off)
                    d.line([p1[:2], p2[:2]], fill=(255, 230, 40), width=3)
                # 当前质心 + 方向箭头 (末两帧 3D 向量)
                p = project((cx, cy, cz), D, view_ang, elev, scale, off)
                r = 6
                d.line([p[0] - r, p[1], p[0] + r, p[1]], fill=(255, 0, 255), width=2)
                d.line([p[0], p[1] - r, p[0], p[1] + r], fill=(255, 0, 255), width=2)
                if len(trail) >= 2:
                    v = np.array(trail[-1]) - np.array(trail[-2])
                    vl = np.linalg.norm(v)
                    if vl > 0.2:
                        pa = project(trail[-1], D, view_ang, elev, scale, off)
                        u = v / vl
                        tip = (pa[0] + u[0] * 20, pa[1] - u[1] * 20 * 0.85)
                        d.line([pa[:2], tip], fill=(60, 255, 120), width=3)
        # 文字信息
        d.text((8, 8), f"BC 3D inference frame {i+1}/{N}", fill=(0, 220, 255))
        big_sz = big[0][1] if big else 0
        d.text((8, VH - 22),
               f"3D centroid=({c3[i][0]/c3[i][1]:.1f},{c3[i][2]/c3[i][3]:.1f},{c3[i][4]/c3[i][5]:.1f})" if i in c3 and c3[i][1] else "no motion",
               fill=(255, 230, 40))
        d.text((8, VH - 44), f"blobs={len(big)} main_surf_voxels={len(cur_surf)}", fill=(255, 160, 30))
        img.save(os.path.join(VIZDIR, f"v{i:03d}.png"))
        hist_surf.append(cur_surf)
        hist_surf = hist_surf[-trail_len:]
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
    ap.add_argument("--d", type=int, default=20, help="体素立方体边长(体素数)")
    ap.add_argument("--frames", type=int, default=36, help="帧数")
    ap.add_argument("--thr", type=float, default=0.5, help="差分阈值")
    ap.add_argument("--area", type=int, default=6, help="3D连通域最小体积(voxel)")
    ap.add_argument("--up", type=int, default=14, help="可视化放大")
    ap.add_argument("--fps", type=float, default=5.0)
    ap.add_argument("--mp4", type=str, default="video_bc3d.mp4")
    a = ap.parse_args()

    print(f"生成 3D 场景: {a.frames}帧 {a.d}^3 体素 (火箭螺旋+自旋+俯仰) ...")
    vol = gen_scene(a.d, a.frames)
    occ = vol.astype(int)
    print(f"  占用体素总数: {int(occ.sum())} / {a.frames * a.d**3}  ({occ.sum()/ (a.frames*a.d**3)*100:.1f}%)")
    build_mo(occ, a.thr)
    print(f"  .mo 生成: {MO} ({os.path.getsize(MO)/1e6:.2f}MB)")
    r = run_bc()
    if r.returncode != 0:
        print("mo2kbc 失败, 原始输出:"); print(open(OUT).read()[:3000]); sys.exit(1)
    c3, z3 = parse()
    if len(c3) != a.frames - 1:
        print("⚠️ 质心数不匹配, 原始输出见 video_bc3d_result.txt")
        print(open(OUT).read()[:2000]); sys.exit(1)
    masks = rebuild_masks(z3, a.frames, a.d)
    print(f"BC 3D 推理完成: {a.frames-1} 差分帧, 3D 掩码重建 {len(masks)} 帧")
    # 汇总
    total_blobs = 0
    for i, m in enumerate(masks):
        lab, nb = ndimage.label(m, structure=np.ones((3, 3, 3)))
        sizes = ndimage.sum(m, lab, range(1, nb + 1)).astype(int)
        slices = ndimage.find_objects(lab)
        blobs = [(sl, int(sz)) for sl, sz in zip(slices, sizes) if sl is not None]
        blobs.sort(key=lambda t: -t[1])
        big = [b for b in blobs if b[1] >= a.area]
        total_blobs += len(big)
        cx, cy, cz = c3[i + 1][0] / c3[i + 1][1], c3[i + 1][2] / c3[i + 1][3], c3[i + 1][4] / c3[i + 1][5]
        if big:
            zs, ys, xs = big[0][0]
            print(f"  d{i+1:3d} 主物体3D bbox=({xs.start},{ys.start},{zs.start})-({xs.stop-1},{ys.stop-1},{zs.stop-1}) 体积={big[0][1]} 质心=({cx:.1f},{cy:.1f},{cz:.1f})")
        else:
            print(f"  d{i+1:3d} 无显著运动 质心=({cx:.1f},{cy:.1f},{cz:.1f})")
    print(f"\n≥{a.area}voxel 连通域总数: {total_blobs}")
    out = render_3d(vol, masks, c3, a.d, upscale=a.up, fps=a.fps, out_mp4=a.mp4, area=a.area)
    print(f"\n=== BC 纯 3D 物体推理链路 PASS ===")
    print(f"  .mo   : {MO} ({os.path.getsize(MO)/1e6:.2f}MB)")
    print(f"  .kbc  : {KBC} ({os.path.getsize(KBC)/1e6:.2f}MB)")
    print(f"  mp4   : {out}")
    print(f"  viz   : {VIZDIR}/v*.png")


if __name__ == "__main__":
    main()
