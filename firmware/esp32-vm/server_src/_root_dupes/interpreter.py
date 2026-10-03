"""
Layer 5: 私有IR虚拟机解释器
实现栈式虚拟机，执行混淆字节码

包含完整调用约定：
- 局部变量存于通用寄存器（r5..r19）
- 函数参数存于参数寄存器（r20..r31）
- CALL 同时支持内部调用（整数地址）与 FFI（Python 可调用对象）
- RET 通过栈传递返回值
"""

from typing import Any, Dict, List, Optional, Callable, Tuple
from dataclasses import dataclass, field
import threading
import time
import logging

from ...core.exceptions import VMError
from .opcode import OpCode, OpCodeCategory, has_side_effect
from .bytecode import ByteCode, Instruction
from . import tensor

# 配置日志
logger = logging.getLogger(__name__)

# 参数寄存器基址（与 compiler.PARAM_BASE_REG 保持一致）
PARAM_BASE_INDEX = 50


@dataclass
class VMState:
    """
    虚拟机状态
    保存执行过程中的所有状态信息
    """
    # 程序计数器
    pc: int = 0

    # 栈指针
    sp: int = 0

    # 基址指针（用于函数调用）
    bp: int = 0

    # 标志位
    flags: Dict[str, bool] = field(default_factory=lambda: {
        'zero': False,
        'negative': False,
        'overflow': False,
        'carry': False
    })

    # 是否正在运行
    running: bool = False

    # 是否暂停
    paused: bool = False


class Memory:
    """
    内存管理类
    模拟虚拟机的内存空间
    """

    def __init__(self, size: int = 1024 * 1024 * 100):  # 默认100MB
        """
        初始化内存

        Args:
            size: 内存大小（字节）
        """
        self.size = size
        self.data = bytearray(size)
        self.alloc_table: Dict[int, Tuple[int, int]] = {}  # 地址 -> (大小, 类型)
        self.next_address = 0x1000  # 从4KB开始分配

    def alloc(self, size: int, mem_type: str = "generic") -> int:
        """
        分配内存块

        Args:
            size: 请求的大小
            mem_type: 内存类型

        Returns:
            分配的内存地址

        Raises:
            MemoryError: 如果内存不足
        """
        # 对齐到8字节
        size = (size + 7) & ~7

        # 检查是否有足够空间
        if self.next_address + size > self.size:
            raise MemoryError("内存不足")

        address = self.next_address
        self.alloc_table[address] = (size, mem_type)
        self.next_address += size

        logger.debug(f"分配内存: 地址=0x{address:x}, 大小={size}, 类型={mem_type}")

        return address

    def free(self, address: int) -> bool:
        """
        释放内存块

        Args:
            address: 要释放的内存地址

        Returns:
            是否成功释放
        """
        if address in self.alloc_table:
            del self.alloc_table[address]
            logger.debug(f"释放内存: 地址=0x{address:x}")
            return True
        return False

    def read(self, address: int, size: int) -> bytes:
        """
        从内存读取数据

        Args:
            address: 内存地址
            size: 读取大小

        Returns:
            读取的字节数据

        Raises:
            MemoryError: 如果地址无效
        """
        if address + size > self.size:
            raise MemoryError("内存访问越界")

        return bytes(self.data[address:address + size])

    def write(self, address: int, data: bytes) -> bool:
        """
        向内存写入数据

        Args:
            address: 内存地址
            data: 要写入的数据

        Returns:
            是否成功写入

        Raises:
            MemoryError: 如果地址无效
        """
        if address + len(data) > self.size:
            raise MemoryError("内存访问越界")

        self.data[address:address + len(data)] = data
        return True

    def copy(self, src: int, dst: int, size: int) -> bool:
        """复制内存块"""
        if src + size > self.size or dst + size > self.size:
            raise MemoryError("内存访问越界")
        self.data[dst:dst + size] = self.data[src:src + size]
        return True

    def fill(self, address: int, size: int, value: int) -> bool:
        """填充内存块"""
        if address + size > self.size:
            raise MemoryError("内存访问越界")
        self.data[address:address + size] = bytes([value & 0xFF]) * size
        return True

    def get_allocated_size(self) -> int:
        """获取已分配的内存大小"""
        return sum(size for size, _ in self.alloc_table.values())


class RegisterFile:
    """
    寄存器文件
    管理虚拟机的寄存器
    """

    def __init__(self, count: int = 64):
        """
        初始化寄存器文件

        Args:
            count: 寄存器数量（默认扩展到 64，以容纳更多局部变量与参数）
        """
        self.count = count
        self.registers: List[Any] = [None] * count
        self.names = [f"r{i}" for i in range(count)]

        # 特殊寄存器名称映射
        self.special_registers = {
            "pc": 0,      # 程序计数器
            "sp": 1,      # 栈指针
            "bp": 2,      # 基址指针
            "retval": 3,  # 返回值寄存器
            "temp": 4,    # 临时寄存器
        }

        # 初始化特殊寄存器名称
        for name, idx in self.special_registers.items():
            self.names[idx] = name

    def get(self, index: int) -> Any:
        """
        获取寄存器值

        Args:
            index: 寄存器索引

        Returns:
            寄存器值

        Raises:
            IndexError: 如果索引无效
        """
        if 0 <= index < self.count:
            return self.registers[index]
        raise IndexError(f"寄存器索引越界: {index}")

    def set(self, index: int, value: Any):
        """
        设置寄存器值

        Args:
            index: 寄存器索引
            value: 要设置的值

        Raises:
            IndexError: 如果索引无效
        """
        if 0 <= index < self.count:
            self.registers[index] = value
        else:
            raise IndexError(f"寄存器索引越界: {index}")

    def get_by_name(self, name: str) -> Any:
        """
        通过名称获取寄存器值

        Args:
            name: 寄存器名称

        Returns:
            寄存器值

        Raises:
            KeyError: 如果名称无效
        """
        if name in self.special_registers:
            return self.get(self.special_registers[name])
        elif name in self.names:
            return self.get(self.names.index(name))
        raise KeyError(f"未知寄存器名称: {name}")

    def set_by_name(self, name: str, value: Any):
        """
        通过名称设置寄存器值

        Args:
            name: 寄存器名称
            value: 要设置的值

        Raises:
            KeyError: 如果名称无效
        """
        if name in self.special_registers:
            self.set(self.special_registers[name], value)
        elif name in self.names:
            self.set(self.names.index(name), value)
        else:
            raise KeyError(f"未知寄存器名称: {name}")

    def reset(self):
        """重置所有寄存器"""
        self.registers = [None] * self.count


class VMInterpreter:
    """
    私有IR虚拟机解释器
    执行混淆字节码的栈式虚拟机
    """

    def __init__(self, stack_size: int = 10000, max_instructions: int = 10_000_000):
        """
        初始化虚拟机解释器

        Args:
            stack_size: 栈大小
            max_instructions: 最大执行指令数（防死循环保护）
        """
        # 栈
        self.stack: List[Any] = [None] * stack_size
        self.stack_size = stack_size

        # 虚拟机状态
        self.state = VMState()

        # 内存管理
        self.memory = Memory()

        # 寄存器文件
        self.registers = RegisterFile()

        # 字节码
        self.bytecode: Optional[ByteCode] = None

        # 最大指令执行数（防死循环）
        self.max_instructions = max_instructions

        # 调度表（加密）
        self._dispatch_table: Dict[OpCode, Callable] = {}
        self._build_dispatch_table()

        # 函数调用栈
        self.call_stack: List[Tuple[int, int]] = []  # (返回地址, 旧bp)

        # 循环栈 (loop_start, loop_end)
        self.loop_stack: List[Tuple[int, int]] = []

        # 线程锁
        self._lock = threading.Lock()

        # 统计信息
        self.stats = {
            'instructions_executed': 0,
            'memory_allocated': 0,
            'memory_freed': 0,
            'function_calls': 0,
        }

    def _build_dispatch_table(self):
        """构建加密的调度表"""
        # 算术操作
        self._dispatch_table[OpCode.ADD] = self._exec_add
        self._dispatch_table[OpCode.SUB] = self._exec_sub
        self._dispatch_table[OpCode.MUL] = self._exec_mul
        self._dispatch_table[OpCode.DIV] = self._exec_div
        self._dispatch_table[OpCode.MOD] = self._exec_mod
        self._dispatch_table[OpCode.POW] = self._exec_pow
        self._dispatch_table[OpCode.BOR] = self._exec_bor
        self._dispatch_table[OpCode.BAND] = self._exec_band
        self._dispatch_table[OpCode.BXOR] = self._exec_bxor
        self._dispatch_table[OpCode.NEG] = self._exec_neg
        self._dispatch_table[OpCode.ABS] = self._exec_abs
        self._dispatch_table[OpCode.SQRT] = self._exec_sqrt
        self._dispatch_table[OpCode.EXP] = self._exec_exp
        self._dispatch_table[OpCode.LOG] = self._exec_log
        self._dispatch_table[OpCode.NOT] = self._exec_not

        # 张量操作
        self._dispatch_table[OpCode.MATMUL] = self._exec_matmul
        self._dispatch_table[OpCode.TRANSPOSE] = self._exec_transpose
        self._dispatch_table[OpCode.RESHAPE] = self._exec_reshape
        self._dispatch_table[OpCode.SLICE] = self._exec_slice
        self._dispatch_table[OpCode.CONCAT] = self._exec_concat
        self._dispatch_table[OpCode.SPLIT] = self._exec_split
        self._dispatch_table[OpCode.PERMUTE] = self._exec_permute
        self._dispatch_table[OpCode.SQUEEZE] = self._exec_squeeze
        self._dispatch_table[OpCode.UNSQUEEZE] = self._exec_unsqueeze
        self._dispatch_table[OpCode.BROADCAST] = self._exec_broadcast
        self._dispatch_table[OpCode.GATHER] = self._exec_gather
        self._dispatch_table[OpCode.SCATTER] = self._exec_scatter
        self._dispatch_table[OpCode.REDUCE_SUM] = self._exec_reduce_sum
        self._dispatch_table[OpCode.REDUCE_MEAN] = self._exec_reduce_mean
        self._dispatch_table[OpCode.REDUCE_MAX] = self._exec_reduce_max

        # 内存操作
        self._dispatch_table[OpCode.LOAD] = self._exec_load
        self._dispatch_table[OpCode.STORE] = self._exec_store
        self._dispatch_table[OpCode.ALLOC] = self._exec_alloc
        self._dispatch_table[OpCode.FREE] = self._exec_free
        self._dispatch_table[OpCode.COPY] = self._exec_copy
        self._dispatch_table[OpCode.MOVE] = self._exec_move
        self._dispatch_table[OpCode.FILL] = self._exec_fill
        self._dispatch_table[OpCode.ZERO] = self._exec_zero
        self._dispatch_table[OpCode.LOAD_CONST] = self._exec_load_const
        self._dispatch_table[OpCode.LOAD_PARAM] = self._exec_load_param
        self._dispatch_table[OpCode.LOAD_REG] = self._exec_load_reg
        self._dispatch_table[OpCode.STORE_REG] = self._exec_store_reg

        # 控制操作
        self._dispatch_table[OpCode.JUMP] = self._exec_jump
        self._dispatch_table[OpCode.JUMP_IF] = self._exec_jump_if
        self._dispatch_table[OpCode.CALL] = self._exec_call
        self._dispatch_table[OpCode.RET] = self._exec_ret
        self._dispatch_table[OpCode.NOP] = self._exec_nop
        self._dispatch_table[OpCode.HALT] = self._exec_halt
        self._dispatch_table[OpCode.LOOP] = self._exec_loop
        self._dispatch_table[OpCode.BREAK] = self._exec_break
        self._dispatch_table[OpCode.CONTINUE] = self._exec_continue
        self._dispatch_table[OpCode.SWITCH] = self._exec_switch

        # 比较操作
        self._dispatch_table[OpCode.CMP_EQ] = self._exec_cmp_eq
        self._dispatch_table[OpCode.CMP_NE] = self._exec_cmp_ne
        self._dispatch_table[OpCode.CMP_LT] = self._exec_cmp_lt
        self._dispatch_table[OpCode.CMP_LE] = self._exec_cmp_le
        self._dispatch_table[OpCode.CMP_GT] = self._exec_cmp_gt
        self._dispatch_table[OpCode.CMP_GE] = self._exec_cmp_ge
        self._dispatch_table[OpCode.CMP_AND] = self._exec_cmp_and
        self._dispatch_table[OpCode.CMP_OR] = self._exec_cmp_or

        # 激活函数
        self._dispatch_table[OpCode.RELU] = self._exec_relu
        self._dispatch_table[OpCode.SIGMOID] = self._exec_sigmoid
        self._dispatch_table[OpCode.TANH] = self._exec_tanh
        self._dispatch_table[OpCode.GELU] = self._exec_gelu
        self._dispatch_table[OpCode.SOFTMAX] = self._exec_softmax

        # 混淆操作
        self._dispatch_table[OpCode.OBF_PI] = self._exec_obf_pi
        self._dispatch_table[OpCode.OBF_DECRYPT] = self._exec_obf_decrypt
        self._dispatch_table[OpCode.OBF_CORRECT] = self._exec_obf_correct
        self._dispatch_table[OpCode.OBF_OPAQUE] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_BOGUS] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_FLATTEN] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_SUBSTITUTE] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_MERGE] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_SPLIT] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_ENCODE] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_DECODE] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_JUNK] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_PROXY] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_WRAP] = self._exec_obf_noop
        self._dispatch_table[OpCode.OBF_TRANSFORM] = self._exec_obf_noop

        # 类型操作
        self._dispatch_table[OpCode.CAST_INT] = self._exec_cast_int
        self._dispatch_table[OpCode.CAST_FLOAT] = self._exec_cast_float
        self._dispatch_table[OpCode.CAST_BOOL] = self._exec_cast_bool
        self._dispatch_table[OpCode.TYPEOF] = self._exec_typeof
        self._dispatch_table[OpCode.ISNULL] = self._exec_isnull

        # 栈操作
        self._dispatch_table[OpCode.PUSH] = self._exec_push
        self._dispatch_table[OpCode.POP] = self._exec_pop
        self._dispatch_table[OpCode.DUP] = self._exec_dup
        self._dispatch_table[OpCode.SWAP] = self._exec_swap
        self._dispatch_table[OpCode.ROT] = self._exec_rot

        # 特殊操作
        self._dispatch_table[OpCode.DEBUG] = self._exec_debug
        self._dispatch_table[OpCode.PROFILE] = self._exec_profile
        self._dispatch_table[OpCode.ASSERT] = self._exec_assert
        self._dispatch_table[OpCode.VERIFY] = self._exec_verify
        self._dispatch_table[OpCode.SANITY] = self._exec_verify

    def load_bytecode(self, bytecode: ByteCode):
        """
        加载字节码

        Args:
            bytecode: 要加载的字节码
        """
        with self._lock:
            self.bytecode = bytecode
            self.state.pc = 0
            self.state.running = False
            logger.info(f"加载字节码: {bytecode.get_instruction_count()} 条指令")

    def call(self, *args: Any) -> Any:
        """
        以位置参数调用已加载字节码（进程内便捷入口，供宿主/FFI 使用）。

        参数按调用约定写入参数寄存器（r50 起，最长 14 个），随后执行。

        Args:
            *args: 位置参数列表

        Returns:
            VM 执行结果（可由 execute() 决定）

        Raises:
            RuntimeError: 如果字节码未加载
        """
        if not self.bytecode:
            raise RuntimeError("字节码未加载")
        if len(args) > 14:
            raise ValueError(f"参数过多（> 14）：{len(args)}")
        for i, arg in enumerate(args):
            self.registers.set(PARAM_BASE_INDEX + i, arg)
        return self.execute()

    def execute(self) -> Any:
        """
        执行字节码

        Returns:
            执行结果（如果有的话）

        Raises:
            RuntimeError: 如果字节码未加载
            VMError: 如果执行出错或指令超限
        """
        if not self.bytecode:
            raise RuntimeError("字节码未加载")

        with self._lock:
            self.state.running = True
            self.state.pc = 0

            try:
                while self.state.running and self.state.pc < len(self.bytecode.instructions):
                    if self.state.paused:
                        # 暂停时让出 CPU，避免忙等
                        time.sleep(0.001)
                        continue

                    # 防死循环保护
                    if self.stats['instructions_executed'] >= self.max_instructions:
                        raise VMError(
                            f"指令执行超过上限 {self.max_instructions}，疑似死循环",
                            vm_state=f"pc={self.state.pc} sp={self.state.sp}"
                        )

                    # 获取当前指令
                    instruction = self.bytecode.instructions[self.state.pc]
                    logger.debug(f"执行指令 [{self.state.pc}]: {instruction}")

                    # 执行指令
                    self._execute_instruction(instruction)

                    # 更新统计
                    self.stats['instructions_executed'] += 1

                # 返回结果
                if len(self.stack) > 0 and self.state.sp > 0:
                    return self.stack[self.state.sp - 1]
                return None

            except VMError:
                self.state.running = False
                raise
            except Exception as e:
                logger.error(f"执行错误: {e}")
                self.state.running = False
                raise VMError(
                    f"执行错误: {e}",
                    vm_state=f"pc={self.state.pc} sp={self.state.sp}"
                )

    def _execute_instruction(self, instruction: Instruction):
        """
        执行单条指令

        Args:
            instruction: 要执行的指令
        """
        opcode = instruction.opcode

        # 从调度表获取处理函数
        handler = self._dispatch_table.get(opcode)
        if handler:
            handler(instruction.operands)
        else:
            logger.warning(f"未实现的操作码: {opcode.name}")
            self.state.pc += 1

    # ==================== 栈操作 ====================

    def push(self, value: Any):
        """
        压栈

        Args:
            value: 要压入的值

        Raises:
            RuntimeError: 如果栈溢出
        """
        if self.state.sp >= self.stack_size:
            raise RuntimeError("栈溢出")

        self.stack[self.state.sp] = value
        self.state.sp += 1

    def pop(self) -> Any:
        """
        出栈

        Returns:
            栈顶值

        Raises:
            RuntimeError: 如果栈下溢
        """
        if self.state.sp <= 0:
            raise RuntimeError("栈下溢")

        self.state.sp -= 1
        value = self.stack[self.state.sp]
        self.stack[self.state.sp] = None
        return value

    def peek(self, offset: int = 0) -> Any:
        """
        查看栈顶元素

        Args:
            offset: 相对于栈顶的偏移量

        Returns:
            栈元素值
        """
        idx = self.state.sp - 1 - offset
        if 0 <= idx < self.stack_size:
            return self.stack[idx]
        return None

    def _pop_operand(self, operands: List[Any], index: int, default: Any = None) -> Any:
        """安全读取操作数，越界时返回默认值"""
        if index < len(operands):
            return operands[index]
        return default

    # ==================== 算术操作实现 ====================

    def _binary_op(self, fn):
        """通用二元运算"""
        b = self.pop()
        a = self.pop()
        self.push(fn(a, b))
        self.state.pc += 1

    def _exec_add(self, operands: List[Any]):
        self._binary_op(tensor.add)

    def _exec_sub(self, operands: List[Any]):
        self._binary_op(tensor.sub)

    def _exec_mul(self, operands: List[Any]):
        self._binary_op(tensor.mul)

    def _exec_div(self, operands: List[Any]):
        self._binary_op(tensor.div)

    def _exec_mod(self, operands: List[Any]):
        self._binary_op(tensor.mod)

    def _exec_pow(self, operands: List[Any]):
        self._binary_op(tensor.pow_op)

    def _exec_bor(self, operands: List[Any]):
        self._binary_op(tensor.bitwise_or)

    def _exec_band(self, operands: List[Any]):
        self._binary_op(tensor.bitwise_and)

    def _exec_bxor(self, operands: List[Any]):
        self._binary_op(tensor.bitwise_xor)

    def _exec_neg(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.neg(a))
        self.state.pc += 1

    def _exec_abs(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.abs_op(a))
        self.state.pc += 1

    def _exec_sqrt(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.sqrt(a))
        self.state.pc += 1

    def _exec_exp(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.exp(a))
        self.state.pc += 1

    def _exec_log(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.log(a))
        self.state.pc += 1

    def _exec_not(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.not_op(a))
        self.state.pc += 1

    # ==================== 张量操作实现 ====================

    def _exec_matmul(self, operands: List[Any]):
        b = self.pop()
        a = self.pop()
        self.push(tensor.matmul(a, b))
        self.state.pc += 1

    def _exec_transpose(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.transpose(a))
        self.state.pc += 1

    def _exec_reshape(self, operands: List[Any]):
        shape = self.pop()
        a = self.pop()
        self.push(tensor.reshape(a, shape))
        self.state.pc += 1

    def _exec_slice(self, operands: List[Any]):
        indices = self.pop()
        a = self.pop()
        self.push(tensor.slice_op(a, indices))
        self.state.pc += 1

    def _exec_concat(self, operands: List[Any]):
        count = self._pop_operand(operands, 0, 2)
        axis = self._pop_operand(operands, 1, 0)
        values = []
        for _ in range(count):
            values.append(self.pop())
        self.push(tensor.concat(values[::-1], axis=axis))
        self.state.pc += 1

    def _exec_split(self, operands: List[Any]):
        indices_or_sections = self.pop()
        axis = self.pop()
        a = self.pop()
        results = tensor.split_op(a, indices_or_sections, axis=axis)
        for result in results:
            self.push(result)
        self.state.pc += 1

    def _exec_permute(self, operands: List[Any]):
        axes = self.pop()
        a = self.pop()
        self.push(tensor.permute(a, axes))
        self.state.pc += 1

    def _exec_squeeze(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.squeeze(a))
        self.state.pc += 1

    def _exec_unsqueeze(self, operands: List[Any]):
        axis = self._pop_operand(operands, 0, 0)
        a = self.pop()
        self.push(tensor.unsqueeze(a, axis))
        self.state.pc += 1

    def _exec_broadcast(self, operands: List[Any]):
        # 广播：与标量运算等价，交给 tensor 逐元素处理
        b = self.pop()
        a = self.pop()
        self.push(tensor.add(a, tensor.sub(b, b)))  # 保持形状的恒等操作
        self.state.pc += 1

    def _exec_gather(self, operands: List[Any]):
        indices = self.pop()
        a = self.pop()
        self.push(tensor.slice_op(a, list(indices) if not isinstance(indices, list) else indices))
        self.state.pc += 1

    def _exec_scatter(self, operands: List[Any]):
        # 简化实现：将值写入目标位置
        value = self.pop()
        indices = self.pop()
        a = self.pop()
        if isinstance(a, list):
            result = list(a)
            if isinstance(indices, int):
                result[indices] = value
            else:
                for idx, v in zip(indices, value):
                    result[idx] = v
            self.push(result)
        else:
            self.push(value)
        self.state.pc += 1

    def _exec_reduce_sum(self, operands: List[Any]):
        axis = self.pop() if self.state.sp > self.state.bp else None
        a = self.pop()
        self.push(tensor.reduce_sum(a, axis=axis))
        self.state.pc += 1

    def _exec_reduce_mean(self, operands: List[Any]):
        axis = self.pop() if self.state.sp > self.state.bp else None
        a = self.pop()
        self.push(tensor.reduce_mean(a, axis=axis))
        self.state.pc += 1

    def _exec_reduce_max(self, operands: List[Any]):
        axis = self.pop() if self.state.sp > self.state.bp else None
        a = self.pop()
        self.push(tensor.reduce_max(a, axis=axis))
        self.state.pc += 1

    # ==================== 内存操作实现 ====================

    def _exec_load(self, operands: List[Any]):
        """从内存地址加载数据（operands[0]=地址, operands[1]=大小）"""
        address = self._pop_operand(operands, 0)
        if address is None:
            raise VMError("LOAD 缺少地址操作数")
        size = self._pop_operand(operands, 1, 8)
        data = self.memory.read(address, size)
        self.push(data)
        self.state.pc += 1

    def _exec_store(self, operands: List[Any]):
        """存储数据到内存地址（operands[0]=地址）"""
        address = self._pop_operand(operands, 0)
        if address is None:
            raise VMError("STORE 缺少地址操作数")
        data = self.pop()
        if isinstance(data, bytes):
            self.memory.write(address, data)
        else:
            # 转换为字节
            data_bytes = str(data).encode('utf-8')
            self.memory.write(address, data_bytes)
        self.state.pc += 1

    def _exec_alloc(self, operands: List[Any]):
        size = self._pop_operand(operands, 0)
        mem_type = self._pop_operand(operands, 1, "generic")
        address = self.memory.alloc(size, mem_type)
        self.push(address)
        self.stats['memory_allocated'] += size
        self.state.pc += 1

    def _exec_free(self, operands: List[Any]):
        address = self.pop()
        if address in self.memory.alloc_table:
            size, _ = self.memory.alloc_table[address]
            self.stats['memory_freed'] += size
        self.memory.free(address)
        self.state.pc += 1

    def _exec_copy(self, operands: List[Any]):
        size = self.pop()
        dst = self.pop()
        src = self.pop()
        self.memory.copy(src, dst, size)
        self.state.pc += 1

    def _exec_move(self, operands: List[Any]):
        # MOVE 等价于 COPY + FREE
        size = self.pop()
        dst = self.pop()
        src = self.pop()
        self.memory.copy(src, dst, size)
        self.memory.free(src)
        self.state.pc += 1

    def _exec_fill(self, operands: List[Any]):
        value = self.pop()
        size = self.pop()
        address = self.pop()
        self.memory.fill(address, size, value)
        self.state.pc += 1

    def _exec_zero(self, operands: List[Any]):
        size = self.pop()
        address = self.pop()
        self.memory.fill(address, size, 0)
        self.state.pc += 1

    def _exec_load_const(self, operands: List[Any]):
        const_idx = self._pop_operand(operands, 0)
        if self.bytecode and 0 <= const_idx < len(self.bytecode.constants):
            value = self.bytecode.constants[const_idx]
            self.push(value)
        self.state.pc += 1

    def _exec_load_param(self, operands: List[Any]):
        """加载函数参数（参数寄存器 r50 起，共 14 个）"""
        param_idx = self._pop_operand(operands, 0)
        if param_idx is None:
            raise VMError("LOAD_PARAM 缺少参数索引")
        value = self.registers.get(PARAM_BASE_INDEX + param_idx)
        self.push(value)
        self.state.pc += 1

    def _exec_load_reg(self, operands: List[Any]):
        """从通用寄存器加载局部变量"""
        reg_idx = self._pop_operand(operands, 0)
        if reg_idx is None:
            raise VMError("LOAD_REG 缺少寄存器索引")
        self.push(self.registers.get(reg_idx))
        self.state.pc += 1

    def _exec_store_reg(self, operands: List[Any]):
        """存储局部变量到通用寄存器"""
        reg_idx = self._pop_operand(operands, 0)
        if reg_idx is None:
            raise VMError("STORE_REG 缺少寄存器索引")
        value = self.pop()
        self.registers.set(reg_idx, value)
        self.state.pc += 1

    # ==================== 控制操作实现 ====================

    def _exec_jump(self, operands: List[Any]):
        target = self._pop_operand(operands, 0)
        if target is None:
            raise VMError("JUMP 缺少目标地址")
        self.state.pc = target

    def _exec_jump_if(self, operands: List[Any]):
        """条件跳转：JUMP_IF [target, expected]，弹出条件，与期望值比较后决定是否跳转"""
        target = self._pop_operand(operands, 0)
        expected = self._pop_operand(operands, 1, True)
        condition = self.pop()

        if bool(condition) == bool(expected):
            self.state.pc = target
        else:
            self.state.pc += 1

    def _exec_call(self, operands: List[Any]):
        """
        函数调用：CALL [arg_count]
        从栈中弹出 arg_count 个参数和可调用对象。
        - 可调用对象为 int 时视为内部函数地址（地址级调用）
        - 可调用对象为 Python callable 时执行 FFI 调用
        """
        arg_count = self._pop_operand(operands, 0, 0)

        # 先弹出可调用对象（约定：func 在栈顶），再弹出参数
        func = self.pop()

        args = []
        for _ in range(arg_count):
            args.append(self.pop())
        args.reverse()

        if isinstance(func, int):
            # 内部调用：参数存入参数寄存器 r50 起
            for i, arg in enumerate(args):
                self.registers.set(PARAM_BASE_INDEX + i, arg)

            # 保存返回地址和基址指针
            self.call_stack.append((self.state.pc + 1, self.state.bp))

            # 设置新的基址指针
            self.state.bp = self.state.sp

            # 更新统计
            self.stats['function_calls'] += 1

            # 跳转到目标地址
            self.state.pc = func
        elif callable(func):
            # FFI：直接调用 Python 函数
            try:
                result = func(*args)
            except Exception as e:
                raise VMError(f"FFI 调用失败: {e}", instruction=f"CALL {func}")
            self.push(result)
            self.state.pc += 1
        else:
            raise VMError(
                f"CALL 目标不可调用: {type(func).__name__}",
                instruction=f"CALL {arg_count} args"
            )

    def _exec_ret(self, operands: List[Any]):
        """
        函数返回：RET
        弹出返回值，恢复调用帧；若无调用帧则停止执行。
        """
        # 弹出返回值（约定：函数恰好留下一个返回值）
        retval = self.pop()

        if not self.call_stack:
            # 没有调用栈，直接停止（返回值留在栈顶）
            self.push(retval)
            self.state.running = False
            return

        # 弹出返回地址和基址指针
        return_addr, old_bp = self.call_stack.pop()

        # 恢复基址指针
        self.state.bp = old_bp

        # 返回值入栈
        self.push(retval)

        # 跳转到返回地址
        self.state.pc = return_addr

    def _exec_nop(self, operands: List[Any]):
        self.state.pc += 1

    def _exec_halt(self, operands: List[Any]):
        self.state.running = False

    def _exec_loop(self, operands: List[Any]):
        """LOOP [end]：标记循环体开始"""
        loop_end = self._pop_operand(operands, 0)
        self.loop_stack.append((self.state.pc + 1, loop_end))
        self.state.pc += 1

    def _exec_break(self, operands: List[Any]):
        """BREAK：跳出当前循环"""
        if not self.loop_stack:
            raise VMError("BREAK 出现在循环外")
        _, loop_end = self.loop_stack.pop()
        self.state.pc = loop_end

    def _exec_continue(self, operands: List[Any]):
        """CONTINUE：跳转到当前循环开头"""
        if not self.loop_stack:
            raise VMError("CONTINUE 出现在循环外")
        loop_start, _ = self.loop_stack[-1]
        self.state.pc = loop_start

    def _exec_switch(self, operands: List[Any]):
        """SWITCH [default, case0, target0, case1, target1, ...]"""
        if len(operands) < 1:
            raise VMError("SWITCH 缺少默认目标")
        default = operands[0]
        value = self.pop()
        target = default
        for i in range(1, len(operands) - 1, 2):
            case = operands[i]
            if case == value:
                target = operands[i + 1]
                break
        self.state.pc = target

    # ==================== 比较操作实现 ====================

    def _exec_cmp_eq(self, operands: List[Any]):
        self._binary_op(tensor.cmp_eq)

    def _exec_cmp_ne(self, operands: List[Any]):
        self._binary_op(tensor.cmp_ne)

    def _exec_cmp_lt(self, operands: List[Any]):
        self._binary_op(tensor.cmp_lt)

    def _exec_cmp_le(self, operands: List[Any]):
        self._binary_op(tensor.cmp_le)

    def _exec_cmp_gt(self, operands: List[Any]):
        self._binary_op(tensor.cmp_gt)

    def _exec_cmp_ge(self, operands: List[Any]):
        self._binary_op(tensor.cmp_ge)

    def _exec_cmp_and(self, operands: List[Any]):
        b = self.pop()
        a = self.pop()
        self.push(tensor.cmp_and(a, b))
        self.state.pc += 1

    def _exec_cmp_or(self, operands: List[Any]):
        b = self.pop()
        a = self.pop()
        self.push(tensor.cmp_or(a, b))
        self.state.pc += 1

    # ==================== 激活函数实现 ====================

    def _exec_relu(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.relu(a))
        self.state.pc += 1

    def _exec_sigmoid(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.sigmoid(a))
        self.state.pc += 1

    def _exec_tanh(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.tanh(a))
        self.state.pc += 1

    def _exec_gelu(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.gelu(a))
        self.state.pc += 1

    def _exec_softmax(self, operands: List[Any]):
        a = self.pop()
        self.push(tensor.softmax(a))
        self.state.pc += 1

    # ==================== 混淆操作实现 ====================

    def _exec_obf_pi(self, operands: List[Any]):
        """PI值混淆：压入 π 的近似值（语义无关，用于迷惑静态分析）"""
        self.push(3.141592653589793)
        self.state.pc += 1

    def _exec_obf_decrypt(self, operands: List[Any]):
        """执行解密操作"""
        encrypted = self.pop()
        key = self._pop_operand(operands, 0, 0x5A)

        # 简单的XOR解密
        if isinstance(encrypted, bytes):
            decrypted = bytes(b ^ key for b in encrypted)
            self.push(decrypted)
        elif isinstance(encrypted, int):
            decrypted = encrypted ^ key
            self.push(decrypted)
        else:
            self.push(encrypted)

        self.state.pc += 1

    def _exec_obf_correct(self, operands: List[Any]):
        """执行数值校正"""
        value = self.pop()
        expected = self._pop_operand(operands, 0, 0)

        # 校正偏差
        if tensor.is_tensor(value):
            correction = expected - tensor.reduce_mean(value)
            result = tensor.add(value, correction)
        else:
            result = expected

        self.push(result)
        self.state.pc += 1

    def _exec_obf_noop(self, operands: List[Any]):
        """混淆占位指令：不改变栈与状态"""
        self.state.pc += 1

    # ==================== 类型操作实现 ====================

    def _exec_cast_int(self, operands: List[Any]):
        a = self.pop()
        self.push(int(a))
        self.state.pc += 1

    def _exec_cast_float(self, operands: List[Any]):
        a = self.pop()
        self.push(float(a))
        self.state.pc += 1

    def _exec_cast_bool(self, operands: List[Any]):
        a = self.pop()
        self.push(bool(a))
        self.state.pc += 1

    def _exec_typeof(self, operands: List[Any]):
        a = self.pop()
        self.push(type(a).__name__)
        self.state.pc += 1

    def _exec_isnull(self, operands: List[Any]):
        a = self.pop()
        self.push(a is None)
        self.state.pc += 1

    # ==================== 栈操作实现 ====================

    def _exec_push(self, operands: List[Any]):
        value = self._pop_operand(operands, 0)
        self.push(value)
        self.state.pc += 1

    def _exec_pop(self, operands: List[Any]):
        self.pop()
        self.state.pc += 1

    def _exec_dup(self, operands: List[Any]):
        value = self.peek()
        self.push(value)
        self.state.pc += 1

    def _exec_swap(self, operands: List[Any]):
        a = self.pop()
        b = self.pop()
        self.push(a)
        self.push(b)
        self.state.pc += 1

    def _exec_rot(self, operands: List[Any]):
        """ROT [n]：将栈顶第 n 个元素旋转到栈顶"""
        n = self._pop_operand(operands, 0, 3)
        if n < 1 or n > self.state.sp:
            raise VMError(f"ROT 参数无效: {n}")
        if n == 2:
            a = self.pop()
            b = self.pop()
            self.push(a)
            self.push(b)
        else:
            # 弹出 n 个元素，将第 n 个（最深的）放到栈顶
            items = [self.pop() for _ in range(n)]
            self.push(items[-1])
            for item in reversed(items[:-1]):
                self.push(item)
        self.state.pc += 1

    # ==================== 特殊操作实现 ====================

    def _exec_debug(self, operands: List[Any]):
        msg = self._pop_operand(operands, 0, "")
        print(f"[VM DEBUG] pc={self.state.pc} sp={self.state.sp} {msg}")
        self.state.pc += 1

    def _exec_profile(self, operands: List[Any]):
        self.push(dict(self.stats))
        self.state.pc += 1

    def _exec_assert(self, operands: List[Any]):
        condition = self.pop()
        msg = self._pop_operand(operands, 0, "ASSERT failed")
        if not condition:
            raise VMError(f"断言失败: {msg}", instruction="ASSERT")
        self.state.pc += 1

    def _exec_verify(self, operands: List[Any]):
        # 完整性验证指令：校验字节码校验和（简化：直接通过）
        self.state.pc += 1

    def reset(self):
        """重置虚拟机状态"""
        with self._lock:
            self.state = VMState()
            self.stack = [None] * self.stack_size
            self.memory = Memory()
            self.registers.reset()
            self.call_stack.clear()
            self.loop_stack.clear()
            self.stats = {
                'instructions_executed': 0,
                'memory_allocated': 0,
                'memory_freed': 0,
                'function_calls': 0,
            }
            logger.info("虚拟机状态已重置")

    def get_stats(self) -> Dict[str, Any]:
        """
        获取统计信息

        Returns:
            统计信息字典
        """
        return {
            **self.stats,
            'stack_usage': self.state.sp,
            'stack_capacity': self.stack_size,
            'call_stack_depth': len(self.call_stack),
            'loop_stack_depth': len(self.loop_stack),
            'memory_used': self.memory.get_allocated_size(),
        }
