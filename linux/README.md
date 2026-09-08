# linux/ — 嵌入式 Linux 内核核心（TinyEMU）

> xiaomo 体系里的「真 Linux 内核」模块 —— 基于 Fabrice Bellard 的 **TinyEMU**
> 嵌入式 RISC-V 全系统模拟器，只留纯内核模拟核心（无 SDL 图形、无网络 FS），
> **原生跑通真 Linux 4.15 内核**，含 busybox shell。

## 为什么是 TinyEMU

- **~100KB 级别小型模拟器**，纯 C 实现、零外部依赖、易嵌入（`temu_init/temu_exec` API）
- 是 Bellard（QEMU 作者）官方认可的可嵌入式 Linux 内核核心
- 本机已实测：**真 Linux 4.15 riscv64 内核放进去 boot → 出 busybox shell → 跑命令 → 退出**
- 对应 xiaomo primitive 阶段路线的「下一站」：中断→MMU→流水线→总线/DMA→FreeRTOS→**真 Linux**

## 目录结构

```
linux/
├── linux_embed_driver.py        # 驱动脚本：等提示符→发命令→收输出→退出（独立 temu）
├── embed/                       # ★ 阶段六：嵌入式 C 模块封装（lib 级）
│   ├── linux_embed.h            #   API：linux_init / linux_exec / linux_read_output / linux_end
│   ├── linux_embed.c            #   环形缓冲控制台 + 执行循环 + 块设备移植
│   ├── linux_embed_demo.c       #   验证驱动（8 命令全 PASS）
│   ├── Makefile                 #   统一宏重编 TinyEMU 核心（无 x86/SLIRP/SDL/FS_NET）
│   └── build/                   #   重编译的核心 .o
├── tinyemu-2019-12-21/          # TinyEMU 源码（已做 macOS 适配编译）
│   ├── temu                     # 编译产物（432KB 可执行）
│   └── ...                      # riscv_cpu / riscv_machine / softfp 等核心
└── diskimage-linux-riscv-2018-09-23/
    ├── bbl64.bin                # BBL 引导器（拉起内核，53KB）
    ├── kernel-riscv64.bin       # Linux 内核（3.9MB, v4.15 riscv64）
    ├── root-riscv64.bin         # 根文件系统（ext2, 4MB, busybox）
    ├── root-riscv64.cfg         # VM 配置（128MB 内存, console=hvc0, 带网络）
    └── root-riscv64-embed.cfg   # 嵌入版配置（去 eth0，零网络依赖）
```

## 阶段六：嵌入式 C 模块封装（embed/）

把 TinyEMU 从「独立二进制」升级为「C 模块」，与 primitive/ 中
interrupt/mmu/pipeline/bus/freertos 的模块范式一致，可被任意宿主程序链接驱动。

```c
#include "linux_embed.h"

LinuxVM *vm = linux_init("root-riscv64-embed.cfg");   // 创建并启动内核
linux_exec(vm, "", "~ #", 120000);                     // 等 busybox 提示符（boot 完成）
linux_exec(vm, "uname -a", "Linux", 15000);            // 注入命令 + 等预期输出
char buf[65536]; int n = linux_read_output(vm, buf, sizeof(buf));  // 取回 guest 输出
linux_end(vm);                                         // 关闭
```

**关键设计**：
- **自研环形缓冲控制台**：不再用 `temu.c` 的 `console_init`（依赖 termios 终端 + select 事件循环），
  实现为纯内存 `CharacterDevice`——guest 的 virtio-console 输出落到 4MB 环形缓冲，
  输入从命令队列注入。任意宿主程序都能驱动内核。
- **块设备移植**：`temu.c` 里的 `block_device_init` / `bf_*` 是 static 函数，已移植为 embed 私有实现。
- **统一宏重编核心**：TinyEMU 核心 .o 在 `build/` 用统一宏重编
  （`-DCONFIG_RISCV_MAX_XLEN=128`，无 CONFIG_X86EMU/SLIRP/SDL/FS_NET），
  规避复用旧 .o 带进 x86 符号导致的链接失败（`_pc_machine_class` undefined）。

**验证 demo**（`./linux_embed_demo`）：boot → 8 命令注入驱动 → 全 PASS：

```
[boot] ✅ shell 就绪
$ uname -a        → Linux 4.15.0-00049-ga3b1e7a-dirty riscv64 GNU/Linux  [PASS]
$ id              → uid=0(root) gid=0(root)               [PASS]
$ cat /proc/version → Linux version 4.15 ... gcc 7.3.1    [PASS]
$ cat /proc/cpuinfo → hart:0 isa:rv64acdfimsu mmu:sv48    [PASS]
$ free            → Mem: 57264                            [PASS]
$ ls /            → bin dev etc lib mnt proc root sbin sys tmp usr var  [PASS]
$ echo '2+2=4'|bc → 4                                     [PASS]
$ echo LINUX-EMBED-OK → OK                                [PASS]
=== 结果汇总: PASS=8 FAIL=0 ===
```

## 使用

```bash
# 一键驱动（等 boot → 跑 uname/id/free 等 → 退出）
python3 linux_embed_driver.py

# ★ 嵌入式 C 模块验证（阶段六，环形缓冲控制台驱动）
cd embed && make && ./linux_embed_demo

# 手动交互
cd linux/diskimage-linux-riscv-2018-09-23
../tinyemu-2019-12-21/temu -ctrlc root-riscv64.cfg
# ✓ Ctrl-A x 退出
```

## 实测输出（真机验证）

```
~ # uname -a
Linux localhost 4.15.0-00049-ga3b1e7a-dirty #10 ... riscv64 GNU/Linux
~ # id
uid=0(root) gid=0(root)
~ # cat /proc/cpuinfo
hart : 0   isa : rv64acdfimsu   mmu : sv48
~ # free
Mem: 57264 total
~ # ls /
bin dev etc lib lost+found mnt proc root sbin sys tmp usr var
~ # echo LINUX-EMBED-OK
LINUX-EMBED-OK
```

## macOS 构建适配（重要，踩坑记录）

TinyEMU 原本面向 Linux，macOS（Intel, Big Sur, clang 13）编译需 6 处适配：

| # | 问题 | 修复 |
|---|------|------|
| 1 | `<byteswap.h>` 不存在 | 自写 `byteswap.h` shim → `__builtin_bswap16/32/64` |
| 2 | `<linux/if_tun.h>` 不存在 | 写 stub 只补 `IFF_TAP/IFF_NO_PI/TUNSETIFF` 常量（macOS 已有 ifreq） |
| 3 | `<sys/statfs.h>` 不存在 | 换 `<sys/mount.h>` |
| 4 | `<sys/sysmacros.h>` Linux 专属 | `__APPLE__` 守卫 |
| 5 | `st_atim/st_mtim/st_ctim` 字段名 | `#define st_atim st_atimespec` 等兼容宏 |
| 6 | `-lrt` 链接失败 | 移除（macOS 实时库在 libc） |

另：Makefile 关闭 `CONFIG_FS_NET` 与 `CONFIG_SDL`（无需 libcurl/openssl/SDL），`CFLAGS += -I.`。

## 已知限制 / 待办

- 128MB 内存内的精简 busybox 系统（可 `-m` 调大）
- 无图形/网络（按嵌入式最小核心设计，需要时再开 CONFIG_SDL/FS_NET）
- 内核 v4.15（2018 版），若需新版内核需自行编译 riscv64 kernel（patches 在官方包）
- ✅ 已封装成 xiaomo 的 lib 级模块（`embed/linux_embed.h`，`linux_init/linux_exec` 接口），
  与 primitive/ 中 interrupt/mmu/pipeline/bus/freertos 并列，为「阶段六」
- 待做：四端下沉（把 embed 模块分发到 kickpi/server/phone 验证跨端），或作为
  xiaomo 主程序的可选链接模块真正并入 .mo 字节码核
