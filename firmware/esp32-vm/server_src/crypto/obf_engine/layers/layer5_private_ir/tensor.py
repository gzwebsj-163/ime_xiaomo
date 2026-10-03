"""
张量运算模块（Layer 5 私有IR虚拟机依赖）

提供与 numpy 等价的张量运算接口，numpy 可用时自动走 numpy 加速，
否则回退到纯 Python 实现，保证虚拟机可以在任意环境运行。
"""

from __future__ import annotations

import math
from typing import Any, List, Optional, Union, Sequence

try:
    import numpy as _np
    _HAS_NUMPY = True
except ImportError:  # pragma: no cover - 无 numpy 环境的回退
    _np = None
    _HAS_NUMPY = False


def available() -> bool:
    """numpy 是否可用"""
    return _HAS_NUMPY


def is_tensor(value: Any) -> bool:
    """判断值是否为张量（numpy 数组或任意非空 list/tuple）"""
    if _HAS_NUMPY and isinstance(value, _np.ndarray):
        return True
    return isinstance(value, (list, tuple)) and len(value) > 0


def shape_of(value: Any) -> tuple:
    """获取张量形状"""
    if _HAS_NUMPY and isinstance(value, _np.ndarray):
        return tuple(value.shape)
    shape = []
    cur = value
    while isinstance(cur, (list, tuple)):
        shape.append(len(cur))
        if len(cur) == 0:
            break
        cur = cur[0]
    return tuple(shape)


# ==================== 基础二元运算 ====================

def add(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return _np.add(a, b)
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x + y)
    return a + b


def sub(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return _np.subtract(a, b)
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x - y)
    return a - b


def mul(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return _np.multiply(a, b)
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x * y)
    return a * b


def div(a: Any, b: Any) -> Any:
    """安全除法：标量除零返回 0，张量逐元素安全除"""
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        with _np.errstate(divide='ignore', invalid='ignore'):
            result = _np.divide(a, b)
            if isinstance(b, _np.ndarray) or (isinstance(b, (int, float)) and b == 0):
                return _np.where(b == 0, 0, result)
            return result
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop_safe_div(a, b)
    return a / b if b != 0 else 0


def mod(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return _np.mod(a, b)
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x % y if y != 0 else 0)
    return a % b if b != 0 else 0


def pow_op(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return _np.power(a, b)
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x ** y)
    return a ** b


def bitwise_or(a: Any, b: Any) -> Any:
    return a | b


def bitwise_and(a: Any, b: Any) -> Any:
    return a & b


def bitwise_xor(a: Any, b: Any) -> Any:
    return a ^ b


# ==================== 一元运算 ====================

def neg(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.negative(a)
    if is_tensor(a):
        return _elementwise_unop(a, lambda x: -x)
    return -a


def abs_op(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.abs(a)
    if is_tensor(a):
        return _elementwise_unop(a, lambda x: abs(x))
    return abs(a)


def sqrt(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.sqrt(a)
    if is_tensor(a):
        return _elementwise_unop(a, lambda x: math.sqrt(x) if x >= 0 else float('nan'))
    return a ** 0.5


def exp(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.exp(a)
    if is_tensor(a):
        return _elementwise_unop(a, math.exp)
    return math.exp(a)


def log(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.log(a)
    if is_tensor(a):
        return _elementwise_unop(a, lambda x: math.log(x) if x > 0 else float('-inf'))
    return math.log(a) if a > 0 else float('-inf')


def not_op(a: Any) -> bool:
    return not bool(a)


# ==================== 比较运算 ====================

def cmp_eq(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return a == b
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x == y)
    return a == b


def cmp_ne(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return a != b
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x != y)
    return a != b


def cmp_lt(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return a < b
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x < y)
    return a < b


def cmp_le(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return a <= b
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x <= y)
    return a <= b


def cmp_gt(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return a > b
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x > y)
    return a > b


def cmp_ge(a: Any, b: Any) -> Any:
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return a >= b
    if is_tensor(a) or is_tensor(b):
        return _elementwise_binop(a, b, lambda x, y: x >= y)
    return a >= b


def cmp_and(a: Any, b: Any) -> Any:
    return bool(a) and bool(b)


def cmp_or(a: Any, b: Any) -> Any:
    return bool(a) or bool(b)


# ==================== 矩阵 / 张量运算 ====================

def matmul(a: Any, b: Any) -> Any:
    """矩阵乘法，对齐 numpy 语义：
    - 2D@2D -> 2D
    - 1D@2D -> 1D（a 视为行向量 (1,n)）
    - 2D@1D -> 1D（b 视为列向量 (m,1)）
    - 1D@1D -> 标量（点积）
    """
    if _HAS_NUMPY and (isinstance(a, _np.ndarray) or isinstance(b, _np.ndarray)):
        return _np.matmul(a, b)
    a = _as_list(a)
    b = _as_list(b)
    a_is_1d = not is_2d(a)
    b_is_1d = not is_2d(b)

    if a_is_1d and b_is_1d:
        return sum(x * y for x, y in zip(a, b))

    a_2d = [a] if a_is_1d else a        # 1D -> (1,n)
    b_2d = [[x] for x in b] if b_is_1d else b   # 1D -> (m,1)

    rows, inner = len(a_2d), len(a_2d[0])
    cols = len(b_2d[0])
    if inner != len(b_2d):
        raise ValueError(f"matmul 维度不匹配: {(rows, inner)} @ ({len(b_2d)}, {cols})")
    result = [
        [sum(a_2d[i][k] * b_2d[k][j] for k in range(inner)) for j in range(cols)]
        for i in range(rows)
    ]
    if a_is_1d:
        return result[0]                # 去掉前置的 1 维
    if b_is_1d:
        return [row[0] for row in result]  # 去掉后置的 1 维
    return result


def transpose(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.transpose(a)
    if not is_2d(a):
        return a
    return [list(row) for row in zip(*a)]


def reshape(a: Any, shape: Sequence[int]) -> Any:
    """重塑张量；-1 表示自动推导"""
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.reshape(a, shape)
    flat = _flatten(_as_list(a))
    dims = list(shape)
    if -1 in dims:
        known = 1
        for d in dims:
            if d != -1:
                known *= d
        auto = len(flat) // known if known else 0
        dims = [auto if d == -1 else d for d in dims]
    if _prod(dims) != len(flat):
        raise ValueError(f"reshape 元素数不匹配: {_prod(dims)} != {len(flat)}")
    return _from_shape(flat, dims)


def slice_op(a: Any, indices: Any) -> Any:
    """切片：indices 可为 int / slice / tuple"""
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return a[indices]
    if isinstance(indices, tuple):
        result = a
        for idx in indices:
            result = result[idx]
        return result
    return a[indices]


def concat(values: List[Any], axis: int = 0) -> Any:
    """拼接：沿 axis 拼接列表中的张量"""
    if _HAS_NUMPY and any(isinstance(v, _np.ndarray) for v in values):
        return _np.concatenate(values, axis=axis)
    values = [_as_list(v) for v in values]
    if axis == 0:
        result = []
        for v in values:
            result.extend(v)
        return result
    if axis == 1:
        return [sum((v[i] for v in values), []) for i in range(len(values[0]))]
    raise ValueError(f"不支持的拼接轴: {axis}")


def split_op(a: Any, indices_or_sections: Any, axis: int = 0) -> List[Any]:
    """分割：按 sections 数量均分或按索引切分"""
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return list(_np.split(a, indices_or_sections, axis=axis))
    a = _as_list(a)
    if isinstance(indices_or_sections, int):
        n = indices_or_sections
        size = (len(a) + n - 1) // n
        return [a[i * size:(i + 1) * size] for i in range(n) if a[i * size:(i + 1) * size]]
    # 按索引切分
    result, start = [], 0
    for idx in indices_or_sections:
        result.append(a[start:idx])
        start = idx
    result.append(a[start:])
    return result


def permute(a: Any, axes: Sequence[int]) -> Any:
    """维度置换（仅 2D 常用场景）"""
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.transpose(a, axes)
    if len(axes) == 2:
        return transpose(a)
    return a


def squeeze(a: Any) -> Any:
    """去除长度为 1 的维度"""
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.squeeze(a)
    a = _as_list(a)
    if len(a) == 1 and isinstance(a[0], list):
        return a[0]
    return a


def unsqueeze(a: Any, axis: int = 0) -> Any:
    """增加长度为 1 的维度"""
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.expand_dims(a, axis)
    if axis == 0:
        return [a]
    if axis == 1:
        return [[x] for x in a]
    return a


def reduce_sum(a: Any, axis: Optional[int] = None) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.sum(a, axis=axis)
    if axis is None:
        return sum(_flatten(_as_list(a)))
    a = _as_list(a)
    if axis == 0:
        return [sum(col) for col in zip(*a)]
    return [sum(row) for row in a]


def reduce_mean(a: Any, axis: Optional[int] = None) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.mean(a, axis=axis)
    total = reduce_sum(a, axis=axis)
    if axis is None:
        count = len(_flatten(_as_list(a)))
        return total / count if count else 0
    a = _as_list(a)
    count = len(a) if axis == 0 else len(a[0])
    if isinstance(total, list):
        return [t / count for t in total]
    return total / count


def reduce_max(a: Any, axis: Optional[int] = None) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.max(a, axis=axis)
    a = _as_list(a)
    if axis is None:
        return max(_flatten(a))
    if axis == 0:
        return [max(col) for col in zip(*a)]
    return [max(row) for row in a]


# ==================== 激活函数 ====================

def relu(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.maximum(a, 0)
    if is_tensor(a):
        return _elementwise_unop(a, lambda x: max(x, 0))
    return max(a, 0)


def sigmoid(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return 1 / (1 + _np.exp(-a))
    if is_tensor(a):
        return _elementwise_unop(a, lambda x: 1 / (1 + math.exp(-x)))
    return 1 / (1 + math.exp(-a))


def tanh(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return _np.tanh(a)
    if is_tensor(a):
        return _elementwise_unop(a, math.tanh)
    return math.tanh(a)


def gelu(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return 0.5 * a * (1 + _np.tanh(_np.sqrt(2 / math.pi) * (a + 0.044715 * a ** 3)))
    if is_tensor(a):
        return _elementwise_unop(a, lambda x: 0.5 * x * (1 + math.tanh(math.sqrt(2 / math.pi) * (x + 0.044715 * x ** 3))))
    return 0.5 * a * (1 + math.tanh(math.sqrt(2 / math.pi) * (a + 0.044715 * a ** 3)))


def softmax(a: Any) -> Any:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        exp_a = _np.exp(a - _np.max(a))
        return exp_a / _np.sum(exp_a)
    if is_tensor(a):
        flat = _flatten(_as_list(a))
        m = max(flat)
        exps = [math.exp(x - m) for x in flat]
        total = sum(exps)
        out = [e / total for e in exps]
        return _from_shape(out, shape_of(a))
    return 1.0  # 标量退化为恒等


# ==================== 内部工具 ====================

def _elementwise_binop(a: Any, b: Any, fn) -> Any:
    """逐元素二元运算（支持 numpy 风格的广播）"""
    # 标量 x 任意：直接广播
    if isinstance(b, (int, float, bool)) and not isinstance(b, bool) or \
       (isinstance(b, (int, float, bool)) and isinstance(a, (list, tuple))):
        return _map_nested(a, lambda x: fn(x, b))
    if isinstance(a, (int, float, bool)) and isinstance(b, (list, tuple)):
        return _map_nested(b, lambda y: fn(a, y))
    # 布尔标量 x 张量
    if type(b) is bool and isinstance(a, (list, tuple)):
        return _map_nested(a, lambda x: fn(x, b))
    if type(a) is bool and isinstance(b, (list, tuple)):
        return _map_nested(b, lambda y: fn(a, y))

    # 形状相同：直接逐元素
    if _same_shape(a, b):
        if is_2d(a):
            return [[fn(a[i][j], b[i][j]) for j in range(len(a[i]))] for i in range(len(a))]
        return [fn(x, y) for x, y in zip(a, b)]

    # 尝试广播：
    sa = shape_of(a)
    sb = shape_of(b)
    # 情况1: a 为 2D (1, N) 且 b 为 1D (N,) -> 等价于 (1, N) + (1, N) 按行
    if len(sa) == 2 and len(sb) == 1 and sa[1] == sb[0]:
        return [[fn(a[i][j], b[j]) for j in range(sa[1])] for i in range(sa[0])]
    # 情况2: a 为 1D (N,) 且 b 为 2D (M, N)
    if len(sa) == 1 and len(sb) == 2 and sa[0] == sb[1]:
        return [[fn(a[j], b[i][j]) for j in range(sb[1])] for i in range(sb[0])]
    # 情况3: 行向量 (1, N) + 列向量 (M, 1) -> 广播到 (M, N)
    if len(sa) == 2 and len(sb) == 2 and sa[0] == 1 and sb[1] == 1 and sa[1] == sb[0]:
        n = sa[1]
        return [[fn(a[0][j], b[i][0]) for j in range(n)] for i in range(n)]

    raise ValueError(f"形状不匹配: {shape_of(a)} vs {shape_of(b)}")


def _elementwise_binop_safe_div(a: Any, b: Any) -> Any:
    """安全除法（处理除零，支持广播）"""
    if isinstance(b, (int, float, bool)):
        return _map_nested(a, lambda x: x / b if b != 0 else 0)
    if isinstance(a, (int, float, bool)):
        return _map_nested(b, lambda y: a / y if y != 0 else 0)
    if _same_shape(a, b):
        if is_2d(a):
            return [[a[i][j] / b[i][j] if b[i][j] != 0 else 0 for j in range(len(a[i]))] for i in range(len(a))]
        return [x / y if y != 0 else 0 for x, y in zip(a, b)]
    # 广播规则同 _elementwise_binop
    sa = shape_of(a)
    sb = shape_of(b)
    if len(sa) == 2 and len(sb) == 1 and sa[1] == sb[0]:
        return [[a[i][j] / b[j] if b[j] != 0 else 0 for j in range(sa[1])] for i in range(sa[0])]
    if len(sa) == 1 and len(sb) == 2 and sa[0] == sb[1]:
        return [[a[j] / b[i][j] if b[i][j] != 0 else 0 for j in range(sb[1])] for i in range(sb[0])]
    raise ValueError(f"形状不匹配: {shape_of(a)} vs {shape_of(b)}")


def _elementwise_unop(a: Any, fn) -> Any:
    return _map_nested(a, fn)


def _map_nested(a: Any, fn):
    if isinstance(a, list):
        return [_map_nested(x, fn) for x in a]
    if isinstance(a, tuple):
        return tuple(_map_nested(x, fn) for x in a)
    return fn(a)


def _as_list(a: Any) -> list:
    if _HAS_NUMPY and isinstance(a, _np.ndarray):
        return a.tolist()
    return list(a) if isinstance(a, (list, tuple)) else a


def _flatten(a: Any) -> list:
    if not isinstance(a, (list, tuple)):
        return [a]
    result = []
    for x in a:
        result.extend(_flatten(x))
    return result


def _from_shape(flat: list, shape: tuple) -> Any:
    """按形状从扁平列表还原嵌套结构"""
    if len(shape) == 0:
        return flat[0]
    if len(shape) == 1:
        return flat
    stride = _prod(shape[1:])
    return [_from_shape(flat[i * stride:(i + 1) * stride], shape[1:]) for i in range(shape[0])]


def _prod(seq: Sequence[int]) -> int:
    result = 1
    for x in seq:
        result *= x
    return result


def is_2d(a: Any) -> bool:
    return isinstance(a, list) and len(a) > 0 and isinstance(a[0], list)


def _same_shape(a: Any, b: Any) -> bool:
    return shape_of(a) == shape_of(b)


# 统一导出
__all__ = [
    'available', 'is_tensor', 'shape_of',
    'add', 'sub', 'mul', 'div', 'mod', 'pow_op',
    'bitwise_or', 'bitwise_and', 'bitwise_xor',
    'neg', 'abs_op', 'sqrt', 'exp', 'log', 'not_op',
    'cmp_eq', 'cmp_ne', 'cmp_lt', 'cmp_le', 'cmp_gt', 'cmp_ge', 'cmp_and', 'cmp_or',
    'matmul', 'transpose', 'reshape', 'slice_op', 'concat', 'split_op',
    'permute', 'squeeze', 'unsqueeze', 'reduce_sum', 'reduce_mean', 'reduce_max',
    'relu', 'sigmoid', 'tanh', 'gelu', 'softmax',
]
