"""
Layer 5: 私有IR指令集架构
定义完整的指令集架构、语义描述和类型检查规则
"""

from typing import Dict, List, Tuple, Any, Optional, Set
from dataclasses import dataclass, field
from enum import Enum

from .opcode import OpCode, OpCodeCategory


class DataType(Enum):
    """
    数据类型枚举
    定义指令集支持的所有数据类型
    """
    INT8 = "int8"
    INT16 = "int16"
    INT32 = "int32"
    INT64 = "int64"
    FLOAT32 = "float32"
    FLOAT64 = "float64"
    BOOL = "bool"
    STRING = "string"
    TENSOR = "tensor"
    VOID = "void"
    ANY = "any"


@dataclass
class InstructionSemantics:
    """
    指令语义描述
    详细描述指令的功能、操作数类型和结果类型
    """
    opcode: OpCode
    mnemonic: str  # 指令助记符
    description: str  # 语义描述
    operand_types: List[DataType]  # 操作数类型
    result_type: DataType  # 结果类型
    side_effects: bool  # 是否有副作用
    example: str  # 示例用法
    constraints: List[str] = field(default_factory=list)  # 约束条件


@dataclass
class InstructionEncoding:
    """
    指令编码格式
    定义指令在字节码中的编码方式
    """
    opcode: OpCode
    encoding_format: str  # 编码格式描述
    byte_size: int  # 指令字节数
    has_variable_operands: bool  # 是否有可变操作数
    operand_encoding: Dict[str, str]  # 操作数编码方式


class TypeChecker:
    """
    类型检查器
    验证指令的类型正确性
    """

    def __init__(self):
        """初始化类型检查器"""
        self.type_rules: Dict[OpCode, List[Tuple[List[DataType], DataType]]] = {}
        self._build_type_rules()

    def _build_type_rules(self):
        """构建类型检查规则"""
        # 算术操作类型规则
        self.type_rules[OpCode.ADD] = [
            ([DataType.INT32, DataType.INT32], DataType.INT32),
            ([DataType.INT64, DataType.INT64], DataType.INT64),
            ([DataType.FLOAT32, DataType.FLOAT32], DataType.FLOAT32),
            ([DataType.FLOAT64, DataType.FLOAT64], DataType.FLOAT64),
            ([DataType.TENSOR, DataType.TENSOR], DataType.TENSOR),
        ]

        self.type_rules[OpCode.SUB] = self.type_rules[OpCode.ADD]
        self.type_rules[OpCode.MUL] = self.type_rules[OpCode.ADD]

        self.type_rules[OpCode.DIV] = [
            ([DataType.FLOAT32, DataType.FLOAT32], DataType.FLOAT32),
            ([DataType.FLOAT64, DataType.FLOAT64], DataType.FLOAT64),
            ([DataType.TENSOR, DataType.TENSOR], DataType.TENSOR),
        ]

        self.type_rules[OpCode.MOD] = [
            ([DataType.INT32, DataType.INT32], DataType.INT32),
            ([DataType.INT64, DataType.INT64], DataType.INT64),
        ]

        # 张量操作类型规则
        self.type_rules[OpCode.MATMUL] = [
            ([DataType.TENSOR, DataType.TENSOR], DataType.TENSOR),
        ]

        self.type_rules[OpCode.TRANSPOSE] = [
            ([DataType.TENSOR], DataType.TENSOR),
        ]

        self.type_rules[OpCode.RESHAPE] = [
            ([DataType.TENSOR, DataType.INT32], DataType.TENSOR),
        ]

        # 内存操作类型规则
        self.type_rules[OpCode.LOAD] = [
            ([DataType.INT32], DataType.ANY),
            ([DataType.INT64], DataType.ANY),
        ]

        self.type_rules[OpCode.STORE] = [
            ([DataType.INT32, DataType.ANY], DataType.VOID),
        ]

        # 比较操作类型规则
        self.type_rules[OpCode.CMP_EQ] = [
            ([DataType.ANY, DataType.ANY], DataType.BOOL),
        ]

        self.type_rules[OpCode.CMP_NE] = self.type_rules[OpCode.CMP_EQ]
        self.type_rules[OpCode.CMP_LT] = self.type_rules[OpCode.CMP_EQ]
        self.type_rules[OpCode.CMP_LE] = self.type_rules[OpCode.CMP_EQ]
        self.type_rules[OpCode.CMP_GT] = self.type_rules[OpCode.CMP_EQ]
        self.type_rules[OpCode.CMP_GE] = self.type_rules[OpCode.CMP_EQ]

    def check_instruction(self, opcode: OpCode, operand_types: List[DataType]) -> Optional[DataType]:
        """
        检查指令的类型正确性

        Args:
            opcode: 操作码
            operand_types: 操作数类型列表

        Returns:
            结果类型，如果类型不匹配返回None
        """
        if opcode not in self.type_rules:
            return DataType.ANY  # 未定义规则的指令允许任意类型

        for rule_operand_types, result_type in self.type_rules[opcode]:
            if self._match_types(operand_types, rule_operand_types):
                return result_type

        return None  # 类型不匹配

    def _match_types(self, actual: List[DataType], expected: List[DataType]) -> bool:
        """
        匹配实际类型与期望类型

        Args:
            actual: 实际操作数类型
            expected: 期望操作数类型

        Returns:
            是否匹配
        """
        if len(actual) != len(expected):
            return False

        for act, exp in zip(actual, expected):
            if exp == DataType.ANY:
                continue  # ANY类型接受任意类型
            if act != exp:
                return False

        return True


class InstructionSet:
    """
    指令集架构
    定义完整的指令集架构，包括语义、编码和类型规则
    """

    def __init__(self):
        """初始化指令集架构"""
        self.semantics: Dict[OpCode, InstructionSemantics] = {}
        self.encodings: Dict[OpCode, InstructionEncoding] = {}
        self.type_checker = TypeChecker()

        # 构建指令集定义
        self._build_semantics()
        self._build_encodings()

    def _build_semantics(self):
        """构建指令语义描述"""

        # ==================== 算术操作 ====================
        self.semantics[OpCode.ADD] = InstructionSemantics(
            opcode=OpCode.ADD,
            mnemonic="ADD",
            description="将两个操作数相加，结果压入栈顶",
            operand_types=[DataType.ANY, DataType.ANY],
            result_type=DataType.ANY,
            side_effects=False,
            example="ADD  // 弹出a和b，压入a+b",
            constraints=["操作数必须为数值类型或张量"]
        )

        self.semantics[OpCode.SUB] = InstructionSemantics(
            opcode=OpCode.SUB,
            mnemonic="SUB",
            description="将两个操作数相减，结果压入栈顶",
            operand_types=[DataType.ANY, DataType.ANY],
            result_type=DataType.ANY,
            side_effects=False,
            example="SUB  // 弹出a和b，压入a-b",
            constraints=["操作数必须为数值类型或张量"]
        )

        self.semantics[OpCode.MUL] = InstructionSemantics(
            opcode=OpCode.MUL,
            mnemonic="MUL",
            description="将两个操作数相乘，结果压入栈顶",
            operand_types=[DataType.ANY, DataType.ANY],
            result_type=DataType.ANY,
            side_effects=False,
            example="MUL  // 弹出a和b，压入a*b",
            constraints=["操作数必须为数值类型或张量"]
        )

        self.semantics[OpCode.DIV] = InstructionSemantics(
            opcode=OpCode.DIV,
            mnemonic="DIV",
            description="将两个操作数相除，结果压入栈顶",
            operand_types=[DataType.ANY, DataType.ANY],
            result_type=DataType.ANY,
            side_effects=False,
            example="DIV  // 弹出a和b，压入a/b",
            constraints=["除数不能为0，操作数必须为数值类型或张量"]
        )

        self.semantics[OpCode.MOD] = InstructionSemantics(
            opcode=OpCode.MOD,
            mnemonic="MOD",
            description="计算第一个操作数除以第二个操作数的余数",
            operand_types=[DataType.INT32, DataType.INT32],
            result_type=DataType.INT32,
            side_effects=False,
            example="MOD  // 弹出a和b，压入a%b",
            constraints=["操作数必须为整数类型，除数不能为0"]
        )

        self.semantics[OpCode.NEG] = InstructionSemantics(
            opcode=OpCode.NEG,
            mnemonic="NEG",
            description="对栈顶元素取负",
            operand_types=[DataType.ANY],
            result_type=DataType.ANY,
            side_effects=False,
            example="NEG  // 弹出a，压入-a",
            constraints=["操作数必须为数值类型"]
        )

        self.semantics[OpCode.ABS] = InstructionSemantics(
            opcode=OpCode.ABS,
            mnemonic="ABS",
            description="计算栈顶元素的绝对值",
            operand_types=[DataType.ANY],
            result_type=DataType.ANY,
            side_effects=False,
            example="ABS  // 弹出a，压入|a|",
            constraints=["操作数必须为数值类型"]
        )

        self.semantics[OpCode.SQRT] = InstructionSemantics(
            opcode=OpCode.SQRT,
            mnemonic="SQRT",
            description="计算栈顶元素的平方根",
            operand_types=[DataType.FLOAT64],
            result_type=DataType.FLOAT64,
            side_effects=False,
            example="SQRT  // 弹出a，压入√a",
            constraints=["操作数必须为非负数值"]
        )

        self.semantics[OpCode.EXP] = InstructionSemantics(
            opcode=OpCode.EXP,
            mnemonic="EXP",
            description="计算以e为底的指数",
            operand_types=[DataType.FLOAT64],
            result_type=DataType.FLOAT64,
            side_effects=False,
            example="EXP  // 弹出a，压入e^a",
            constraints=["操作数必须为数值类型"]
        )

        self.semantics[OpCode.LOG] = InstructionSemantics(
            opcode=OpCode.LOG,
            mnemonic="LOG",
            description="计算以e为底的自然对数",
            operand_types=[DataType.FLOAT64],
            result_type=DataType.FLOAT64,
            side_effects=False,
            example="LOG  // 弹出a，压入ln(a)",
            constraints=["操作数必须为正数"]
        )

        # ==================== 张量操作 ====================
        self.semantics[OpCode.MATMUL] = InstructionSemantics(
            opcode=OpCode.MATMUL,
            mnemonic="MATMUL",
            description="计算两个张量的矩阵乘法",
            operand_types=[DataType.TENSOR, DataType.TENSOR],
            result_type=DataType.TENSOR,
            side_effects=False,
            example="MATMUL  // 弹出A和B，压入A@B",
            constraints=["张量维度必须匹配矩阵乘法要求"]
        )

        self.semantics[OpCode.TRANSPOSE] = InstructionSemantics(
            opcode=OpCode.TRANSPOSE,
            mnemonic="TRANSPOSE",
            description="转置张量",
            operand_types=[DataType.TENSOR],
            result_type=DataType.TENSOR,
            side_effects=False,
            example="TRANSPOSE  // 弹出A，压入A^T",
            constraints=["输入必须为张量"]
        )

        self.semantics[OpCode.RESHAPE] = InstructionSemantics(
            opcode=OpCode.RESHAPE,
            mnemonic="RESHAPE",
            description="改变张量的形状",
            operand_types=[DataType.TENSOR, DataType.INT32],
            result_type=DataType.TENSOR,
            side_effects=False,
            example="RESHAPE [2,3]  // 将张量重塑为2x3形状",
            constraints=["新形状的总元素数必须等于原张量的元素数"]
        )

        self.semantics[OpCode.SLICE] = InstructionSemantics(
            opcode=OpCode.SLICE,
            mnemonic="SLICE",
            description="对张量进行切片操作",
            operand_types=[DataType.TENSOR, DataType.INT32],
            result_type=DataType.TENSOR,
            side_effects=False,
            example="SLICE [0:2]  // 取张量的前两行",
            constraints=["切片索引必须在张量维度范围内"]
        )

        self.semantics[OpCode.CONCAT] = InstructionSemantics(
            opcode=OpCode.CONCAT,
            mnemonic="CONCAT",
            description="拼接多个张量",
            operand_types=[DataType.TENSOR, DataType.INT32],
            result_type=DataType.TENSOR,
            side_effects=False,
            example="CONCAT axis=0  // 沿第0维拼接张量",
            constraints=["拼接维度以外的其他维度大小必须相同"]
        )

        # ==================== 内存操作 ====================
        self.semantics[OpCode.LOAD] = InstructionSemantics(
            opcode=OpCode.LOAD,
            mnemonic="LOAD",
            description="从内存地址加载数据到栈顶",
            operand_types=[DataType.INT32],
            result_type=DataType.ANY,
            side_effects=False,
            example="LOAD addr  // 从地址addr加载数据",
            constraints=["地址必须有效且已分配"]
        )

        self.semantics[OpCode.STORE] = InstructionSemantics(
            opcode=OpCode.STORE,
            mnemonic="STORE",
            description="将栈顶数据存储到指定内存地址",
            operand_types=[DataType.INT32, DataType.ANY],
            result_type=DataType.VOID,
            side_effects=True,
            example="STORE addr  // 将栈顶数据存到地址addr",
            constraints=["地址必须有效且有足够空间"]
        )

        self.semantics[OpCode.ALLOC] = InstructionSemantics(
            opcode=OpCode.ALLOC,
            mnemonic="ALLOC",
            description="分配指定大小的内存块",
            operand_types=[DataType.INT32],
            result_type=DataType.INT32,
            side_effects=True,
            example="ALLOC 1024  // 分配1024字节内存",
            constraints=["大小必须为正整数"]
        )

        self.semantics[OpCode.FREE] = InstructionSemantics(
            opcode=OpCode.FREE,
            mnemonic="FREE",
            description="释放指定地址的内存块",
            operand_types=[DataType.INT32],
            result_type=DataType.VOID,
            side_effects=True,
            example="FREE addr  // 释放地址addr的内存",
            constraints=["地址必须指向已分配的内存"]
        )

        # ==================== 控制操作 ====================
        self.semantics[OpCode.JUMP] = InstructionSemantics(
            opcode=OpCode.JUMP,
            mnemonic="JUMP",
            description="无条件跳转到指定地址",
            operand_types=[DataType.INT32],
            result_type=DataType.VOID,
            side_effects=True,
            example="JUMP target  // 跳转到target地址",
            constraints=["目标地址必须在指令序列范围内"]
        )

        self.semantics[OpCode.JUMP_IF] = InstructionSemantics(
            opcode=OpCode.JUMP_IF,
            mnemonic="JUMP_IF",
            description="如果栈顶为真，跳转到指定地址",
            operand_types=[DataType.INT32, DataType.BOOL],
            result_type=DataType.VOID,
            side_effects=True,
            example="JUMP_IF target  // 如果栈顶为真，跳转到target",
            constraints=["目标地址必须在指令序列范围内"]
        )

        self.semantics[OpCode.CALL] = InstructionSemantics(
            opcode=OpCode.CALL,
            mnemonic="CALL",
            description="调用函数",
            operand_types=[DataType.INT32],
            result_type=DataType.ANY,
            side_effects=True,
            example="CALL func_addr  // 调用地址为func_addr的函数",
            constraints=["函数地址必须有效"]
        )

        self.semantics[OpCode.RET] = InstructionSemantics(
            opcode=OpCode.RET,
            mnemonic="RET",
            description="从函数返回",
            operand_types=[],
            result_type=DataType.VOID,
            side_effects=True,
            example="RET  // 返回到调用者",
            constraints=["必须在函数内部使用"]
        )

        self.semantics[OpCode.HALT] = InstructionSemantics(
            opcode=OpCode.HALT,
            mnemonic="HALT",
            description="停止程序执行",
            operand_types=[],
            result_type=DataType.VOID,
            side_effects=True,
            example="HALT  // 停止执行",
            constraints=["无"]
        )

        # ==================== 比较操作 ====================
        self.semantics[OpCode.CMP_EQ] = InstructionSemantics(
            opcode=OpCode.CMP_EQ,
            mnemonic="CMP_EQ",
            description="比较两个值是否相等",
            operand_types=[DataType.ANY, DataType.ANY],
            result_type=DataType.BOOL,
            side_effects=False,
            example="CMP_EQ  // 弹出a和b，压入(a==b)",
            constraints=["操作数类型必须兼容"]
        )

        self.semantics[OpCode.CMP_LT] = InstructionSemantics(
            opcode=OpCode.CMP_LT,
            mnemonic="CMP_LT",
            description="比较第一个值是否小于第二个值",
            operand_types=[DataType.ANY, DataType.ANY],
            result_type=DataType.BOOL,
            side_effects=False,
            example="CMP_LT  // 弹出a和b，压入(a<b)",
            constraints=["操作数必须为数值类型"]
        )

        # ==================== 激活函数 ====================
        self.semantics[OpCode.RELU] = InstructionSemantics(
            opcode=OpCode.RELU,
            mnemonic="RELU",
            description="ReLU激活函数: max(0, x)",
            operand_types=[DataType.TENSOR],
            result_type=DataType.TENSOR,
            side_effects=False,
            example="RELU  // 弹出x，压入max(0,x)",
            constraints=["输入必须为张量"]
        )

        self.semantics[OpCode.SIGMOID] = InstructionSemantics(
            opcode=OpCode.SIGMOID,
            mnemonic="SIGMOID",
            description="Sigmoid激活函数: 1/(1+e^(-x))",
            operand_types=[DataType.TENSOR],
            result_type=DataType.TENSOR,
            side_effects=False,
            example="SIGMOID  // 弹出x，压入1/(1+e^(-x))",
            constraints=["输入必须为张量"]
        )

        self.semantics[OpCode.SOFTMAX] = InstructionSemantics(
            opcode=OpCode.SOFTMAX,
            mnemonic="SOFTMAX",
            description="Softmax激活函数",
            operand_types=[DataType.TENSOR],
            result_type=DataType.TENSOR,
            side_effects=False,
            example="SOFTMAX  // 弹出x，压入softmax(x)",
            constraints=["输入必须为张量"]
        )

        # ==================== 混淆操作 ====================
        self.semantics[OpCode.OBF_PI] = InstructionSemantics(
            opcode=OpCode.OBF_PI,
            mnemonic="OBF_PI",
            description="使用PI值进行混淆保护",
            operand_types=[],
            result_type=DataType.FLOAT64,
            side_effects=False,
            example="OBF_PI  // 压入混淆的PI值",
            constraints=["用于混淆保护"]
        )

        self.semantics[OpCode.OBF_DECRYPT] = InstructionSemantics(
            opcode=OpCode.OBF_DECRYPT,
            mnemonic="OBF_DECRYPT",
            description="解密数据",
            operand_types=[DataType.ANY],
            result_type=DataType.ANY,
            side_effects=False,
            example="OBF_DECRYPT key  // 使用key解密栈顶数据",
            constraints=["用于数据解密"]
        )

        self.semantics[OpCode.OBF_CORRECT] = InstructionSemantics(
            opcode=OpCode.OBF_CORRECT,
            mnemonic="OBF_CORRECT",
            description="校正混淆后的数值",
            operand_types=[DataType.ANY],
            result_type=DataType.ANY,
            side_effects=False,
            example="OBF_CORRECT expected  // 校正到期望值",
            constraints=["用于数值校正"]
        )

    def _build_encodings(self):
        """构建指令编码格式"""

        # 基本编码：操作码(1字节) + 操作数数量(1字节) + 操作数(变长)
        base_encoding = {
            'opcode': 'byte',
            'operand_count': 'byte',
            'operands': 'varlen'
        }

        # 为每个操作码设置编码
        for opcode in OpCode:
            self.encodings[opcode] = InstructionEncoding(
                opcode=opcode,
                encoding_format="opcode + operand_count + operands",
                byte_size=2,  # 最小字节数（操作码+数量）
                has_variable_operands=True,
                operand_encoding=base_encoding
            )

    def get_semantics(self, opcode: OpCode) -> Optional[InstructionSemantics]:
        """
        获取指令语义

        Args:
            opcode: 操作码

        Returns:
            指令语义描述，如果不存在返回None
        """
        return self.semantics.get(opcode)

    def get_encoding(self, opcode: OpCode) -> Optional[InstructionEncoding]:
        """
        获取指令编码

        Args:
            opcode: 操作码

        Returns:
            指令编码格式，如果不存在返回None
        """
        return self.encodings.get(opcode)

    def validate_instruction(self, opcode: OpCode, operands: List[Any]) -> Tuple[bool, Optional[str]]:
        """
        验证指令的正确性

        Args:
            opcode: 操作码
            operands: 操作数列表

        Returns:
            (是否有效, 错误信息)
        """
        # 检查操作码是否存在
        if opcode not in self.semantics:
            return False, f"未知操作码: {opcode}"

        semantics = self.semantics[opcode]

        # 检查操作数数量
        expected_count = len(semantics.operand_types)
        if expected_count != len(operands) and semantics.side_effects == False:
            # 对于有副作用的指令，操作数数量可能不同
            if opcode not in [OpCode.CALL, OpCode.JUMP_IF]:
                return False, f"操作数数量不匹配: 期望{expected_count}, 实际{len(operands)}"

        # 检查类型约束
        # 这里简化处理，实际应用中需要更严格的类型检查

        return True, None

    def get_instruction_info(self, opcode: OpCode) -> Dict[str, Any]:
        """
        获取指令的完整信息

        Args:
            opcode: 操作码

        Returns:
            指令信息字典
        """
        semantics = self.get_semantics(opcode)
        encoding = self.get_encoding(opcode)

        info = {
            'opcode': opcode,
            'opcode_value': opcode.value,
            'opcode_name': opcode.name,
            'category': OpCodeCategory.categorize(opcode),
        }

        if semantics:
            info.update({
                'mnemonic': semantics.mnemonic,
                'description': semantics.description,
                'operand_types': [t.value for t in semantics.operand_types],
                'result_type': semantics.result_type.value,
                'side_effects': semantics.side_effects,
                'example': semantics.example,
                'constraints': semantics.constraints,
            })

        if encoding:
            info.update({
                'encoding_format': encoding.encoding_format,
                'byte_size': encoding.byte_size,
                'has_variable_operands': encoding.has_variable_operands,
            })

        return info

    def list_instructions_by_category(self, category: str) -> List[OpCode]:
        """
        获取指定类别的所有指令

        Args:
            category: 指令类别名称

        Returns:
            该类别的操作码列表
        """
        result = []

        category_map = {
            'ARITHMETIC': OpCodeCategory.ARITHMETIC,
            'TENSOR': OpCodeCategory.TENSOR,
            'MEMORY': OpCodeCategory.MEMORY,
            'CONTROL': OpCodeCategory.CONTROL,
            'COMPARISON': OpCodeCategory.COMPARISON,
            'ACTIVATION': OpCodeCategory.ACTIVATION,
            'OBFUSCATION': OpCodeCategory.OBFUSCATION,
            'TYPE': OpCodeCategory.TYPE,
            'STACK': OpCodeCategory.STACK,
            'SPECIAL': OpCodeCategory.SPECIAL,
        }

        if category in category_map:
            result = category_map[category]

        return result

    def generate_documentation(self) -> str:
        """
        生成指令集文档

        Returns:
            文档字符串
        """
        doc_lines = []
        doc_lines.append("=" * 80)
        doc_lines.append("私有IR指令集架构文档")
        doc_lines.append("=" * 80)
        doc_lines.append("")

        # 按类别组织文档
        categories = [
            ("算术操作", OpCodeCategory.ARITHMETIC),
            ("张量操作", OpCodeCategory.TENSOR),
            ("内存操作", OpCodeCategory.MEMORY),
            ("控制操作", OpCodeCategory.CONTROL),
            ("比较操作", OpCodeCategory.COMPARISON),
            ("激活函数", OpCodeCategory.ACTIVATION),
            ("混淆操作", OpCodeCategory.OBFUSCATION),
        ]

        for category_name, category_opcodes in categories:
            doc_lines.append(f"{category_name}")
            doc_lines.append("-" * 80)

            for opcode in category_opcodes:
                semantics = self.get_semantics(opcode)
                if semantics:
                    doc_lines.append(f"  {semantics.mnemonic} (0x{opcode.value:02X})")
                    doc_lines.append(f"    {semantics.description}")
                    doc_lines.append(f"    操作数类型: {[t.value for t in semantics.operand_types]}")
                    doc_lines.append(f"    结果类型: {semantics.result_type.value}")
                    doc_lines.append(f"    示例: {semantics.example}")
                    if semantics.constraints:
                        doc_lines.append(f"    约束: {semantics.constraints}")
                    doc_lines.append("")

            doc_lines.append("")

        doc_lines.append("=" * 80)

        return "\n".join(doc_lines)


# 指令集实例（全局）
INSTRUCTION_SET = InstructionSet()


def get_instruction_set() -> InstructionSet:
    """
    获取全局指令集实例

    Returns:
        InstructionSet实例
    """
    return INSTRUCTION_SET