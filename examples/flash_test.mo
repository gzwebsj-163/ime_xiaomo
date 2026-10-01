# =====================================================================
# flash_test.mo — hw_flash ESP32 ROM 下载协议烧录层 字节码端到端 (2026-09-30)
#
# hw_flash("cmd") 是字节码内置 (同 hw_dev/hw_wdbg/hw_core/hw_main, 解释器不含):
#   编译+运行: ./xiaomo mo2kbc examples/flash_test.mo
#   或两步:    ./xiaomo mo2kbc -o examples/flash_test.kbc examples/flash_test.mo
#              ./xiaomo kvm examples/flash_test.kbc
#
# 意义: 把「给 ESP32 烧固件」从外部工具 (esptool/Python) 收进 xiaomo 架构本身,
#       纯 C 全跨式六模式, 烧录可由 .mo 字节码驱动。
# 返回码: 0=OK / 1=NOARGS / 2=BADARG / 3=IOERR / 4=PROTO / 5=CHECKSUM /
#         6=BADSIZE / 255=未识别(-1回绕) / 254=help(-2回绕)
# 链路: .mo hw_flash() → mo2kbc OP_HW_FLASH_CALL → kvm_run → hw_flash_cmd()
# =====================================================================

>> print >> "=== hw_flash ESP32 ROM flasher via kbc ==="

# 1) 运行模式 → 0 (HOST; 其余见 -DHW_FLASH_MODE_OVERRIDE 编译矩阵)
void m1 : int = hw_flash("mode")
>> print >> "mode  rc=" >> ${m1}

# 2) SYNC 握手 → 0
void y1 : int = hw_flash("sync")
>> print >> "sync  rc=" >> ${y1}

# 3) 芯片探测 (magic 0x60001F10 → 9 = ESP32-S3) → 0
void k1 : int = hw_flash("chip")
>> print >> "chip  rc=" >> ${k1}

# 4) 端到端烧录 4096B (4×1024B 块) → 0 (设备 MD5 == 本地 MD5)
void r1 : int = hw_flash("run 4096")
>> print >> "run4k rc=" >> ${r1}

# 5) 双参内置 (判别性): hw_flash("run ", 2048) → "run 2048" → 0
#    拼接失败则只剩 "run " (空 n) → 回退默认 4096, 仍 0; 故另验 2048 的块数
void r2 : int = hw_flash("run ", 2048)
>> print >> "run2k rc=" >> ${r2}

# 6) 尺寸越界 (> 模型容量 32768) → 6 (BADSIZE)
void r3 : int = hw_flash("run 99999")
>> print >> "runsz rc=" >> ${r3}

# 7) 本地镜像 MD5 对黄金值 → 0
void d1 : int = hw_flash("md5")
>> print >> "md5   rc=" >> ${d1}

# 8) SLIP 转义黄金向量回环 → 0
void l1 : int = hw_flash("slip")
>> print >> "slip  rc=" >> ${l1}

# 9) 统计快照 → 0
void t1 : int = hw_flash("stat")
>> print >> "stat  rc=" >> ${t1}

# 10) 汇总 / 帮助 / 未识别
void g1 : int = hw_flash("card")
>> print >> "card  rc=" >> ${g1}
void g2 : int = hw_flash("help")
>> print >> "help  rc=" >> ${g2}
void g3 : int = hw_flash("bogus")
>> print >> "bogus rc=" >> ${g3}
