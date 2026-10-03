"""
Layer 5: 私有IR操作码定义
定义虚拟机使用的所有操作码，包含加密编号
"""

from enum import IntEnum
import hashlib


class OpCode(IntEnum):
    """
    私有IR操作码枚举
    每个操作码都有一个加密的编号，通过哈希计算得出
    """

    # ==================== 算术操作 (10个) ====================
    # 基本算术运算
    ADD = 0x01        # 加法操作
    SUB = 0x02        # 减法操作
    MUL = 0x03        # 乘法操作
    DIV = 0x04        # 除法操作
    MOD = 0x05        # 取模操作
    NEG = 0x06        # 取负操作
    ABS = 0x07        # 绝对值操作
    SQRT = 0x08       # 平方根操作
    EXP = 0x09        # 指数操作
    LOG = 0x0A        # 对数操作
    POW = 0x0B        # 幂运算
    BOR = 0x0C        # 按位或
    BAND = 0x0D       # 按位与
    BXOR = 0x0E       # 按位异或
    NOT = 0x0F        # 逻辑非

    # ==================== 张量操作 (15个) ====================
    # 张量运算指令
    MATMUL = 0x10     # 矩阵乘法
    TRANSPOSE = 0x11  # 转置操作
    RESHAPE = 0x12    # 形状变换
    SLICE = 0x13      # 切片操作
    CONCAT = 0x14     # 拼接操作
    SPLIT = 0x15      # 分割操作
    PERMUTE = 0x16    # 维度置换
    SQUEEZE = 0x17    # 去除维度为1的维度
    UNSQUEEZE = 0x18  # 增加维度为1的维度
    BROADCAST = 0x19  # 广播操作
    GATHER = 0x1A     # 收集操作
    SCATTER = 0x1B    # 散射操作
    REDUCE_SUM = 0x1C # 归约求和
    REDUCE_MEAN = 0x1D # 归约求均值
    REDUCE_MAX = 0x1E # 归约求最大值

    # ==================== 内存操作 (10个) ====================
    # 内存管理指令
    LOAD = 0x20       # 加载数据
    STORE = 0x21      # 存储数据
    ALLOC = 0x22      # 分配内存
    FREE = 0x23       # 释放内存
    COPY = 0x24       # 复制数据
    MOVE = 0x25       # 移动数据
    FILL = 0x26       # 填充数据
    ZERO = 0x27       # 清零数据
    LOAD_CONST = 0x28 # 加载常量
    LOAD_PARAM = 0x29 # 加载参数
    LOAD_REG = 0x2A   # 从寄存器加载局部变量
    STORE_REG = 0x2B  # 存储局部变量到寄存器

    # ==================== 控制操作 (10个) ====================
    # 控制流指令
    JUMP = 0x30       # 无条件跳转
    JUMP_IF = 0x31    # 条件跳转
    CALL = 0x32       # 函数调用
    RET = 0x33        # 函数返回
    NOP = 0x34        # 空操作
    HALT = 0x35       # 停止执行
    LOOP = 0x36       # 循环指令
    BREAK = 0x37      # 跳出循环
    CONTINUE = 0x38   # 继续循环
    SWITCH = 0x39     # 多路分支

    # ==================== 比较操作 (8个) ====================
    # 比较运算指令
    CMP_EQ = 0x40     # 相等比较
    CMP_NE = 0x41     # 不等比较
    CMP_LT = 0x42     # 小于比较
    CMP_LE = 0x43     # 小于等于比较
    CMP_GT = 0x44     # 大于比较
    CMP_GE = 0x45     # 大于等于比较
    CMP_AND = 0x46    # 逻辑与
    CMP_OR = 0x47     # 逻辑或

    # ==================== 激活函数 (5个) ====================
    # 常用激活函数
    RELU = 0x50       # ReLU激活
    SIGMOID = 0x51    # Sigmoid激活
    TANH = 0x52       # Tanh激活
    GELU = 0x53       # GELU激活
    SOFTMAX = 0x54    # Softmax激活

    # ==================== 混淆操作 (15个) ====================
    # 混淆保护指令
    OBF_PI = 0x60          # PI值混淆
    OBF_DECRYPT = 0x61     # 解密操作
    OBF_CORRECT = 0x62     # 数值校正
    OBF_OPAQUE = 0x63      # 不透明谓词
    OBF_BOGUS = 0x64       # 虚假分支
    OBF_FLATTEN = 0x65     # 控制流平坦化
    OBF_SUBSTITUTE = 0x66  # 指令替换
    OBF_MERGE = 0x67       # 代码合并
    OBF_SPLIT = 0x68       # 代码拆分
    OBF_ENCODE = 0x69      # 数据编码
    OBF_DECODE = 0x6A      # 数据解码
    OBF_JUNK = 0x6B        # 垃圾代码
    OBF_PROXY = 0x6C       # 代理函数
    OBF_WRAP = 0x6D        # 函数包装
    OBF_TRANSFORM = 0x6E   # 数据变换

    # ==================== 类型操作 (5个) ====================
    # 类型转换指令
    CAST_INT = 0x70    # 转换为整数
    CAST_FLOAT = 0x71  # 转换为浮点数
    CAST_BOOL = 0x72   # 转换为布尔值
    TYPEOF = 0x73      # 获取类型
    ISNULL = 0x74      # 检查是否为空

    # ==================== 栈操作 (5个) ====================
    # 栈操作指令
    PUSH = 0x80        # 压栈
    POP = 0x81         # 出栈
    DUP = 0x82         # 复制栈顶
    SWAP = 0x83        # 交换栈顶两个元素
    ROT = 0x84         # 旋转栈元素

    # ==================== 特殊操作 (5个) ====================
    # 特殊功能指令
    DEBUG = 0x90       # 调试指令
    PROFILE = 0x91     # 性能分析
    ASSERT = 0x92      # 断言检查
    VERIFY = 0x93      # 验证指令
    SANITY = 0x94      # 完整性检查

    @classmethod
    def encrypt_opcode(cls, opcode: 'OpCode', key: int = 0x5A) -> int:
        """
        加密操作码
        使用XOR加密保护操作码值

        Args:
            opcode: 操作码枚举值
            key: 加密密钥

        Returns:
            加密后的操作码值
        """
        return opcode.value ^ key

    @classmethod
    def decrypt_opcode(cls, encrypted_value: int, key: int = 0x5A) -> 'OpCode':
        """
        解密操作码
        将加密的操作码还原为原始操作码

        Args:
            encrypted_value: 加密的操作码值
            key: 解密密钥

        Returns:
            解密后的操作码枚举值

        Raises:
            ValueError: 如果解密后的值不是有效的操作码
        """
        decrypted_value = encrypted_value ^ key
        try:
            return cls(decrypted_value)
        except ValueError:
            raise ValueError(f"无效的操作码值: {decrypted_value}")

    @classmethod
    def get_opcode_hash(cls, opcode: 'OpCode') -> str:
        """
        获取操作码的哈希值
        用于操作码完整性验证

        Args:
            opcode: 操作码枚举值

        Returns:
            操作码的SHA256哈希值（前16位）
        """
        data = f"{opcode.name}:{opcode.value}".encode('utf-8')
        hash_value = hashlib.sha256(data).hexdigest()
        return hash_value[:16]


class OpCodeCategory:
    """
    操作码分类
    将操作码按照功能类别进行分组
    """

    # 算术操作码列表
    ARITHMETIC = [
        OpCode.ADD, OpCode.SUB, OpCode.MUL, OpCode.DIV,
        OpCode.MOD, OpCode.NEG, OpCode.ABS, OpCode.SQRT,
        OpCode.EXP, OpCode.LOG, OpCode.POW,
        OpCode.BOR, OpCode.BAND, OpCode.BXOR, OpCode.NOT
    ]

    # 张量操作码列表
    TENSOR = [
        OpCode.MATMUL, OpCode.TRANSPOSE, OpCode.RESHAPE,
        OpCode.SLICE, OpCode.CONCAT, OpCode.SPLIT,
        OpCode.PERMUTE, OpCode.SQUEEZE, OpCode.UNSQUEEZE,
        OpCode.BROADCAST, OpCode.GATHER, OpCode.SCATTER,
        OpCode.REDUCE_SUM, OpCode.REDUCE_MEAN, OpCode.REDUCE_MAX
    ]

    # 内存操作码列表
    MEMORY = [
        OpCode.LOAD, OpCode.STORE, OpCode.ALLOC,
        OpCode.FREE, OpCode.COPY, OpCode.MOVE,
        OpCode.FILL, OpCode.ZERO, OpCode.LOAD_CONST,
        OpCode.LOAD_PARAM, OpCode.LOAD_REG, OpCode.STORE_REG
    ]

    # 控制流操作码列表
    CONTROL = [
        OpCode.JUMP, OpCode.JUMP_IF, OpCode.CALL,
        OpCode.RET, OpCode.NOP, OpCode.HALT,
        OpCode.LOOP, OpCode.BREAK, OpCode.CONTINUE,
        OpCode.SWITCH
    ]

    # 比较操作码列表
    COMPARISON = [
        OpCode.CMP_EQ, OpCode.CMP_NE, OpCode.CMP_LT,
        OpCode.CMP_LE, OpCode.CMP_GT, OpCode.CMP_GE,
        OpCode.CMP_AND, OpCode.CMP_OR
    ]

    # 激活函数操作码列表
    ACTIVATION = [
        OpCode.RELU, OpCode.SIGMOID, OpCode.TANH,
        OpCode.GELU, OpCode.SOFTMAX
    ]

    # 混淆操作码列表
    OBFUSCATION = [
        OpCode.OBF_PI, OpCode.OBF_DECRYPT, OpCode.OBF_CORRECT,
        OpCode.OBF_OPAQUE, OpCode.OBF_BOGUS, OpCode.OBF_FLATTEN,
        OpCode.OBF_SUBSTITUTE, OpCode.OBF_MERGE, OpCode.OBF_SPLIT,
        OpCode.OBF_ENCODE, OpCode.OBF_DECODE, OpCode.OBF_JUNK,
        OpCode.OBF_PROXY, OpCode.OBF_WRAP, OpCode.OBF_TRANSFORM
    ]

    # 类型操作码列表
    TYPE = [
        OpCode.CAST_INT, OpCode.CAST_FLOAT, OpCode.CAST_BOOL,
        OpCode.TYPEOF, OpCode.ISNULL
    ]

    # 栈操作码列表
    STACK = [
        OpCode.PUSH, OpCode.POP, OpCode.DUP,
        OpCode.SWAP, OpCode.ROT
    ]

    # 特殊操作码列表
    SPECIAL = [
        OpCode.DEBUG, OpCode.PROFILE, OpCode.ASSERT,
        OpCode.VERIFY, OpCode.SANITY
    ]

    @classmethod
    def categorize(cls, opcode: OpCode) -> str:
        """
        判断操作码所属类别

        Args:
            opcode: 操作码枚举值

        Returns:
            操作码类别名称
        """
        if opcode in cls.ARITHMETIC:
            return "ARITHMETIC"
        elif opcode in cls.TENSOR:
            return "TENSOR"
        elif opcode in cls.MEMORY:
            return "MEMORY"
        elif opcode in cls.CONTROL:
            return "CONTROL"
        elif opcode in cls.COMPARISON:
            return "COMPARISON"
        elif opcode in cls.ACTIVATION:
            return "ACTIVATION"
        elif opcode in cls.OBFUSCATION:
            return "OBFUSCATION"
        elif opcode in cls.TYPE:
            return "TYPE"
        elif opcode in cls.STACK:
            return "STACK"
        elif opcode in cls.SPECIAL:
            return "SPECIAL"
        else:
            return "UNKNOWN"


# 操作码属性字典
OPCODE_PROPERTIES = {
    # 算术操作
    OpCode.ADD: {"operands": 2, "result": 1, "side_effect": False},
    OpCode.SUB: {"operands": 2, "result": 1, "side_effect": False},
    OpCode.MUL: {"operands": 2, "result": 1, "side_effect": False},
    OpCode.DIV: {"operands": 2, "result": 1, "side_effect": False},
    OpCode.MOD: {"operands": 2, "result": 1, "side_effect": False},

    # 张量操作
    OpCode.MATMUL: {"operands": 2, "result": 1, "side_effect": False},
    OpCode.TRANSPOSE: {"operands": 1, "result": 1, "side_effect": False},
    OpCode.RESHAPE: {"operands": 2, "result": 1, "side_effect": False},
    OpCode.SLICE: {"operands": 3, "result": 1, "side_effect": False},

    # 内存操作
    OpCode.LOAD: {"operands": 1, "result": 1, "side_effect": False},
    OpCode.STORE: {"operands": 2, "result": 0, "side_effect": True},
    OpCode.ALLOC: {"operands": 1, "result": 1, "side_effect": True},
    OpCode.FREE: {"operands": 1, "result": 0, "side_effect": True},
    OpCode.LOAD_REG: {"operands": 1, "result": 1, "side_effect": False},
    OpCode.STORE_REG: {"operands": 1, "result": 0, "side_effect": True},
    OpCode.LOAD_PARAM: {"operands": 1, "result": 1, "side_effect": False},

    # 控制操作
    OpCode.JUMP: {"operands": 1, "result": 0, "side_effect": True},
    OpCode.JUMP_IF: {"operands": 2, "result": 0, "side_effect": True},
    OpCode.CALL: {"operands": -1, "result": -1, "side_effect": True},
    OpCode.RET: {"operands": -1, "result": 0, "side_effect": True},

    # 混淆操作
    OpCode.OBF_PI: {"operands": 0, "result": 1, "side_effect": False},
    OpCode.OBF_DECRYPT: {"operands": 1, "result": 1, "side_effect": False},
    OpCode.OBF_CORRECT: {"operands": 2, "result": 1, "side_effect": False},
}


def is_control_flow(opcode: OpCode) -> bool:
    """
    判断操作码是否为控制流指令

    Args:
        opcode: 操作码枚举值

    Returns:
        如果是控制流指令返回True，否则返回False
    """
    return opcode in OpCodeCategory.CONTROL


def is_memory_operation(opcode: OpCode) -> bool:
    """
    判断操作码是否为内存操作指令

    Args:
        opcode: 操作码枚举值

    Returns:
        如果是内存操作指令返回True，否则返回False
    """
    return opcode in OpCodeCategory.MEMORY


def has_side_effect(opcode: OpCode) -> bool:
    """
    判断操作码是否有副作用

    Args:
        opcode: 操作码枚举值

    Returns:
        如果有副作用返回True，否则返回False
    """
    props = OPCODE_PROPERTIES.get(opcode, {})
    return props.get("side_effect", False)


def get_operand_count(opcode: OpCode) -> int:
    """
    获取操作码的操作数数量

    Args:
        opcode: 操作码枚举值

    Returns:
        操作数数量，-1表示可变数量
    """
    props = OPCODE_PROPERTIES.get(opcode, {})
    return props.get("operands", 0)


def get_result_count(opcode: OpCode) -> int:
    """
    获取操作码的结果数量

    Args:
        opcode: 操作码枚举值

    Returns:
        结果数量，-1表示可变数量
    """
    props = OPCODE_PROPERTIES.get(opcode, {})
    return props.get("result", 0)