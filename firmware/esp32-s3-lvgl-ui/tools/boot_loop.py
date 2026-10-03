#!/usr/bin/env python3
"""
boot_loop.py — 反复复位压测，统计崩溃率（竞态类 bug 的唯一有效验证手段）

竞态 bug 的核心特征：同一二进制时而崩时而不崩。
所以「跑一次干净」没有任何证明力 —— 必须 N 次复位统计。

用法: boot_loop.py [次数] [每次采集秒数] [输出目录]
"""
import glob, os, re, sys, time, serial

N    = int(sys.argv[1]) if len(sys.argv) > 1 else 12
SEC  = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0
OUT  = sys.argv[3] if len(sys.argv) > 3 else os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tmp", "bootloop")

os.makedirs(OUT, exist_ok=True)
port = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
print(f"port={port}  复位 {N} 次 × {SEC}s\n")

CRASH = re.compile(
    r"Guru Meditation|LoadProhibited|StoreProhibited|IllegalInstruction|"
    r"InstrFetchProhibited|abort\(\)|assert failed|stack canary|Unhandled"
)
ANSI = re.compile(r"\x1b\[[0-9;]*m")

s = serial.Serial(port, 115200, timeout=0.2)
rows = []
for i in range(1, N + 1):
    s.reset_input_buffer()
    # esptool HardReset 同款：DTR 全程 False，RTS 1 -> 0
    s.setDTR(False); s.setRTS(True); time.sleep(0.2); s.setRTS(False)

    buf = b""
    t0 = time.time()
    while time.time() - t0 < SEC:
        d = s.read(8192)
        if d:
            buf += d
    txt = ANSI.sub("", buf.decode("utf-8", "replace")).replace("\r", "")
    open(f"{OUT}/boot{i:02d}.log", "w").write(txt)

    rst   = re.findall(r"rst:0x[0-9a-f]+", txt)
    hits  = CRASH.findall(txt)
    # 判据只认「UI 建完 + 无异常」，与固件版本无关
    # （"刷新任务已启动" 只有修复后的固件才打印，仅作信息参考）
    ready = ("LCD 就绪" in txt) and ("UI 就绪" in txt)
    ok = ready and not hits
    rows.append((i, ok, len(rst), len(hits), set(hits)))
    flag = "✅" if ok else "❌"
    hs = (" " + ",".join(sorted(set(hits)))) if hits else ""
    print(f"  [{i:2d}/{N}] {flag} boot={len(rst)} UI就绪={'Y' if ready else 'N'}"
          f" 异常={len(hits)}{hs}")

s.close()

bad = [r for r in rows if not r[1]]
print(f"\n结果: {N - len(bad)}/{N} 正常启动，{len(bad)} 次异常")
if bad:
    print("失败轮次:", ", ".join(str(r[0]) for r in bad))
    print("→ 竞态未消除，需继续排查")
else:
    print("→ 全部正常，竞态已消除（p < 1/%d 量级）" % N)
