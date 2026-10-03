# -*- coding: utf-8 -*-
"""
edge_vm.py — ESP32-S3 边缘私有IR虚拟机 (MicroPython版)
在 ESP32-S3 上本地解析并执行服务器私有的 PRIV_IR 字节码(.bc)，
实现分布式边缘推理，分担服务器算力。

特性:
  - 兼容服务器 bytecode.py 序列化格式 (PRIV_IR 魔数 + XOR加密0x5A + 指令)
  - 支持 MLP/RNN 推理所需指令子集 (matmul/add/relu/sigmoid/tanh/softmax...)
  - 可选 ulab 加速矩阵乘法 (MicroPython版numpy)，无 ulab 时纯Python回退
  - 纯MicroPython标准库 (struct/math), 无第三方依赖

用法:
  from edge_vm import EdgeVM
  vm = EdgeVM(bc_bytes, params=[x,w1,b1,w2,b2])
  result = vm.run()
"""

import struct
import math

# ================= 操作码子集 =================
# (与服务器 opcode.py 一致, 仅实现推理所需)
OP_ADD = 0x01
OP_SUB = 0x02
OP_MUL = 0x03
OP_DIV = 0x04
OP_MATMUL = 0x10
OP_TRANSPOSE = 0x11
OP_RESHAPE = 0x12
OP_SLICE = 0x13
OP_CONCAT = 0x14
OP_REDUCE_SUM = 0x1C
OP_REDUCE_MEAN = 0x1D
OP_LOAD_PARAM = 0x29
OP_LOAD_REG = 0x2A
OP_STORE_REG = 0x2B
OP_LOAD_CONST = 0x2C
OP_JUMP = 0x30
OP_JUMP_IF = 0x31
OP_CALL = 0x32
OP_RET = 0x33
OP_NOP = 0x34
OP_HALT = 0x35
OP_CMP_EQ = 0x40
OP_RELU = 0x50
OP_SIGMOID = 0x51
OP_TANH = 0x52
OP_GELU = 0x53
OP_SOFTMAX = 0x54
OP_CAST_FLOAT = 0x71
OP_PUSH = 0x80
OP_POP = 0x81

OP_NAMES = {
    OP_ADD:'ADD', OP_SUB:'SUB', OP_MUL:'MUL', OP_DIV:'DIV',
    OP_MATMUL:'MATMUL', OP_TRANSPOSE:'TRANSPOSE', OP_RESHAPE:'RESHAPE',
    OP_SLICE:'SLICE', OP_CONCAT:'CONCAT', OP_REDUCE_SUM:'REDUCE_SUM',
    OP_REDUCE_MEAN:'REDUCE_MEAN', OP_LOAD_PARAM:'LOAD_PARAM',
    OP_LOAD_REG:'LOAD_REG', OP_STORE_REG:'STORE_REG', OP_LOAD_CONST:'LOAD_CONST',
    OP_JUMP:'JUMP', OP_JUMP_IF:'JUMP_IF', OP_CALL:'CALL', OP_RET:'RET',
    OP_NOP:'NOP', OP_HALT:'HALT', OP_CMP_EQ:'CMP_EQ',
    OP_RELU:'RELU', OP_SIGMOID:'SIGMOID', OP_TANH:'TANH',
    OP_GELU:'GELU', OP_SOFTMAX:'SOFTMAX', OP_CAST_FLOAT:'CAST_FLOAT',
    OP_PUSH:'PUSH', OP_POP:'POP',
}

MAGIC = b'PRIV_IR'
VERSION = 1
ENC_KEY = 0x5A
PARAM_BASE = 50

# ============ ulab 加速 (可选) ============
try:
    import ulab.numpy as _np
    _HAS_ULAB = True
except ImportError:
    _HAS_ULAB = False

# ============ 张量运算 (纯Python, 与服务器tensor.py一致) ============

def _flatten(a):
    if not isinstance(a, (list, tuple)):
        return [a]
    r = []
    for x in a:
        r.extend(_flatten(x))
    return r

def _shape(a):
    s = []
    c = a
    while isinstance(c, (list, tuple)):
        s.append(len(c))
        if len(c) == 0:
            break
        c = c[0]
    return tuple(s)

def _is2d(a):
    return isinstance(a, list) and len(a) > 0 and isinstance(a[0], list)

def _mapn(a, fn):
    if isinstance(a, list):
        return [_mapn(x, fn) for x in a]
    if isinstance(a, tuple):
        return tuple(_mapn(x, fn) for x in a)
    return fn(a)

def _fromshape(flat, shape):
    if len(shape) == 0:
        return flat[0]
    if len(shape) == 1:
        return list(flat)
    stride = 1
    for d in shape[1:]:
        stride *= d
    return [_fromshape(flat[i * stride:(i + 1) * stride], shape[1:]) for i in range(shape[0])]

def t_matmul(a, b):
    """矩阵乘法，语义与服务器tensor.matmul一致"""
    if _HAS_ULAB:
        ra = _np.array(a, dtype=_np.float_) if isinstance(a, list) else a
        rb = _np.array(b, dtype=_np.float_) if isinstance(b, list) else b
        try:
            r = ra @ rb
            # 统一返回 list
            if len(r.shape) == 0:
                return float(r)
            return r.tolist()
        except Exception:
            pass
    a1 = not _is2d(a)
    b1 = not _is2d(b)
    if a1 and b1:
        return sum(x * y for x, y in zip(a, b))
    a2 = [a] if a1 else a
    b2 = [[x] for x in b] if b1 else b
    rows = len(a2)
    inner = len(a2[0])
    cols = len(b2[0])
    if inner != len(b2):
        raise ValueError('matmul dim mismatch: (%d,%d)@(%d,%d)' % (rows, inner, len(b2), cols))
    res = [[0.0] * cols for _ in range(rows)]
    for i in range(rows):
        ai = a2[i]
        for k in range(inner):
            aik = ai[k]
            if aik == 0:
                continue
            bk = b2[k]
            ri = res[i]
            for j in range(cols):
                ri[j] += aik * bk[j]
    if a1:
        return res[0]
    if b1:
        return [row[0] for row in res]
    return res

def t_add(a, b):
    if isinstance(a, (int, float)) and isinstance(b, list):
        return _mapn(b, lambda y: a + y)
    if isinstance(b, (int, float)) and isinstance(a, list):
        return _mapn(a, lambda x: x + b)
    if _is2d(a):
        return [[a[i][j] + b[i][j] for j in range(len(a[i]))] for i in range(len(a))]
    if _is2d(b):
        return [[a[i][j] + b[i][j] for j in range(len(b[i]))] for i in range(len(b))]
    return [x + y for x, y in zip(a, b)]

def t_relu(a):
    if isinstance(a, list):
        return _mapn(a, lambda x: x if x > 0 else 0)
    return a if a > 0 else 0

def t_softmax(a):
    flat = _flatten(a)
    m = max(flat)
    ex = [math.exp(x - m) for x in flat]
    tot = sum(ex)
    out = [e / tot for e in ex]
    return _fromshape(out, _shape(a))

def t_transpose(a):
    if not _is2d(a):
        return a
    return [list(r) for r in zip(*a)]

def t_cast_float(a):
    if isinstance(a, list):
        return _mapn(a, float)
    return float(a)

# ============ 字节码解析器 ============

class _BR:
    __slots__ = ('d', 'p')
    def __init__(self, data):
        self.d = data
        self.p = 0
    def byte(self):
        v = self.d[self.p]; self.p += 1; return v
    def u16(self):
        v = struct.unpack('<H', self.d[self.p:self.p + 2])[0]; self.p += 2; return v
    def u32(self):
        v = struct.unpack('<I', self.d[self.p:self.p + 4])[0]; self.p += 4; return v
    def i8(self):
        v = struct.unpack('<b', self.d[self.p:self.p + 1])[0]; self.p += 1; return v
    def i16(self):
        v = struct.unpack('<h', self.d[self.p:self.p + 2])[0]; self.p += 2; return v
    def i32(self):
        v = struct.unpack('<i', self.d[self.p:self.p + 4])[0]; self.p += 4; return v
    def f64(self):
        v = struct.unpack('<d', self.d[self.p:self.p + 8])[0]; self.p += 8; return v
    def bytes(self, n):
        v = bytes(self.d[self.p:self.p + n]); self.p += n; return v

def _decrypt(data):
    """位置相关XOR解密 (与服务器一致: byte ^ (key+i)%256)"""
    key = ENC_KEY
    out = bytearray(len(data))
    for i in range(len(data)):
        out[i] = data[i] ^ ((key + i) & 0xFF)
    return out

def deserialize(raw):
    """解析 .bc 字节码，返回 (metadata_dict, instructions)
    instructions: list of (opcode, operand_list)"""
    if not raw.startswith(MAGIC):
        raw = _decrypt(raw)
    r = _BR(raw)
    if r.bytes(7) != MAGIC:
        raise ValueError('bad magic')
    if r.u16() != VERSION:
        raise ValueError('bad version')
    # 元数据
    mlen = r.u32()
    meta_str = bytes(r.bytes(mlen)).decode('utf-8')
    meta = {}
    try:
        import json
        meta = json.loads(meta_str)
    except Exception:
        pass
    # 常量池
    constants = []
    cc = r.u32()
    for _ in range(cc):
        t = r.byte()
        if t == 0x01:
            val = r.i8()
        elif t == 0x02:
            val = r.f64()
        elif t == 0x03:
            L = r.u32(); val = bytes(r.bytes(L)).decode('utf-8')
        elif t == 0x04:
            val = bool(r.byte())
        elif t == 0x05:
            L = r.u32(); val = r.bytes(L)
        else:
            val = None
        constants.append(val)
    # 名称表
    names = []
    nc = r.u32()
    for _ in range(nc):
        L = r.u32(); names.append(bytes(r.bytes(L)).decode('utf-8'))
    # 指令
    instrs = []
    ic = r.u32()
    for _ in range(ic):
        op = r.byte() ^ ENC_KEY
        oc = r.byte()
        ops = []
        for _ in range(oc):
            t = r.byte()
            if t == 0x01:
                v = r.i8()
            elif t == 0x02:
                v = r.i16()
            elif t == 0x03:
                v = r.i32()
            elif t == 0x04:
                v = r.f64()
            elif t == 0x05:
                L = r.u32(); v = bytes(r.bytes(L)).decode('utf-8')
            elif t == 0x06:
                v = bool(r.byte())
            elif t == 0x07:
                L = r.u32(); v = r.bytes(L)
            else:
                v = None
            ops.append(v)
        instrs.append((op, ops))
    return meta, constants, names, instrs

# ============ 解释器 ============

class EdgeVM:
    def __init__(self, bc_bytes, params=None, max_instr=100000):
        meta, constants, names, self.instrs = deserialize(bc_bytes)
        self.meta = meta
        self.constants = constants
        self.regs = [None] * 64
        if params:
            for i, v in enumerate(params):
                self.regs[PARAM_BASE + i] = v
        self.stack = []
        self.max_instr = max_instr

    def _push(self, v):
        self.stack.append(v)
    def _pop(self):
        return self.stack.pop()

    def run(self):
        pc = 0
        n = 0
        instrs = self.instrs
        while pc < len(instrs) and n < self.max_instr:
            n += 1
            op, ops = instrs[pc]
            if op == OP_ADD:
                b = self._pop(); a = self._pop(); self._push(t_add(a, b))
            elif op == OP_SUB:
                b = self._pop(); a = self._pop()
                if isinstance(a, list): self._push([x - y for x, y in zip(a, b)])
                else: self._push(a - b)
            elif op == OP_MUL:
                b = self._pop(); a = self._pop()
                if isinstance(a, list): self._push([x * y for x, y in zip(a, b)])
                else: self._push(a * b)
            elif op == OP_DIV:
                b = self._pop(); a = self._pop()
                if isinstance(a, list): self._push([(x / y if y else 0) for x, y in zip(a, b)])
                else: self._push(a / b if b else 0)
            elif op == OP_MATMUL:
                b = self._pop(); a = self._pop(); self._push(t_matmul(a, b))
            elif op == OP_TRANSPOSE:
                a = self._pop(); self._push(t_transpose(a))
            elif op == OP_LOAD_REG:
                self._push(self.regs[ops[0]])
            elif op == OP_STORE_REG:
                self.regs[ops[0]] = self._pop()
            elif op == OP_LOAD_PARAM:
                self._push(self.regs[PARAM_BASE + ops[0]])
            elif op == OP_LOAD_CONST:
                self._push(self.constants[ops[0]])
            elif op == OP_RELU:
                a = self._pop(); self._push(t_relu(a))
            elif op == OP_SOFTMAX:
                a = self._pop(); self._push(t_softmax(a))
            elif op == OP_SIGMOID:
                a = self._pop()
                if isinstance(a, list): self._push(_mapn(a, lambda x: 1.0 / (1.0 + math.exp(-x))))
                else: self._push(1.0 / (1.0 + math.exp(-a)))
            elif op == OP_TANH:
                a = self._pop()
                if isinstance(a, list): self._push(_mapn(a, math.tanh))
                else: self._push(math.tanh(a))
            elif op == OP_CAST_FLOAT:
                a = self._pop(); self._push(t_cast_float(a))
            elif op == OP_NOP:
                pass
            elif op == OP_HALT or op == OP_RET:
                break
            elif op == OP_JUMP:
                pc = ops[0] - 1
            elif op == OP_JUMP_IF:
                target = ops[0]
                expected = ops[1] if len(ops) > 1 else True
                cond = self._pop()
                if bool(cond) == bool(expected):
                    pc = target - 1
            elif op == OP_PUSH:
                self._push(ops[0] if ops else None)
            elif op == OP_POP:
                self._pop()
            pc += 1
        return self.stack[-1] if self.stack else None


def disassemble(bc_bytes):
    """反汇编字节码为可读文本"""
    meta, constants, names, instrs = deserialize(bc_bytes)
    lines = []
    lines.append('== %s | %d instructions ==' % (meta.get('graph_name', 'graph'), len(instrs)))
    for i, (op, ops) in enumerate(instrs):
        lines.append('  [%3d] %-12s %s' % (i, OP_NAMES.get(op, '0x%02X' % op), ops))
    return '\n'.join(lines)
