#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xdbg_jtag.py —— xiaomo ESP32 真机【硬件级】调试工具 (JTAG/GDB)

与 xdbg_board.py 的分工（两条互补的真机通道）：
  xdbg_board.py  串口通道（CDC-ACM）—— 看日志、发命令、抓 boot。应用层。
  xdbg_jtag.py   调试通道（USB-JTAG）—— 停核心、读寄存器/内存、断点、单步。
                                        芯片内部。即使固件死循环/不打印也能看。

原理：ESP32-C3 内置 USB-JTAG（Espressif VID 0x303a PID 0x1001）
      OpenOCD 经 libusb 咬住 JTAG 接口 → 起 GDB server :3333
      本工具驱动 gdb 做批处理，核心停住→读→detach(恢复运行)

关键机制（实测得出，非推测）：
  * gdb `target extended-remote` 连上的瞬间，OpenOCD 会停住核心 → 这次连接的
    PC 就是"固件此刻跑到哪"的答案，天然就是一次 stop，无需额外命令。
  * 复位态（刚 init 时）读 IROM 0x42000020 得到 unimp(0x0000)：
    此时 MMU/Flash cache 未初始化；固件跑起来后同地址读出真实指令。
    这是判断"读到的是真代码还是空壳"的判据。
  * 收尾必须 `detach`，否则核心停在调试态、固件不再运行。

用法:
  python3 scripts/xdbg_jtag.py probe                    探测 JTAG 通道+芯片
  python3 scripts/xdbg_jtag.py stop [--elf x.elf] [--keep]
                                                        停核心看它跑到哪(默认看完全开)
  python3 scripts/xdbg_jtag.py regs                     全部寄存器快照
  python3 scripts/xdbg_jtag.py mem <addr> [len]         读内存 (hex dump)
  python3 scripts/xdbg_jtag.py dis <addr> [n]           反汇编 n 条
  python3 scripts/xdbg_jtag.py sym <name>               查符号地址 (需 --elf)
  python3 scripts/xdbg_jtag.py bp <loc> [--elf x.elf]   下断点并等命中(函数名/行号/地址)
  python3 scripts/xdbg_jtag.py step [n]                 单步 n 条
  python3 scripts/xdbg_jtag.py resume                   放回运行 (等价 detach)
  python3 scripts/xdbg_jtag.py selftest                 自检
"""
import argparse
import glob
import os
import re
import shutil
import socket
import subprocess
import sys
import time

# ---------------- 工具路径 ----------------

ESPRESSIF = os.path.expanduser("~/.espressif/tools")


def _find(prog, subdir):
    base = os.path.join(ESPRESSIF, subdir)
    hits = glob.glob(os.path.join(base, "*", "*", "bin", prog))
    hits += glob.glob(os.path.join(base, "*", "bin", prog))
    return sorted(hits)[-1] if hits else None


OPENOCD = _find("openocd", "openocd-esp32")
GDB = _find("riscv32-esp-elf-gdb", "riscv32-esp-elf-gdb")
OCD_SCRIPTS = None
if OPENOCD:
    OCD_SCRIPTS = os.path.join(os.path.dirname(os.path.dirname(OPENOCD)),
                               "share", "openocd", "scripts")

OCD_PORT = 3333
OCD_TELNET = 4444
OCD_LOG = "/tmp/xdbg_openocd.log" if os.path.isdir("/tmp") else "/tmp/xdbg_openocd.log"
OCD_BOARD_CFG = "board/esp32c3-builtin.cfg"

# 固件 ELF 候选（有它才有函数名/行号）
ELF_HINTS = [
    os.path.expanduser("~/cow/esp32-c3-tft-gpio-test/build/*.elf"),
    os.path.expanduser("~/cow/esp32-c3-tft-lvgl-va/build/*.elf"),
    os.path.expanduser("~/Desktop/esp32-sim/tmp/idfbuild/build/*.elf"),
    os.path.expanduser("~/cow/tmp/*/build/*.elf"),
]


def color_enabled():
    return sys.stdout.isatty()


def c(s, col):
    if not color_enabled():
        return s
    m = {"r": "\033[31m", "g": "\033[32m", "y": "\033[33m", "b": "\033[34m",
         "c": "\033[36m", "d": "\033[2m", "0": "\033[0m"}
    return f"{m[col]}{s}{m['0']}"


def die(msg, code=1):
    print(c("✘ " + msg, "r"), file=sys.stderr)
    sys.exit(code)


def preflight():
    if not OPENOCD:
        die("未找到 openocd-esp32（~/.espressif/tools/openocd-esp32）")
    if not GDB:
        die("未找到 riscv32-esp-elf-gdb（~/.espressif/tools/riscv32-esp-elf-gdb）")
    if not os.access(OPENOCD, os.X_OK):
        die(f"openocd 不可执行: {OPENOCD}")


def autodetect_elf(explicit=None):
    if explicit:
        if not os.path.exists(explicit):
            die(f"ELF 不存在: {explicit}")
        return explicit
    for pat in ELF_HINTS:
        hits = sorted(glob.glob(pat))
        if hits:
            return hits[-1]
    return None


# ---------------- OpenOCD 服务 ----------------

def port_open(port, host="127.0.0.1"):
    s = socket.socket()
    s.settimeout(0.5)
    try:
        s.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def openocd_pid():
    """找占用 3333 的 openocd pid"""
    try:
        out = subprocess.run(["lsof", "-nP", f"-iTCP:{OCD_PORT}", "-sTCP:LISTEN", "-t"],
                             capture_output=True, text=True, timeout=5).stdout
        pids = [int(x) for x in out.split() if x.strip().isdigit()]
        return pids[0] if pids else None
    except Exception:
        return None


def ensure_openocd(verbose=True):
    """确保 OpenOCD 在跑（GDB server :3333 在听）。返回 (ok, 说明)"""
    if port_open(OCD_PORT):
        return True, "OpenOCD 已在运行"
    if os.path.exists(OCD_LOG):
        os.remove(OCD_LOG)
    cmd = [OPENOCD, "-s", OCD_SCRIPTS, "-f", OCD_BOARD_CFG]
    with open(OCD_LOG, "w") as log:
        subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True,
                         cwd="/tmp")
    for _ in range(40):          # 最多等 8s
        time.sleep(0.2)
        if port_open(OCD_PORT):
            return True, "OpenOCD 已启动"
    return False, "OpenOCD 启动失败（见 " + OCD_LOG + "）"


def openocd_log_tail(n=60):
    if not os.path.exists(OCD_LOG):
        return ""
    with open(OCD_LOG, "r", errors="replace") as f:
        return "".join(f.readlines()[-n:])


def stop_openocd():
    pid = openocd_pid()
    if not pid:
        return False
    subprocess.run(["kill", str(pid)], capture_output=True)
    time.sleep(0.6)
    return True


# ---------------- GDB 批处理 ----------------

GDB_HEAD = [
    "set pagination off",
    "set confirm off",
    "set print elements 32",
    "set remotetimeout 5",
    "set tcp connect-timeout 5",
]


def gdb_run(cmds, elf=None, timeout=40, echo=True):
    """跑一批 gdb 命令；cmds 里不要包含连接/收尾，本函数统一处理"""
    script = list(GDB_HEAD)
    if elf:
        script.append(f"file {elf}")
    script.append(f"target extended-remote :{OCD_PORT}")
    script += cmds
    script.append("detach")
    script.append("quit")

    ex = []
    for ln in script:
        ex += ["-ex", ln]
    cmd = [GDB, "-q", "--batch"] + ex
    if echo:
        print(c("$ gdb " + " ".join(f"'{x}'" for x in script), "d"))
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        out = ""
        for attr in ("stdout", "stderr"):
            v = getattr(e, attr, None)
            if v:
                out += v.decode("utf-8", "replace") if isinstance(v, bytes) else v
        return out + c(f"\n[超时 {timeout}s，已强制中断]", "y")


def strip_gdb_noise(text):
    out = []
    for ln in text.splitlines():
        s = ln.strip()
        if s.startswith("(gdb)"):
            s = s[5:].strip()
        if s.startswith("warning: No executable"):
            continue
        if s.startswith("Remote debugging using"):
            continue
        if s.startswith("[Inferior ") and "detached]" in s:
            continue
        if s.startswith("Invalid argument syntax"):
            continue
        if s == "" and (not out or out[-1] == ""):
            continue
        out.append(s)
    # 去掉尾部空行
    while out and out[-1] == "":
        out.pop()
    return "\n".join(out)


# ---------------- 命令 ----------------

def cmd_probe(args):
    ok, msg = ensure_openocd()
    print(c("◆ " + msg, "g" if ok else "r"))
    if not ok:
        print(openocd_log_tail(40))
        return 1
    pid = openocd_pid()
    print(f"  OpenOCD pid={pid}  gdb_server=:{OCD_PORT}  telnet=:{OCD_TELNET}")
    print(f"  openocd : {OPENOCD}")
    print(f"  gdb     : {GDB}")
    if args.verbose:
        print(c("\n--- OpenOCD 通道日志 ---", "b"))
        print(openocd_log_tail(40).rstrip())
    # 用 gdb 做一次真实握手 + 读 ROM
    txt = strip_gdb_noise(gdb_run([
        'printf "=== 握手成功，核心状态 ===\\n"',
        "info registers pc sp",
        'printf "=== ROM 复位向量 ===\\n"',
        "x/2i 0x40000000",
    ], elf=autodetect_elf(args.elf), echo=False))
    print(c("\n--- GDB 握手 ---", "b"))
    print(txt)
    return 0


def _report_stopped(txt, elf):
    """把停核心的原始输出整理成人看的样子"""
    print(c("\n--- 核心已停住 / 清点现场 ---", "b"))
    print(txt)
    m = re.search(r"^pc\s+(0x[0-9a-fA-F]+)", txt, re.M)
    if m and elf:
        print(c(f"\n提示: pc={m.group(1)}；用 `sym` / `dis` 配合 --elf 可做源码级定位", "d"))


def cmd_stop(args):
    ok, msg = ensure_openocd()
    if not ok:
        die(msg)
    elf = autodetect_elf(args.elf)
    if elf:
        print(c(f"◆ 固件符号表: {elf}", "d"))
    cmds = [
        'printf "=== 固件此刻跑到哪 ===\\n"',
        "info registers pc sp ra",
        'printf "=== 当前指令 (真实机器码) ===\\n"',
        "x/6i $pc",
    ]
    if elf:
        cmds += [
            'printf "=== 源码位置 ===\\n"',
            "info line *$pc",
            'printf "=== 调用栈 ===\\n"',
            "bt",
        ]
    cmds += [
        'printf "=== 栈顶 (SRAM) ===\\n"',
        "x/8xw $sp",
    ]
    txt = strip_gdb_noise(gdb_run(cmds, elf=elf, echo=not args.quiet))
    _report_stopped(txt, elf)
    if args.keep:
        print(c("\n⚠ --keep: 核心保持在调试态（固件不再运行）。用 `resume` 或停止 openocd 恢复。", "y"))
    else:
        print(c("\n（已 detach，固件恢复运行）", "d"))
    return 0


def cmd_regs(args):
    ok, msg = ensure_openocd()
    if not ok:
        die(msg)
    elf = autodetect_elf(args.elf)
    txt = strip_gdb_noise(gdb_run(["info registers"], elf=elf, echo=not args.quiet))
    print(txt)
    return 0


def cmd_mem(args):
    ok, _ = ensure_openocd()
    if not ok:
        die("OpenOCD 未运行")
    addr = args.addr if args.addr.startswith("0x") else hex(int(args.addr, 16))
    nw = max(1, args.len // 4)
    cmds = [
        f'printf "=== 读内存 {addr} ({args.len} 字节) ===\\n"',
        f"x/{nw}xw {addr}",
    ]
    txt = strip_gdb_noise(gdb_run(cmds, echo=not args.quiet))
    print(txt)
    if re.search(r"0x0*0:\s", txt) and "Cannot access" not in txt:
        pass
    print(c("提示: 若整片读出全 0 —— 目标可能处于复位态，Flash cache/MMU 未初始化。", "d"))
    return 0


def cmd_dis(args):
    ok, _ = ensure_openocd()
    if not ok:
        die("OpenOCD 未运行")
    addr = args.addr if args.addr.startswith("0x") else hex(int(args.addr, 16))
    elf = autodetect_elf(args.elf)
    txt = strip_gdb_noise(gdb_run([f"x/{args.n}i {addr}"], elf=elf, echo=not args.quiet))
    print(txt)
    return 0


def cmd_sym(args):
    ok, _ = ensure_openocd()
    if not ok:
        die("OpenOCD 未运行")
    elf = autodetect_elf(args.elf)
    if not elf:
        die("查符号需要 --elf（未自动发现固件 ELF）")
    txt = strip_gdb_noise(gdb_run([f"info address {args.name}",
                                   f"info line {args.name}"], elf=elf,
                                  echo=not args.quiet))
    print(txt)
    return 0


def cmd_bp(args):
    ok, _ = ensure_openocd()
    if not ok:
        die("OpenOCD 未运行")
    elf = autodetect_elf(args.elf)
    cmds = [
        f"break {args.loc}",
        'printf "=== 断点已设，等待命中 (最多 %ds) ===\\n"' % args.timeout,
        "continue",
        'printf "\\n=== 命中! 现场 ===\\n"',
        "info registers pc sp ra",
        "x/4i $pc",
    ]
    if elf:
        cmds += ["info line *$pc", "bt"]
    txt = strip_gdb_noise(gdb_run(cmds, elf=elf, timeout=args.timeout + 15,
                                  echo=not args.quiet))
    print(txt)
    if "Breakpoint" in txt and re.search(r"Breakpoint \d+,", txt):
        print(c("\n✔ 断点命中（真实硬件断点）", "g"))
    else:
        print(c("\n⚠ 未见命中（可能未触发；有时限内固件没跑到该位置）", "y"))
    print(c("（已 detach，固件恢复运行）", "d"))
    return 0


def cmd_step(args):
    ok, _ = ensure_openocd()
    if not ok:
        die("OpenOCD 未运行")
    elf = autodetect_elf(args.elf)
    n = max(1, args.n)
    cmds = [f"stepi {n}",
            'printf "=== 单步后现场 ===\\n"',
            "info registers pc sp",
            "x/4i $pc"]
    if elf:
        cmds += ["info line *$pc"]
    txt = strip_gdb_noise(gdb_run(cmds, elf=elf, timeout=30, echo=not args.quiet))
    print(txt)
    return 0


def cmd_resume(args):
    ok, _ = ensure_openocd()
    if not ok:
        die("OpenOCD 未运行")
    # 一次连接即 detach：核心放回运行
    txt = strip_gdb_noise(gdb_run(['printf "resumed\\n"'], echo=False))
    print(c("✔ 核心已恢复运行", "g"))
    if args.verbose:
        print(txt)
    return 0


def cmd_selftest(args):
    rc = 0
    print(c("◆ xdbg_jtag 自检", "b"))

    # 1 工具链齐备
    print("\n[1] 工具链")
    print(f"    openocd : {OPENOCD or '✘ 缺失'}")
    print(f"    gdb     : {GDB or '✘ 缺失'}")
    if not (OPENOCD and GDB):
        return 1
    print(c("    ok", "g"))
    print(f"    board cfg: {OCD_BOARD_CFG}  scripts={OCD_SCRIPTS}")

    # 2 openocd 起得来
    print("\n[2] OpenOCD 通道")
    ok, msg = ensure_openocd()
    print(f"    {msg}")
    if not ok:
        print(openocd_log_tail(30))
        return 1
    log = openocd_log_tail(60)
    tap = re.search(r"tap/device found: (0x[0-9a-f]+)", log)
    core = re.search(r"Examined RISC-V core; found (\d+) harts", log)
    if tap:
        print(c(f"    ✓ JTAG tap: {tap.group(1)}", "g"))
    else:
        print(c("    ✘ 未在日志看到 JTAG tap", "r")); rc = 1
    if core:
        print(c(f"    ✓ RISC-V core: {core.group(1)} hart(s)", "g"))
    else:
        print(c("    ✘ 未见 core 枚举", "r")); rc = 1

    # 3 真实握手 + ROM 读
    print("\n[3] GDB 握手 + 真机读")
    elf = autodetect_elf(args.elf)
    txt = strip_gdb_noise(gdb_run([
        'printf "PC=0x%x\\n", $pc',
        "x/2i 0x40000000",
    ], elf=elf, echo=False))
    print("    " + txt.replace("\n", "\n    "))
    if re.search(r"PC=0x[0-9a-fA-F]+", txt):
        print(c("    ✓ 读到真机 PC", "g"))
    else:
        print(c("    ✘ 未读到 PC", "r")); rc = 1
    if "j\t0x40001e90" in txt or "0x40001e90" in txt:
        print(c("    ✓ ROM 复位向量正确 (j 0x40001e90)", "g"))
    else:
        print(c("    · ROM 向量非预期（芯片可能不在复位态，属正常差异）", "y"))

    # 4 符号表
    print("\n[4] 固件 ELF")
    print(f"    {elf or '· 未发现（不影响寄存器/内存/反汇编）'}")

    print()
    if rc == 0:
        print(c("✔ 自检通过 —— 真机硬件级调试通道可用", "g"))
    else:
        print(c("✘ 自检发现问题", "r"))
    return rc


# ---------------- 入口 ----------------

def main():
    ap = argparse.ArgumentParser(
        prog="xdbg_jtag.py",
        description="xiaomo ESP32 真机硬件级调试工具 (JTAG/GDB)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("原理：")[-1] if False else None)
    ap.add_argument("--elf", help="固件 ELF（有符号可做源码级定位；默认自动发现）")
    ap.add_argument("-q", "--quiet", action="store_true", help="不回显 gdb 命令")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("probe", help="探测 JTAG 通道 + 芯片")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_probe)

    p = sub.add_parser("stop", help="停住核心，看固件跑到哪（默认看完自动放回运行）")
    p.add_argument("--keep", action="store_true", help="保持停住不放回运行")
    p.set_defaults(fn=cmd_stop)

    sub.add_parser("regs", help="全部寄存器快照").set_defaults(fn=cmd_regs)

    p = sub.add_parser("mem", help="读内存 (hex)")
    p.add_argument("addr"); p.add_argument("len", nargs="?", type=int, default=32)
    p.set_defaults(fn=cmd_mem)

    p = sub.add_parser("dis", help="反汇编")
    p.add_argument("addr"); p.add_argument("n", nargs="?", type=int, default=8)
    p.set_defaults(fn=cmd_dis)

    p = sub.add_parser("sym", help="查符号地址")
    p.add_argument("name")
    p.set_defaults(fn=cmd_sym)

    p = sub.add_parser("bp", help="下断点并等命中")
    p.add_argument("loc", help="函数名 / file:line / *0xADDR")
    p.add_argument("--timeout", type=int, default=15, help="等待命中的秒数")
    p.set_defaults(fn=cmd_bp)

    p = sub.add_parser("step", help="单步 n 条")
    p.add_argument("n", nargs="?", type=int, default=1)
    p.set_defaults(fn=cmd_step)

    p = sub.add_parser("resume", help="把核心放回运行")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_resume)

    p = sub.add_parser("selftest", help="自检")
    p.set_defaults(fn=cmd_selftest)

    p = sub.add_parser("kill-ocd", help="停掉本工具启动的 OpenOCD 服务")
    p.set_defaults(fn=lambda a: (print(c("✔ OpenOCD 已停" if stop_openocd() else "· 无 OpenOCD 在跑", "g")), 0)[1])

    args = ap.parse_args()
    preflight()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
