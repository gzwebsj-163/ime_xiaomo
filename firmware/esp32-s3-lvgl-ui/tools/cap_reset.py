#!/usr/bin/env python3
"""硬复位 + 抓完整启动日志（含 WiFi 扫描/连接过程）。

与 boot_cap.py 的区别（重要）：
    boot_cap.py 在**复位之后**才 reset_input_buffer()，存在竞态 —— 若 boot 输出
    来得比 clear 快，banner 就被吃掉了。
    本脚本把清缓冲放在**复位之前**，复位后立刻开始收，不丢任何一行。

用法: cap_reset.py <out.log> [秒数] [串口]
"""
import glob, sys, time, serial

out  = sys.argv[1]
dur  = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
port = sys.argv[3] if len(sys.argv) > 3 else sorted(glob.glob("/dev/cu.usbmodem*"))[0]

s = serial.Serial(port, 115200, timeout=0.2)
s.reset_input_buffer()          # ← 先清，再复位
s.dtr = False                   # esptool reset.py 的 HardReset 序列
s.rts = True
time.sleep(0.2)
s.rts = False

t0 = time.time()
n = 0
with open(out, "wb", buffering=0) as f:
    while time.time() - t0 < dur:
        d = s.read(4096)
        if d:
            f.write(d); n += len(d)
s.close()
print(f"captured {n} bytes -> {out}  ({port})")
