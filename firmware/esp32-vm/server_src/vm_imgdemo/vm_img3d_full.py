# -*- coding: utf-8 -*-
"""
vm_img3d_full.py — 全链纯 VM 三维渲染（分形生成 → 法线 → 旋转 → 光照 → 配色 全在 VM）

与 vm_height3d.py（宿主注入梯度）不同，本版本把「分形高度场生成」也搬进 VM：
  VM 只注入 X/Y 网格 + 参数, 其余 100% 由私有 IR 虚拟机张量指令完成：

  1) 分形高度场 (VM 内生成, sin 走 FFI)
       base = sqrt(X²+Y²)
       Z = sin(Xk1 + Yk2 + basek3) + 0.6·sin(Xk4 + Yk5 + 2·base·k6)
  2) 中心梯度 (SLICE 前后差)
       dzdx = Z[1:,1:] - Z[:-1,1:]      ·gx
       dzdy = Z[1:,1:] - Z[1:,:-1]      ·gy
       Zc   = Z[1:,1:]
  3) 法向量 (NEG/SQRT/DIV)
       norm = sqrt(dzdx²+dzdy²+1)
       N = [-dzdx, -dzdy, 1]/norm
  4) 3D 旋转  R·N  (3×3, 寄存器注入 9 系数)
  5) 朗伯光照  bright = L·N'  (clamp FFI)
  6) 高度假彩 × 光照                (MUL/ADD)
  7) 宿主仅做: 内核 (H-1,W-1) 放大回 H×W + PNG 编码

寄存器约定:
  r5 = X  r6 = Y        (宿主注入网格)
  r50..r55 = k1..k6      r56 = gx  r57 = gy     (分形/梯度参数)
  r58..r66 = 旋转矩阵 9 系数(row-major)
  r67 = Lx  r68 = Ly    (Lz 放常量池 C_LZ)
  r69 = zscale          (法线陡峭度)
  r5,r6,r7 输出 R,G,B (复用 r5,r6,r7 还是单独? 用 r5..r7 会被 X/Y 覆盖, 故用 r70..72)
  r70,r71,r72 = 输出 R,G,B
"""
import struct
import zlib
import math

from server_src.crypto.obf_engine.layers.layer5_private_ir.bytecode import ByteCode, Instruction
from server_src.crypto.obf_engine.layers.layer5_private_ir.opcode import OpCode
from server_src.crypto.obf_engine.layers.layer5_private_ir.interpreter import VMInterpreter

# ---------------- 宿主 FFI（仅 VM 缺失的琐碎算子） ----------------
def _f_sin(x):
    if isinstance(x, list):
        return [_f_sin(e) for e in x]
    return math.sin(x)

def _ff_clamp(x, lo=0.0, hi=1.0):
    if isinstance(x, list):
        return [_ff_clamp(e, lo, hi) for e in x]
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x

# ---------------- 寄存器分配 (限 64 个: r0..r63) ----------------
R_X, R_Y = 5, 6                          # 注入网格
T0, T1, T2, T3 = 7, 8, 9, 10             # 临时
T4, T5, T6, T7 = 11, 12, 13, 14          # 临时
RP0 = 50                                 # k1 起始
K1, K2, K3, K4, K5, K6 = 50, 51, 52, 53, 54, 55
GX, GY = 56, 57
R_R, R_G, R_B = 58, 59, 60              # 输出 R,G,B

# ---------------- 常量池布局 ----------------
# 0..12 固定标量/FFI; 13..18 切片; 19..27 旋转矩阵9元; 28..30 光源xyz; 31 zscale
C_ONE = 0       # 1.0
C_AMB = 1       # 0.16 环境光
C_LO = 2        # 0.0
C_HI = 3        # 1.0
C_SIN = 4       # sin FFI
C_CLAMP = 5     # clamp FFI
C_R0 = 6        # 0.30
C_R1 = 7        # 1.30
C_R2 = 8        # -0.60
C_G0 = 9        # 0.10
C_G1 = 10       # 0.90
C_B0 = 11       # 0.90
C_B1 = 12       # -0.60
C_K = 13        # 0.84 = 1 - ambient
C_TWO = 14      # 2.0
C_SIX = 15      # 0.6
C_S1 = 16       # slice(1,None), slice(1,None)    Z[1:,1:]
C_S2 = 17       # slice(None,-1), slice(1,None)   Z[:-1,1:]
C_S3 = 18       # slice(1,None), slice(None,-1)   Z[1:,:-1]
C_ROT = 19      # 旋转矩阵 9 元  19..27
C_LX = 28       # 光源 x
C_LY = 29       # 光源 y
C_LZ = 30       # 光源 z
C_ZSCALE = 31   # 高度缩放

I, O = Instruction, OpCode


def _build_instructions():
    ins = []

    def e(op, ops=[]):
        ins.append(I(op, ops))

    def load(r):
        e(O.LOAD_REG, [r])

    def store(r):
        e(O.STORE_REG, [r])

    def const(c):
        e(O.LOAD_CONST, [c])

    def mul(a, b, o):
        load(a); load(b); e(O.MUL, []); store(o)

    def add(a, b, o):
        load(a); load(b); e(O.ADD, []); store(o)

    def mulacc(a, b, acc):
        load(a); load(b); e(O.MUL, [])
        load(acc); e(O.ADD, []); store(acc)

    def cmul(cidx, r, o):
        """常量池标量(idx)×寄存器 -> o"""
        const(cidx); load(r); e(O.MUL, []); store(o)

    # ============ 0) base = sqrt(X²+Y²) -> T4 ============
    mul(R_X, R_X, T0)                   # T0 = X²
    mul(R_Y, R_Y, T1)                   # T1 = Y²
    add(T0, T1, T0)                     # T0 = X²+Y²
    load(T0); e(O.SQRT, []); store(T4)  # T4 = base

    # ============ 1) fractal 高度场 ============
    # z1 = sin(Xk1 + Yk2 + base·k3) -> T0
    mul(K1, R_X, T1)                    # T1 = X·k1
    mul(K2, R_Y, T2)                    # T2 = Y·k2
    add(T1, T2, T1)                     # T1 = Xk1+Yk2
    mul(K3, T4, T2)                     # T2 = base·k3
    add(T1, T2, T1)                     # T1 = Xk1+Yk2+base·k3
    load(T1); const(C_SIN); e(O.CALL, [1]); store(T0)  # T0 = z1
    # 约定: 先压参数 -> load(T1), 再压 func(栈顶) -> const(C_SIN) ✓

    # z2 = sin(Xk4 + Yk5 + 2·base·k6), ·0.6 -> T5(部分)
    mul(K4, R_X, T1)                    # T1 = X·k4
    mul(K5, R_Y, T3)                    # T3 = Y·k5
    add(T1, T3, T1)                     # T1 = Xk4+Yk5
    cmul(C_TWO, T4, T3)                 # T3 = 2·base
    mul(K6, T3, T3)                     # T3 = 2·base·k6
    add(T1, T3, T1)                     # T1 = Xk4+Yk5+2basek6
    load(T1); const(C_SIN); e(O.CALL, [1]); store(T2)  # T2 = z2
    cmul(C_SIX, T2, T2)                 # T2 = 0.6·z2
    add(T0, T2, T5)                     # T5 = Z = z1 + 0.6·z2

    # ============ 2) 中心对齐梯度 (H-1,W-1) ============
    load(T5); const(C_S1); e(O.SLICE, []); store(T3)   # T3 = Z[1:,1:] (Zc)
    load(T5); const(C_S2); e(O.SLICE, []); store(T0)   # T0 = Z[:-1,1:]
    load(T3); load(T0); e(O.SUB, []); store(T0)        # T0 = Zc - 上 = Δy
    mul(GX, T0, T0)                     # T0 = dzdx = Δy·gx
    load(T5); const(C_S3); e(O.SLICE, []); store(T1)   # T1 = Z[1:,:-1]
    load(T3); load(T1); e(O.SUB, []); store(T1)        # T1 = Zc - 左 = Δx
    mul(GY, T1, T1)                     # T1 = dzdy = Δx·gy

    # ============ 3) 法向量 ============
    # nx = -dzdx·zscale ; ny = -dzdy·zscale ; nz = 1
    cmul(C_ZSCALE, T0, T6)
    load(T6); e(O.NEG, []); store(T6)   # T6 = nx
    cmul(C_ZSCALE, T1, T7)
    load(T7); e(O.NEG, []); store(T7)   # T7 = ny
    const(C_ONE); store(T2)             # T2 = nz

    # 归一化: len = sqrt(nx²+ny²+nz²); n / len
    mul(T6, T6, T4)                     # T4 = nx²
    mulacc(T7, T7, T4)                  # T4 += ny²
    mulacc(T2, T2, T4)                  # T4 += nz²
    load(T4); e(O.SQRT, []); store(T4)  # T4 = len
    const(C_ONE); load(T4); e(O.DIV, []); store(T4)  # T4 = 1/len
    mul(T6, T4, T6)                     # nx / len
    mul(T7, T4, T7)                     # ny / len
    mul(T2, T4, T2)                     # nz / len

    # ============ 4) 3D 旋转 R·N (常量 C_ROT..C_ROT+8) ============
    # nx' = R0·nx + R1·ny + R2·nz -> T3
    cmul(C_ROT+0, T6, T3); cmul(C_ROT+1, T7, T0); add(T3, T0, T3)
    cmul(C_ROT+2, T2, T0); add(T3, T0, T3)
    # ny' = R3·nx + R4·ny + R5·nz -> T4
    cmul(C_ROT+3, T6, T4); cmul(C_ROT+4, T7, T0); add(T4, T0, T4)
    cmul(C_ROT+5, T2, T0); add(T4, T0, T4)
    # nz' = R6·nx + R7·ny + R8·nz -> T1
    cmul(C_ROT+6, T6, T1); cmul(C_ROT+7, T7, T0); add(T1, T0, T1)
    cmul(C_ROT+8, T2, T0); add(T1, T0, T1)

    # ============ 5) 朗伯光照 bright = Lx·nx' + Ly·ny' + lz·nz' -> T2 ============
    cmul(C_LX, T3, T2)
    cmul(C_LY, T4, T0); add(T2, T0, T2)
    cmul(C_LZ, T1, T0); add(T2, T0, T2)
    load(T2); const(C_LO); const(C_HI); const(C_CLAMP); e(O.CALL, [3]); store(T2)  # clamp(bright,0,1)

    # ============ 6) shade = ambient + K·bright -> T2 ============
    cmul(C_K, T2, T2)
    const(C_AMB); load(T2); e(O.ADD, []); store(T2)   # T2 = shade

    # ============ 7) 高度假彩 × shade (t = Zc = T3) ============
    mul(T3, T3, T4)                     # T4 = t²
    # R = (R0 + R1·t + R2·t²) · shade
    cmul(C_R2, T4, T0); cmul(C_R1, T3, T1); add(T1, T0, T1)
    const(C_R0); load(T1); e(O.ADD, []); store(T1)
    mul(T1, T2, R_R)
    # G = (G0 + G1·t²) · shade
    cmul(C_G1, T4, T0); const(C_G0); load(T0); e(O.ADD, []); store(T0)
    mul(T0, T2, R_G)
    # B = (B0 + B1·t) · shade
    cmul(C_B1, T3, T0); const(C_B0); load(T0); e(O.ADD, []); store(T0)
    mul(T0, T2, R_B)

    e(O.HALT, [])
    return ins


def build_bytecode(zscale=80.0, lx=0.6, ly=-0.4, lz=0.85, rot=None):
    if rot is None:
        rot = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
    bc = ByteCode()
    bc.metadata = {'graph_name': 'img3d_full', 'desc': 'full-chain pure-VM 3D fractal relief'}
    bc.constants = [
        1.0,            # 0 C_ONE
        0.16,           # 1 C_AMB
        0.0,            # 2 C_LO
        1.0,            # 3 C_HI
        _f_sin,         # 4 C_SIN
        _ff_clamp,      # 5 C_CLAMP
        0.30, 1.30, -0.60,   # 6,7,8 R0,R1,R2
        0.10, 0.90,          # 9,10 G0,G1
        0.90, -0.60,         # 11,12 B0,B1
        0.84,           # 13 C_K
        2.0,            # 14 C_TWO
        0.6,            # 15 C_SIX
        (slice(1, None), slice(1, None)),      # 16 C_S1
        (slice(None, -1), slice(1, None)),     # 17 C_S2
        (slice(1, None), slice(None, -1)),     # 18 C_S3
    ]
    # 19..27 旋转矩阵 9 元
    bc.constants += [float(v) for row in rot for v in row]
    # 28,29,30 光源 xyz; 31 zscale
    bc.constants += [float(lx), float(ly), float(lz), float(zscale)]
    bc.instructions = _build_instructions()
    return bc


def rotation_matrix(az, el):
    import math
    ca, sa = math.cos(az), math.sin(az)
    ce, se = math.cos(el), math.sin(el)
    # 先绕X(el)再绕Y(az): R = Ry(az)·Rx(el)
    R = [
        [ca, sa * se, sa * ce],
        [0, ce, -se],
        [-sa, ca * se, ca * ce],
    ]
    return R


def generate(H, W, params, rot, light, zscale=80.0, x_scale=6.0):
    """
    params: [k1..k6, gx, gy]
    rot: 3×3 matrix
    light: (lx,ly,lz)
    返回 numpy (H-1, W-1, 3) [0,1] 内核
    """
    import numpy as np
    if len(params) != 8:
        raise ValueError('params must have 8 elements [k1..k6,gx,gy]')
    xs = np.linspace(-x_scale, x_scale, W, dtype=float)
    ys = np.linspace(-x_scale, x_scale, H, dtype=float)
    X, Y = np.meshgrid(xs, ys)

    lz = float(light[2])
    bc = build_bytecode(zscale=zscale, lx=light[0], ly=light[1], lz=lz, rot=rot)
    vm = VMInterpreter()
    vm.load_bytecode(bc)
    vm.registers.set(R_X, X)
    vm.registers.set(R_Y, Y)
    for i, v in enumerate(params):
        vm.registers.set(RP0 + i, float(v))

    vm.execute()
    R = np.array(vm.registers.get(R_R), dtype=float)
    G = np.array(vm.registers.get(R_G), dtype=float)
    B = np.array(vm.registers.get(R_B), dtype=float)
    return np.stack([R, G, B], axis=-1)


def to_png(rgb, path):
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
    # [k1..k6, gx, gy]
    params = [0.7, 0.9, 0.5, 1.3, -1.1, 0.4, 0.12, 0.12]
    rot = rotation_matrix(0.6, -0.3)
    t0 = time.time()
    rgb = generate(300, 300, params, rot, (0.6, -0.4, 0.85), zscale=80.0, x_scale=4.5)
    t1 = time.time()
    H, W, _ = rgb.shape
    # 最近邻放大回 H+1 × W+1
    big = np.zeros((H + 1, W + 1, 3))
    big[:H, :W] = rgb
    big[H, :W] = rgb[-1]
    big[:H, W] = rgb[:, -1]
    big[H, W] = rgb[-1, -1]
    print('VM full-chain 3D render (%dx%d kernel) took %.2fs' % (H, W, t1 - t0))
    to_png(big, 'img3d_out/full_chain_test.png')
    print('saved img3d_out/full_chain_test.png')
