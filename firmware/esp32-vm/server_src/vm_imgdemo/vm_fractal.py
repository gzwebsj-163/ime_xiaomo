# -*- coding: utf-8 -*-
"""
vm_fractal.py — 纯 VM 生成分形/噪声图

核心：让「私有 IR 虚拟机」用批量随机张量运算直接算出图像像素矩阵，
宿主只提供 VM 缺失的超越函数(sin) 与 PNG 编码，图像计算本体 100% 由 VM 完成。

算法：虹彩星云
  base     = sqrt(X^2 + Y^2)                  # 径向基
  n1       = sin(X·k1 + Y·k2)                 # 主随机相位噪声
  n2       = sin(X·k3 + Y·k4 + base·k5)       # 分形细节
  R = sigmoid(a1·n1 + b1·n2 + c1·base)
  G = sigmoid(a2·n1 + b2·n2 + c2·base)
  B = sigmoid(a3·n1 + b3·n2 + c3·base)

使用 VM 指令：MUL / ADD / SQRT / SIGMOID / LOAD_REG / STORE_REG / LOAD_CONST / CALL / RET
使用 FFI：f_sin（VM 缺 sin 指令，宿主补逐元素 sin）

寄存器约定（宿主调用前注入）：
  r50 = X 网格 (H,W)   r51 = Y 网格 (H,W)
  r52..r65 = 14 随机参数 [k1..k5, a1,a2,a3, b1,b2,b3, c1,c2,c3]
  r5,r6,r7 = 输出 R,G,B    r8..r11 = 临时
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

RX, RY = 5, 6
RR, RG, RB = 11, 12, 13
T1, T2, T3, T4 = 7, 8, 9, 10
RP0 = 50   # 参数区 r50..r63 (14 个)


def _build_instructions():
    I, O = Instruction, OpCode
    ins = []
    def e(op, ops=[]): ins.append(I(op, ops))
    def mul(a, b, out):
        e(O.LOAD_REG, [a]); e(O.LOAD_REG, [b]); e(O.MUL, []); e(O.STORE_REG, [out])
    def add(a, b, out):
        e(O.LOAD_REG, [a]); e(O.LOAD_REG, [b]); e(O.ADD, []); e(O.STORE_REG, [out])
    def fma_scalar(s, m, acc):
        """acc -= s*m  (accumulate into register acc)"""
        e(O.LOAD_REG, [s]); e(O.LOAD_REG, [m]); e(O.MUL, [])
        e(O.LOAD_REG, [acc]); e(O.ADD, []); e(O.STORE_REG, [acc])

    # base = sqrt(X*X + Y*Y)  -> T2
    mul(RX, RX, T1)                            # T1 = X*X
    mul(RY, RY, T4)                            # T4 = Y*Y
    add(T1, T4, T3)                            # T3 = X*X+Y*Y
    e(O.LOAD_REG, [T3]); e(O.SQRT, []); e(O.STORE_REG, [T2])   # T2 = base

    # n1 = sin(X*k1 + Y*k2) -> T1
    e(O.LOAD_REG, [RP0+0]); e(O.LOAD_REG, [RX]); e(O.MUL, []); e(O.STORE_REG, [T1])
    e(O.LOAD_REG, [RP0+1]); e(O.LOAD_REG, [RY]); e(O.MUL, []); e(O.STORE_REG, [T4])
    add(T1, T4, T4)
    e(O.LOAD_REG, [T4]); e(O.LOAD_CONST, [0]); e(O.CALL, [1]); e(O.STORE_REG, [T1])   # T1 = n1

    # n2 = sin(X*k3 + Y*k4 + base*k5) -> T3
    e(O.LOAD_REG, [RP0+2]); e(O.LOAD_REG, [RX]); e(O.MUL, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [RP0+3]); e(O.LOAD_REG, [RY]); e(O.MUL, []); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [RP0+4]); e(O.LOAD_REG, [T2]); e(O.MUL, []); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [T4]); e(O.LOAD_CONST, [0]); e(O.CALL, [1]); e(O.STORE_REG, [T3])   # T3 = n2

    # R = sigmoid(a1*n1 + b1*n2 + c1*base + biasR) -> RR
    e(O.LOAD_REG, [RP0+5]); e(O.LOAD_REG, [T1]); e(O.MUL, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [RP0+8]); e(O.LOAD_REG, [T3]); e(O.MUL, []); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [RP0+11]); e(O.LOAD_REG, [T2]); e(O.MUL, []); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])
    e(O.LOAD_CONST, [1]); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])   # +biasR
    e(O.LOAD_REG, [T4]); e(O.SIGMOID, []); e(O.STORE_REG, [RR])

    # G = sigmoid(a2*n1 + b2*n2 + c2*base + biasG) -> RG
    e(O.LOAD_REG, [RP0+6]); e(O.LOAD_REG, [T1]); e(O.MUL, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [RP0+9]); e(O.LOAD_REG, [T3]); e(O.MUL, []); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [RP0+12]); e(O.LOAD_REG, [T2]); e(O.MUL, []); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])
    e(O.LOAD_CONST, [2]); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])   # +biasG
    e(O.LOAD_REG, [T4]); e(O.SIGMOID, []); e(O.STORE_REG, [RG])

    # B = sigmoid(a3*n1 + b3*n2 + c3*base + biasB) -> RB
    e(O.LOAD_REG, [RP0+7]); e(O.LOAD_REG, [T1]); e(O.MUL, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [RP0+10]); e(O.LOAD_REG, [T3]); e(O.MUL, []); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])
    e(O.LOAD_REG, [RP0+13]); e(O.LOAD_REG, [T2]); e(O.MUL, []); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])
    e(O.LOAD_CONST, [3]); e(O.LOAD_REG, [T4]); e(O.ADD, []); e(O.STORE_REG, [T4])   # +biasB
    e(O.LOAD_REG, [T4]); e(O.SIGMOID, []); e(O.STORE_REG, [RB])

    e(O.HALT, [])
    return ins


def build_bytecode():
    bc = ByteCode()
    bc.metadata = {'graph_name': 'iridescent_nebula', 'desc': 'pure-VM fractal'}
    bc.constants = [_f_sin, -1.5, -1.5, -1.5]    # 0:SIN, 1..3: 通道偏置
    bc.instructions = _build_instructions()
    return bc


def generate(H, W, params, x_scale=6.0):
    """用 VM 生成分形图。params: 14 个随机值。返回 numpy (H,W,3) [0,1]"""
    import numpy as np
    if len(params) != 14:
        raise ValueError('params must have 14 elements')
    xs = np.linspace(-x_scale, x_scale, W, dtype=float)
    ys = np.linspace(-x_scale, x_scale, H, dtype=float)
    X, Y = np.meshgrid(xs, ys)

    bc = build_bytecode()
    vm = VMInterpreter()
    vm.load_bytecode(bc)
    vm.registers.set(RX, X.tolist())
    vm.registers.set(RY, Y.tolist())
    for i, v in enumerate(params):
        vm.registers.set(RP0 + i, float(v))

    vm.execute()
    R = np.array(vm.registers.get(RR), dtype=float)
    G = np.array(vm.registers.get(RG), dtype=float)
    B = np.array(vm.registers.get(RB), dtype=float)
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
