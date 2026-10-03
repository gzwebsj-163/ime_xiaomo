#!/usr/bin/env python3
"""烧录后采集串口日志（含 HardReset），落盘供统计。用法: cap.py <out.log> [秒数]"""
import glob, os, sys, time, serial

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tmp", "serial.log")
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0

port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
s = serial.Serial(port, 115200, timeout=0.2)
s.reset_input_buffer()
# esptool HardReset 同款序列：DTR 全程 False，RTS 1 -> 0
s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False)
time.sleep(0.1)
buf = b""
t0 = time.time()
while time.time() - t0 < dur:
    d = s.read(8192)
    if d:
        buf += d
s.close()
open(out, "wb").write(buf)
print(f"port={port}  bytes={len(buf)}  -> {out}")
