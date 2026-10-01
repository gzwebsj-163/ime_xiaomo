# =====================================================================
# dmc_test.mo — hw_dmc DMC 主从链路协议层 字节码端到端 (2026-10-01)
# 编译+运行: ./xiaomo mo2kbc examples/dmc_test.mo
# 或两步:    ./xiaomo mo2kbc -o examples/dmc_test.kbc examples/dmc_test.mo
#            ./xiaomo kvm examples/dmc_test.kbc
# 链路: .mo hw_dmc() → mo2kbc OP_HW_DMC_CALL → kvm_run → hw_dmc_cmd()
#
# 单参: hw_dmc("cmd")        → OP_HW_DMC_CALL R_TMP, ci, 0
# 双参: hw_dmc("fmt", 数值)  → 数值先求值进 R_TMP, imm=R_TMP+1
#       VM 侧还原寄存器, 把数值十进制**拼到命令串尾**再调 hw_dmc_cmd
#       (所以 "frame " + 7 到 cmd 层是 "frame 7"; cmd 层按前缀匹配,
#        尾部数字被忽略 → 返回帧长 255。数字是活的, 不是编译期常量。)
#
# ⚠️ 负码约定: -1 未识别 / -2 help / -4 状态不允许 / -7 载荷超上界
# ⚠️ 头文件旧版曾承诺"双参: frame <cmd> <plen> | state <n>" 但从未实现,
#    照着写只会全拿 -1。已删除该承诺 (见 include/hw_dmc.h L227-238)。
# =====================================================================

>> print >> "=== hw_dmc DMC via kbc ==="

# ---- 单参路径 ----
void p1 : int = hw_dmc("count")
>> print >> "count    rc=" >> ${p1}

void p2 : int = hw_dmc("golden")
>> print >> "golden   rc=" >> ${p2}

void p3 : int = hw_dmc("ok")
>> print >> "ok       rc=" >> ${p3}

void p4 : int = hw_dmc("states")
>> print >> "states   rc=" >> ${p4}

void p5 : int = hw_dmc("errs")
>> print >> "errs     rc=" >> ${p5}

void p6 : int = hw_dmc("maxpay")
>> print >> "maxpay   rc=" >> ${p6}

void p7 : int = hw_dmc("maxframe")
>> print >> "maxframe rc=" >> ${p7}

void p8 : int = hw_dmc("crcvec")
>> print >> "crcvec   rc=" >> ${p8}

void p9 : int = hw_dmc("hello")
>> print >> "hello    rc=" >> ${p9}

# ---- 双参路径: 数值动态拼接 (与单参是两条不同的编译分支) ----
# 编译产物 imm=R_TMP+1 → VM 侧拼成 "add 100" 再进 hw_dmc_cmd。
# add = 100 + CMD_NUM(9) = 109; sub = 100 - 9 = 91;
# mul = 2 * MAX_PAYLOAD(249) = 498  ← 证明数值真参与了运算,
#                                     不是被静默忽略后返回常量。
void v100 : int = 100
void d1 : int = hw_dmc("add ", v100)
>> print >> "add100  rc=" >> ${d1}

void d2 : int = hw_dmc("sub ", v100)
>> print >> "sub100  rc=" >> ${d2}

void v2 : int = 2
void d3 : int = hw_dmc("mul ", v2)
>> print >> "mul2    rc=" >> ${d3}

void v7 : int = 7
void d4 : int = hw_dmc("num ", v7)
>> print >> "num7    rc=" >> ${d4}

# ---- 负向: 双参路径的诚实失败 (静默忽略数值才是灾难) ----
void vbig : int = 99999999
void d5 : int = hw_dmc("num ", vbig)
>> print >> "overflow rc=" >> ${d5}

void d6 : int = hw_dmc("frame ", v7)
>> print >> "badpfx  rc=" >> ${d6}

# ---- 负向: help 必须是 -2, 未识别必须是 -1 (两者不可混同) ----
void h1 : int = hw_dmc("help")
>> print >> "help     rc=" >> ${h1}

void u1 : int = hw_dmc("zzz")
>> print >> "zzz      rc=" >> ${u1}

>> print >> "=== done ==="
