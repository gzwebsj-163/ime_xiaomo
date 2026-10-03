"""
Layer 5: 私有IR字节码格式
定义字节码的结构、序列化和反序列化方法
"""

import struct
import json
import builtins
import importlib
from typing import List, Tuple, Any, Optional, Union
from dataclasses import dataclass
import hashlib

from .opcode import OpCode, OpCodeCategory


@dataclass
class Instruction:
    """
    单条指令
    包含操作码和操作数
    """
    opcode: OpCode
    operands: List[Any]
    line_number: int = 0
    metadata: Optional[dict] = None

    def __post_init__(self):
        """初始化后验证指令"""
        if not isinstance(self.operands, list):
            self.operands = [self.operands] if self.operands else []

    def __repr__(self) -> str:
        """字符串表示"""
        operand_str = ", ".join(str(op) for op in self.operands)
        return f"{self.opcode.name}({operand_str})"

    def get_size(self) -> int:
        """
        获取指令的字节大小

        Returns:
            指令占用的字节数
        """
        # 操作码占1字节
        size = 1

        # 操作数占用的字节数
        for operand in self.operands:
            if isinstance(operand, int):
                # 整数：根据大小选择编码
                if -128 <= operand <= 127:
                    size += 1  # int8
                elif -32768 <= operand <= 32767:
                    size += 2  # int16
                else:
                    size += 4  # int32
            elif isinstance(operand, float):
                size += 8  # float64
            elif isinstance(operand, str):
                # 字符串：长度(2字节) + 内容
                size += 2 + len(operand.encode('utf-8'))
            else:
                # 其他类型：默认4字节
                size += 4

        return size


class ByteCode:
    """
    字节码类
    管理指令序列、常量池和元数据
    """

    # 字节码魔术数和版本
    MAGIC_NUMBER = b'PRIV_IR'
    VERSION = 1

    def __init__(self):
        """初始化字节码"""
        self.instructions: List[Instruction] = []
        self.constants: List[Any] = []  # 常量池
        self.names: List[str] = []  # 名称表
        self.metadata: dict = {}  # 元数据
        self._encryption_key: int = 0x5A  # 默认加密密钥

    # ==================== Callable 引用序列化（tag 0x08） ====================
    # 对于 callable 常量（函数、类、builtin），不能直接 pickle，序列化为
    # "module_qualname" 字符串：
    #   - builtins.abs               → "builtins.abs"
    #   - builtins.list              → "builtins.list"
    #   - tensor.matmul (模块函数)   → "crypto.obf_engine.layers.layer5_private_ir.tensor.matmul"
    #   - compiler._floordiv         → "crypto.obf_engine.layers.layer5_private_ir.compiler._floordiv"

    @staticmethod
    def _encode_callable_ref(func: Any) -> Optional[str]:
        """尝试为 callable 生成可重新解析的 qualname 字符串，失败返回 None"""
        if not callable(func):
            return None
        # builtins 模块对象（如 abs / list / tuple / set / dict）
        name = getattr(func, '__name__', None)
        if name is None:
            return None
        if getattr(builtins, name, None) is func:
            return f"builtins.{name}"
        # 普通模块级函数 / 类
        module_name = getattr(func, '__module__', None)
        qualname = getattr(func, '__qualname__', None) or name
        if module_name and qualname and '<locals>' not in qualname:
            return f"{module_name}:{qualname}"
        return None

    @staticmethod
    def _decode_callable_ref(ref: str) -> Optional[Any]:
        """通过 qualname 字符串重新解析 callable，失败返回 None"""
        if not isinstance(ref, str) or ':' not in ref:
            if isinstance(ref, str) and ref.startswith('builtins.'):
                return getattr(builtins, ref[len('builtins.'):], None)
            return None
        module_name, qualname = ref.split(':', 1)
        try:
            mod = importlib.import_module(module_name)
        except Exception:
            return None
        try:
            obj = mod
            for part in qualname.split('.'):
                obj = getattr(obj, part)
            return obj
        except Exception:
            return None

    @staticmethod
    def _decode_builtins_ref(ref: str) -> Optional[Any]:
        """兼容形如 "builtins.xxx" 的引用"""
        if isinstance(ref, str) and ref.startswith('builtins.'):
            return getattr(builtins, ref[len('builtins.'):], None)
        return None

    def add_instruction(self, instruction: Instruction) -> int:
        """
        添加指令到字节码

        Args:
            instruction: 要添加的指令

        Returns:
            指令的索引位置
        """
        self.instructions.append(instruction)
        return len(self.instructions) - 1

    def add_constant(self, value: Any) -> int:
        """
        添加常量到常量池

        Args:
            value: 常量值

        Returns:
            常量的索引
        """
        # 检查是否已存在
        for idx, const in enumerate(self.constants):
            if const == value:
                return idx

        self.constants.append(value)
        return len(self.constants) - 1

    def add_name(self, name: str) -> int:
        """
        添加名称到名称表

        Args:
            name: 名称字符串

        Returns:
            名称的索引
        """
        if name in self.names:
            return self.names.index(name)

        self.names.append(name)
        return len(self.names) - 1

    def get_instruction(self, index: int) -> Optional[Instruction]:
        """
        获取指定索引的指令

        Args:
            index: 指令索引

        Returns:
            指令对象，如果索引无效返回None
        """
        if 0 <= index < len(self.instructions):
            return self.instructions[index]
        return None

    def set_metadata(self, key: str, value: Any):
        """
        设置元数据

        Args:
            key: 元数据键
            value: 元数据值
        """
        self.metadata[key] = value

    def get_metadata(self, key: str) -> Optional[Any]:
        """
        获取元数据

        Args:
            key: 元数据键

        Returns:
            元数据值，如果不存在返回None
        """
        return self.metadata.get(key)

    def serialize(self, encrypt: bool = True) -> bytes:
        """
        将字节码序列化为二进制格式

        Args:
            encrypt: 是否加密字节码

        Returns:
            序列化后的字节数据
        """
        data = bytearray()

        # 写入文件头
        data.extend(self.MAGIC_NUMBER)
        data.extend(struct.pack('<H', self.VERSION))

        # 写入元数据
        metadata_bytes = json.dumps(self.metadata, ensure_ascii=False, default=str).encode('utf-8')
        data.extend(struct.pack('<I', len(metadata_bytes)))
        data.extend(metadata_bytes)

        # 写入常量池
        self._serialize_constants(data)

        # 写入名称表
        self._serialize_names(data)

        # 写入指令序列
        self._serialize_instructions(data)

        # 计算校验和
        checksum = hashlib.sha256(bytes(data)).digest()[:16]
        data.extend(checksum)

        # 加密
        if encrypt:
            data = self._encrypt_bytes(bytes(data))

        return bytes(data)

    def _serialize_constants(self, data: bytearray):
        """
        序列化常量池

        Args:
            data: 输出字节数组
        """
        # 写入常量数量
        data.extend(struct.pack('<I', len(self.constants)))

        # 写入每个常量
        for const in self.constants:
            if isinstance(const, bool):
                data.append(0x04)  # bool类型标记
                data.append(1 if const else 0)
            elif isinstance(const, int):
                data.append(0x01)  # int类型标记
                data.extend(struct.pack('<q', const))
            elif isinstance(const, float):
                data.append(0x02)  # float类型标记
                data.extend(struct.pack('<d', const))
            elif isinstance(const, str):
                data.append(0x03)  # str类型标记
                encoded = const.encode('utf-8')
                data.extend(struct.pack('<I', len(encoded)))
                data.extend(encoded)
            elif isinstance(const, bytes):
                data.append(0x05)  # bytes类型标记
                data.extend(struct.pack('<I', len(const)))
                data.extend(const)
            else:
                # callable 引用：尝试序列化为 0x08 tag（module_qualname 字符串）
                ref = self._encode_callable_ref(const) if callable(const) else None
                if ref is not None:
                    data.append(0x08)  # callable-ref 类型标记
                    encoded = ref.encode('utf-8')
                    data.extend(struct.pack('<I', len(encoded)))
                    data.extend(encoded)
                else:
                    # 其他类型：转换为字符串（保留兼容性）
                    data.append(0x03)
                    encoded = str(const).encode('utf-8')
                    data.extend(struct.pack('<I', len(encoded)))
                    data.extend(encoded)

    def _serialize_names(self, data: bytearray):
        """
        序列化名称表

        Args:
            data: 输出字节数组
        """
        # 写入名称数量
        data.extend(struct.pack('<I', len(self.names)))

        # 写入每个名称
        for name in self.names:
            encoded = name.encode('utf-8')
            data.extend(struct.pack('<I', len(encoded)))
            data.extend(encoded)

    def _serialize_instructions(self, data: bytearray):
        """
        序列化指令序列

        Args:
            data: 输出字节数组
        """
        # 写入指令数量
        data.extend(struct.pack('<I', len(self.instructions)))

        # 写入每条指令
        for inst in self.instructions:
            # 写入操作码（加密）
            opcode_value = OpCode.encrypt_opcode(inst.opcode, self._encryption_key)
            data.append(opcode_value)

            # 写入操作数数量
            data.append(len(inst.operands))

            # 写入操作数
            for operand in inst.operands:
                self._serialize_operand(data, operand)

    def _serialize_operand(self, data: bytearray, operand: Any):
        """
        序列化单个操作数

        Args:
            data: 输出字节数组
            operand: 操作数值
        """
        if isinstance(operand, bool):
            data.append(0x06)  # bool标记
            data.append(1 if operand else 0)
        elif isinstance(operand, int):
            if -128 <= operand <= 127:
                data.append(0x01)  # int8标记
                data.extend(struct.pack('<b', operand))
            elif -32768 <= operand <= 32767:
                data.append(0x02)  # int16标记
                data.extend(struct.pack('<h', operand))
            else:
                data.append(0x03)  # int32标记
                data.extend(struct.pack('<i', operand))
        elif isinstance(operand, float):
            data.append(0x04)  # float64标记
            data.extend(struct.pack('<d', operand))
        elif isinstance(operand, str):
            data.append(0x05)  # string标记
            encoded = operand.encode('utf-8')
            data.extend(struct.pack('<I', len(encoded)))
            data.extend(encoded)
        elif isinstance(operand, bytes):
            data.append(0x07)  # bytes标记
            data.extend(struct.pack('<I', len(operand)))
            data.extend(operand)
        else:
            # 其他类型：None标记
            data.append(0x00)

    @classmethod
    def deserialize(cls, data: bytes, decrypt: bool = True) -> 'ByteCode':
        """
        从二进制数据反序列化字节码

        Args:
            data: 序列化的字节数据
            decrypt: 是否解密字节码

        Returns:
            ByteCode对象

        Raises:
            ValueError: 如果数据格式无效
        """
        bytecode = cls()

        # 解密
        if decrypt:
            data = bytecode._decrypt_bytes(data)

        # 创建字节读取器
        reader = ByteReader(data)

        # 验证魔术数和版本
        magic = reader.read_bytes(len(cls.MAGIC_NUMBER))
        if magic != cls.MAGIC_NUMBER:
            raise ValueError(f"无效的字节码文件：魔术数不匹配")

        version = reader.read_uint16()
        if version != cls.VERSION:
            raise ValueError(f"不支持的字节码版本：{version}")

        # 读取元数据
        metadata_len = reader.read_uint32()
        metadata_str = reader.read_bytes(metadata_len).decode('utf-8')
        try:
            bytecode.metadata = json.loads(metadata_str)
        except json.JSONDecodeError:
            bytecode.metadata = {}

        # 读取常量池
        bytecode._deserialize_constants(reader)

        # 读取名称表
        bytecode._deserialize_names(reader)

        # 读取指令序列
        bytecode._deserialize_instructions(reader)

        # 验证校验和
        expected_checksum = reader.read_bytes(16)
        actual_checksum = hashlib.sha256(data[:-16]).digest()[:16]
        if expected_checksum != actual_checksum:
            raise ValueError("字节码校验和不匹配，文件可能已损坏")

        return bytecode

    def _deserialize_constants(self, reader: 'ByteReader'):
        """
        反序列化常量池

        Args:
            reader: 字节读取器
        """
        count = reader.read_uint32()
        for _ in range(count):
            type_tag = reader.read_byte()
            if type_tag == 0x01:  # int
                value = reader.read_int64()
            elif type_tag == 0x02:  # float
                value = reader.read_float64()
            elif type_tag == 0x03:  # str
                length = reader.read_uint32()
                value = reader.read_bytes(length).decode('utf-8')
            elif type_tag == 0x04:  # bool
                value = bool(reader.read_byte())
            elif type_tag == 0x05:  # bytes
                length = reader.read_uint32()
                value = reader.read_bytes(length)
            elif type_tag == 0x08:  # callable 引用（module_qualname 字符串）
                length = reader.read_uint32()
                ref = reader.read_bytes(length).decode('utf-8')
                value = self._decode_callable_ref(ref)
                if value is None:
                    raise ValueError(f"无法解析 callable 引用: {ref!r}")
            else:
                raise ValueError(f"未知的常量类型标记：{type_tag}")

            self.constants.append(value)

    def _deserialize_names(self, reader: 'ByteReader'):
        """
        反序列化名称表

        Args:
            reader: 字节读取器
        """
        count = reader.read_uint32()
        for _ in range(count):
            length = reader.read_uint32()
            name = reader.read_bytes(length).decode('utf-8')
            self.names.append(name)

    def _deserialize_instructions(self, reader: 'ByteReader'):
        """
        反序列化指令序列

        Args:
            reader: 字节读取器
        """
        count = reader.read_uint32()
        for _ in range(count):
            # 读取操作码
            opcode_encrypted = reader.read_byte()
            opcode = OpCode.decrypt_opcode(opcode_encrypted, self._encryption_key)

            # 读取操作数数量
            operand_count = reader.read_byte()

            # 读取操作数
            operands = []
            for _ in range(operand_count):
                operand = self._deserialize_operand(reader)
                operands.append(operand)

            # 创建指令
            instruction = Instruction(opcode=opcode, operands=operands)
            self.instructions.append(instruction)

    def _deserialize_operand(self, reader: 'ByteReader') -> Any:
        """
        反序列化单个操作数

        Args:
            reader: 字节读取器

        Returns:
            操作数值
        """
        type_tag = reader.read_byte()

        if type_tag == 0x01:  # int8
            return reader.read_int8()
        elif type_tag == 0x02:  # int16
            return reader.read_int16()
        elif type_tag == 0x03:  # int32
            return reader.read_int32()
        elif type_tag == 0x04:  # float64
            return reader.read_float64()
        elif type_tag == 0x05:  # string
            length = reader.read_uint32()
            return reader.read_bytes(length).decode('utf-8')
        elif type_tag == 0x06:  # bool
            return bool(reader.read_byte())
        elif type_tag == 0x07:  # bytes
            length = reader.read_uint32()
            return reader.read_bytes(length)
        else:  # None
            return None

    def _encrypt_bytes(self, data: bytes) -> bytes:
        """
        加密字节数据

        Args:
            data: 原始数据

        Returns:
            加密后的数据
        """
        result = bytearray()
        key = self._encryption_key

        for i, byte in enumerate(data):
            # 使用位置相关的密钥加密
            pos_key = (key + i) % 256
            encrypted_byte = (byte ^ pos_key) & 0xFF
            result.append(encrypted_byte)

        return bytes(result)

    def _decrypt_bytes(self, data: bytes) -> bytes:
        """
        解密字节数据

        Args:
            data: 加密的数据

        Returns:
            解密后的数据
        """
        # XOR加密是对称的，加密和解密使用相同操作
        return self._encrypt_bytes(data)

    def get_instruction_count(self) -> int:
        """获取指令数量"""
        return len(self.instructions)

    def get_byte_size(self) -> int:
        """获取字节码总大小"""
        return len(self.serialize(encrypt=False))

    def optimize(self):
        """
        优化字节码
        移除冗余指令，合并相似操作
        """
        optimized = []
        i = 0
        while i < len(self.instructions):
            inst = self.instructions[i]

            # 移除连续的NOP指令
            if inst.opcode == OpCode.NOP:
                while i + 1 < len(self.instructions) and self.instructions[i + 1].opcode == OpCode.NOP:
                    i += 1

            # 合并连续的PUSH-POP对
            if inst.opcode == OpCode.PUSH and i + 1 < len(self.instructions):
                next_inst = self.instructions[i + 1]
                if next_inst.opcode == OpCode.POP:
                    # 跳过这对指令
                    i += 2
                    continue

            optimized.append(inst)
            i += 1

        self.instructions = optimized

    def disassemble(self) -> str:
        """
        反汇编字节码为可读格式

        Returns:
            反汇编字符串
        """
        lines = []
        lines.append("=" * 60)
        lines.append("私有IR字节码反汇编")
        lines.append("=" * 60)
        lines.append(f"指令数量: {len(self.instructions)}")
        lines.append(f"常量数量: {len(self.constants)}")
        lines.append(f"名称数量: {len(self.names)}")
        lines.append("")
        lines.append("常量池:")
        for idx, const in enumerate(self.constants):
            lines.append(f"  [{idx:4d}] {repr(const)}")
        lines.append("")
        lines.append("名称表:")
        for idx, name in enumerate(self.names):
            lines.append(f"  [{idx:4d}] {name}")
        lines.append("")
        lines.append("指令序列:")
        lines.append("-" * 60)

        for idx, inst in enumerate(self.instructions):
            category = OpCodeCategory.categorize(inst.opcode)
            lines.append(f"  {idx:4d}: {inst!r:40s} [{category}]")

        lines.append("=" * 60)

        return "\n".join(lines)


class ByteReader:
    """
    字节读取辅助类
    用于从字节数组中读取各种数据类型
    """

    def __init__(self, data: bytes):
        """
        初始化字节读取器

        Args:
            data: 字节数据
        """
        self.data = data
        self.pos = 0

    def read_byte(self) -> int:
        """读取单字节"""
        if self.pos >= len(self.data):
            raise ValueError("数据读取超出范围")
        byte = self.data[self.pos]
        self.pos += 1
        return byte

    def read_bytes(self, count: int) -> bytes:
        """读取多字节"""
        if self.pos + count > len(self.data):
            raise ValueError("数据读取超出范围")
        result = self.data[self.pos:self.pos + count]
        self.pos += count
        return result

    def read_int8(self) -> int:
        """读取8位有符号整数"""
        return struct.unpack('<b', self.read_bytes(1))[0]

    def read_int16(self) -> int:
        """读取16位有符号整数"""
        return struct.unpack('<h', self.read_bytes(2))[0]

    def read_int32(self) -> int:
        """读取32位有符号整数"""
        return struct.unpack('<i', self.read_bytes(4))[0]

    def read_int64(self) -> int:
        """读取64位有符号整数"""
        return struct.unpack('<q', self.read_bytes(8))[0]

    def read_uint8(self) -> int:
        """读取8位无符号整数"""
        return struct.unpack('<B', self.read_bytes(1))[0]

    def read_uint16(self) -> int:
        """读取16位无符号整数"""
        return struct.unpack('<H', self.read_bytes(2))[0]

    def read_uint32(self) -> int:
        """读取32位无符号整数"""
        return struct.unpack('<I', self.read_bytes(4))[0]

    def read_uint64(self) -> int:
        """读取64位无符号整数"""
        return struct.unpack('<Q', self.read_bytes(8))[0]

    def read_float32(self) -> float:
        """读取32位浮点数"""
        return struct.unpack('<f', self.read_bytes(4))[0]

    def read_float64(self) -> float:
        """读取64位浮点数"""
        return struct.unpack('<d', self.read_bytes(8))[0]

    def remaining(self) -> int:
        """获取剩余字节数"""
        return len(self.data) - self.pos

    def is_end(self) -> bool:
        """判断是否已到达末尾"""
        return self.pos >= len(self.data)