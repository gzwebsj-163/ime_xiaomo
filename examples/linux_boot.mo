# xiaomo 阶段六收官: .mo 源码直驱真 Linux 内核
# 用法: ./xiaomo-linux mo2kbc examples/linux_boot.mo
#
# 链路: .mo 源码 → mo2kbc 编译(LINUX_* opcode) → KVM 字节码核
#       → linux_embed 弱符号桥 → TinyEMU 真内核 (Linux 4.15 riscv64)
# 注意: 若用不带 TinyEMU 的主构建 ./xiaomo 跑, LINUX opcode 走弱符号
#       空实现 → 直接跳过, 不崩。

>> print >> "=== .mo -> KBC -> 真 Linux 内核 ==="

# 1. 启动 Linux VM (cfg 路径入常量池, 返回句柄)
void h : int = linux_init("linux/diskimage-linux-riscv-2018-09-23/root-riscv64-embed.cfg")

# 2. 等 busybox 提示符 (空命令 = 只等预期输出 "~ #", boot 完成)
linux_exec(${h}, "", "~ #")

# 3. 注入命令 + 等预期输出 + 读取 guest 输出
linux_exec(${h}, "uname -a", "GNU/Linux")
linux_read(${h})

linux_exec(${h}, "id", "root")
linux_read(${h})

linux_exec(${h}, "cat /proc/cpuinfo", "mmu:sv48")
linux_read(${h})

linux_exec(${h}, "free", "Mem")
linux_read(${h})

# ⚠️ 注意: 嵌入 rootfs 的 busybox 没有 bc 命令!
#     旧用例 echo '2+2=4' | bc 是假通过: expect="4" 匹配的是命令回显里的 '4',
#     bc 从未真正运行 (sh: bc: not found)。
# 真计算改用 busybox ash 内建算术 $((...)), 回显 echo $((7*6)) 不含 "42",
#     只有真实算出的 42 才会命中 — 唯一对应真实计算输出。
linux_exec(${h}, "echo $((7*6))", "42")
linux_read(${h})

# 4. 销毁 VM
linux_end(${h})

>> print >> "=== 全部完成 ==="
