# =====================================================================
# dc_test.mo — hw_dc DC 电源信号层 字节码端到端 (2026-10-01)
# hw_dc("cmd") 是字节码内置 (同 hw_core/hw_pin, 解释器不含):
#   编译+运行: ./xiaomo mo2kbc examples/dc_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/dc_test.kbc examples/dc_test.mo
#              ./xiaomo kvm examples/dc_test.kbc
# 返回码: count=8 / datacount=7 / ok=1 (黄金校验和匹配) /
#         base N=参考预值 (IN 0x059=89 / OUT 0x080=128) /
#         data N=组合帧 (DATA_5=0x0D9=217) / set N V=0 / sig N=当前值 /
#         range N=1 在窗口内|0 越界 / 255=未识别 / 254=help
# 链路: .mo hw_dc() → mo2kbc OP_HW_DC_CALL → kvm_run → hw_dc_cmd()
# =====================================================================

>> print >> "=== hw_dc DC via kbc ==="

# 1) 计数: 信号 8 路 / 组合数据帧 7 组
void c1 : int = hw_dc("count")
>> print >> "count  rc=" >> ${c1}
void c2 : int = hw_dc("datacount")
>> print >> "dcount rc=" >> ${c2}

# 2) 黄金校验和自证 → 1 (不匹配则 0)
void c3 : int = hw_dc("ok")
>> print >> "ok     rc=" >> ${c3}

# 3) 参考预值 (用户原式): base 0=IN=0x059=89 / base 1=OUT=0x0080=128
void c4 : int = hw_dc("base 0")
>> print >> "baseIN rc=" >> ${c4}
void c5 : int = hw_dc("base 1")
>> print >> "baseOT rc=" >> ${c5}

# 4) 组合数据帧: data 4 → DATA_5=0x0D9=217 (0x059|0x080)
void c6 : int = hw_dc("data 4")
>> print >> "data4  rc=" >> ${c6}

# 5) 置值/读回: set 0 42 → 0; sig 0 → 42; 默认窗口 [0,0] → range 0 越界=0
void c7 : int = hw_dc("set 0 42")
>> print >> "set    rc=" >> ${c7}
void c8 : int = hw_dc("sig 0")
>> print >> "sig0   rc=" >> ${c8}
void c9 : int = hw_dc("range 0")
>> print >> "range0 rc=" >> ${c9}

# 6) 双参内置动态拼接铁证 (imm=R_TMP+1 编码):
#    hw_dc("data ", 4) → "data 4" → 217 (拼接失败 → "data " → 0)
#    hw_dc("base ", 1) → "base 1" → 128 (拼接失败 → 0 → 89)
void d1 : int = hw_dc("data ", 4)
>> print >> "dyn4   rc=" >> ${d1}
void d2 : int = hw_dc("base ", 1)
>> print >> "dynB   rc=" >> ${d2}

# 7) 未识别/帮助 → -1/-2 回绕 (uint8 → 255/254)
void e1 : int = hw_dc("bogus")
>> print >> "bogus  rc=" >> ${e1}
void e2 : int = hw_dc("help")
>> print >> "help   rc=" >> ${e2}
