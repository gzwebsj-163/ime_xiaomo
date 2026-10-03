#!/usr/bin/env python3
"""
boot_cap_spiab.py — 硬复位抓完整 boot 日志（SPI A/B 实验专用）

与本项目既有 boot_cap.py 同款语义，但：
  1) 写死 115200（本板 921600 会报 Invalid head of packet）
  2) 抓完立刻把关键行摘出来，避免 tail 掩盖「崩溃-重启循环」
     —— 方法论：必须数 rst:0x 次数，不能只看尾部

用法：python3 boot_cap_spiab.py <输出文件> [等待秒]
"""
import sys, time, serial

PORT = '/dev/cu.usbmodem5B5E0508531'
OUT  = sys.argv[1] if len(sys.argv) > 1 else '/tmp/spiab_boot.log'
WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0

s = serial.Serial()
s.port, s.baudrate, s.timeout = PORT, 115200, 0.3
s.open()

# 硬复位序列（抄 esptool reset.py HardReset）：DTR 全程 False，RTS 1→0.2s→0
s.dtr = False
s.rts = True
time.sleep(0.15)
s.rts = False
time.sleep(0.1)

buf = b''
t0 = time.time()
while time.time() - t0 < WAIT:
    c = s.read(4096)
    if c:
        buf += c
s.close()

open(OUT, 'wb').write(buf)
print(f'bytes={len(buf)}  rst:0x x{buf.count(b"rst:0x")}')

# 崩溃关键字体检
CRASH = [b'PANIC', b'Guru Meditation', b'Backtrace', b'assert failed',
         b'Illegal instruction', b'LoadProhibited', b'StoreProhibited',
         b'abort()', b'watchdog']
hits = {k.decode(): buf.count(k) for k in CRASH if buf.count(k)}
print('crash:', hits if hits else 'ALL 0')

# 打印 [AB] 相关行
txt = buf.decode('utf-8', 'replace')
ab = [l for l in txt.splitlines() if '[AB' in l or 'spi_ab' in l or 's3_host' in l]
print(f'--- AB lines: {len(ab)} ---')
for l in ab:
    print(l)
