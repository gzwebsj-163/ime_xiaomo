#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
回烧产品固件三段（0x0 bootloader / 0x8000 分区表 / 0x10000 app），NVS 0x9000 不动。

铁律来源（VERDICT_RUBRIC_267c1ca9.md §六）：
  1. 无论 A/B 实验结论如何，收尾必须回烧产品固件——板子只有这一份带屏固件。
  2. 本板 esptool 只能用 115200（921600 会报 Invalid head of packet(0x00)）。
  3. 认指纹不认文件名：刷前逐项核对 md5，刷后回读逐位对拍。
  4. 分区表决定后续 app 能否被 bootloader 认出来，必须先刷 0x8000 再刷 0x10000。
  5. NVS 在 0x9000，保存 WiFi 凭据与 lang，**任何情况下不擦**。

本脚本只做「刷 + 验」，不做判定；判定请另看抓的 boot 日志。
"""
import hashlib
import os
import subprocess
import sys
import time

# ── 待回烧产物的活体指纹（2026-10-03 自算，改产物必须同步改这里）──
PRODUCT = "/Users/root1/cow/esp32-s3-lvgl-ui/build_v3"
SEGMENTS = [
    # (文件, 刷写偏移, 期望 md5, 期望字节数)
    ("bootloader/bootloader.bin",         0x0,     "28d9e1055aa43c5a71c67dcd6306841f", 21056),
    ("partition_table/partition-table.bin", 0x8000, "17e5eda197eed6bac53ee6d983b40664", 3072),
    ("esp32-s3-lvgl-ui.bin",              0x10000, "fe0b41df2e45f33013cc3e85f575b3e4", 1650656),
]
READBACK_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "logs", "restore_readback")
BAUD = 115200


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def find_port():
    """找 ESP32 串口。排除蓝牙与 iPhone 配对口这类系统占位。"""
    import glob
    cands = []
    for p in sorted(glob.glob("/dev/cu.*")):
        low = p.lower()
        if any(k in low for k in ("bluetooth", "iphone", "debug-console", "wlan")):
            continue
        cands.append(p)
    return cands


def esptool(port, args, timeout=180):
    cmd = ["python3", "-m", "esptool", "--chip", "esp32s3", "--port", port,
           "--baud", str(BAUD)] + args
    print("  $ " + " ".join(cmd))
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def main():
    print("=" * 72)
    print("回烧产品固件三段 · 铁律执行脚本")
    print("=" * 72)

    # ── 闸 0：端口存在？──
    ports = find_port()
    if not ports:
        print("\n❌ 找不到可用串口（已排除蓝牙/iPhone 等系统占位）。板子没插或没枚举。")
        print("   可用口：", ports if ports else "无")
        return 2
    if len(ports) > 1:
        print(f"\n⚠️  发现多个候选口 {ports}，请用 --port 指定，避免刷错板子。")
        return 2
    port = ports[0]
    print(f"\n[闸0] 目标串口: {port}")

    # ── 闸 1：刷之前逐项核对本地产物指纹──
    print("\n[闸1] 核对本地产物指纹（认指纹不认文件名）")
    ok = True
    for rel, off, want_md5, want_len in SEGMENTS:
        path = os.path.join(PRODUCT, rel)
        if not os.path.exists(path):
            print(f"  ❌ 缺失 {rel}")
            ok = False
            continue
        got_md5, got_len = md5(path), os.path.getsize(path)
        match = (got_md5 == want_md5 and got_len == want_len)
        print(f"  {'✅' if match else '❌'} {rel}")
        print(f"      {got_md5} {got_len}B  (期望 {want_md5} {want_len}B)")
        if not match:
            ok = False
    if not ok:
        print("\n❌ 闸1 不通过：本地产物与预期指纹不符，拒绝刷写。")
        print("   若你确实改过固件，请先更新本脚本顶部的 SEGMENTS 期望值并留痕。")
        return 3

    # ── 闸 2：分组刷（0x0 / 0x8000 / 0x10000）──
    # 分区表必须先于 app 写入；esptool write_flash 会按地址从小到大排列，
    # 但显式分组更稳：先 bootloader+分区表，再 app。
    print("\n[闸2] 刷写 0x0 + 0x8000（bootloader 与分区表）")
    boot_args = ["write_flash", "-z"]
    for rel, off, _, _ in SEGMENTS[:2]:
        boot_args += [hex(off), os.path.join(PRODUCT, rel)]
    r = esptool(port, boot_args)
    if r.returncode != 0:
        print(r.stdout[-2000:]); print(r.stderr[-2000:])
        print("❌ 闸2 失败：bootloader/分区表未写成功，**不要继续刷 app**（会留下 bootloader 认不出 app 的半成品状态）。")
        return 4
    print("  ✅ 0x0 / 0x8000 写入完成")

    print("\n[闸2] 刷写 0x10000（app）")
    rel, off, _, _ = SEGMENTS[2]
    r = esptool(port, ["write_flash", "-z", hex(off), os.path.join(PRODUCT, rel)])
    if r.returncode != 0:
        print(r.stdout[-2000:]); print(r.stderr[-2000:])
        return 5
    print("  ✅ 0x10000 写入完成")

    # ── 闸 3：回读逐位对拍（只对拍与本地产物等长的那一段）──
    print("\n[闸3] 回读逐位对拍")
    os.makedirs(READBACK_DIR, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S")
    all_ok = True
    for rel, off, want_md5, want_len in SEGMENTS:
        dst = os.path.join(READBACK_DIR, f"{stamp}_{os.path.basename(rel)}")
        r = esptool(port, ["read_flash", hex(off), str(want_len), dst], timeout=300)
        if r.returncode != 0 or not os.path.exists(dst):
            print(f"  ❌ {rel} 回读失败")
            all_ok = False
            continue
        # 长度必须严格等于镜像长度：多读会带上相邻分区的垃圾字节，
        # 直接 cmp 必报假差异（家族坑）。
        rb_len = os.path.getsize(dst)
        with open(dst, "rb") as f:
            rb = f.read(want_len)
        with open(os.path.join(PRODUCT, rel), "rb") as f:
            want = f.read()
        same = (rb == want)
        print(f"  {'✅' if same else '❌'} {rel}  回读{want_len}B(mtime后{rb_len}B) "
              f"md5={hashlib.md5(rb).hexdigest()}")
        if not same:
            all_ok = False

    print("\n" + "=" * 72)
    if not all_ok:
        print("❌ 回读对拍未全绿。板子状态存疑，**不要拔线**，先查上面的输出。")
        return 6
    print("✅ 三段回读逐位一致。产品固件已恢复。")
    print("   还需人工/脚本确认：单次 rst:0x1（无崩溃重启循环）+")
    print("   日志出现 'LCD 就绪: 320x240'。这两项不归本脚本管。")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    sys.exit(main())
