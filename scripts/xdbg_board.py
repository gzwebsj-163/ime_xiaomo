#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
xdbg_board.py —— xiaomo ESP32 真机调试工具 (2026-09-28)

把真机排障手工流程固化 (历史坑驱动设计):
  list      列 USB 串口 (排除 PTY —— 历史坑: PTY 混进真机列表导致 Errno 16 busy)
  reset     程序化硬复位抓 boot 日志 (坑: 烧完再开串口只能看到循环体;
            USB-Serial-JTAG 板必须 DTR 全程 False + RTS 1→0.2s→0,HardReset 序列)
  watch     实时串口流 + 关键行高亮 (PASS 绿/ERROR 红/EVT 蓝)
  analyze   日志体检: [N] 步骤提取 + PASS/FAIL 统计 + 错误行 + 耗时

用法:
  python3 scripts/xdbg_board.py list
  python3 scripts/xdbg_board.py reset  [-p PORT] [-t 12] [-b 115200] [-o out.log]
  python3 scripts/xdbg_board.py watch  [-p PORT] [-b 115200] [--filter pat]
  python3 scripts/xdbg_board.py analyze <capture.log> [--steps] [--errors]
"""
import argparse
import glob
import os
import re
import sys

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("缺少 pyserial: pip3 install pyserial (esptool 自带)")
    sys.exit(1)

ANSI = {"red": "\033[31m", "green": "\033[32m", "yellow": "\033[33m",
        "blue": "\033[34m", "cyan": "\033[36m", "off": "\033[0m", "dim": "\033[2m"}

RE_STEP = re.compile(r"\[(\d{1,2})\]\s*(\S.*)$")
# 错误匹配: FAIL 整词 / fails=N(N>0) / error / panic 等; 排除 "fails = 0"、"ALL PASS" 误报
RE_FAIL = re.compile(
    r"\bFAIL\b|fails?\s*[=:]\s*[1-9]|[1-9]\d*\s*fails?\b|\berror\b|\bERROR\b|panic|assert|abort|Guru|Backtrace")
RE_PASS = re.compile(r"PASS|OK\b", re.I)
RE_EVT = re.compile(r"\[EVT\]|E\(\d+\)|esp_err|E ")
RE_TIME = re.compile(r"t=(\d+)\s*ms")
RE_ANSI = re.compile(r"\x1b\[[0-9;]*m")


def use_color():
    return sys.stdout.isatty()


def paint(s, color):
    return f"{ANSI[color]}{s}{ANSI['off']}" if use_color() else s


# ---------------- 端口 ----------------

def list_ports():
    """列真实 USB 串口; 排除 PTY (cu/tty 对里 PTY 会被误当真机)"""
    ports = serial.tools.list_ports.comports()
    if not ports:
        print("(未发现任何串口设备)")
        return None
    for p in ports:
        tag = " <-- 疑似 ESP32 (USB/JTAG)" if "USB" in (p.description or "").upper() or "modem" in p.device else ""
        print(f"{p.device:28s} {p.description or '?'}{tag}")
    return ports


def auto_pick(port=None):
    if port:
        return port
    ports = [p.device for p in serial.tools.list_ports.comports()]
    usb = [d for d in ports if "usb" in d.lower() or "modem" in d.lower()]
    if not usb:
        sys.exit("未发现 USB 串口 (-p 指定端口; list 查看)")
    if len(usb) > 1:
        print(f"多个候选 {usb}, 取第一个; 建议显式 -p")
    return usb[0]


# ---------------- 复位 + 抓日志 ----------------

def hard_reset(port, secs, out, baud):
    """
    esptool HardReset 语义 (USB-Serial-JTAG 板坑):
    DTR 全程 False (拉高 DTR 会进下载模式变体 → boot:0x0 DOWNLOAD 卡死假象)
    RTS True → 0.2s → False  → 制造冷启动, 从第一条 boot 行抓起
    """
    ser = serial.Serial(port, baud)
    lines = []
    try:
        ser.dtr = False
        ser.rts = True
        ser.reset_input_buffer()
        import time
        time.sleep(0.2)
        ser.rts = False
        t0 = time.time()
        buf = b""
        while time.time() - t0 < secs:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    s = line.decode("utf-8", "replace").rstrip("\r")
                    lines.append(s)
                    print(paint(s, "dim"))
            else:
                time.sleep(0.02)
    finally:
        ser.close()
    if out:
        with open(out, "w") as f:
            f.write("\n".join(lines) + "\n")
        print(paint(f"-- 已存 {out} ({len(lines)} 行) --", "cyan"))
    return lines


# ---------------- 实时 watch ----------------

def watch(port, baud, filt):
    ser = serial.Serial(port, baud)
    print(paint(f"-- watch {port} @ {baud} (Ctrl-C 退出) --", "cyan"))
    buf = b""
    try:
        while True:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    s = line.decode("utf-8", "replace").rstrip("\r")
                    if filt and filt.lower() not in s.lower():
                        continue
                    if RE_FAIL.search(s):
                        print(paint(s, "red"))
                    elif RE_EVT.search(s):
                        print(paint(s, "yellow"))
                    elif RE_PASS.search(s):
                        print(paint(s, "green"))
                    else:
                        print(s)
            else:
                import time
                time.sleep(0.02)
    except KeyboardInterrupt:
        print(paint("\n-- 停止 --", "cyan"))
    finally:
        ser.close()


# ---------------- 日志体检 ----------------

def analyze(path, show_steps, show_errors):
    if not os.path.exists(path):
        sys.exit(f"文件不存在: {path}")
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    lines = [RE_ANSI.sub("", s).rstrip() for s in lines]  # 剥离 ANSI 颜色码
    steps, errors, warn = [], [], []
    npass = nfail = 0
    times = []
    boot_ok = False
    for i, s in enumerate(lines, 1):
        m = RE_STEP.search(s)
        if m:
            steps.append((i, m.group(1), m.group(2).strip()))
        if re.search(r"rst:0x|boot:",
                     s) and re.search(r"rst:0x|ESP-IDF|^I \(", s):
            boot_ok = True
        if re.search(r"\bPASS\b", s):
            npass += 1
        if RE_FAIL.search(s):
            nfail += 1
            errors.append((i, s))
        if re.match(r"^W \(", s):
            warn.append((i, s))
        t = RE_TIME.search(s)
        if t:
            times.append((i, int(t.group(1)), s.strip()))

    print(f"==== 日志体检: {path} ({len(lines)} 行) ====")
    print(f"PASS 计数: {npass} | 疑似错误行: {nfail} | IDF 警告 W(): {len(warn)}")
    if steps:
        print(f"\n{'步骤':>6} {'行号':>6}  内容")
        for ln, no, s in steps:
            mark = paint("FAIL?", "red") if RE_FAIL.search(s) else (
                paint("ok", "green") if RE_PASS.search(s) else "")
            print(f"[{no:>4}] {ln:>6}  {s[:90]} {mark}")
    if times:
        last = times[-1]
        print(f"\n耗时标记: {len(times)} 处, 末尾 t={last[1]}ms (行 {last[0]}: {last[2][:70]})")
    if errors:
        print(f"\n错误行 ({len(errors)}):")
        for ln, s in errors[:20]:
            print(f"  {ln:>6}: {s[:100]}")
        if len(errors) > 20:
            print(f"  ... 另 {len(errors) - 20} 条 (--errors 看: 已全列)")
    verdict = "✔ 看起来健康" if nfail == 0 and npass > 0 else ("⚠ 有 FAIL 行" if npass else "无明确判定")
    print(f"\n体检结论: {paint(verdict, 'green' if nfail == 0 else 'red')}")
    if not show_steps and not show_errors:
        if warn:
            print(paint(f"提示: {len(warn)} 条 IDF 警告, 未展开", "dim"))


# ---------------- CLI ----------------

def main():
    ap = argparse.ArgumentParser(description="xiaomo ESP32 真机调试工具")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list")

    p = sub.add_parser("reset")
    p.add_argument("-p", "--port")
    p.add_argument("-t", "--secs", type=float, default=12)
    p.add_argument("-b", "--baud", type=int, default=115200)
    p.add_argument("-o", "--out", default=None)

    p = sub.add_parser("watch")
    p.add_argument("-p", "--port")
    p.add_argument("-b", "--baud", type=int, default=115200)
    p.add_argument("--filter", default=None)

    p = sub.add_parser("analyze")
    p.add_argument("log")
    p.add_argument("--steps", action="store_true")
    p.add_argument("--errors", action="store_true")

    a = ap.parse_args()
    if a.cmd == "list":
        list_ports()
    elif a.cmd == "reset":
        port = auto_pick(a.port)
        print(paint(f"-- hard reset {port} (DTR=False, RTS 1→0.2s→0), 抓 {a.secs}s --", "cyan"))
        hard_reset(port, a.secs, a.out, a.baud)
    elif a.cmd == "watch":
        watch(auto_pick(a.port), a.baud, a.filter)
    elif a.cmd == "analyze":
        analyze(a.log, a.steps, a.errors)


if __name__ == "__main__":
    main()
