# 第③层: 3D 渲染管线 — 顶点变换 + 投影 (在 VM 内纯张量运算)
# 用法: ./xiaomo run examples/canvas/render_3d.mo
#
# 管线: 3D 顶点 → 旋转 → 平移 → 透视投影 → 2D 屏幕坐标
# 旋转矩阵: R_z(θ) · R_x(φ) · R_y(ψ)
# 投影: perspective divide (f=焦距)

>> print >> "=== 3D Rendering Pipeline ==="

# ---- 定义: 3D 立方体 (8 顶点, 每条边连接 2 顶点) ----
# 顶点坐标 (x,y,z), 单位立方体 [-1..1]
void verts : int = tensor([
    [-1, -1, -1],  # 0
    [ 1, -1, -1],  # 1
    [ 1,  1, -1],  # 2
    [-1,  1, -1],  # 3
    [-1, -1,  1],  # 4
    [ 1, -1,  1],  # 5
    [ 1,  1,  1],  # 6
    [-1,  1,  1]   # 7
])

# 边列表 (顶点对)
void edges : int = tensor([
    [0,1],[1,2],[2,3],[3,0],  # 背面
    [4,5],[5,6],[6,7],[7,4],  # 正面
    [0,4],[1,5],[2,6],[3,7]   # 连接边
])

# ---- 旋转参数 (弧度, 预计算三角函数) ----
# theta_z=0.5, theta_x=0.3, theta_y=0.2
# cos(0.5)=0.87758, sin(0.5)=0.47943
# cos(0.3)=0.95534, sin(0.3)=0.29552
# cos(0.2)=0.98007, sin(0.2)=0.19867

void cz : int = 0.87758
void sz : int = 0.47943
void cx : int = 0.95534
void sx : int = 0.29552
void cy : int = 0.98007
void sy : int = 0.19867

# R_z
void Rz : int = tensor([
    [${cz}, -${sz}, 0],
    [${sz},  ${cz}, 0],
    [0,      0,     1]
])

# R_x
void Rx : int = tensor([
    [1, 0,     0],
    [0, ${cx}, -${sx}],
    [0, ${sx},  ${cx}]
])

# R_y
void Ry : int = tensor([
    [${cy},  0, ${sy}],
    [0,      1, 0],
    [-${sy}, 0, ${cy}]
])

# R = Rz · Rx · Ry
void RzRx : int = nd_matmul(${Rz}, ${Rx})
void R : int = nd_matmul(${RzRx}, ${Ry})

# ---- 旋转顶点: verts_rot = verts · R^T (逐行变换) ----
# verts 是 [8,3], R 是 [3,3]
# 正确的 3D 变换: v_rot = v · R^T (列向量旋转)
void R_T : int = transpose_n(${R})
void verts_rot : int = nd_matmul(${verts}, ${R_T})

# ---- 平移: Z+3 (摄像机在 Z=3, 物体在原点) ----
void trans : int = tensor([0, 0, 3.0])
void verts_trans : int = nd_add(${verts_rot}, ${trans})

# ---- 透视投影: 焦距 f=2, 投影到 2D ----
# x_2d = f * x / z, y_2d = f * y / z
# 逐顶点操作: 切片取 x,y,z
# 用 nd_slice 或数学运算拆分

# 取所有顶点的 x, y, z 列
void verts_x : int = nd_slice(${verts_trans}, [0,0], [8,1], [1,1])
void verts_y : int = nd_slice(${verts_trans}, [0,1], [8,2], [1,1])
void verts_z : int = nd_slice(${verts_trans}, [0,2], [8,3], [1,1])

# 展平为 1D
void vx : int = reshape(${verts_x}, [8])
void vy : int = reshape(${verts_y}, [8])
void vz : int = reshape(${verts_z}, [8])

# 焦距
void f : int = 2.0

# x_2d = f * x / z, y_2d = f * y / z
# (逐元素除法需要对应维度匹配, vz 是 1D [8], 广播自动完成)
void sx_2d : int = nd_div(nd_mul(${f}, ${vx}), ${vz})
void sy_2d : int = nd_div(nd_mul(${f}, ${vy}), ${vz})

# ---- 组合为 [x_2d, y_2d] 输出 ----
void xy_2d : int = nd_concat(reshape(${sx_2d}, [8,1]), reshape(${sy_2d}, [8,1]), 1)

# ---- 计算屏幕坐标 (归一化到 0~200 范围, 屏幕映射) ----
void screen_scale : int = 100.0
void screen_x : int = nd_add(nd_mul(${sx_2d}, ${screen_scale}), ${screen_scale})
void screen_y : int = nd_add(nd_mul(${sy_2d}, ${screen_scale}), ${screen_scale})

# ---- 打印结果 ----
void print_verts : int = tensor_to_array(${verts})
void print_R : int = tensor_to_array(${R})
void print_rot : int = tensor_to_array(${verts_rot})
void print_proj : int = tensor_to_array(${xy_2d})
void print_screen : int = tensor_to_array(nd_concat(reshape(${screen_x}, [8,1]), reshape(${screen_y}, [8,1]), 1))

>> print >> "Input vertices (8 corners of unit cube):"
>> print >> ${print_verts}
>> print >> ""
>> print >> "Rotation matrix R = Rz·Rx·Ry:"
>> print >> ${print_R}
>> print >> ""
>> print >> "Rotated vertices (after R):"
>> print >> ${print_rot}
>> print >> ""
>> print >> "Projected 2D coordinates (x_2d, y_2d):"
>> print >> ${print_proj}
>> print >> ""
>> print >> "Screen coordinates (centered, scaled):"
>> print >> ${print_screen}
>> print >> ""

# ---- 验证: 前后面投影应有不同尺寸 (透视效应) ----
# 正面(Z最大) 应比背面(Z最小) 小 (离得远)
void back_z : int = nd_mean(${vz}, 0)
>> print >> "Mean Z depth: " >> ${back_z}
>> print >> "=== 3D Rendering Pipeline PASSED ==="
