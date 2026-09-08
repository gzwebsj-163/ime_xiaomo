# 石头骨骼 (3D 椭球体网格骨架)
# 管线: 椭球体场 → MarchingCubes → 旋转变换 → 透视投影 → 线框光栅化
# 用法: ./xiaomo run examples/canvas/stone_skeleton.mo
# 在 Kickpi 上运行同样管线

>> print >> "=== Stone Skeleton 3D Mesh ==="
>> print >> ""

# ======== Step 1: 椭球体距离场 (石头形状) ========
# ellipsoid_field(D, H, W, rx, ry, rz, cx, cy, cz)
# D×H×W=16×16×16 网格, rx=6, ry=4, rz=3 (椭球, 模拟自然石头)
# 中心在 (7.5, 7.5, 7.5) 让椭球居中
void field : int = ellipsoid_field(16, 16, 16, 6.0, 4.0, 3.0, 7.5, 7.5, 7.5)
void _d : int = nd_info(${field})

# ======== Step 2: Marching Cubes ========
void mesh_3d : int = marching_cubes(${field}, 0.0)
void nverts : int = dim_n(${mesh_3d}, 0)
void ntris_int : int = nd_div(${nverts}, 3.0)
void nverts_print : int = tensor_to_array(${nverts})
void ntris_print : int = tensor_to_array(${ntris_int})

>> print >> "Step 2 — Marching Cubes: " >> ${nverts_print} >> " vertices, " >> ${ntris_print} >> " triangles"
>> print >> ""

# ======== Step 3: 居中 (椭球中心 7.5,7.5,7.5) ========
void center : int = tensor([7.5, 7.5, 7.5])
void mesh_c : int = nd_sub(${mesh_3d}, ${center})

# ======== Step 4: 旋转矩阵 R_z(0.8)·R_x(0.5)·R_y(0.3) ========
void cz : int = 0.69671
void sz : int = 0.71736
void cx : int = 0.87758
void sx : int = 0.47943
void cy : int = 0.95534
void sy : int = 0.29552

void Rz : int = tensor([[${cz}, -${sz}, 0],[${sz}, ${cz}, 0],[0, 0, 1]])
void Rx : int = tensor([[1, 0, 0],[0, ${cx}, -${sx}],[0, ${sx}, ${cx}]])
void Ry : int = tensor([[${cy}, 0, ${sy}],[0, 1, 0],[-${sy}, 0, ${cy}]])
void R : int = nd_matmul(nd_matmul(${Rz}, ${Rx}), ${Ry})
void R_T : int = transpose_n(${R})

# ======== Step 5: 旋转 ========
void mesh_rot : int = nd_matmul(${mesh_c}, ${R_T})

# ======== Step 6: 平移 Z+15 ========
void trans : int = tensor([0, 0, 15.0])
void mesh_t : int = nd_add(${mesh_rot}, ${trans})

# ======== Step 7: 透视投影 ========
void vx : int = nd_slice(${mesh_t}, [0,0], [999999,1], [1,1])
void vy : int = nd_slice(${mesh_t}, [0,1], [999999,2], [1,1])
void vz : int = nd_slice(${mesh_t}, [0,2], [999999,3], [1,1])

void f : int = 2.0
void sx_2d : int = nd_div(nd_mul(${f}, ${vx}), ${vz})
void sy_2d : int = nd_div(nd_mul(${f}, ${vy}), ${vz})

# ======== Step 8: 屏幕映射 ========
void screen_scale : int = 80.0
void scx : int = 100.0
void scy : int = 100.0
void screen_x : int = nd_add(nd_mul(${sx_2d}, ${screen_scale}), ${scx})
void screen_y : int = nd_add(nd_mul(${sy_2d}, ${screen_scale}), ${scy})
void screen_xy : int = nd_concat(${screen_x}, ${screen_y}, 1)

# ======== Step 9: 线框光栅化 (用 rasterize_tri 填充) ========
void img_filled : int = rasterize_tri(${screen_xy}, -1, 200, 200)
void _i : int = nd_info(${img_filled})

# ======== 输出 PGM ========
>> print >> "%!PGM_BEGIN 200 200"
void i : int = 0
while ${i} < 200:
    void row_i : int = nd_slice(${img_filled}, [${i},0], [${i}+1,200], [1,1])
    void row_arr : int = tensor_to_array(${row_i})
    >> print >> ${row_arr}
    void i : int = ${i} + 1
>> print >> "%!PGM_END"

# ======== 统计 ========
void sum_img : int = nd_sum(${img_filled}, -1)
void fill_ratio : int = nd_div(${sum_img}, 40000.0)
void fill_print : int = tensor_to_array(${fill_ratio})
>> print >> "Fill ratio: " >> ${fill_print}

# ======== 输出顶点坐标 (3D 骨架数据) ========
>> print >> ""
>> print >> "=== 3D Skeleton (Vertices) ==="
>> print >> "Total vertices: " >> ${nverts_print}
>> print >> "Format: [vertex_id] x y z"
void vi : int = 0
while ${vi} < 100:  # 只打印前 100 个顶点
    void vx_i : int = nd_slice(${mesh_rot}, [${vi},0], [${vi}+1,1], [1,1])
    void vy_i : int = nd_slice(${mesh_rot}, [${vi},1], [${vi}+1,2], [1,1])
    void vz_i : int = nd_slice(${mesh_rot}, [${vi},2], [${vi}+1,3], [1,1])
    void vx_v : int = tensor_to_array(${vx_i})
    void vy_v : int = tensor_to_array(${vy_i})
    void vz_v : int = tensor_to_array(${vz_i})
    >> print >> ${vi} >> ": " >> ${vx_v} >> " " >> ${vy_v} >> " " >> ${vz_v}
    void vi : int = ${vi} + 1
>> print >> "... (truncated, total " >> ${nverts_print} >> " vertices)"

>> print >> ""
>> print >> "=== Stone Skeleton COMPLETE ==="