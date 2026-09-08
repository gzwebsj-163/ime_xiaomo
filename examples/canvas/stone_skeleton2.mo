# 石头骨架 — 填充 + 线框双输出
>> print >> "=== Stone Skeleton (Filled + Wireframe) ==="
>> print >> ""

# Step 1: 椭球体距离场 (石头形状)
void field : int = ellipsoid_field(16, 16, 16, 6.0, 4.0, 3.0, 7.5, 7.5, 7.5)
void _d : int = nd_info(${field})

# Step 2: Marching Cubes
void mesh_3d : int = marching_cubes(${field}, 0.0)
void nverts : int = dim_n(${mesh_3d}, 0)
void ntris_int : int = nd_div(${nverts}, 3.0)
>> print >> "Vertices: " >> ${nverts} >> ", Triangles: " >> ${ntris_int}

# Step 3: 居中
void center : int = tensor([7.5, 7.5, 7.5])
void mesh_c : int = nd_sub(${mesh_3d}, ${center})

# Step 4: 旋转矩阵
void czz : int = 0.69671
void szz : int = 0.71736
void cxx : int = 0.87758
void sxx : int = 0.47943
void cyy : int = 0.95534
void syy : int = 0.29552
void Rz : int = tensor([[${czz}, -${szz}, 0],[${szz}, ${czz}, 0],[0, 0, 1]])
void Rx : int = tensor([[1, 0, 0],[0, ${cxx}, -${sxx}],[0, ${sxx}, ${cxx}]])
void Ry : int = tensor([[${cyy}, 0, ${syy}],[0, 1, 0],[-${syy}, 0, ${cyy}]])
void R : int = nd_matmul(nd_matmul(${Rz}, ${Rx}), ${Ry})
void R_T : int = transpose_n(${R})
void mesh_rot : int = nd_matmul(${mesh_c}, ${R_T})

# Step 5: 平移 Z+15
void trans : int = tensor([0, 0, 15.0])
void mesh_t : int = nd_add(${mesh_rot}, ${trans})

# Step 6: 透视投影 → 屏幕映射
void W_img : int = 200
void H_img : int = 200
void vx : int = nd_slice(${mesh_t}, [0,0], [999999,1], [1,1])
void vy : int = nd_slice(${mesh_t}, [0,1], [999999,2], [1,1])
void vz : int = nd_slice(${mesh_t}, [0,2], [999999,3], [1,1])
void f : int = 2.0
void sx_2d : int = nd_div(nd_mul(${f}, ${vx}), ${vz})
void sy_2d : int = nd_div(nd_mul(${f}, ${vy}), ${vz})
void screen_scale : int = 80.0
void scx : int = 100.0
void scy : int = 100.0
void screen_x : int = nd_add(nd_mul(${sx_2d}, ${screen_scale}), ${scx})
void screen_y : int = nd_add(nd_mul(${sy_2d}, ${screen_scale}), ${scy})
void screen_xy : int = nd_concat(${screen_x}, ${screen_y}, 1)

# Step 7: 填充渲染
void img_filled : int = rasterize_tri(${screen_xy}, -1, ${W_img}, ${H_img})

# Step 8: 线框渲染 (骨架)
void img_wire : int = rasterize_wire(${screen_xy}, -1, ${W_img}, ${H_img})

>> print >> ""
>> print >> "=== PGM: FILLED ==="
>> print >> "%!PGM_BEGIN_FILLED 200 200"
void i : int = 0
while ${i} < 200:
    void row_i : int = nd_slice(${img_filled}, [${i},0], [${i}+1,200], [1,1])
    void row_arr : int = tensor_to_array(${row_i})
    >> print >> ${row_arr}
    void i : int = ${i} + 1

>> print >> ""
>> print >> "=== PGM: WIRE ==="
>> print >> "%!PGM_BEGIN_WIRE 200 200"
void j : int = 0
while ${j} < 200:
    void row_j : int = nd_slice(${img_wire}, [${j},0], [${j}+1,200], [1,1])
    void row_arr2 : int = tensor_to_array(${row_j})
    >> print >> ${row_arr2}
    void j : int = ${j} + 1

# 统计
void sum_filled : int = nd_sum(${img_filled}, -1)
void sum_wire : int = nd_sum(${img_wire}, -1)
void fill_ratio : int = nd_div(${sum_filled}, 40000.0)
void wire_ratio : int = nd_div(${sum_wire}, 40000.0)
>> print >> "Filled ratio: " >> ${fill_ratio}
>> print >> "Wireframe ratio: " >> ${wire_ratio}
>> print >> "=== Stone Skeleton COMPLETE ==="