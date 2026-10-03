"""
Layer 5: 私有IR编译器
将推理函数/推理图/数据编译为私有IR字节码

编译策略：
- Python 函数基于 AST 编译（跨 Python 版本稳定，不依赖 dis 字节码格式）
- 跳转使用标签化 + 二次解析，目标索引准确
- 局部变量映射到通用寄存器（r5..r19），参数映射到参数寄存器（r20..r31）
- CALL 支持 Python 可调用对象（FFI）与整数地址（内部调用）
"""

from typing import Any, Callable, Dict, List, Optional, Tuple, Union
import ast
import inspect
import textwrap
import builtins
import logging
from dataclasses import dataclass

from .opcode import OpCode, OpCodeCategory, get_operand_count, has_side_effect
from .bytecode import ByteCode, Instruction


# 配置日志
logger = logging.getLogger(__name__)

# 寄存器分配常量（与解释器保持一致）
# 寄存器文件扩展到 64 个：
#   r0..r4   特殊寄存器 (pc, sp, bp, retval, temp)
#   r5..r49  局部变量寄存器（共 45 个，含临时分配）
#   r50..r63 参数寄存器（共 14 个）
LOCAL_BASE_REG = 5      # 局部变量寄存器起始（r5..r49，共45个）
PARAM_BASE_REG = 50     # 参数寄存器起始（r50..r63，共14个）
MAX_LOCALS = 45
MAX_PARAMS = 14


@dataclass
class FunctionInfo:
    """
    函数信息
    存储函数的元数据
    """
    name: str
    args: List[str]
    defaults: Dict[str, Any]
    code_object: Any
    source: Optional[str] = None
    docstring: Optional[str] = None


class CompileError(ValueError):
    """编译错误"""
    pass


class _Label:
    """跳转标签占位"""
    __slots__ = ('name',)

    def __init__(self, name: str):
        self.name = name

    def __repr__(self):
        return f"<label:{self.name}>"


class IRCompiler:
    """
    IR编译器
    将Python函数/推理图/数据编译为私有IR字节码
    """

    def __init__(self, optimization_level: int = 1):
        """
        初始化编译器

        Args:
            optimization_level: 优化级别（0-无优化，1-基本优化，2-激进优化）
        """
        self.optimization_level = optimization_level
        self.bytecode: Optional[ByteCode] = None
        self.function_info: Optional[FunctionInfo] = None
        self._local_regs: Dict[str, int] = {}   # 局部变量名 -> 寄存器
        self._param_regs: Dict[str, int] = {}   # 参数名 -> 寄存器
        self._local_counter = 0
        self._globals_dict: Dict[str, Any] = {}
        self._label_counter = 0

        # AST 编译内部状态
        self._instrs: List[Instruction] = []
        self._labels: Dict[str, int] = {}
        self._label_refs: List[Tuple[str, int]] = []
        self._const_cache: Dict[Any, int] = {}
        self._loop_ctx: List[Tuple[str, str]] = []  # (continue_label, break_label)

    # ==================== 公共接口 ====================

    def compile(
        self,
        source: Union[Callable, Dict[str, Any], str],
        encrypt: bool = True,
        obfuscation_level: int = 0
    ) -> ByteCode:
        """
        编译源代码为字节码（通用接口）

        支持多种输入类型：
        - Python函数
        - 推理图定义
        - 数据字节串

        Args:
            source: 输入源（函数、推理图或数据）
            encrypt: 是否加密字节码
            obfuscation_level: 混淆级别（0-无混淆，1-轻量，2-中等，3-重度）

        Returns:
            编译后的字节码

        Example:
            >>> compiler = IRCompiler()
            >>> bytecode = compiler.compile(my_function)
            >>> bytecode = compiler.compile({'nodes': [...]}, obfuscation_level=2)
        """
        # 根据输入类型选择编译方法
        if callable(source):
            # 编译Python函数
            bytecode = self.compile_function(source, encrypt=False)
        elif isinstance(source, dict):
            # 编译推理图
            bytecode = self.compile_inference_graph(source)
        elif isinstance(source, (str, bytes)):
            # 编译数据为简单的字节码
            bytecode = self._compile_data(source)
        else:
            raise TypeError(f"不支持的输入类型: {type(source)}")

        # 添加混淆保护
        if obfuscation_level > 0:
            bytecode = self.add_obfuscation(bytecode, level=obfuscation_level)

        logger.info(f"编译完成: {len(bytecode.instructions)} 条指令")

        return bytecode

    # ==================== AST 编译 ====================

    def compile_function(self, func: Callable, encrypt: bool = True) -> ByteCode:
        """
        将函数编译为字节码（AST 编译）

        Args:
            func: 要编译的函数
            encrypt: 是否加密字节码

        Returns:
            编译后的字节码

        Raises:
            CompileError: 如果无法获取源码或包含不支持的语法
        """
        logger.info(f"开始编译函数: {func.__name__}")

        # 初始化编译状态
        self.bytecode = ByteCode()
        self._reset_ast_state()
        self._globals_dict = getattr(func, '__globals__', {})

        # 获取函数信息
        self.function_info = FunctionInfo(
            name=func.__name__,
            args=list(func.__code__.co_varnames[:func.__code__.co_argcount]),
            defaults={},
            code_object=func.__code__,
            source=inspect.getsource(func) if _source_available(func) else None,
            docstring=func.__doc__
        )

        # 获取源码并解析 AST
        try:
            source = inspect.getsource(func)
        except (OSError, TypeError) as e:
            raise CompileError(
                f"无法获取函数 {func.__name__} 的源码（请在文件中定义函数）: {e}"
            )
        tree = ast.parse(textwrap.dedent(source))

        fn_def = None
        for node in tree.body:
            if isinstance(node, ast.FunctionDef) and node.name == func.__name__:
                fn_def = node
                break
        if fn_def is None:
            raise CompileError(f"在源码中找不到函数定义: {func.__name__}")

        # 分配参数寄存器
        self._param_regs = {
            arg: PARAM_BASE_REG + i for i, arg in enumerate(self.function_info.args)
        }
        if len(self.function_info.args) > MAX_PARAMS:
            raise CompileError(f"参数过多（> {MAX_PARAMS}）: {func.__name__}")

        # 编译函数体
        for stmt in fn_def.body:
            self._compile_stmt(stmt)

        # 隐式返回 None
        if not self._ends_with_return():
            self._emit(OpCode.LOAD_CONST, [self._const_idx(None)])
            self._emit(OpCode.RET)

        # 解析跳转标签
        self._resolve_labels()

        # 写入字节码
        self.bytecode.instructions = self._instrs

        # 优化字节码
        if self.optimization_level > 0:
            self._optimize_bytecode()

        # 设置元数据
        self.bytecode.set_metadata('function_name', func.__name__)
        self.bytecode.set_metadata('arg_count', len(self.function_info.args))
        self.bytecode.set_metadata('optimization_level', self.optimization_level)
        self.bytecode.set_metadata('input_names', list(self.function_info.args))
        self.bytecode.set_metadata('schema', {
            name: {'type': 'auto'} for name in self.function_info.args
        })

        logger.info(f"函数编译完成: {len(self.bytecode.instructions)} 条指令")

        return self.bytecode

    def _ends_with_return(self) -> bool:
        """函数体是否以 return 结尾"""
        return bool(self._instrs) and self._instrs[-1].opcode == OpCode.RET

    def _reset_ast_state(self):
        """重置 AST 编译状态"""
        self._instrs = []
        self._labels = {}
        self._label_refs = []
        self._const_cache = {}
        self._local_regs = {}
        self._param_regs = {}
        self._local_counter = 0
        self._label_counter = 0
        self._loop_ctx = []

    # ==================== 指令发射 ====================

    def _emit(self, opcode: OpCode, operands: Optional[List[Any]] = None) -> int:
        """发射一条指令，返回索引"""
        idx = len(self._instrs)
        self._instrs.append(Instruction(opcode, operands or []))
        return idx

    def _emit_jump(self, opcode: OpCode, label: str, extra: Optional[List[Any]] = None) -> int:
        """发射带标签目标的跳转指令"""
        idx = len(self._instrs)
        self._instrs.append(Instruction(opcode, [label] + (extra or [])))
        self._label_refs.append((label, idx))
        return idx

    def _new_label(self, prefix: str = "L") -> str:
        """创建唯一标签名"""
        name = f"{prefix}{self._label_counter}"
        self._label_counter += 1
        return name

    def _resolve_labels(self):
        """将标签占位解析为指令索引"""
        for label, instr_idx in self._label_refs:
            if label not in self._labels:
                raise CompileError(f"未定义的跳转标签: {label}")
            target = self._labels[label]
            inst = self._instrs[instr_idx]
            inst.operands[0] = target

    def _const_idx(self, value: Any) -> int:
        """获取常量池索引（带缓存）"""
        if value in self._const_cache:
            return self._const_cache[value]
        # 布尔与整数需区分（序列化时布尔优先）
        for idx, const in enumerate(self.bytecode.constants):
            if const == value and type(const) is type(value):
                return idx
        idx = self.bytecode.add_constant(value)
        self._const_cache[value] = idx
        return idx

    # ==================== 语句编译 ====================

    def _compile_stmt(self, stmt: ast.stmt):
        """编译语句"""
        if isinstance(stmt, ast.Return):
            self._compile_expr(stmt.value if stmt.value is not None else ast.Constant(None))
            self._emit(OpCode.RET)
        elif isinstance(stmt, ast.Assign):
            self._compile_assign(stmt.targets, stmt.value)
        elif isinstance(stmt, ast.AnnAssign):
            if stmt.value is None:
                return
            self._compile_assign([stmt.target], stmt.value)
        elif isinstance(stmt, ast.AugAssign):
            self._compile_aug_assign(stmt)
        elif isinstance(stmt, ast.If):
            self._compile_if(stmt)
        elif isinstance(stmt, ast.While):
            self._compile_while(stmt)
        elif isinstance(stmt, ast.For):
            self._compile_for(stmt)
        elif isinstance(stmt, ast.Expr):
            self._compile_expr(stmt.value)
            self._emit(OpCode.POP)
        elif isinstance(stmt, ast.Break):
            if not self._loop_ctx:
                raise CompileError("break 出现在循环外")
            _, break_label = self._loop_ctx[-1]
            self._emit_jump(OpCode.JUMP, break_label)
        elif isinstance(stmt, ast.Continue):
            if not self._loop_ctx:
                raise CompileError("continue 出现在循环外")
            continue_label, _ = self._loop_ctx[-1]
            self._emit_jump(OpCode.JUMP, continue_label)
        elif isinstance(stmt, ast.Pass):
            pass
        else:
            raise CompileError(f"暂不支持的语句: {type(stmt).__name__}")

    def _compile_assign(self, targets: List[ast.expr], value: ast.expr):
        """编译赋值（仅支持单个 Name 目标）"""
        if len(targets) != 1 or not isinstance(targets[0], ast.Name):
            raise CompileError("仅支持单变量赋值（Name 目标）")
        name = targets[0].id
        # 先分配寄存器，确保自引用（如 x = x + 1）能正确解析
        reg = self._local_reg(name)
        self._compile_expr(value)
        self._emit(OpCode.STORE_REG, [reg])

    def _compile_aug_assign(self, stmt: ast.AugAssign):
        """编译增强赋值（仅支持 Name 目标）"""
        if not isinstance(stmt.target, ast.Name):
            raise CompileError("仅支持 Name 目标的增强赋值")
        name = stmt.target.id
        reg = self._local_reg(name)
        self._compile_expr(ast.Name(id=name, ctx=ast.Load()))
        self._compile_expr(stmt.value)
        self._emit(_binop_to_opcode(stmt.op))
        self._emit(OpCode.STORE_REG, [reg])

    def _compile_if(self, stmt: ast.If):
        """编译 if/else"""
        else_label = self._new_label("Lelse")
        end_label = self._new_label("Lend")
        self._compile_expr(stmt.test)
        self._emit_jump(OpCode.JUMP_IF, else_label, [False])
        for s in stmt.body:
            self._compile_stmt(s)
        self._emit_jump(OpCode.JUMP, end_label)
        self._labels[else_label] = len(self._instrs)
        for s in stmt.orelse:
            self._compile_stmt(s)
        self._labels[end_label] = len(self._instrs)

    def _compile_while(self, stmt: ast.While):
        """编译 while 循环"""
        start_label = self._new_label("Lwstart")
        end_label = self._new_label("Lwend")
        self._labels[start_label] = len(self._instrs)
        self._compile_expr(stmt.test)
        self._emit_jump(OpCode.JUMP_IF, end_label, [False])
        # continue 回到条件判断处，break 跳出循环
        self._loop_ctx.append((start_label, end_label))
        for s in stmt.body:
            self._compile_stmt(s)
        self._loop_ctx.pop()
        self._emit_jump(OpCode.JUMP, start_label)
        if stmt.orelse:
            for s in stmt.orelse:
                self._compile_stmt(s)
        self._labels[end_label] = len(self._instrs)

    def _compile_for(self, stmt: ast.For):
        """
        编译 for 循环
        仅支持 for x in range(...) 形式，展开为索引循环
        """
        if not (isinstance(stmt.iter, ast.Call) and isinstance(stmt.iter.func, ast.Name)
                and stmt.iter.func.id == 'range'):
            raise CompileError("for 循环目前仅支持 range(...) 迭代器")

        if not isinstance(stmt.target, ast.Name):
            raise CompileError("for 循环目标必须是单个变量")

        loop_var = stmt.target.id
        range_args = stmt.iter.args
        if len(range_args) not in (1, 2, 3):
            raise CompileError("range() 参数个数必须为 1~3")

        start_label = self._new_label("Lfstart")
        end_label = self._new_label("Lfend")

        # 分配寄存器
        idx_reg = self._local_reg(loop_var)          # 循环变量
        stop_reg = self._fresh_reg('range_stop')     # 上界
        step_reg = self._fresh_reg('range_step')     # 步长

        # 初始化
        if len(range_args) == 1:
            self._compile_expr(ast.Constant(0))
            self._emit(OpCode.STORE_REG, [idx_reg])
            self._compile_expr(range_args[0])
            self._emit(OpCode.STORE_REG, [stop_reg])
            self._compile_expr(ast.Constant(1))
            self._emit(OpCode.STORE_REG, [step_reg])
        else:
            self._compile_expr(range_args[0])
            self._emit(OpCode.STORE_REG, [idx_reg])
            self._compile_expr(range_args[1])
            self._emit(OpCode.STORE_REG, [stop_reg])
            if len(range_args) == 3:
                self._compile_expr(range_args[2])
                self._emit(OpCode.STORE_REG, [step_reg])
            else:
                self._compile_expr(ast.Constant(1))
                self._emit(OpCode.STORE_REG, [step_reg])

        # 循环条件
        self._labels[start_label] = len(self._instrs)
        self._emit(OpCode.LOAD_REG, [idx_reg])
        self._emit(OpCode.LOAD_REG, [stop_reg])
        self._emit(OpCode.CMP_LT)
        self._emit_jump(OpCode.JUMP_IF, end_label, [False])

        # 递增段标签（continue 跳到这里）
        inc_label = self._new_label("Lfinc")

        # 循环体
        self._loop_ctx.append((inc_label, end_label))
        for s in stmt.body:
            self._compile_stmt(s)
        self._loop_ctx.pop()
        if stmt.orelse:
            for s in stmt.orelse:
                self._compile_stmt(s)

        # 递增
        self._labels[inc_label] = len(self._instrs)
        self._emit(OpCode.LOAD_REG, [idx_reg])
        self._emit(OpCode.LOAD_REG, [step_reg])
        self._emit(OpCode.ADD)
        self._emit(OpCode.STORE_REG, [idx_reg])
        self._emit_jump(OpCode.JUMP, start_label)

        self._labels[end_label] = len(self._instrs)

    # ==================== 表达式编译 ====================

    def _compile_expr(self, expr: ast.expr):
        """编译表达式"""
        if isinstance(expr, ast.Constant):
            self._emit(OpCode.LOAD_CONST, [self._const_idx(expr.value)])
        elif isinstance(expr, ast.Name):
            self._compile_name(expr.id)
        elif isinstance(expr, ast.BinOp):
            self._compile_expr(expr.left)
            self._compile_expr(expr.right)
            if isinstance(expr.op, ast.FloorDiv):
                # 地板除：FFI 调用 _floordiv（正确处理向负无穷取整）
                self._emit(OpCode.LOAD_CONST, [self._const_idx(_floordiv)])
                self._emit(OpCode.CALL, [2])
            elif isinstance(expr.op, (ast.LShift, ast.RShift)):
                raise CompileError("暂不支持的二元运算: 移位")
            else:
                self._emit(_binop_to_opcode(expr.op))
        elif isinstance(expr, ast.UnaryOp):
            self._compile_unary(expr)
        elif isinstance(expr, ast.BoolOp):
            self._compile_boolop(expr)
        elif isinstance(expr, ast.Compare):
            self._compile_compare(expr)
        elif isinstance(expr, ast.Call):
            self._compile_call(expr)
        elif isinstance(expr, ast.Subscript):
            self._compile_expr(expr.value)
            self._compile_expr(expr.slice)
            self._emit(OpCode.SLICE)
        elif isinstance(expr, ast.Attribute):
            value = self._resolve_constant_expr(expr)
            if value is None:
                raise CompileError(f"无法解析全局属性引用: {ast.dump(expr)}")
            self._emit(OpCode.LOAD_CONST, [self._const_idx(value)])
        elif isinstance(expr, (ast.List, ast.Tuple, ast.Set)):
            if isinstance(expr, ast.List):
                maker = _make_list
            elif isinstance(expr, ast.Tuple):
                maker = _make_tuple
            else:
                maker = _make_set
            try:
                # 全部为常量字面量时，直接求值
                value = ast.literal_eval(expr)
                self._emit(OpCode.LOAD_CONST, [self._const_idx(value)])
            except (ValueError, TypeError, SyntaxError):
                # 含非常量元素：通过 FFI 调用构造帮助函数
                for elt in expr.elts:
                    self._compile_expr(elt)
                self._emit(OpCode.LOAD_CONST, [self._const_idx(maker)])
                self._emit(OpCode.CALL, [len(expr.elts)])
        elif isinstance(expr, ast.Dict):
            try:
                value = self._eval_literal(expr)
                self._emit(OpCode.LOAD_CONST, [self._const_idx(value)])
            except CompileError:
                # 含非常量：使用 FFI dict(**kv) 构造
                if len(expr.keys) != len(expr.values):
                    raise CompileError("Dict 键值数不匹配")
                maker = dict
                # 按顺序交替 push value, key 再构建——Python dict() 接受交替的 [k1, v1, k2, v2, ...]？
                # 不接受，故走专用帮助函数
                args_flat = []
                for k, v in zip(expr.keys, expr.values):
                    if k is None:
                        # ** 展开不支持
                        raise CompileError("Dict 字面量中的 ** 展开暂不支持")
                    self._compile_expr(k)
                    self._compile_expr(v)
                self._emit(OpCode.LOAD_CONST, [self._const_idx(_make_dict_pairs)])
                self._emit(OpCode.CALL, [2 * len(expr.keys)])
        else:
            raise CompileError(f"暂不支持的表达式: {type(expr).__name__}")

    def _compile_name(self, name: str):
        """编译名字引用"""
        if name in self._param_regs:
            self._emit(OpCode.LOAD_REG, [self._param_regs[name]])
        elif name in self._local_regs:
            self._emit(OpCode.LOAD_REG, [self._local_regs[name]])
        else:
            value = self._resolve_global(name)
            if value is None:
                raise CompileError(f"无法解析全局名称: {name}")
            self._emit(OpCode.LOAD_CONST, [self._const_idx(value)])

    def _compile_unary(self, expr: ast.UnaryOp):
        """编译一元运算"""
        if isinstance(expr.op, ast.USub):
            self._compile_expr(expr.operand)
            self._emit(OpCode.NEG)
        elif isinstance(expr.op, ast.UAdd):
            self._compile_expr(expr.operand)
        elif isinstance(expr.op, ast.Not):
            self._compile_expr(expr.operand)
            self._emit(OpCode.NOT)
        elif isinstance(expr.op, ast.Invert):
            self._compile_expr(expr.operand)
            self._emit(OpCode.LOAD_CONST, [self._const_idx(-1)])
            self._emit(OpCode.BXOR)
        else:
            raise CompileError(f"暂不支持的一元运算: {type(expr.op).__name__}")

    def _compile_boolop(self, expr: ast.BoolOp):
        """编译布尔短路运算"""
        is_and = isinstance(expr.op, ast.And)
        end_label = self._new_label("Lbool")
        self._compile_expr(expr.values[0])
        for value in expr.values[1:]:
            self._emit(OpCode.DUP)
            self._emit_jump(OpCode.JUMP_IF, end_label, [not is_and])
            self._emit(OpCode.POP)
            self._compile_expr(value)
        self._labels[end_label] = len(self._instrs)

    def _compile_compare(self, expr: ast.Compare):
        """
        编译比较表达式（支持链式比较 a < b < c）
        中间比较操作数会重新求值，因此要求比较操作数无副作用（名字/常量）。
        """
        self._compile_expr(expr.left)
        for i, (op, comparator) in enumerate(zip(expr.ops, expr.comparators)):
            if i > 0:
                # 重新求值上一个比较器，作为本次比较的左操作数
                self._compile_expr(expr.comparators[i - 1])
            self._compile_expr(comparator)
            self._emit(_compare_to_opcode(op))
            if i > 0:
                self._emit(OpCode.CMP_AND)

    def _compile_call(self, expr: ast.Call):
        """编译函数调用（FFI：调用编译期解析到的 Python 可调用对象）
        约定：参数先压栈，函数对象最后压栈（位于栈顶），CALL 先弹函数再弹参数。
        """
        if expr.keywords:
            raise CompileError("暂不支持关键字参数")
        func_value = self._resolve_constant_expr(expr.func)
        if func_value is None or not callable(func_value):
            raise CompileError(f"无法解析可调用目标: {ast.dump(expr.func)}")
        for arg in expr.args:
            self._compile_expr(arg)
        self._emit(OpCode.LOAD_CONST, [self._const_idx(func_value)])
        self._emit(OpCode.CALL, [len(expr.args)])

    # ==================== 名称解析 ====================

    def _local_reg(self, name: str) -> int:
        """获取局部变量的寄存器（自动分配）"""
        if name in self._param_regs:
            return self._param_regs[name]
        if name not in self._local_regs:
            if self._local_counter >= MAX_LOCALS:
                raise CompileError(f"局部变量过多（> {MAX_LOCALS}）")
            self._local_regs[name] = LOCAL_BASE_REG + self._local_counter
            self._local_counter += 1
        return self._local_regs[name]

    def _fresh_reg(self, hint: str = "temp") -> int:
        """分配一个新的临时寄存器"""
        if self._local_counter >= MAX_LOCALS:
            raise CompileError(f"临时变量过多（> {MAX_LOCALS}）")
        reg = LOCAL_BASE_REG + self._local_counter
        self._local_counter += 1
        return reg

    def _resolve_global(self, name: str) -> Any:
        """从函数全局作用域和 builtins 解析名字"""
        if name in self._globals_dict:
            return self._globals_dict[name]
        return getattr(builtins, name, None)

    def _resolve_constant_expr(self, expr: ast.expr) -> Any:
        """将表达式解析为常量值（用于全局/属性/可调用对象解析）"""
        if isinstance(expr, ast.Name):
            return self._resolve_global(expr.id)
        if isinstance(expr, ast.Attribute):
            base = self._resolve_constant_expr(expr.value)
            if base is None:
                return None
            try:
                return getattr(base, expr.attr)
            except AttributeError:
                return None
        if isinstance(expr, ast.Constant):
            return expr.value
        return None

    def _eval_literal(self, node: ast.expr) -> Any:
        """仅求值常量字面量（list/tuple/set/dict）"""
        try:
            return ast.literal_eval(node)
        except (ValueError, TypeError) as e:
            raise CompileError(f"仅支持常量字面量: {e}")

    # ==================== 推理图编译 ====================

    def compile_inference_graph(self, graph: Dict[str, Any]) -> ByteCode:
        """
        编译推理图

        图结构:
        {
            'name': 'my_model',
            'inputs': ['x', 'w'],
            'nodes': [
                {'name': 'h', 'type': 'matmul', 'inputs': ['x', 'w']},
                {'name': 'y', 'type': 'relu', 'inputs': ['h']},
            ],
            'outputs': ['y']
        }

        支持的节点类型:
        matmul/transpose/reshape/add/sub/mul/div/relu/sigmoid/tanh/gelu/softmax/
        reduce_sum/reduce_mean/reduce_max/concat/split/conv2d(简化)

        Args:
            graph: 推理图定义

        Returns:
            编译后的字节码
        """
        logger.info(f"开始编译推理图: {graph.get('name', 'unknown')}")

        # 初始化编译状态
        self.bytecode = ByteCode()
        self._reset_ast_state()

        inputs = graph.get('inputs', [])
        nodes = graph.get('nodes', [])
        outputs = graph.get('outputs', [])

        # 输入映射到参数寄存器
        input_regs = {
            name: PARAM_BASE_REG + i for i, name in enumerate(inputs)
        }
        if len(inputs) > MAX_PARAMS:
            raise CompileError(f"输入过多（> {MAX_PARAMS}）")

        # 节点输出映射到局部寄存器
        node_regs: Dict[str, int] = {}

        for node in nodes:
            node_name = node.get('name', 'unnamed')
            node_type = node.get('type', 'unknown')
            node_inputs = node.get('inputs', [])
            params = node.get('params', {})

            # 压入输入值
            for src in node_inputs:
                if src in input_regs:
                    self._emit(OpCode.LOAD_REG, [input_regs[src]])
                elif src in node_regs:
                    self._emit(OpCode.LOAD_REG, [node_regs[src]])
                elif src in params:
                    self._emit(OpCode.LOAD_CONST, [self._const_idx(params[src])])
                else:
                    raise CompileError(f"节点 {node_name} 引用未知输入: {src}")

            # 发射节点运算
            self._emit_graph_node_op(node_type, params, node_name)

            # 存储输出
            out_reg = self._fresh_reg(f"node_{node_name}")
            node_regs[node_name] = out_reg
            self._emit(OpCode.STORE_REG, [out_reg])

        # 输出
        for out in outputs:
            if out in input_regs:
                self._emit(OpCode.LOAD_REG, [input_regs[out]])
            elif out in node_regs:
                self._emit(OpCode.LOAD_REG, [node_regs[out]])
            else:
                raise CompileError(f"输出引用未知节点: {out}")

        # 返回（栈顶保留最后压入的输出）
        self._emit(OpCode.RET)

        # 解析标签并写入
        self._resolve_labels()
        self.bytecode.instructions = self._instrs

        self.bytecode.set_metadata('graph_name', graph.get('name', 'unknown'))
        self.bytecode.set_metadata('input_count', len(inputs))
        self.bytecode.set_metadata('node_count', len(nodes))
        self.bytecode.set_metadata('optimization_level', self.optimization_level)

        # 写入参数 schema（供宿主服务 marshal/unmarshal）
        # 类型编码约定：
        #   'float' / 'int'      → 标量（Python 数字）
        #   'vec' / 'vector'     → 1D 向量
        #   'mat' / 'matrix'     → 2D 张量（MemRef 嵌套列表）
        #   其它 → 从 graph.input_schema 读取，缺失默认 'mat'
        graph_schema = graph.get('input_schema') or graph.get('schema') or {}
        input_schema = {}
        for name in inputs:
            spec = graph_schema.get(name)
            if isinstance(spec, dict):
                input_schema[name] = spec
            elif isinstance(spec, str):
                input_schema[name] = {'type': spec}
            else:
                input_schema[name] = {'type': 'mat'}
        self.bytecode.set_metadata('input_names', list(inputs))
        self.bytecode.set_metadata('schema', input_schema)

        if self.optimization_level > 0:
            self._optimize_bytecode()

        logger.info(f"推理图编译完成: {len(self.bytecode.instructions)} 条指令")

        return self.bytecode

    def _emit_graph_node_op(self, node_type: str, params: Dict[str, Any], node_name: str):
        """为图节点发射运算指令"""
        op_map = {
            'add': OpCode.ADD,
            'sub': OpCode.SUB,
            'mul': OpCode.MUL,
            'div': OpCode.DIV,
            'matmul': OpCode.MATMUL,
            'transpose': OpCode.TRANSPOSE,
            'relu': OpCode.RELU,
            'sigmoid': OpCode.SIGMOID,
            'tanh': OpCode.TANH,
            'gelu': OpCode.GELU,
            'softmax': OpCode.SOFTMAX,
            'reduce_sum': OpCode.REDUCE_SUM,
            'reduce_mean': OpCode.REDUCE_MEAN,
            'reduce_max': OpCode.REDUCE_MAX,
        }
        if node_type in op_map:
            self._emit(op_map[node_type])
        elif node_type == 'reshape':
            shape = params.get('shape')
            if shape is None:
                raise CompileError(f"reshape 节点 {node_name} 缺少 shape 参数")
            self._emit(OpCode.LOAD_CONST, [self._const_idx(shape)])
            self._emit(OpCode.RESHAPE)
        elif node_type == 'concat':
            count = params.get('count', 2)
            axis = params.get('axis', 0)
            self._emit(OpCode.CONCAT, [count, axis])
        elif node_type == 'conv2d':
            # 简化卷积：按矩阵乘法处理（权重作为常量参数）
            weights = params.get('weights')
            if weights is None:
                raise CompileError(f"conv2d 节点 {node_name} 缺少 weights 参数")
            self._emit(OpCode.LOAD_CONST, [self._const_idx(weights)])
            self._emit(OpCode.MATMUL)
        else:
            raise CompileError(f"未知的节点类型: {node_type}")

    # ==================== 数据编译 ====================

    def _compile_data(self, data: Union[str, bytes]) -> ByteCode:
        """
        将数据编译为字节码

        Args:
            data: 输入数据（字符串或字节）

        Returns:
            编译后的字节码
        """
        bytecode = ByteCode()

        # 如果是字符串，转换为字节
        if isinstance(data, str):
            data_bytes = data.encode('utf-8')
        else:
            data_bytes = data

        # 添加常量
        const_idx = bytecode.add_constant(data_bytes)

        # 加载常量
        bytecode.add_instruction(Instruction(OpCode.LOAD_CONST, [const_idx]))

        # 返回
        bytecode.add_instruction(Instruction(OpCode.RET, []))

        return bytecode

    # ==================== 优化 ====================

    def _optimize_bytecode(self):
        """优化字节码"""
        logger.info("开始优化字节码")

        # 应用优化策略
        if self.optimization_level >= 1:
            # 基本优化
            self._optimize_peephole()
            self._optimize_dead_code()

        if self.optimization_level >= 2:
            # 激进优化
            self._optimize_constant_folding()
            self._optimize_strength_reduction()

        logger.info("字节码优化完成")

    def _collect_targets(self) -> set:
        """收集所有跳转目标索引"""
        targets = set()
        for inst in self.bytecode.instructions:
            if inst.opcode in (OpCode.JUMP, OpCode.JUMP_IF) and inst.operands:
                target = inst.operands[0]
                if isinstance(target, int):
                    targets.add(target)
        return targets

    def _optimize_peephole(self):
        """窥孔优化（跳过跳转目标，避免破坏控制流）"""
        if not self.bytecode:
            return

        instructions = self.bytecode.instructions
        targets = self._collect_targets()
        optimized = []
        i = 0

        while i < len(instructions):
            inst = instructions[i]

            # 模式1: PUSH + POP -> 删除这对指令
            if i not in targets and inst.opcode == OpCode.PUSH and i + 1 < len(instructions):
                next_inst = instructions[i + 1]
                if next_inst.opcode == OpCode.POP:
                    i += 2
                    continue

            # 模式2: LOAD_CONST + LOAD_CONST + ADD -> 计算常量加法
            if (i not in targets and i + 1 not in targets and
                    inst.opcode == OpCode.LOAD_CONST and i + 2 < len(instructions)):
                next1 = instructions[i + 1]
                next2 = instructions[i + 2]
                if (next1.opcode == OpCode.LOAD_CONST and next2.opcode == OpCode.ADD):
                    idx1 = inst.operands[0]
                    idx2 = next1.operands[0]
                    if (idx1 < len(self.bytecode.constants) and
                            idx2 < len(self.bytecode.constants)):
                        val1 = self.bytecode.constants[idx1]
                        val2 = self.bytecode.constants[idx2]
                        if isinstance(val1, (int, float)) and isinstance(val2, (int, float)):
                            result = val1 + val2
                            new_idx = self.bytecode.add_constant(result)
                            optimized.append(Instruction(OpCode.LOAD_CONST, [new_idx]))
                            i += 3
                            continue

            # 其他情况，保留指令
            optimized.append(inst)
            i += 1

        self.bytecode.instructions = optimized

    def _optimize_dead_code(self):
        """死代码消除（带跳转目标重映射）"""
        if not self.bytecode:
            return

        instructions = self.bytecode.instructions

        # 标记可达代码
        reachable = set()
        worklist = [0]  # 从第一条指令开始

        while worklist:
            pc = worklist.pop()
            if pc in reachable or pc >= len(instructions):
                continue

            reachable.add(pc)
            inst = instructions[pc]

            if inst.opcode == OpCode.JUMP:
                target = inst.operands[0]
                if isinstance(target, int):
                    worklist.append(target)
            elif inst.opcode == OpCode.JUMP_IF:
                target = inst.operands[0]
                if isinstance(target, int):
                    worklist.append(target)
                worklist.append(pc + 1)
            elif inst.opcode in (OpCode.RET, OpCode.HALT):
                pass
            else:
                worklist.append(pc + 1)

        # 构建 旧索引 -> 新索引 映射
        old_to_new = {}
        new_instructions = []
        for idx, inst in enumerate(instructions):
            if idx in reachable:
                old_to_new[idx] = len(new_instructions)
                new_instructions.append(inst)

        # 重映射跳转目标
        for inst in new_instructions:
            if inst.opcode in (OpCode.JUMP, OpCode.JUMP_IF) and inst.operands:
                old_target = inst.operands[0]
                if isinstance(old_target, int) and old_target in old_to_new:
                    inst.operands[0] = old_to_new[old_target]

        self.bytecode.instructions = new_instructions

    def _optimize_constant_folding(self):
        """常量折叠"""
        pass

    def _optimize_strength_reduction(self):
        """强度削减"""
        pass

    # ==================== 混淆保护 ====================

    def add_obfuscation(self, bytecode: ByteCode, level: int = 1) -> ByteCode:
        """
        添加混淆保护（保证不破坏执行语义）

        Args:
            bytecode: 要混淆的字节码
            level: 混淆级别（1-轻量，2-中等，3-重度）

        Returns:
            混淆后的字节码
        """
        logger.info(f"添加混淆保护，级别: {level}")

        if level >= 1:
            bytecode = self._add_junk_instructions(bytecode)

        if level >= 2:
            bytecode = self._add_opaque_predicates(bytecode)

        if level >= 3:
            bytecode = self._add_bogus_control_flow(bytecode)

        return bytecode

    def _insert_remap(self, bytecode: ByteCode, insertions: Dict[int, List[Instruction]]):
        """
        在指定指令索引前插入指令，并重映射所有跳转目标。
        这是混淆注入的基础设施：任何插入都必须经过这里，否则跳转目标会错位。
        """
        if not insertions:
            return

        instructions = bytecode.instructions
        total_count = len(instructions)

        # 计算每个旧索引位置的累计偏移（包含该位置插入的指令数）
        # shift_at[i] = 所有插入点 key <= i 带来的总偏移
        shift_at = [0] * (total_count + 1)
        running = 0
        for i in range(total_count + 1):
            if i in insertions:
                running += len(insertions[i])
            shift_at[i] = running

        # 执行插入（支持索引 == total_count 的末尾插入）
        new_instructions: List[Instruction] = []
        for idx in range(total_count + 1):
            if idx in insertions:
                new_instructions.extend(insertions[idx])
            if idx < total_count:
                new_instructions.append(instructions[idx])

        # 重映射跳转目标：旧目标 target 前面所有插入点的偏移之和 = shift_at[target]
        def remap(target: int) -> int:
            if target < 0:
                return target
            if target > total_count:
                # 跳转到末尾之后（原本是非法的），映射到新指令末尾
                return len(new_instructions)
            return target + shift_at[target]

        for inst in new_instructions:
            if inst.opcode in (OpCode.JUMP, OpCode.JUMP_IF) and inst.operands:
                target = inst.operands[0]
                if isinstance(target, int):
                    inst.operands[0] = remap(target)

        bytecode.instructions = new_instructions

    def _add_junk_instructions(self, bytecode: ByteCode) -> ByteCode:
        """添加垃圾指令（仅 NOP，语义安全）"""
        import random

        insertions: Dict[int, List[Instruction]] = {}
        for idx, inst in enumerate(bytecode.instructions):
            if random.random() < 0.1:  # 10%概率插入
                insertions.setdefault(idx + 1, []).append(Instruction(OpCode.NOP, []))

        self._insert_remap(bytecode, insertions)
        return bytecode

    def _add_opaque_predicates(self, bytecode: ByteCode) -> ByteCode:
        """
        添加不透明谓词
        在条件跳转前插入 DUP/PUSH 0/CMP_NE/POP 序列：
        不改变栈顶条件值，只增加静态分析的复杂度
        """
        import random

        insertions: Dict[int, List[Instruction]] = {}
        for idx, inst in enumerate(bytecode.instructions):
            if inst.opcode == OpCode.JUMP_IF and random.random() < 0.7:
                insertions[idx] = [
                    Instruction(OpCode.DUP, []),
                    Instruction(OpCode.PUSH, [0]),
                    Instruction(OpCode.CMP_NE, []),
                    Instruction(OpCode.POP, []),
                ]

        self._insert_remap(bytecode, insertions)
        return bytecode

    def _add_bogus_control_flow(self, bytecode: ByteCode) -> ByteCode:
        """添加虚假控制流（PUSH/POP 垃圾对 + NOP，语义中性）"""
        import random

        insertions: Dict[int, List[Instruction]] = {}
        for idx, inst in enumerate(bytecode.instructions):
            junk: List[Instruction] = []
            if random.random() < 0.15:
                junk.append(Instruction(OpCode.PUSH, [random.randint(0, 255)]))
                junk.append(Instruction(OpCode.POP, []))
            if random.random() < 0.1:
                junk.append(Instruction(OpCode.NOP, []))
            if junk:
                insertions[idx] = junk

        self._insert_remap(bytecode, insertions)
        return bytecode

    # ==================== 文件接口 ====================

    def compile_to_file(self, func: Callable, output_path: str, encrypt: bool = True):
        """
        编译函数并保存到文件

        Args:
            func: 要编译的函数
            output_path: 输出文件路径
            encrypt: 是否加密字节码
        """
        bytecode = self.compile_function(func, encrypt)

        # 序列化字节码
        data = bytecode.serialize(encrypt=encrypt)

        # 写入文件
        with open(output_path, 'wb') as f:
            f.write(data)

        logger.info(f"字节码已保存到: {output_path}")

    def load_from_file(self, input_path: str) -> ByteCode:
        """
        从文件加载字节码

        Args:
            input_path: 输入文件路径

        Returns:
            加载的字节码对象
        """
        with open(input_path, 'rb') as f:
            data = f.read()

        bytecode = ByteCode.deserialize(data, decrypt=True)
        logger.info(f"字节码已从文件加载: {input_path}")

        return bytecode


# ==================== 操作符映射 ====================

def _floordiv(a: Any, b: Any) -> Any:
    """地板除（FFI 助手）：标量按 Python // 语义，张量退化为真除"""
    if isinstance(a, (int, float)) and isinstance(b, (int, float)) and b != 0:
        return a // b
    if isinstance(a, (int, float, list, tuple)) and isinstance(b, (int, float, list, tuple)):
        return a / b if b != 0 else 0
    return 0


def _make_list(*args):
    """FFI 助手：list 字面量构造（list(a,b,c) 语法不成立，因此用 helper）"""
    return list(args)


def _make_set(*args):
    """FFI 助手：set 字面量构造"""
    return set(args)


def _make_tuple(*args):
    """FFI 助手：tuple 字面量构造（tuple(*args) 语法不成立，因此用 helper）"""
    return tuple(args)


def _make_dict_pairs(*args):
    """FFI 助手：从 [k1, v1, k2, v2, ...] 构造 dict"""
    if len(args) % 2 != 0:
        raise TypeError("dict literal: 需要成对 [k, v] 参数")
    return {args[i]: args[i + 1] for i in range(0, len(args), 2)}


def _binop_to_opcode(op: ast.operator) -> OpCode:
    """二元运算 AST 节点 -> 操作码"""
    mapping = {
        ast.Add: OpCode.ADD,
        ast.Sub: OpCode.SUB,
        ast.Mult: OpCode.MUL,
        ast.Div: OpCode.DIV,
        ast.Mod: OpCode.MOD,
        ast.Pow: OpCode.POW,
        ast.BitOr: OpCode.BOR,
        ast.BitAnd: OpCode.BAND,
        ast.BitXor: OpCode.BXOR,
    }
    # FloorDiv 由 _compile_expr 特判处理（DIV + CAST_INT），LShift/RShift 不支持
    for cls, code in mapping.items():
        if isinstance(op, cls):
            return code
    raise CompileError(f"暂不支持的二元运算: {type(op).__name__}")


def _compare_to_opcode(op: ast.cmpop) -> OpCode:
    """比较运算 AST 节点 -> 操作码"""
    mapping = {
        ast.Eq: OpCode.CMP_EQ,
        ast.NotEq: OpCode.CMP_NE,
        ast.Lt: OpCode.CMP_LT,
        ast.LtE: OpCode.CMP_LE,
        ast.Gt: OpCode.CMP_GT,
        ast.GtE: OpCode.CMP_GE,
    }
    for cls, code in mapping.items():
        if isinstance(op, cls):
            return code
    raise CompileError(f"暂不支持的比较运算: {type(op).__name__}")


def _source_available(func: Callable) -> bool:
    """源码是否可用"""
    try:
        inspect.getsource(func)
        return True
    except (OSError, TypeError):
        return False
  from cryptography.hazmat.backends import default_backend
