#!/usr/bin/env python3
"""抓取真机串口输出 N 秒，全文落盘（不做 tail，保留开头——崩溃真相常在开头）"""
import os, sys, time, glob, re, serial
SEC = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
OUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tmp", "pinprobe.log")
port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
s = serial.Serial(port, 115200, timeout=0.2)
s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False)   # HardReset
buf = b""; t0 = time.time()
while time.time() - t0 < SEC:
    d = s.read(8192)
    if d: buf += d
txt = re.compile(r"\x1b\[[0-9;]*m").sub("", buf.decode("utf-8", "replace")).replace("\r", "")
open(OUT, "w").write(txt)
print(f"port={port}  抓到 {len(txt)} 字符 -> {OUT}")
