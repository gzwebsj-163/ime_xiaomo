"""Layer 5: Private IR Virtualization — 精简导入(仅运行时执行所需)。
避开 compiler.c / 全量导出, 供 ESP32 VM 桥接使用。
"""
from ...core.exceptions import VMError
from .opcode import (OpCode, OpCodeCategory, OPCODE_PROPERTIES,
                     is_control_flow, is_memory_operation, has_side_effect,
                     get_operand_count, get_result_count)
from .bytecode import (Instruction, ByteCode, ByteReader)
from .interpreter import (VMState, Memory, RegisterFile, VMInterpreter)

__all__ = [
    'VMError', 'OpCode', 'OpCodeCategory', 'OPCODE_PROPERTIES',
    'Instruction', 'ByteCode', 'ByteReader',
    'VMState', 'Memory', 'RegisterFile', 'VMInterpreter',
]
