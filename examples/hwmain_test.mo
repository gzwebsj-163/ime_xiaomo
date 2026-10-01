# =====================================================================
# hwmain_test.mo — hw_main 家族总调度 字节码端到端 (2026-09-28)
# hw_main("cmd") 是字节码内置 (同 hw_core/hw_fault, 解释器不含):
#   编译+运行: ./xiaomo mo2kbc examples/hwmain_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/hwmain_test.kbc examples/hwmain_test.mo
#              ./xiaomo kvm examples/hwmain_test.kbc
# 返回码: count=8 / ok=1 / sum=1604573786 (0x5FAD755A 黄金) /
#         mode=0 (HOST) / idx N=类ID / find ID=表序 /
#         probe ID=0 (健康) / 255=未识别 (uint8 回绕)
# 类 ID (用户定版): ASR=0x58 CORE=0x98 DEV=0x89 DIRECT=0x1EF
#                   FAULT=0x7DE HEX=0xDEF OEM=0xFFD TOKEN=0xEFD
# 链路: .mo hw_main() → mo2kbc OP_HW_MAIN_CALL → kvm_run → hw_main_cmd()
# =====================================================================

>> print >> "=== hw_main family bus via kbc ==="

# 1) 家族类计数 → 8
void c1 : int = hw_main("count")
>> print >> "count rc=" >> ${c1}

# 2) 黄金校验和自证 → 1 (不匹配则 0)
void c2 : int = hw_main("ok")
>> print >> "ok    rc=" >> ${c2}

# 3) 黄金值免断言读出 → 1604573786 (0x5FAD755A)
void c3 : int = hw_main("sum")
>> print >> "sum   rc=" >> ${c3}

# 4) 模式探测 → 0 (HOST) / 其他模式见 -DHW_MAIN_MODE_OVERRIDE 编译矩阵
void m1 : int = hw_main("mode")
>> print >> "mode  rc=" >> ${m1}

# 5) 表序 → 类 ID: idx 3 → 495 (0x1EF DIRECT_CLASS)
void c4 : int = hw_main("idx 3")
>> print >> "idx3  rc=" >> ${c4}

# 6) 类 ID → 表序: find 152 (0x98) → 1 (CORE)
void c5 : int = hw_main("find 152")
>> print >> "find  rc=" >> ${c5}

# 7) 双参内置: hw_main("probe ", 152) → 数值经 imm 寄存器编码动态拼接
#    → "probe 152" → CORE 健康探针 → 0
#    (拼接失败则 find 152 找不到 → -1 回绕 255, rc=0 即 imm 编码生效铁证)
void c6 : int = hw_main("probe ", 152)
>> print >> "probe rc=" >> ${c6}

# 8) 双参再证: hw_main("idx ", 6) → 拼接 "idx 6" → OEM_CLASS 0xFFD = 4093
void c7 : int = hw_main("idx ", 6)
>> print >> "idx6v rc=" >> ${c7}
