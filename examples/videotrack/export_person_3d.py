#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
导出"纯人物 3D 人像模型"(去掉背景后剩下的前景体素).
========================================================
输入: video_bc3d_texture_vol.npz 里的 vol(时空体素, 已去背景) + tex(真实颜色).
输出(标准 3D 模型文件, Blender/MeshLab/在线查看器全兼容):
  1. person_3d.ply        — 体素立方体网格(带顶点真实颜色), 实体模型感
  2. person_3d_points.ply — 带色点云(轻量, 60706 点)
  3. person_3d_smooth.obj — (若 skimage 可用) marching cubes 平滑三角网格+顶点色

坐标: vol[z,y,x] = (时间T, 高H, 宽W) → 三维坐标 (x, y, z)=(宽, 高, 时间).
     时间轴=深度方向(多帧轮廓累积出人物 3D 形态, 即"历史线条"重建).
用法: /usr/local/bin/python3 export_person_3d.py [npz路径]
"""
import os, sys
import numpy as np

BASE = os.path.dirname(os.path.abspath(__file__))
NPZ  = os.path.join(BASE, "video_bc3d_texture_vol.npz")
if len(sys.argv) > 1:
    NPZ = sys.argv[1]

PRE = "person_3d"

# ---------------------------------------------------------------- 体素网格
def write_voxel_ply(vol, tex, path):
    """每个占据体素生成一个单位立方体(8顶点+12三角), 顶点色=纹理色."""
    zz, yy, xx = np.nonzero(vol)
    N = len(zz)
    # 8 个角偏移
    corners = np.array([[0,0,0],[1,0,0],[1,1,0],[0,1,0],
                        [0,0,1],[1,0,1],[1,1,1],[0,1,1]], dtype=float)
    verts = np.stack([xx, yy, zz], axis=1)[:, None, :] + corners[None, :, :]
    verts = verts.reshape(-1, 3)                        # (8N,3)
    cols  = tex[zz, yy, xx][:, None, :].repeat(8, axis=1).reshape(-1, 3)
    # 12 根索引三角形(单位立方体, 逆时针朝外)
    tris = np.array([
        [0,2,1],[0,3,2],[4,5,6],[4,6,7],
        [0,1,5],[0,5,4],[1,2,6],[1,6,5],
        [2,3,7],[2,7,6],[3,0,4],[3,4,7]], dtype=np.int32)
    faces = (tris[None, :, :] + (np.arange(N) * 8)[:, None, None]).reshape(-1, 3)
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\n")
        f.write(f"element vertex {len(verts)}\n")
        f.write("property float x\nproperty float y\nproperty float z\n")
        f.write("property uchar red\nproperty uchar green\nproperty uchar blue\n")
        f.write(f"element face {len(faces)}\n")
        f.write("property list uchar int vertex_indices\nend_header\n")
        for v, c in zip(verts, cols):
            f.write(f"{v[0]:.1f} {v[1]:.1f} {v[2]:.1f} {int(c[0])} {int(c[1])} {int(c[2])}\n")
        for t in faces:
            f.write(f"3 {t[0]} {t[1]} {t[2]}\n")
    return N, len(verts), len(faces)

# ---------------------------------------------------------------- 点云
def write_points_ply(vol, tex, path):
    zz, yy, xx = np.nonzero(vol)
    cols = tex[zz, yy, xx]
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\n")
        f.write(f"element vertex {len(zz)}\n")
        f.write("property float x\nproperty float y\nproperty float z\n")
        f.write("property uchar red\nproperty uchar green\nproperty uchar blue\n")
        f.write("end_header\n")
        for x, y, z, c in zip(xx, yy, zz, cols):
            f.write(f"{float(x):.1f} {float(y):.1f} {float(z):.1f} {int(c[0])} {int(c[1])} {int(c[2])}\n")
    return len(zz)

# ---------------------------------------------------------------- marching cubes 平滑网格
def write_smooth_obj(vol, tex, path):
    try:
        from skimage import measure
    except Exception:
        return False
    verts, faces, normals, _ = measure.marching_cubes(vol, level=0.5)
    # marching_cubes 坐标顺序是 (z,y,x) = vol 的 (axis0,axis1,axis2)
    # 顶点色: 最近体素颜色 (顺序必须与 vol 维度一致)
    vi = np.clip(np.round(verts).astype(int), 0, np.array(vol.shape) - 1)
    cols = tex[vi[:, 0], vi[:, 1], vi[:, 2]]
    with open(path, "w") as f:
        f.write("# smooth person 3d mesh (marching cubes)\n")
        for v, c in zip(verts, cols):
            f.write(f"v {v[0]:.2f} {v[1]:.2f} {v[2]:.2f} {c[0]/255:.3f} {c[1]/255:.3f} {c[2]/255:.3f}\n")
        for ft in faces:
            f.write(f"f {ft[0]+1} {ft[1]+1} {ft[2]+1}\n")
    return len(verts), len(faces)

def main():
    d = np.load(NPZ, allow_pickle=True)
    vol, tex = d["vol"], d["tex"]
    print(f"输入时空体素: {vol.shape}  dtype={vol.dtype}  前景体素={int(vol.sum())}")
    print(f"纹理体素:     {tex.shape}  颜色范围 {tex.min()}~{tex.max()}")

    n = write_voxel_ply(vol, tex, os.path.join(BASE, PRE + ".ply"))
    print(f"[1/3] 体素网格 → {PRE}.ply  ({n[0]} 体素 / {n[1]} 顶点 / {n[2]} 三角)  "
          f"{os.path.getsize(os.path.join(BASE, PRE+'.ply'))/1e6:.1f}MB")

    np_ = write_points_ply(vol, tex, os.path.join(BASE, PRE + "_points.ply"))
    print(f"[2/3] 点云     → {PRE}_points.ply  ({np_} 点)  "
          f"{os.path.getsize(os.path.join(BASE, PRE+'_points.ply'))/1e6:.1f}MB")

    r = write_smooth_obj(vol, tex, os.path.join(BASE, PRE + "_smooth.obj"))
    if r:
        print(f"[3/3] 平滑网格 → {PRE}_smooth.obj  ({r[0]} 顶点 / {r[1]} 三角)")
    else:
        print("[3/3] 平滑网格: skimage 不可用, 跳过(体素网格已够用)")
    print("=== 纯人物 3D 人像模型导出完成 ===")

if __name__ == "__main__":
    main()
