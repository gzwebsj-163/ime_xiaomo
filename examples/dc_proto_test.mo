# =====================================================================
# dc_proto_test.mo — hw_dc DCPP 供电协议(帧层) 字节码端到端 (2026-10-01)
# hw_dc("proto*") 是字节码内置 (同 hw_dc 信号层, 解释器不含):
#   编译+运行: ./xiaomo mo2kbc examples/dc_proto_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/dc_proto_test.kbc examples/dc_proto_test.mo
#              ./xiaomo kvm examples/dc_proto_test.kbc
# 协议: START(80 EF 02) │ VER │ SIDE │ CMD │ LEN │ PAYLOAD │ CKSUM8 │ END(ED FF 0D)
# 返回码: 9=命令数 / ok=1(协议黄金) / build N=帧长 / cmd N=应答字节数|负错误码
#         frame N=帧长(11+len) / opened=1|0 / stat=状态机编号
#         (负错误码: -1 结构/未识别  -4 未知命令  -5 长度  -6 参数  -7 会话未开)
# 链路: .mo hw_dc() → mo2kbc OP_HW_DC_CALL → kvm_run → hw_dc_cmd()
# =====================================================================

>> print >> "=== hw_dc DCPP proto via kbc ==="

# 1) 协议版本 / 命令总数 / 黄金校验和自证
void p1 : int = hw_dc("protover")
>> print >> "ver    rc=" >> ${p1}
void p2 : int = hw_dc("protocount")
>> print >> "count  rc=" >> ${p2}
void p3 : int = hw_dc("protook")
>> print >> "ok     rc=" >> ${p3}

# 2) 帧长公式 = 11 + 负载长 (0→11 / 16→27 / 17→-1 回绕 255)
void f1 : int = hw_dc("protoframe 0")
>> print >> "fr0    rc=" >> ${f1}
void f2 : int = hw_dc("protoframe 16")
>> print >> "fr16   rc=" >> ${f2}
void f3 : int = hw_dc("protoframe 17")
>> print >> "fr17   rc=" >> ${f3}

# 3) 打帧: PING(1)→11 / GET(3)→12 / 未知(0)→-1
void b1 : int = hw_dc("protobuild 1")
>> print >> "bPING  rc=" >> ${b1}
void b2 : int = hw_dc("protobuild 3")
>> print >> "bGET   rc=" >> ${b2}

# 4) 会话门禁: 复位 → 未开会话时 GET → ERR_CLOSED(-7 → 249)
void r1 : int = hw_dc("protoreset")
>> print >> "reset  rc=" >> ${r1}
void g0 : int = hw_dc("protocmd 3")
>> print >> "GETx   rc=" >> ${g0}

# 5) OPEN(8) → 会话开启; protoopened → 1
void o1 : int = hw_dc("protocmd 8")
>> print >> "OPEN   rc=" >> ${o1}
void o2 : int = hw_dc("protoopened")
>> print >> "opened rc=" >> ${o2}

# 6) 会话内合法命令: RANGE(6) / GET(3) / BASE(5) / DATA(7) / PING(1)
void c1 : int = hw_dc("protocmd 6")
>> print >> "RANGE  rc=" >> ${c1}
void c2 : int = hw_dc("protocmd 3")
>> print >> "GET    rc=" >> ${c2}
void c3 : int = hw_dc("protocmd 5")
>> print >> "BASE   rc=" >> ${c3}
void c4 : int = hw_dc("protocmd 7")
>> print >> "DATA   rc=" >> ${c4}

# 7) 上一条应答值 (DATA idx=4 → DATA_5=0x0D9=217)
void v1 : int = hw_dc("protovalue")
>> print >> "value  rc=" >> ${v1}

# 8) PING → 应答 1B (值 0x50=80), 再取上一条应答值
void c5 : int = hw_dc("protocmd 1")
>> print >> "PING   rc=" >> ${c5}
void v2 : int = hw_dc("protovalue")
>> print >> "pong   rc=" >> ${v2}

# 9) CLOSE(9) → 会话关断; 关断后再命令 → ERR_CLOSED(-7)
void z1 : int = hw_dc("protocmd 9")
>> print >> "CLOSE  rc=" >> ${z1}
void z2 : int = hw_dc("protoopened")
>> print >> "opened rc=" >> ${z2}
void z3 : int = hw_dc("protocmd 3")
>> print >> "GETx2  rc=" >> ${z3}
void z4 : int = hw_dc("protostat")
>> print >> "stat   rc=" >> ${z4}

# 10) 双参内置动态拼接 (imm=R_TMP+1): hw_dc("protobuild ", 1) → 11
void d1 : int = hw_dc("protobuild ", 1)
>> print >> "dynB   rc=" >> ${d1}
