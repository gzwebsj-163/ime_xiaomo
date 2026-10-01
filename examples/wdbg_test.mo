# =====================================================================
# wdbg_test.mo — hw_wdbg 无线调试器信号层 字节码端到端 (2026-09-29)
# hw_wdbg("cmd") 是字节码内置 (同 hw_dev/hw_fault/hw_core/hw_main, 解释器不含):
#   编译+运行: ./xiaomo mo2kbc examples/wdbg_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/wdbg_test.kbc examples/wdbg_test.mo
#              ./xiaomo kvm examples/wdbg_test.kbc
# 复刻对象: 立创开源「【AI设计】AI远程调试器」(ESP32-S3 无线串口调试器)
#           串口桥接 2048B + PWM/SPI/I2C 多协议监控 + 自定义线序
# 返回码: 0=OK / 1=NOARGS / 2=BADARG / 3=IOERR / 4=STATE / 255=未识别(-1回绕)
# 链路: .mo hw_wdbg() → mo2kbc OP_HW_WDBG_CALL → kvm_run → hw_wdbg_cmd()
# =====================================================================

>> print >> "=== hw_wdbg wireless-debugger via kbc ==="

# 1) 协议数 → 4 (UART/SPI/I2C/PWM)
void c1 : int = hw_wdbg("count")
>> print >> "count rc=" >> ${c1}

# 2) 运行模式 → 0 (HOST; 其余见 -DHW_WDBG_MODE_OVERRIDE 编译矩阵)
void m1 : int = hw_wdbg("mode")
>> print >> "mode  rc=" >> ${m1}

# 3) 未打开即写 → 4 (STATE)
void e1 : int = hw_wdbg("bridge wr 01")
>> print >> "nowr  rc=" >> ${e1}

# 4) 打开串口桥: 非法档位 → 2 (BADARG); 115200 → 0
void b1 : int = hw_wdbg("bridge open 12345")
>> print >> "badb  rc=" >> ${b1}
void b2 : int = hw_wdbg("bridge open 115200")
>> print >> "open  rc=" >> ${b2}

# 5) 读 DUT 问候 4 字节 (模拟器确定性 4f 4b 0d 0a) → 0
void b3 : int = hw_wdbg("bridge rd 4")
>> print >> "rd4   rc=" >> ${b3}

# 6) 写回环 2 字节 → 0; 读回 → 0
void b4 : int = hw_wdbg("bridge wr 41 42")
>> print >> "wr    rc=" >> ${b4}
void b5 : int = hw_wdbg("bridge rd 2")
>> print >> "rd2   rc=" >> ${b5}

# 7) 波特率动态切换 → 0; 关闭 → 0
void b6 : int = hw_wdbg("bridge baud 921600")
>> print >> "baud  rc=" >> ${b6}
void b7 : int = hw_wdbg("bridge close")
>> print >> "close rc=" >> ${b7}

# 8) PWM: 空测 → 4 (STATE); 注入 1000Hz/250 → 0; 再测 → 0 (可测到)
void p1 : int = hw_wdbg("pwm meas")
>> print >> "pwm0  rc=" >> ${p1}
void p2 : int = hw_wdbg("pwm inject 1000 250")
>> print >> "inj   rc=" >> ${p2}
void p3 : int = hw_wdbg("pwm meas")
>> print >> "pwm1  rc=" >> ${p3}

# 9) 双参内置 (判别性): hw_wdbg("pwm inject 1500 ", 300) → 拼成 "...1500 300" → 0
#    拼接失败则只剩 "pwm inject 1500 " (nt=3) → 1 (NOARGS)
void p4 : int = hw_wdbg("pwm inject 1500 ", 300)
>> print >> "injv  rc=" >> ${p4}

# 10) SPI: Mode 越界 → 2; Mode 3 → 0; 异或回读 (^0xA5) → 0
void s1 : int = hw_wdbg("spi mode 4")
>> print >> "spibad rc=" >> ${s1}
void s2 : int = hw_wdbg("spi mode 3")
>> print >> "spi3  rc=" >> ${s2}
void s3 : int = hw_wdbg("spi xfer 01 02 03")
>> print >> "xfer  rc=" >> ${s3}

# 11) 双参内置 (判别性): hw_wdbg("spi mode ", 2) → "spi mode 2" → 0
#     拼接失败则 "spi mode " (nt=2) → 1 (NOARGS)
void s4 : int = hw_wdbg("spi mode ", 2)
>> print >> "spiv  rc=" >> ${s4}

# 12) I2C: 读 → 0; 写 → 0; 地址越界 → 2
void i1 : int = hw_wdbg("i2c rd 0x50 2")
>> print >> "i2crd rc=" >> ${i1}
void i2 : int = hw_wdbg("i2c wr 0x50 10 20")
>> print >> "i2cwr rc=" >> ${i2}
void i3 : int = hw_wdbg("i2c rd 200 2")
>> print >> "i2cbad rc=" >> ${i3}

# 13) 自定义线序: 数量不符 → 2; 改 UART (5,6) → 0; 复位 → 0
void n1 : int = hw_wdbg("pin set uart 5")
>> print >> "pinbad rc=" >> ${n1}
void n2 : int = hw_wdbg("pin set uart 5 6")
>> print >> "pinset rc=" >> ${n2}

# 14) 双参内置 (判别性): hw_wdbg("pin set uart 7 ", 8) → "pin set uart 7 8" → 0
#     拼接失败则 cnt=1 ≠ 默认 2 → 2 (BADARG)
void n3 : int = hw_wdbg("pin set uart 7 ", 8)
>> print >> "pinsetv rc=" >> ${n3}
void n4 : int = hw_wdbg("pin reset")
>> print >> "pinrst rc=" >> ${n4}

# 15) 汇总 / 帮助 / 未识别
void g1 : int = hw_wdbg("status")
>> print >> "status rc=" >> ${g1}
void g2 : int = hw_wdbg("help")
>> print >> "help  rc=" >> ${g2}
void g3 : int = hw_wdbg("bogus")
>> print >> "bogus rc=" >> ${g3}
