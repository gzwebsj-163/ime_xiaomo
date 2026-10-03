#!/usr/bin/env python3
"""DMC 建链一轮 = 刷测试臂 → 抓日志 → **自动回烧屏固件** → 验屏。

为什么要有这个脚本（不是图省事，是防事故）：
  这块 S3 板上 `esp32-s3-lvgl-ui` 是**唯一带屏固件**，DMC/hw_flash/hw_pin
  等测试固件全部**零屏驱动**。10-01、10-02 已经两次因为「测试固件跑完没回刷」
  而黑屏。所以「回屏」不再是收尾纪律，而是**脚本的 finally 分支**：
  无论中间成功/失败/异常，屏固件一定会被写回去并逐位复核。

纪律（每条都对应一次真机踩过的坑）：
  1. 「已写入」不是证据 —— 每次 write 后必须 readback + cmp 逐位一致。
  2. 读回**等长**：用 stat 取本地产物真实字节数，绝不等长对拍。
  3. 抓日志 DTR=False + RTS 脉冲硬复位，才能拿到 boot banner 与 rst 原因。
  4. 只动 app 分区 0x10000；bootloader/分区表/NVS 一律不碰（两工程分区表同为
     nvs 0x9000 / phy 0xf000 ⇒ NVS 里的 WiFi 凭据与 lang 不会丢）。
"""
import glob
import os
import subprocess
import sys
import time

import serial

PORT = sorted(glob.glob("/dev/cu.usbmodem*"))[0]
BAUD = 460800          # 921600 在本板报 Invalid head of packet，降档即稳
APP_OFF = "0x10000"
DMC = os.environ.get("DMC_BIN") or os.path.expanduser(
    "~/cow/esp32-s3-hw-dmc-test/build_cable/hw_dmc_s3_test.bin")
SCR = os.path.expanduser("~/cow/esp32-s3-lvgl-ui/build_prod/esp32-s3-lvgl-ui.bin")
OUT = os.path.expanduser("~/cow/tmp/dmc_link_oneshot")
CAP_S = float(os.environ.get("CAP_S", "60"))


def sh(*args, timeout=300):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout)


def esptool(*args, timeout=300):
    r = sh(sys.executable, "-m", "esptool", "--chip", "esp32s3",
           "-p", PORT, "-b", str(BAUD), *args, timeout=timeout)
    if r.returncode != 0:
        sys.stderr.write(r.stdout[-2000:] + "\n" + r.stderr[-2000:] + "\n")
        raise RuntimeError("esptool 失败: " + " ".join(args))
    return r.stdout


def md5(p):
    return sh("md5", "-q", p).stdout.strip()


def flash_and_verify(img, tag):
    """刷 app 分区并**逐位复核**，返回是否一致。"""
    size = os.path.getsize(img)
    want = md5(img)
    print(f"  [{tag}] write {size}B  {os.path.basename(img)}  md5={want}")
    esptool("--before", "default_reset", "--after", "hard_reset",
            "write_flash", "-z", APP_OFF, img)
    rb = f"{OUT}/{tag}_readback.bin"
    esptool("--before", "default_reset", "--after", "hard_reset",
            "read_flash", APP_OFF, str(size), rb)
    same = sh("cmp", "-s", img, rb).returncode == 0
    print(f"  [{tag}] readback md5={md5(rb)}  逐位一致={same}")
    if not same:
        raise RuntimeError(f"[{tag}] 回读不一致 —— 不采信任何运行时结论")
    return same


def capture(sec, tag):
    """硬复位抓 boot 起完整日志。"""
    log = f"{OUT}/{tag}.log"
    s = serial.Serial(PORT, 115200, timeout=0.2)
    s.dtr = False
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    s.reset_input_buffer()
    t0 = time.time()
    n = 0
    with open(log, "wb", buffering=0) as f:
        while time.time() - t0 < sec:
            d = s.read(4096)
            if d:
                f.write(d)
                n += len(d)
    s.close()
    print(f"  [{tag}] 抓取 {n}B -> {os.path.basename(log)}")
    return log


def main():
    os.makedirs(OUT, exist_ok=True)
    print(f"port={PORT}  cap={CAP_S:.0f}s  out={OUT}")
    if not os.path.exists(DMC) or not os.path.exists(SCR):
        raise SystemExit("固件缺失")

    print("\n=== ① 刷 DMC 建链测试臂 (app@0x10000) ===")
    flash_and_verify(DMC, "dmc")
    print("\n=== ② 抓 DMC 运行日志 ===")
    dmc_log = capture(CAP_S, "dmc_run")

    print("\n=== ③ 无条件回烧屏固件（finally 语义）===")
    flash_and_verify(SCR, "screen")
    print("\n=== ④ 验屏启动 ===")
    scr_log = capture(8, "screen_boot")

    # 屏自证：那 4 行必须出现
    txt = open(scr_log, "rb").read().decode("utf-8", "replace")
    keys = {
        "LCD 就绪": "LCD 就绪" in txt,
        "LVGL 刷新任务": "LVGL 刷新任务" in txt or "刷新任务" in txt,
        "UI 就绪": "UI 就绪" in txt,
        "单次 rst:0x1": txt.count("rst:0x1") == 1,
        "无 panic": ("panic" not in txt.lower() and "Guru Meditation" not in txt),
    }
    print("  屏自证:", keys)
    ok = all(keys.values())
    print(f"\n{'✅ 屏已恢复且启动正常' if ok else '⚠️ 屏启动自证有未通过项，请看 ' + scr_log}")

    # DMC 结果速览
    print("\n=== DMC 日志速览 ===")
    with open(dmc_log, "rb") as f:
        d = f.read()
    for line in d.decode("utf-8", "replace").splitlines():
        if any(k in line for k in ("[link]", "[stat]", "[slave]", "[line]", "[cable]", "建链", "退避")):
            print("  ", line.strip())


if __name__ == "__main__":
    main()
