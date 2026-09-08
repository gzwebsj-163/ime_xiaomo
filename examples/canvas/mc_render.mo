# 第④层: MC 网格 → 3D 变换 → 2D 光栅化 (全链路)
# 管线: 球体场 → MarchingCubes → 旋转 → 透视投影 → 三角形填充
# 用法: ./xiaomo run examples/canvas/mc_render.mo

>> print >> "=== MC Mesh → 2D Render Pipeline (Full Chain) ==="
>> print >> ""

# ======== Step 1: 生成球体距离场 [12,12,12] ========
# sphere_field(D, H, W, radius, cx, cy, cz)
# 球心在 (5.5, 5.5, 5.5), 半径 5.0
void field : int = sphere_field(12, 12, 12, 5.0, 5.5, 5.5, 5.5)
void _d : int = nd_info(${field})

# ======== Step 2: Marching Cubes (iso=0) ========
void mesh_3d : int = marching_cubes(${field}, 0.0)
void nverts : int = dim_n(${mesh_3d}, 0)
void ntris_int : int = nd_div(${nverts}, 3.0)

>> print >> "Step 2 — Marching Cubes: " >> ${nverts} >> " vertices, " >> ${ntris_int} >> " triangles"
>> print >> ""

# ======== Step 2.5: 居中 (球心在网格中心 5.5,5.5,5.5) ========
void center : int = tensor([5.5, 5.5, 5.5])
void mesh_c : int = nd_sub(${mesh_3d}, ${center})

# ======== Step 3: 旋转矩阵 R_z(0.5)·R_x(0.3)·R_y(0.2) ========
void cz : int = 0.87758
void sz : int = 0.47943
void cx : int = 0.95534
void sx : int = 0.29552
void cy : int = 0.98007
void sy : int = 0.19867

void Rz : int = tensor([[${cz}, -${sz}, 0],[${sz}, ${cz}, 0],[0, 0, 1]])
void Rx : int = tensor([[1, 0, 0],[0, ${cx}, -${sx}],[0, ${sx}, ${cx}]])
void Ry : int = tensor([[${cy}, 0, ${sy}],[0, 1, 0],[-${sy}, 0, ${cy}]])
void R : int = nd_matmul(nd_matmul(${Rz}, ${Rx}), ${Ry})
void R_T : int = transpose_n(${R})

# ======== Step 4: 旋转 ========
# mesh_rot = mesh_3d · R^T
void mesh_rot : int = nd_matmul(${mesh_c}, ${R_T})

# ======== Step 5: 平移 Z+15 (摄像机距离) ========
void trans : int = tensor([0, 0, 15.0])
void mesh_t : int = nd_add(${mesh_rot}, ${trans})

# ======== Step 6: 透视投影 ========
# x_2d = f * x / z,  y_2d = f * y / z  (f=2.0)
# 用 nd_slice 取各列 (大数 999999 自动 clamp 到最大索引)
void vx : int = nd_slice(${mesh_t}, [0,0], [999999,1], [1,1])
void vy : int = nd_slice(${mesh_t}, [0,1], [999999,2], [1,1])
void vz : int = nd_slice(${mesh_t}, [0,2], [999999,3], [1,1])

void f : int = 2.0
void sx_2d : int = nd_div(nd_mul(${f}, ${vx}), ${vz})
void sy_2d : int = nd_div(nd_mul(${f}, ${vy}), ${vz})

# ======== Step 7: 组合为 [N*3, 2] 投影坐标 ========
void proj : int = nd_concat(${sx_2d}, ${sy_2d}, 1)

# ======== Step 8: 屏幕映射 (居中+缩放) ========
void screen_scale : int = 80.0
void scx : int = 100.0
void scy : int = 100.0
void screen_x : int = nd_add(nd_mul(${sx_2d}, ${screen_scale}), ${scx})
void screen_y : int = nd_add(nd_mul(${sy_2d}, ${screen_scale}), ${scy})
void screen_xy : int = nd_concat(${screen_x}, ${screen_y}, 1)

# ======== Step 9: 光栅化 ========
# rasterize_tri(verts_2d, triangles, width, height)
# triangles=-1 表示每 3 个连续顶点为一个三角形 (MC 输出格式)
void img : int = rasterize_tri(${screen_xy}, -1, 200, 200)

void _i : int = nd_info(${img})
>> print >> "Rendered image [200,200]:"

# ======== 输出 PGM 图片数据 (Python 解析用) ========
>> print >> "%!PGM_BEGIN 200 200"
# 逐行输出 200 行像素, 每行 200 个值, 用空格分隔
void i : int = 0
while ${i} < 200:
    void row_i : int = nd_slice(${img}, [${i},0], [${i}+1,200], [1,1])
    void row_arr : int = tensor_to_array(${row_i})
    >> print >> ${row_arr}
    void i : int = ${i} + 1
>> print >> "%!PGM_END"

# ======== 验证: 打印统计信息 ========
void sum_img : int = nd_sum(${img}, -1)
void fill_ratio : int = nd_div(${sum_img}, 40000.0)
void fill_print : int = tensor_to_array(${fill_ratio})
>> print >> "Fill ratio: " >> ${fill_print}
>> print >> ""
>> print >> "=== MC Mesh → 2D Render Pipeline COMPLETE ==="