#!/usr/bin/env python3
"""硬复位一次并抓取完整启动日志（含 panic/复位原因）。

用法: boot_cap.py <out.log> [秒数]
要点（esptool reset.py 的 HardReset 序列）: DTR 全程 False，RTS 1 → 0.2s → 0。
若只 read 而不复位，只能抓到「现在之后」的输出 → 会漏掉 boot banner 和复位原因。
"""
import glob, sys, time, serial

out = sys.argv[1]
dur = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]

s = serial.Serial(port, 115200, timeout=0.2)
s.dtr = False
s.rts = True
time.sleep(0.2)
s.rts = False
s.reset_input_buffer()

t0 = time.time()
with open(out, "wb", buffering=0) as f:
    while time.time() - t0 < dur:
        d = s.read(4096)
        if d:
            f.write(d)
s.close()
print("captured ->", out)
