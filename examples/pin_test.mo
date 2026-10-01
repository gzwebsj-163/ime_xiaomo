# =====================================================================
# pin_test.mo — hw_pin 引脚档案/双模驱动/编程电压层 字节码端到端 (2026-09-30)
#
# hw_pin("cmd") 是字节码内置 (同 hw_dev/hw_wdbg/hw_core/hw_main/hw_flash):
#   编译+运行: ./xiaomo mo2kbc examples/pin_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/pin_test.kbc examples/pin_test.mo
#              ./xiaomo kvm examples/pin_test.kbc
#
# 意义: 把「GPIO 级烧录」收进 xiaomo 架构本身 —— 换目标芯片 = 换一条引脚档案。
#       本示例走 bit-bang 路径驱动 W25Q 器件模型, 证明
#       档案(引脚映射) → 时序(CPOL/CPHA bit-bang) → 协议(0x9F/0x03/0x02/0x20)
#       整条链路可由字节码驱动。
# 返回码: 0=OK / 1=NOARGS / 2=BADARG / 3=IOERR / 4=NOFLASH /
#         5=VERIFY / 6=NODEV / 7=RANGE / 255=未识别(-1回绕) / 254=help(-2回绕)
# 链路: .mo hw_pin() → mo2kbc OP_HW_PIN_CALL → kvm_run → hw_pin_cmd()
# =====================================================================

>> print >> "=== hw_pin pin-profile / dual-mode driver / VPP via kbc ==="

# 1) 运行模式 → 0 (HOST; 其余见 -DHW_PIN_MODE_OVERRIDE 编译矩阵)
void m1 : int = hw_pin("mode")
>> print >> "mode   rc=" >> ${m1}

# 2) 档案表枚举 → 0
void f1 : int = hw_pin("profiles")
>> print >> "profs  rc=" >> ${f1}

# 3) 换档案: esp32-9p → w25q (换目标芯片 = 换档案, 协议代码不动) → 0
void l1 : int = hw_pin("load w25q")
>> print >> "load   rc=" >> ${l1}

# 4) bit-bang RDID: 0x9F → EF 40 18 (W25Q128) → 0
void i1 : int = hw_pin("id")
>> print >> "id     rc=" >> ${i1}

# 5) 扇擦→页编程→回读校验 全链 (256B) → 0
void w1 : int = hw_pin("wtest")
>> print >> "wtest  rc=" >> ${w1}

# 6) 编程电压档位 → 0 (设置 2 档 = 12V)
void v1 : int = hw_pin("vpp 2")
>> print >> "vpp2   rc=" >> ${v1}

# 7) 电压回读 (模拟器固定 5V=5000mV) → 0
void v2 : int = hw_pin("vppread")
>> print >> "vpprd  rc=" >> ${v2}

# 8) 双参内置 (判别性): hw_pin("vpp ", 3) → "vpp 3" → 12500 mV
#    拼接失败则只剩 "vpp " (空参) → 落 0 档 OFF, 不会出现 12500 mV
void d1 : int = hw_pin("vpp ", 3)
>> print >> "vpp3   rc=" >> ${d1}

# 9) 扇区擦除 (独立命令) → 0
void e1 : int = hw_pin("erase 0")
>> print >> "erase  rc=" >> ${e1}

# 10) 统计快照 → 0
void s1 : int = hw_pin("stat")
>> print >> "stat   rc=" >> ${s1}

# 11) 汇总 / 帮助 / 未识别
void g1 : int = hw_pin("card")
>> print >> "card   rc=" >> ${g1}
void g2 : int = hw_pin("help")
>> print >> "help   rc=" >> ${g2}
void g3 : int = hw_pin("bogus")
>> print >> "bogus  rc=" >> ${g3}

# ---- L3 协议插件 #2: UART ISP (STM32 AN3155) ----
# 12) ISP 命令表黄金 (11 条命令 FNV-1a-32) → 0
void p1 : int = hw_pin("isp chk")
>> print >> "ispchk rc=" >> ${p1}

# 13) ISP 握手 0x7F + GetVersion(0x31) + GetID(0x0410) → 0
void p2 : int = hw_pin("isp info")
>> print >> "ispinf rc=" >> ${p2}

# 14) ISP 烧录全链: 全片擦 → 写 256B @0x08000000 → 读回 → 逐字节校验 → 0
void p3 : int = hw_pin("isp wtest")
>> print >> "ispwrt rc=" >> ${p3}
