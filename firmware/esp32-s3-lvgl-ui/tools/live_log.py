#!/usr/bin/env python3
"""常驻串口监听：持续追加到文件，直到被杀。用法: live_log.py <out.log> [秒数]"""
import glob, sys, time, serial
out = sys.argv[1]
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 600.0
port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
s = serial.Serial(port, 115200, timeout=0.2)
s.reset_input_buffer()
t0 = time.time()
with open(out, "ab", buffering=0) as f:
    while time.time() - t0 < dur:
        d = s.read(4096)
        if d:
            f.write(d)
s.close()
print("logger exited")
