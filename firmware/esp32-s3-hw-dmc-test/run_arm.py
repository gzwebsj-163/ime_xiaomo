#!/usr/bin/env python3
"""dmc 三臂真机作业：归档当前 app → 刷指定臂 → 等长回读对拍 → 抓 60s 日志。

用法: run_arm.py <build_dir> <臂号> [抓秒数]

纪律（每条都对应一次真机踩过的坑）:
  1. 刷前把板上当前 app 完整读回归档 —— 被覆盖的固件要能拿回来。
  2. 回读**等长**：用 stat 取本地产物真实字节数，绝不拿「读 1MB 比 217KB」
     这种不等长对拍（今天已因此假报两次不一致）。
  3. 刷完必须 readback + cmp 逐位一致，「已写入」不是证据，产物一致才是。
  4. 抓日志用 DTR=False + RTS 脉冲硬复位，保证拿到 boot banner 与 rst 原因；
     tail -20 会把崩溃-重启循环伪装成「一次干净启动」。
"""
import glob
import os
import subprocess
import sys
import time

import serial

PORT = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
BAK = os.path.expanduser("~/cow/tmp/flashbak-20261002")
OFFSET = "0x10000"
PART_SIZE = 1024 * 1024


def esptool(*args, **kw):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32s3", "-p", PORT,
           "-b", "460800", *args]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=kw.get("timeout", 180))
    if r.returncode != 0:
        print(r.stdout[-2500:])
        print(r.stderr[-2500:])
        raise SystemExit("esptool 失败: " + " ".join(args))
    return r.stdout


def md5(p):
    return subprocess.run(["md5", "-q", p], capture_output=True, text=True).stdout.strip()


def main():
    bdir = sys.argv[1]
    arm = sys.argv[2]
    cap_s = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0

    img = os.path.join(bdir, "hw_dmc_s3_test.bin")
    size = os.path.getsize(img)          # ← 真实长度，绝不写死
    want = md5(img)
    print(f"=== 臂 {arm}  本地产物 {size} B  md5 {want}")

    # 1) 归档板上当前 app（覆盖前先留底）
    cur = f"{BAK}/pre_arm{arm}_app_{PART_SIZE}.bin"
    print(f"[1/4] 归档当前 app -> {os.path.basename(cur)}")
    esptool("read_flash", OFFSET, str(PART_SIZE), cur)

    # 2) 只刷 app
    print("[2/4] 刷 0x10000 (bootloader/分区表/NVS/storage 一律不动)")
    esptool("--before", "default_reset", "--after", "hard_reset",
            "write_flash", "-z", OFFSET, img)

    # 3) 等长回读对拍
    rb = f"{BAK}/arm{arm}_readback.bin"
    print(f"[3/4] 回读 {size} B 对拍")
    esptool("--before", "default_reset", "--after", "hard_reset",
            "read_flash", OFFSET, str(size), rb)
    got = md5(rb)
    same = subprocess.run(["cmp", "-s", img, rb]).returncode == 0
    print(f"      本地 {want}\n      回读 {got}   逐位一致={same}")
    if got != want or not same:
        raise SystemExit("✘ 回读不一致 —— 中止，不采信任何运行时结论")

    # 4) 抓日志
    log = f"{BAK}/arm{arm}.log"
    print(f"[4/4] 硬复位抓 {cap_s:.0f}s -> {os.path.basename(log)}")
    s = serial.Serial(PORT, 115200, timeout=0.2)
    s.dtr = False
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    s.reset_input_buffer()
    t0 = time.time()
    with open(log, "wb", buffering=0) as f:
        while time.time() - t0 < cap_s:
            d = s.read(4096)
            if d:
                f.write(d)
    s.close()
    print(f"=== 臂 {arm} 完成: {log} ({os.path.getsize(log)} B)")


if __name__ == "__main__":
    main()
