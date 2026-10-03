# =====================================================================
# usbpd_test.mo — hw_usbpd USB-C 快充协议层 字节码端到端 (2026-10-03)
# 编译+运行: ./xiaomo mo2kbc examples/usbpd_test.mo
# 或两步:    ./xiaomo mo2kbc -o examples/usbpd_test.kbc examples/usbpd_test.mo
#            ./xiaomo kvm examples/usbpd_test.kbc
# 链路: .mo hw_usbpd() → mo2kbc OP_HW_USBPD_CALL → kvm_run → hw_usbpd_cmd()
#
# 单参: hw_usbpd("cmd")         → OP_HW_USBPD_CALL R_TMP, ci, 0
# 双参: hw_usbpd("fmt", 数值)   → 数值先求值进 R_TMP, imm=R_TMP+1
#       VM 侧把数值十进制【直接追加到命令串尾】(不加分隔符), 所以
#       格式串必须【自带尾空格】, 否则拼成 "at2700" 匹配不上。
#       ⚠️ 家族约定: dmc_test.mo 里 hw_dmc("add ", v100) 同款。
#
# ⚠️ 码位约定 (2026-10-03 实测, 七个负码两两互斥):
#    -1 ERR_PARAM  参数非法     -2 ERR_NODEV  BSP 未装该动作
#    -3 ERR_HW      真机失败     -4 ERR_UNSUPPORT 本层不实现
#    -5 ERR_FLOAT   悬空拾波     -6 HELP        要用法
#    -7 ERR_UNKNOWN 命令未识别
# =====================================================================

>> print >> "=== hw_usbpd USB-C PD via kbc ==="

# ---- 单参: 标量档案 (非布尔的可证伪整数) ----
void s1 : int = hw_usbpd("rows")
>> print >> "rows      rc=" >> ${s1}

void s2 : int = hw_usbpd("tol")
>> print >> "tol       rc=" >> ${s2}

void s3 : int = hw_usbpd("golden")
>> print >> "golden    rc=" >> ${s3}

void s4 : int = hw_usbpd("ratio")
>> print >> "ratio     rc=" >> ${s4}

void s5 : int = hw_usbpd("quiet")
>> print >> "quiet     rc=" >> ${s5}

void s6 : int = hw_usbpd("rail")
>> print >> "rail      rc=" >> ${s6}

# ---- 单参: 未装 BSP 时的诚实失败 ----
# 🕳️ 本示例跑在未装 BSP 的宿主上, sample 必须【诚实失败】ERR_NODEV(-2),
#   绝不能返回 0mV 再判一个假协议档 —— 那是最典型的假成功。
void n1 : int = hw_usbpd("sample")
>> print >> "sample    rc=" >> ${n1}

void n2 : int = hw_usbpd("dp")
>> print >> "dp        rc=" >> ${n2}

void n3 : int = hw_usbpd("pdpoll")
>> print >> "pdpoll    rc=" >> ${n3}

# ---- 双参: at <mv> 无硬件查判据 (承重口, 不是死代码) ----
# 🔑 四档取值互不相同 (9/11/1/12), 任何一档塌成别的档, 打印值即暴露。
void v2700 : int = 2700
void d1 : int = hw_usbpd("at ", v2700)
>> print >> "at_2700   rc=" >> ${d1}

void v1200 : int = 1200
void d2 : int = hw_usbpd("at ", v1200)
>> print >> "at_1200   rc=" >> ${d2}

void v0 : int = 0
void d3 : int = hw_usbpd("at ", v0)
>> print >> "at_0      rc=" >> ${d3}

void v2000 : int = 2000
void d4 : int = hw_usbpd("at ", v2000)
>> print >> "at_2000   rc=" >> ${d4}

# ---- 负向: 双参路径的三个守卫 (静默放行才是灾难) ----
# 上界 = rail*ratio/1000 = 2850*2960/1000 = 8436mV
void vover : int = 8437
void g1 : int = hw_usbpd("at ", vover)
>> print >> "at_8437   rc=" >> ${g1}

void vneg : int = -5
void g2 : int = hw_usbpd("at ", vneg)
>> print >> "at_-5     rc=" >> ${g2}

# ---- 负向: help 与未识别是两个码, 不可混同 ----
void h1 : int = hw_usbpd("help")
>> print >> "help      rc=" >> ${h1}

void u1 : int = hw_usbpd("zzz")
>> print >> "zzz       rc=" >> ${u1}

# ---- 边界: 正好等于上界必须放行 (守卫写成 >= 而非 > 仍要能抓到) ----
void vcap : int = 8436
void b1 : int = hw_usbpd("at ", vcap)
>> print >> "at_8436   rc=" >> ${b1}

>> print >> "=== done ==="
