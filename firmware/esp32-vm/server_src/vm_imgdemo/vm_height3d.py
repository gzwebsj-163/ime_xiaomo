# -*- coding: utf-8 -*-
"""
vm_height3d.py — 用「私有 IR 虚拟机」对 2D 高度场做三维运算并渲染立体图

核心理念：三维变换主链（法向量 → 3D 旋转 → 朗伯光照 → 高度假彩）100% 由 VM
张量指令 (MUL/ADD/NEG/SQRT/DIV) 批量逐像素算出，宿主仅注入：
  - 高度场的 x/y 方向梯度 (H,W) 矩阵   (邻域卷积 VM 无此算子，宿主算)
  - 3×3 旋转矩阵 9 系数 + 光源方向 3 分量 + 高度缩放 zscale
  - ff_clamp 裁剪 FFI（VM 缺 clamp 指令）

寄存器约定（宿主调用前注入）：
  r50 = dzdx (H,W)   r51 = dzdy (H,W)   r52 = t 归一化高度 (H,W)
  r53..r61 = 旋转矩阵 9 系数 (row-major R00..R22)
  r62 = Lx, r63 = Ly, r64 = Lz, r65 = zscale
  r5,r6,r7 = 输出 R,G,B  r8..r15 = 临时
"""
import struct
import zlib

from server_src.crypto.obf_engine.layers.layer5_private_ir.bytecode import ByteCode, Instruction
from server_src.crypto.obf_engine.layers.layer5_private_ir.opcode import OpCode
from server_src.crypto.obf_engine.layers.layer5_private_ir.interpreter import VMInterpreter


# ---------------- 宿主 FFI（仅 VM 缺失的琐碎算子） ----------------
def _ff_clamp(x, lo=0.0, hi=1.0):
    if isinstance(x, list):
        return [_ff_clamp(e, lo, hi) for e in x]
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x


# ---------------- 寄存器分配 ----------------
DZDX, DZDY, T = 50, 51, 52          # 注入的梯度/归一化高度
RBASE = 53                          # 旋转矩阵 R00..R22 -> r53..r61
LX, LY = 62, 63                     # 光源 x,y 分量 (Lz 放常量池)
RR, RG, RB = 5, 6, 7
TM1, TM2, TM3, TM4, TM5, TM6, TM7, TM8 = 8, 9, 10, 11, 12, 13, 14, 15

# ---------------- 常量池布局 ----------------
C_ONE = 0       # 1.0
C_AMBIENT = 1   # 0.16
C_LO = 2        # 0.0
C_HI = 3        # 1.0
C_CLAMP = 4     # ff_clamp
C_R0 = 5        # R基色 0.30
C_R1 = 6        # 1.30
C_R2 = 7        # -0.60
C_G0 = 8        # 0.10
C_G1 = 9        # 0.90
C_B0 = 10       # 0.90
C_B1 = 11       # -0.60
C_K = 12        # (1-ambient)=0.84
C_LZ = 13       # 光源 z 分量 (宿主注入)
C_ZSCALE = 14   # 高度缩放 (宿主注入)

I, O = Instruction, OpCode


def _build_instructions():
    ins = []

    def e(op, ops=[]): ins.append(I(op, ops))
    def load(r): e(O.LOAD_REG, [r])
    def store(r): e(O.STORE_REG, [r])
    def const(c): e(O.LOAD_CONST, [c])

    def mul(a, b, o):
        load(a); load(b); e(O.MUL, []); store(o)

    def add(a, b, o):
        load(a); load(b); e(O.ADD, []); store(o)

    def mulacc(a, b, acc):
        load(a); load(b); e(O.MUL, [])
        load(acc); e(O.ADD, []); store(acc)

    def cmul(c, r, o):
        """常数×寄存器 -> o"""
        const(c); load(r); e(O.MUL, []); store(o)

    def cmul_c(cidx, r, o):
        """常量池标量(idx)×寄存器 -> o"""
        const(cidx); load(r); e(O.MUL, []); store(o)

    # ---------- 1) 法向量 ----------
    cmul_c(C_ZSCALE, DZDX, TM1)      # TM1 = dzdx·zscale
    load(TM1); e(O.NEG, []); store(TM1)  # TM1 = -dzdx·s = nx
    cmul_c(C_ZSCALE, DZDY, TM2)      # TM2 = dzdy·s
    load(TM2); e(O.NEG, []); store(TM2)  # TM2 = -dzdy·s = ny
    const(C_ONE); store(TM3)         # TM3 = nz = 1

    # ---------- 2) 归一化 ----------
    mul(TM1, TM1, TM4)               # TM4 = nx²
    mulacc(TM2, TM2, TM4)
    mulacc(TM3, TM3, TM4)
    load(TM4); e(O.SQRT, []); store(TM4)   # TM4 = len
    const(C_ONE); load(TM4); e(O.DIV, []); store(TM5)  # TM5 = 1/len
    mul(TM1, TM5, TM1)               # 归一化 nx
    mul(TM2, TM5, TM2)
    mul(TM3, TM5, TM3)

    # ---------- 3) 3D 旋转 R·n ----------
    mul(RBASE+0, TM1, TM6); mulacc(RBASE+1, TM2, TM6); mulacc(RBASE+2, TM3, TM6)   # nx'
    mul(RBASE+3, TM1, TM7); mulacc(RBASE+4, TM2, TM7); mulacc(RBASE+5, TM3, TM7)   # ny'
    mul(RBASE+6, TM1, TM8); mulacc(RBASE+7, TM2, TM8); mulacc(RBASE+8, TM3, TM8)   # nz'

    # ---------- 4) 朗伯光照 bright = L·n' -> TM5 ----------
    mul(LX, TM6, TM5)                       # bright = Lx·nx'
    mulacc(LY, TM7, TM5)                    # bright += Ly·ny'
    const(C_LZ); load(TM8); e(O.MUL, []); load(TM5); e(O.ADD, []); store(TM5)  # += Lz·nz'
    # clamp(bright, 0, 1)  (CALL: 参数先压栈, func最后)
    load(TM5); const(C_LO); const(C_HI); const(C_CLAMP); e(O.CALL, [3]); store(TM5)

    # ---------- 5) shade = ambient + K·bright -> TM4 ----------
    cmul(C_K, TM5, TM4)
    const(C_AMBIENT); load(TM4); e(O.ADD, []); store(TM4)  # TM4 = shade

    # ---------- 6) 高度假彩 × shade ----------
    # t² = TM1
    mul(T, T, TM1)
    # R = (R0 + R1·t + R2·t²) · shade
    cmul(C_R2, TM1, TM2)             # TM2 = R2·t²
    cmul(C_R1, T, TM3); load(TM3); load(TM2); e(O.ADD, []); store(TM3)  # TM3 = R1·t + R2·t²
    const(C_R0); load(TM3); e(O.ADD, []); store(TM3)                     # TM3 = R0+R1t+R2t²
    mul(TM3, TM4, RR)                # RR = R·shade
    # G = (G0 + G1·t²) · shade
    cmul(C_G1, TM1, TM2)             # TM2 = G1·t²
    const(C_G0); load(TM2); e(O.ADD, []); store(TM2)
    mul(TM2, TM4, RG)
    # B = (B0 + B1·t) · shade
    cmul(C_B1, T, TM2)
    const(C_B0); load(TM2); e(O.ADD, []); store(TM2)
    mul(TM2, TM4, RB)

    e(O.HALT, [])
    return ins


def build_bytecode(zscale=90.0, lz=0.9):
    bc = ByteCode()
    bc.metadata = {'graph_name': 'height3d_render', 'desc': 'pure-VM 3D relief render'}
    bc.constants = [1.0, 0.16, 0.0, 1.0, _ff_clamp,
                    0.30, 1.30, -0.60, 0.10, 0.90,
                    0.90, -0.60, 0.84, float(lz), float(zscale)]
    bc.instructions = _build_instructions()
    return bc


def rotation_matrix(az, el):
    """生成 3×3 旋转矩阵 (row-major 9 系数)。az 绕Y, el 绕X。"""
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


def generate(H, W, dzdx, dzdy, t, rot, light, zscale=90.0):
    """
    用 VM 渲染 3D 高度场。
    dzdx/dzdy/t: (H,W) list/matrix
    rot: 3×3 matrix (list of 3 lists)
    light: (Lx,Ly,Lz) tuple (需已归一化或任意, VM内不强制归一)
    zscale: 高度缩放(法线陡峭度)
    返回 numpy (H,W,3) [0,1]
    """
    import numpy as np
    dzdx = np.asarray(dzdx, dtype=float).tolist()
    dzdy = np.asarray(dzdy, dtype=float).tolist()
    t = np.asarray(t, dtype=float).tolist()
    lz = float(light[2])

    bc = build_bytecode(zscale=zscale, lz=lz)
    vm = VMInterpreter()
    vm.load_bytecode(bc)
    vm.registers.set(DZDX, dzdx)
    vm.registers.set(DZDY, dzdy)
    vm.registers.set(T, t)
    flat = [float(v) for row in rot for v in row]
    for i, v in enumerate(flat):
        vm.registers.set(RBASE + i, float(v))
    vm.registers.set(LX, float(light[0]))
    vm.registers.set(LY, float(light[1]))

    vm.execute()
    R = np.array(vm.registers.get(RR), dtype=float)
    G = np.array(vm.registers.get(RG), dtype=float)
    B = np.array(vm.registers.get(RB), dtype=float)
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
