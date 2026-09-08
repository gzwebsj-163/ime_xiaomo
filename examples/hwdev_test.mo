# =====================================================================
# hwdev_test.mo — hw_dev 命令分发 字节码端到端 (2026-09-07)
# hw_dev("cmd") 是字节码内置 (同 input_wait/llm_query, 解释器不含):
#   编译+运行: ./xiaomo mo2kbc examples/hwdev_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/hwdev_test.kbc examples/hwdev_test.mo
#              ./xiaomo kvm examples/hwdev_test.kbc
# 返回码: 0x00=命令命中执行, 255 (0xFF)=未命中
# 链路: .mo hw_dev() → mo2kbc OP_HW_DEV_CALL → kvm_run → hw_dev_dispatch()
# ⚠️ 静态命令表带 "\n\r " 尾缀 (串口行协议), .mo 字符串不转义,
#    本文件 echo/self 行内嵌真实 CR/LF 控制字节 — 手改时请保留!
# =====================================================================

>> print >> "=== hw_dev dispatch via kbc ==="

# 1) 静态命令 echo 命中 → 0
void r1 : int = hw_dev("echo
 hello")
>> print >> "echo  rc=" >> ${r1}

# 2) 静态命令 self 命中 → 0
void r2 : int = hw_dev("self
 ")
>> print >> "self  rc=" >> ${r2}

# 3) 未注册命令 → 255 (0xFF)
void r3 : int = hw_dev("bogus
 ")
>> print >> "bogus rc=" >> ${r3}

# 4) 双参内置: 数值经 imm 寄存器编码, 十进制动态拼接进命令串 (2026-09-07)
#    i2c read 99 → h=99 无效 → 0x04; 若拼接失败则 params='read' 缺参 → 0x01
#    (0x04 vs 0x01 正好区分 imm 编码拼接是否生效)
void p1 : int = hw_dev("i2c
 read ", 99)
>> print >> "i2c99 rc=" >> ${p1}

# 5) echo 双参: params = 'n=42' → 0x00
void p2 : int = hw_dev("echo
 n=", 42)
>> print >> "echo42 rc=" >> ${p2}
