# =====================================================================
# hwcore_test.mo — hw_core 内核 DNA 编码层 字节码端到端 (2026-09-28)
# hw_core("cmd") 是字节码内置 (同 hw_fault/input_wait, 解释器不含):
#   编译+运行: ./xiaomo mo2kbc examples/hwcore_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/hwcore_test.kbc examples/hwcore_test.mo
#              ./xiaomo kvm examples/hwcore_test.kbc
# 返回码: count=10 / ok=1 (黄金校验和匹配) / idx N=DNA编码 /
#         slot N CODE=0 / free N=0 / slots=已用槽位数 / 255=未识别
# DNA 编码: [0]DEF=0x0DEF=3567 [1]DFF=0x0DFF=3583 [2]SIN=0x00EF=239
#   [3]SID=0x00EE=238 [6]DDT=0x0080=128 [9]TTU=0x0FFD=4093
# 链路: .mo hw_core() → mo2kbc OP_HW_CORE_CALL → kvm_run → hw_core_cmd()
# =====================================================================

>> print >> "=== hw_core DNA via kbc ==="

# 1) 表项数 → 10
void c1 : int = hw_core("count")
>> print >> "count rc=" >> ${c1}

# 2) 黄金校验和自证 → 1 (不匹配则 0)
void c2 : int = hw_core("ok")
>> print >> "ok    rc=" >> ${c2}

# 3) 单参取编码: idx 0 → 3567 (0x0DEF) / idx 9 → 4093 (0x0FFD)
void c3 : int = hw_core("idx 0")
>> print >> "idx0  rc=" >> ${c3}
void c4 : int = hw_core("idx 9")
>> print >> "idx9  rc=" >> ${c4}

# 4) 双参内置: hw_core("idx ", 3) → 数值经 imm 寄存器编码动态拼接 → "idx 3" → 221
#    (拼接失败则 strtol 空串/越界 → -1 回绕 255, rc=221 即 imm 编码生效铁证)
void c5 : int = hw_core("idx ", 3)
>> print >> "idx3v rc=" >> ${c5}

# 5) 槽位生命周期: 装载(slot 3 ← 238=SID) → 0; 计数 1 → 卸载 → 0; 计数 0
void s1 : int = hw_core("slot 3 238")
>> print >> "slot  rc=" >> ${s1}
void s2 : int = hw_core("slots")
>> print >> "used1 rc=" >> ${s2}
void s3 : int = hw_core("free 3")
>> print >> "free  rc=" >> ${s3}
void s4 : int = hw_core("slots")
>> print >> "used2 rc=" >> ${s4}

# 6) 模式探测 → HOST=0 (宿主) / 其他模式见 -DHW_CORE_MODE_OVERRIDE 编译矩阵
void m1 : int = hw_core("mode")
>> print >> "mode  rc=" >> ${m1}
