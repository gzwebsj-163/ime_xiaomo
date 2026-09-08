#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
人脸 / 身体 分割 → 各自转 mesh → 渲染
========================================
输入: video_bc3d_texture_vol.npz (vol=时空体素已去背景, tex=真实纹理, spine=质心)
核心: **逐帧自适应分割** —— 因吃播中人物低头/抬头, 头部在每帧的 y 位置变化大,
      不能固定 y 阈值. 改为每帧按该帧轮廓的垂直跨度取"上部比例"为人脸区,
      其余为身体区, 各自沿时间堆叠成独立体素.
输出:
  face_vol.npy / body_vol.npy          — 分割后的两个体素
  face.obj  /  body.obj                — marching cubes 平滑网格(带顶点真实色)
  face_body_texture.mp4                — 真实纹理渲染(脸暖色描边强调)
  face_body_solid.mp4                  — 纯色分区演示(脸=肤色, 身=衣蓝)
  fb_preview_*.png                     — 四视角预览

用法: /usr/local/bin/python3 face_body_mesh.py [--frac 0.30] [--render-only]
"""
import argparse, math, os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

BASE   = os.path.dirname(os.path.abspath(__file__))
NPZ    = os.path.join(BASE, "video_bc3d_texture_vol.npz")
FFMPEG = "/opt/local/bin/ffmpeg"
VIZ_TEX = os.path.join(BASE, "vframes_fb_texture")
VIZ_SOL = os.path.join(BASE, "vframes_fb_solid")
PRE = os.path.join(BASE, "fb_preview")


def split_face_body(vol, frac=0.30, closing=(3, 3, 3)):
    """逐帧: 该帧轮廓垂直跨度取上部 frac 比例=人脸区, 其余=身体区.
    返回 face_vol(T,H,W), body_vol(T,H,W) 均为 bool."""
    T, H, W = vol.shape
    face = np.zeros_like(vol)
    body = np.zeros_like(vol)
    for t in range(T):
        m = vol[t]
        ys = np.nonzero(m.any(axis=1))[0]          # 有内容的行
        if len(ys) == 0:
            continue
        y_top, y_bot = ys[0], ys[-1]
        split = int(y_top + (y_bot - y_top + 1) * frac)
        face[t, y_top:split, :] = m[y_top:split, :]
        body[t, split:y_bot + 1, :] = m[split:y_bot + 1, :]
    # 各自 3D 闭运算平滑(时间轴连接成连续体)
    if closing:
        face = ndimage.binary_closing(face, structure=np.ones(closing))
        body = ndimage.binary_closing(body, structure=np.ones(closing))
    return face.astype(bool), body.astype(bool)


def mesh_obj(vol, tex, path):
    """marching cubes → obj(带顶点色). 返回 (nvert,nface) 或 None."""
    from skimage import measure
    if int(vol.sum()) < 3:
        return None
    verts, faces, _, _ = measure.marching_cubes(vol, level=0.5)
    vi = np.clip(np.round(verts).astype(int), 0, np.array(vol.shape) - 1)
    cols = tex[vi[:, 0], vi[:, 1], vi[:, 2]]
    with open(path, "w") as f:
        f.write("# mesh from face/body split\n")
        for v, c in zip(verts, cols):
            f.write(f"v {v[0]:.2f} {v[1]:.2f} {v[2]:.2f} "
                    f"{c[0]/255:.3f} {c[1]/255:.3f} {c[2]/255:.3f}\n")
        for ft in faces:
            f.write(f"f {ft[0]+1} {ft[1]+1} {ft[2]+1}\n")
    return len(verts), len(faces)


def project(coords, c, scale, view_ang, elev_ang, off):
    x = coords[:, 0] - c[0]; y = coords[:, 1] - c[1]; z = coords[:, 2] - c[2]
    ca, sa = math.cos(view_ang), math.sin(view_ang)
    ce, se = math.cos(elev_ang), math.sin(elev_ang)
    xr = ca * x + sa * z
    zr = -sa * x + ca * z
    yr = ce * y - se * zr
    zr2 = se * y + ce * zr
    sx = off[0] + xr * scale
    sy = off[1] - yr * scale * 0.85
    return np.stack([sx, sy, zr2], axis=1)


def render_dual(face_vol, body_vol, tex, out_mp4, vizdir, solid=False,
                fps=12, step_deg=5.0, elev=26.0, save_previews=True):
    """合并渲染 face+body 两套体素. solid=True 时纯色分区, 否则真实纹理.
    face 用暖色强调, body 用冷色强调(便于看出分割)."""
    os.makedirs(vizdir, exist_ok=True)
    for f in os.listdir(vizdir):
        os.remove(os.path.join(vizdir, f))
    T, H, W = face_vol.shape
    max_dim = max(T, H, W)
    VW = int(max_dim * 18) // 2 * 2
    VH = int(max_dim * 15) // 2 * 2
    off = (VW / 2.0, VH / 2.0 + max_dim * 1.2)
    scale = (VW * 0.42) / max_dim
    c = (W / 2.0, H / 2.0, T / 2.0)
    elev_r = math.radians(elev)
    r = max(1, int(scale * 0.55))

    def pack(vol_):
        zz, yy, xx = np.nonzero(vol_)
        coords = np.stack([xx, yy, zz], axis=1).astype(float)
        cols = tex[zz, yy, xx].astype(float)
        return coords, cols, len(zz)

    fc, ft, nf = pack(face_vol)
    bc, bt, nb = pack(body_vol)
    coords = np.concatenate([fc, bc])
    if solid:
        # 纯色: 脸=肤色(240,180,140), 身=衣蓝(70,130,220)
        ncols = np.vstack([np.tile([240, 180, 140], (nf, 1)),
                           np.tile([70, 130, 220], (nb, 1))])
    else:
        ncols = np.concatenate([ft, bt])
    isface = np.array([True] * nf + [False] * nb)
    n_occ = len(coords)
    n_frames = int(360 / step_deg)
    preview_angles = set(range(0, 360, 90))
    for fi in range(n_frames):
        view_ang = math.radians(-25 + fi * step_deg)
        scr = project(coords, c, scale, view_ang, elev_r, off)
        order = np.argsort(-scr[:, 2])
        canvas = np.full((VH, VW, 3), (16, 18, 30), dtype=np.uint8)
        dmin, dmax = scr[:, 2].min(), scr[:, 2].max()
        drng = max(dmax - dmin, 1e-6)
        for idx in order:
            sx, sy, dep = scr[idx]
            col = ncols[idx]
            shade = 1.0 - 0.28 * (dep - dmin) / drng
            col = np.clip(col * shade, 0, 255).astype(np.uint8)
            x0 = int(sx - r); x1 = int(sx + r) + 1
            y0 = int(sy - r); y1 = int(sy + r) + 1
            if x1 <= 0 or y1 <= 0 or x0 >= VW or y0 >= VH:
                continue
            x0 = max(x0, 0); y0 = max(y0, 0)
            x1 = min(x1, VW); y1 = min(y1, VH)
            canvas[y0:y1, x0:x1] = col
        img = Image.fromarray(canvas, "RGB")
        d = ImageDraw.Draw(img, "RGBA")
        # 底座网格
        for gx in range(0, W + 1, max(1, W // 5)):
            a = project(np.array([[gx, 0, 0]]), c, scale, view_ang, elev_r, off)
            b = project(np.array([[gx, 0, T - 1]]), c, scale, view_ang, elev_r, off)
            d.line([(float(a[0][0]), float(a[0][1])),
                    (float(b[0][0]), float(b[0][1]))], fill=(70, 84, 120), width=1)
        for gz in range(0, T + 1, max(1, T // 5)):
            a = project(np.array([[0, 0, gz]]), c, scale, view_ang, elev_r, off)
            b = project(np.array([[W - 1, 0, gz]]), c, scale, view_ang, elev_r, off)
            d.line([(float(a[0][0]), float(a[0][1])),
                    (float(b[0][0]), float(b[0][1]))], fill=(70, 84, 120), width=1)
        tag = "SOLID split (face=skin / body=cloth)" if solid else \
              "FACE+BODY real texture (face warm-ring)"
        d.text((8, 8), f"{tag}  frame {fi+1}/{n_frames}", fill=(140, 220, 255))
        d.text((8, VH - 24),
               f"face_voxels={nf}  body_voxels={nb}  total={n_occ}  "
               f"T={T} H={H} W={W}", fill=(210, 210, 210))
        img.save(os.path.join(vizdir, f"v{fi:03d}.png"))
        if save_previews and fi in preview_angles:
            img.copy().save(f"{PRE}_{'solid' if solid else 'tex'}_{fi:03d}.png")
    out = os.path.join(BASE, out_mp4)
    r = subprocess.run([FFMPEG, "-y", "-framerate", str(fps),
                        "-i", os.path.join(vizdir, "v%03d.png"),
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "23", out],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("mp4 失败:", r.stderr[-1500:]); sys.exit(1)
    return out, nf, nb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frac", type=float, default=0.30, help="每帧上部比例=人脸区")
    ap.add_argument("--render-only", action="store_true")
    a = ap.parse_args()

    d = np.load(NPZ, allow_pickle=True)
    vol, tex = d["vol"], d["tex"]
    T, H, W = vol.shape
    print(f"输入时空体素 {vol.shape}, 占用 {int(vol.sum())} voxel")

    face_path = os.path.join(BASE, "face_vol.npy")
    body_path = os.path.join(BASE, "body_vol.npy")
    if a.render_only:
        face = np.load(face_path); body = np.load(body_path)
        print(f"[render-only] face={int(face.sum())} voxel  body={int(body.sum())} voxel")
    else:
        print(f"[1/4] 逐帧自适应分割 (head_frac={a.frac}) ...")
        face, body = split_face_body(vol, a.frac)
        np.save(face_path, face); np.save(body_path, body)
        print(f"  → face={int(face.sum())} voxel  body={int(body.sum())} voxel  "
              f"(总 {int(face.sum())+int(body.sum())})")

    print("[2/4] face/body 各自 marching cubes 转网格 ...")
    nf = mesh_obj(face, tex, os.path.join(BASE, "face.obj"))
    nb = mesh_obj(body, tex, os.path.join(BASE, "body.obj"))
    for nm, obj in (("face", nf), ("body", nb)):
        print(f"  → {nm}.obj  {obj[0]}顶点/{obj[1]}三角 "
              f"{os.path.getsize(os.path.join(BASE, nm+'.obj'))/1e3:.0f}KB")

    print("[3/4] 渲染: 真实纹理(人脸暖色强调) ...")
    o1, nf_, nb_ = render_dual(face, body, tex, "face_body_texture.mp4", VIZ_TEX, solid=False)
    print(f"  → {o1}  face={nf_} body={nb_}")
    print("[4/4] 渲染: 纯色分区演示 ...")
    o2, _, _ = render_dual(face, body, tex, "face_body_solid.mp4", VIZ_SOL, solid=True)
    print(f"  → {o2}")
    print("=== 人脸/身体 分割 + 网格 + 渲染 完成 ===")
    print("  网格: face.obj / body.obj (Blender 可开)")
    print("  视频: face_body_texture.mp4 / face_body_solid.mp4")
    print("  预览: fb_preview_tex_*.png / fb_preview_solid_*.png")


if __name__ == "__main__":
    main()
