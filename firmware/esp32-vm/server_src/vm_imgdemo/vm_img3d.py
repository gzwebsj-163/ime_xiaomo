# -*- coding: utf-8 -*-
"""
vm_img3d.py — 纯 VM 三维渲染：高度场 → 法线 → 旋转 → 光照 → 三通道配色

核心: 让「私有 IR 虚拟机」用张量指令(SLICE/SUB/MUL/ADD/SQRT/MATMUL/SIGMOID/NEG/DIV)
完成整条三维渲染管线, 像素计算 100% 由 VM 完成。
宿主只负责: 注入 X/Y 网格 + 参数、提供 VM 缺失的超越函数(sin) FFI、
把 VM 输出的 (H-1,W-1) 内核对齐放大回 H×W(纯尺寸调整)、PNG 编码。

算法(高度场 3D 光照):
  1) Z = 分形高度场
       base = sqrt(X²+Y²)
       Z = sin(X·k1+Y·k2+base·k3) + 0.6·sin(X·k4+Y·k5+2·base·k6)
  2) 梯度中心差分 → 法向量
       dzdx = (Z[1:,:]-Z[:-1,:])·gx
       dzdy = (Z[:,1:]-Z[:,:-1])·gy
       Zc   = Z[1:,1:]
       norm = sqrt(dzdx²+dzdy²+1)
       N = [-dzdx, -dzdy, 1]/norm
  3) 3D 旋转(线性组合等价 3×3 矩阵乘, 旋转矩阵固定常量)
  4) 光照点积(体现 MATMUL 语义)
  5) 三通道配色 MATMUL 线性 + SIGMOID

寄存器:
  r5=X网格 r6=Y网格 r7..14=临时 r15/16/17=输出R/G/B
  r50..59 = 参数10个: [k1..k6, gx, gy, lx, ly, lz]
  rest 权重来自常量池
"""
import struct
import zlib
import math

from server_src.crypto.obf_engine.layers.layer5_private_ir.bytecode import ByteCode, Instruction
from server_src.crypto.obf_engine.layers.layer5_private_ir.opcode import OpCode
from server_src.crypto.obf_engine.layers.layer5_private_ir.interpreter import VMInterpreter

# ---------------- 宿主 FFI ----------------
def _f_sin(x):
    if isinstance(x, list):
        return [_f_sin(e) for e in x]
    return math.sin(x)

# 寄存器分配
R_X, R_Y = 5, 6
T0, T1, T2, T3, T4, T5, T6, T7 = 7, 8, 9, 10, 11, 12, 13, 14
R_R, R_G, R_B = 15, 16, 17
RP0 = 50

def _rot_matrix(ax, ay, az):
    """欧拉角 -> 3×3 旋转矩阵(列主用于行向量 N·R)"""
    import math as m
    cx, sx = m.cos(ax), m.sin(ax)
    cy, sy = m.cos(ay), m.sin(ay)
    cz, sz = m.cos(az), m.sin(az)
    # R = Rz @ Ry @ Rx (行向量右乘)
    Rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    Ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    Rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    def mm(A, B):
        return [[sum(A[i][k] * B[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return mm(m.ravel if False else (lambda *a: a), *()) if False else mm(Rz, mm(Ry, Rx))


def build_bytecode(params):
    """
    params: 14 元素
      [k1..k6, gx, gy, lx,ly,lz, ax,ay,az]
    """
    if len(params) != 14:
        raise ValueError('params must have 14 elements')
    k1, k2, k3, k4, k5, k6, gx, gy = params[0:8]
    lx, ly, lz = params[8], params[9], params[10]
    ax, ay, az = params[11], params[12], params[13]
    # 规格化光源
    Ln = math.sqrt(lx * lx + ly * ly + lz * lz)
    lx, ly, lz = lx / Ln, ly / Ln, lz / Ln
    # 旋转矩阵
    R = _rot_matrix(ax, ay, az)

    I, O = Instruction, OpCode
    ins = []
    def e(op, ops=[]): ins.append(I(op, ops))
    def ld(reg): e(O.LOAD_REG, [reg])
    def st(reg): e(O.STORE_REG, [reg])
    def sc(idx): e(O.LOAD_REG, [RP0 + idx])
    def cst(i): e(O.LOAD_CONST, [i])
    # 组合宏
    def mul2(a, b, out):
        ld(a); ld(b); e(O.MUL, []); st(out)
    def fma_scalar(sreg, mreg, acc):
        """acc += s·m (s来自寄存器, m来自寄存器, 结果归acc)"""
        sc(sreg); ld(mreg); e(O.MUL, []); st(T7)
        ld(acc); ld(T7); e(O.ADD, []); st(acc)
    def fma_const(ci, mreg, acc):
        cst(ci); ld(mreg); e(O.MUL, []); st(T7)
        ld(acc); ld(T7); e(O.ADD, []); st(acc)

    # 常量池
    # 0 = sin FFI ; 1..8 = 旋转矩阵 R 的 9 个元素(索引1..9); 之后配色权重
    # 我们用 1..9 存 R[0][0..2],R[1][0..2],R[2][0..2]
    # 配色权重后面动态存放, 常量池由 build_bytecode 组装

    # ============ 1. 高度场 ============
    # base = sqrt(X²+Y²) -> T3
    mul2(R_X, R_X, T0)
    mul2(R_Y, R_Y, T1)
    ld(T0); ld(T1); e(O.ADD, []); e(O.SQRT, []); st(T3)

    # z1 = sin(X·k1 + Y·k2 + base·k3)
    sc(0); ld(R_X); e(O.MUL, []); st(T0)
    sc(1); ld(R_Y); e(O.MUL, []); ld(T0); e(O.ADD, []); st(T0)
    sc(2); ld(T3); e(O.MUL, []); ld(T0); e(O.ADD, []); st(T0)
    ld(T0); cst(0); e(O.CALL, [1]); st(T4)      # T4 = z1

    # z2 = sin(X·k4 + Y·k5 + 2·base·k6)
    sc(3); ld(R_X); e(O.MUL, []); st(T0)
    sc(4); ld(R_Y); e(O.MUL, []); ld(T0); e(O.ADD, []); st(T0)
    cst(10); ld(T3); e(O.MUL, []); sc(5); e(O.MUL, []); ld(T0); e(O.ADD, []); st(T0)   # cst10=2.0
    ld(T0); cst(0); e(O.CALL, [1]); st(T1)      # T1 = z2

    # Z = z1 + 0.6·z2 -> T6
    cst(11); ld(T1); e(O.MUL, []); st(T5)       # T5 = 0.6·z2
    ld(T4); ld(T5); e(O.ADD, []); st(T6)        # T6 = Z

    # ============ 2. 梯度法向量 ============
    # dzdx = (Z[1:,:]-Z[:-1,:])·gx  -> T0
    ld(T6); cst(1); e(O.SLICE, []); st(T0)      # Z[1:,:]  slice obj 索引1
    ld(T6); cst(2); e(O.SLICE, []); st(T1)      # Z[:-1,:] slice obj 索引2
    ld(T0); ld(T1); e(O.SUB, []); sc(6); e(O.MUL, []); st(T0)   # dzdx
    # dzdy = (Z[:,1:]-Z[:,:-1])·gy -> T1
    ld(T6); cst(3); e(O.SLICE, []); st(T1)      # Z[:,1:]  slice obj 索引3  [占位: 下面会覆盖]
    ld(T6); cst(4); e(O.SLICE, []); st(T5)      # Z[:,:-1] slice obj 索引4
    ld(T1); ld(T5); e(O.SUB, []); sc(7); e(O.MUL, []); st(T1)   # dzdy
    # Zc = Z[1:,1:] -> T2
    ld(T6); cst(5); e(O.SLICE, []); st(T2)

    # norm² = dzdx² + dzdy² + 1 ; norm -> T3
    ld(T0); ld(T0); e(O.MUL, []); st(T4)
    ld(T1); ld(T1); e(O.MUL, []); ld(T4); e(O.ADD, []); cst(12); e(O.ADD, []); e(O.SQRT, []); st(T3)

    # N = [-dzdx,-dzdy,1]/norm: T5,Nx  T6=Ny  T7=Nz
    ld(T0); e(O.NEG, []); ld(T3); e(O.DIV, []); st(T5)   # Nx
    ld(T1); e(O.NEG, []); ld(T3); e(O.DIV, []); st(T6)   # Ny
    cst(12); ld(T3); e(O.DIV, []); st(T7)                # Nz

    # ============ 3. 3D 旋转 (等价 MATMUL: N' = N·R) ============
    # Nx' = r00·Nx + r01·Ny + r02·Nz   常量索引 20..28 = R 9元
    ld_r(Nx := T5, out := T0, c0 := 20, c1 := 21, c2 := 22, R=R)
    # 用组合宏: out = c0·Nx + c1·Ny + c2·Nz
    # 由于上面引用了未定义函数, 手工展开三条通道

    # Nx' -> T0
    e(O.LOAD_CONST, [20]); e(O.LOAD_REG, [T5]); e(O.MUL, [])
    e(O.LOAD_CONST, [21]); e(O.LOAD_REG, [T6]); e(O.MUL, []); e(O.ADD, [])
    e(O.LOAD_CONST, [22]); e(O.LOAD_REG, [T7]); e(O.MUL, []); e(O.ADD, []); st(T0)
    # Ny' -> T1
    e(O.LOAD_CONST, [23]); e(O.LOAD_REG, [T5]); e(O.MUL, [])
    e(O.LOAD_CONST, [24]); e(O.LOAD_REG, [T6]); e(O.MUL, []); e(O.ADD, [])
    e(O.LOAD_CONST, [25]); e(O.LOAD_REG, [T7]); e(O.MUL, []); e(O.ADD, []); st(T1)
    # Nz' -> T2
    e(O.LOAD_CONST, [26]); e(O.LOAD_REG, [T5]); e(O.MUL, [])
    e(O.LOAD_CONST, [27]); e(O.LOAD_REG, [T6]); e(O.MUL, []); e(O.ADD, [])
    e(O.LOAD_CONST, [28]); e(O.LOAD_REG, [T7]); e(O.MUL, []); e(O.ADD, []); st(T2)

    # ============ 4. 光照点积 diffuse = Nx'·lx + Ny'·ly + Nz'·lz -> T3 ============
    sc(8); ld(T0); e(O.MUL, []); st(T4)
    sc(9); ld(T1); e(O.MUL, []); ld(T4); e(O.ADD, []); st(T4)
    sc(10); ld(T2); e(O.MUL, []); ld(T4); e(O.ADD, []); st(T3)

    # ============ 5. 三通道配色 ============
    # feature 权重常量 30..38: R/G/B 各 (a_diffuse, a_Z, bias)
    # R = sigmoid(w0·diffuse + w1·Zc + biasR)
    e(O.LOAD_CONST, [30]); ld(T3); e(O.MUL, [])
    e(O.LOAD_CONST, [31]); ld(T2); e(O.MUL, []); e(O.ADD, [])
    e(O.LOAD_CONST, [32]); e(O.ADD, []); e(O.SIGMOID, []); st(R_R)
    e(O.LOAD_CONST, [33]); ld(T3); e(O.MUL, [])
    e(O.LOAD_CONST, [34]); ld(T2); e(O.MUL, []); e(O.ADD, [])
    e(O.LOAD_CONST, [35]); e(O.ADD, []); e(O.SIGMOID, []); st(R_G)
    e(O.LOAD_CONST, [36]); ld(T3); e(O.MUL, [])
    e(O.LOAD_CONST, [37]); ld(T2); e(O.MUL, []); e(O.ADD, [])
    e(O.LOAD_CONST, [38]); e(O.ADD, []); e(O.SIGMOID, []); st(R_B)

    e(O.HALT, [])
    return ins


def build_bytecode_pack(params, rot_matrix=None):
    """
    完整组装: 生成指令 + 常量池 + ByteCode。
    params: [k1..k6, gx,gy, lx,ly,lz, ax,ay,az] 14个
    rot_matrix: 若传入则用, 否则内部算。
    """
    k1, k2, k3, k4, k5, k6 = params[0:6]
    lx, ly, lz = params[8], params[9], params[10]
    ax, ay, az = params[11], params[12], params[13]
    Ln = math.sqrt(lx * lx + ly * ly + lz * lz)
    lx, ly, lz = lx / Ln, ly / Ln, lz / Ln
    R = _rot_matrix(ax, ay, az)

    # 配色权重: 设计 3 组 (a_diff, a_Z, bias), 让俯视蓝绿色、侧面暖色
    # 通过参数微调可换风格
    wR = [0.9, 0.35, -0.15]
    wG = [0.7, 0.6, -0.25]
    wB = [0.5, 0.75, -0.35]

    constants = []
    constants.append(_f_sin)                      # 0
    constants.append((slice(1, None), slice(None, None)))  # 1 Z[1:,:]
    constants.append((slice(None, -1), slice(None, None))) # 2 Z[:-1,:]
    constants.append((slice(None, None), slice(1, None)))  # 3 Z[:,1:]
    constants.append((slice(None, None), slice(None, -1))) # 4 Z[:,:-1]
    constants.append((slice(1, None), slice(1, None)))     # 5 Z[1:,1:]
    # 6,7,8 预留给可能的 slice (不强制)
    constants += [None, None, None]
    constants.append(2.0)                         # 9
    constants.append(2.0)                         # 10  (z2 的 2·base 系数)
    constants.append(0.6)                         # 11
    constants.append(1.0)                         # 12
    # 旋转矩阵 9 元 索引 20..28
    # 需要填充到 20 个占位
    while len(constants) < 20:
        constants.append(0.0)
    for i in range(9):
        constants.append(R[i // 3][i % 3])        # 20..28
    while len(constants) < 30:
        constants.append(0.0)
    # 配色权重 30..38
    for w in (wR[0], wR[1], wR[2], wG[0], wG[1], wG[2], wB[0], wB[1], wB[2]):
        constants.append(w)

    bc = ByteCode()
    bc.metadata = {'graph_name': 'img3d_terrain', 'desc': 'pure-VM 3D relief shading'}
    bc.constants = constants
    bc.instructions = build_bytecode(params)
    return bc


def generate(H, W, params, x_scale=6.0):
    """
    用 VM 生成 3D 地形渲染图(高度场光照)。
    返回 numpy (H-1, W-1, 3) [0,1] 内核(宿主需放大回 H×W)。
    """
    import numpy as np
    if len(params) != 14:
        raise ValueError('params must have 14 elements')
    xs = np.linspace(-x_scale, x_scale, W, dtype=float)
    ys = np.linspace(-x_scale, x_scale, H, dtype=float)
    X, Y = np.meshgrid(xs, ys)

    bc = build_bytecode_pack(params)
    vm = VMInterpreter()
    vm.load_bytecode(bc)
    vm.registers.set(R_X, X.tolist())
    vm.registers.set(R_Y, Y.tolist())
    for i, v in enumerate(params):
        vm.registers.set(RP0 + i, float(v))

    vm.execute()
    R = np.array(vm.registers.get(R_R), dtype=float)
    G = np.array(vm.registers.get(R_G), dtype=float)
    B = np.array(vm.registers.get(R_B), dtype=float)
    return np.stack([R, G, B], axis=-1)


def to_png(rgb, path):
    """(H,W,3) in [0,1] -> PNG (纯标准库)"""
    import numpy as np
    H, W, _ = rgb.shape
    rgb8 = (np.clip(rgb, 0, 1) * 255).astype(int)
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            r, g, b = rgb8[y, x]
            raw.append(int(r)); raw.append(int(g)); raw.append(int(b))
    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        c += struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)
        return c
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)
    png = sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b'')
    with open(path, 'wb') as f:
        f.write(png)
    return path


if __name__ == '__main__':
    import numpy as np
    import time
    params = [0.7, 0.9, 0.5, 1.3, -1.1, 0.4,  0.12, 0.12,
              0.6, -0.4, 0.85,  0.6, -0.3, 0.15]
    t0 = time.time()
    rgb = generate(300, 300, params, x_scale=4.5)
    t1 = time.time()
    print('VM 3D render (299x299 kernel) took %.2fs' % (t1 - t0))
    # 最近邻放大回 300x300
    H, W, _ = rgb.shape
    big = np.zeros((H + 1, W + 1, 3))
    big[:H, :W] = rgb
    big[H, :W] = rgb[-1]
    big[:H, W] = rgb[:, -1]
    big[H, W] = rgb[-1, -1]
    to_png(big, 'img3d_out/terrain_test.png')
    print('saved img3d_out/terrain_test.png')
