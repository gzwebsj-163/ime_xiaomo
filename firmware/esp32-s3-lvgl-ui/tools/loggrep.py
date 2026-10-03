#!/usr/bin/env python3
"""按 UTF-8 正确读 ESP32 串口日志并过滤。

⚠️ 为什么不能用 strings：strings 只提取 ASCII 可打印序列，遇到 UTF-8 中文字节
   （>=0x80）会断开，于是 "第1次开机：已写入" 会被截成 "[persist] "。
   用 strings 看中文日志会得到**残缺证据**，据此下的判断是错的。
用法: loggrep.py <logfile> [正则]
"""
import re, sys

data = open(sys.argv[1], "rb").read()
txt = data.decode("utf-8", errors="replace")
txt = re.sub(r"\x1b\[[0-9;]*m", "", txt)      # 去 ANSI 颜色
pat = re.compile(sys.argv[2]) if len(sys.argv) > 2 else None
for line in txt.splitlines():
    line = line.strip()
    if not line:
        continue
    if pat is None or pat.search(line):
        print(line)
