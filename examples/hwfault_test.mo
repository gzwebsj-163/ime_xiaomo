# =====================================================================
# hwfault_test.mo — hw_fault TFT 模组故障诊断 字节码端到端 (2026-09-24)
# hw_fault("cmd") 是字节码内置 (同 hw_dev/input_wait, 解释器不含):
#   编译+运行: ./xiaomo mo2kbc examples/hwfault_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/hwfault_test.kbc examples/hwfault_test.mo
#              ./xiaomo kvm examples/hwfault_test.kbc
# 返回码: scan=故障数 (0=健康) / pin N=故障码 / inject 0=成功 / 255=未识别
# 故障码: 0x01 GND_OPEN 0x02 VDD_LOW 0x03 VDD_HIGH 0x04 LED_OPEN
#         0x05 LED_SHORT 0x06 SPI_NOACK 0x07 CTRL_STUCK 0xFE UNPLUGGED
# 链路: .mo hw_fault() → mo2kbc OP_HW_FAULT_CALL → kvm_run → hw_fault_cmd()
# =====================================================================

>> print >> "=== hw_fault diag via kbc ==="

# 1) 健康态 scan → 0 (模拟器干净起步, kvm_run 上电自动 hw_fault_init)
void f1 : int = hw_fault("scan")
>> print >> "scan0 rc=" >> ${f1}

# 2) 注入 P7=VDD_LOW (0x02) → 成功 0 → scan → 1
void i1 : int = hw_fault("inject 7 2")
>> print >> "inject rc=" >> ${i1}
void f2 : int = hw_fault("scan")
>> print >> "scan1 rc=" >> ${f2}

# 3) 单脚诊断 P7 → 2 (VDD_LOW)
void f3 : int = hw_fault("pin 7")
>> print >> "pin7  rc=" >> ${f3}

# 4) 双参内置: hw_fault("pin ", 7) → 数值经 imm 寄存器编码动态拼接 → "pin 7" → 2
#    (拼接失败则 strtol 读到空串 → -1 回绕 255, rc=2 即 imm 编码生效铁证)
void f4 : int = hw_fault("pin ", 7)
>> print >> "pin7v rc=" >> ${f4}

# 5) 清注入 → 0 → scan → 0 (回到健康)
void i2 : int = hw_fault("clear")
>> print >> "clear rc=" >> ${i2}
void f5 : int = hw_fault("scan")
>> print >> "scan2 rc=" >> ${f5}
