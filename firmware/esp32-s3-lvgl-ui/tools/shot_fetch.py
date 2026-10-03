#!/usr/bin/env python3
"""shot_fetch.py — 从串口抓取固件 dump 的屏幕渲染结果，还原成 PNG。

配套：固件需以 -DSHOT_PROBE=1 构建并烧录（同时 CONFIG_LV_USE_SNAPSHOT=y）。

为什么要有这个脚本（2026-09-29）：
  前几轮 UI 改版只能"烧进去 → 让用户看屏幕 → 用户描述问题"。
  布局压字/错位这类问题我无法自证，等于让用户当人眼检测器。
  本脚本把「屏幕真的渲染成什么样」变成可以本地断言的事实。

用法：
  ./tools/shot_fetch.py [--port /dev/cu.usbmodemXXXX] [--out tmp/shots] [--secs 120]

产出：
  <out>/page0.png … page8.png    逐页屏幕渲染图
  终端打印每页的「非黑像素数 / 与上一页的差异像素数」——
  差异=0 说明该页其实没画出东西（比"没崩就算过"强得多的判据）。
"""
import argparse
import glob
import os
import struct
import sys
import time

import serial
from PIL import Image


def pick_port():
    p = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not p:
        sys.exit("找不到 /dev/cu.usbmodem*，板子插好了吗？")
    return p[0]


def hard_reset(s):
    """esptool reset.py 的 HardReset 序列：DTR 全程 False，RTS 1 → 0.2s → 0。
    不这么做的后果：只能抓到「现在之后」的输出，丢掉 boot banner 与复位原因。"""
    s.dtr = False
    s.rts = True
    time.sleep(0.2)
    s.rts = False
    s.reset_input_buffer()


def rgb565_to_rgb(bb):
    """LE uint16 → RGB888。"""
    px = bytearray(len(bb) // 2 * 3)
    for i in range(len(bb) // 2):
        v = bb[2 * i] | (bb[2 * i + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        px[3 * i] = (r << 3) | (r >> 2)
        px[3 * i + 1] = (g << 2) | (g >> 4)
        px[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(px)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--out", default="tmp/shots")
    ap.add_argument("--secs", type=float, default=150.0)
    ap.add_argument("--raw", default="tmp/shot_raw.bin")
    args = ap.parse_args()

    port = args.port or pick_port()
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(os.path.dirname(args.raw) or ".", exist_ok=True)

    s = serial.Serial(port, 115200, timeout=0.2)
    hard_reset(s)
    print(f"port={port}  抓取中（最多 {args.secs:.0f}s，等 ##DONE）...")

    buf = bytearray()
    pages = {}      # page -> (w,h,stride,cf,rgb888)
    masks = {}      # page -> bytes
    cur_page = None
    done = False

    t0 = time.time()
    while time.time() - t0 < args.secs and not done:
        chunk = s.read(65536)
        if chunk:
            buf += chunk

        # ---- 状态机：只按「精确字节数」推进，不靠猜分隔符 ----
        while True:
            i = buf.find(b"##")
            if i < 0:
                buf.clear()
                break
            if i > 0:
                del buf[:i]          # 丢掉前面的日志文本

            nl = buf.find(b"\n")
            if nl < 0:
                break                # 行还没收全

            line = bytes(buf[:nl])
            if line.startswith(b"##DONE"):
                done = True
                print("收到 ##DONE")
                del buf[:nl + 1]
                break

            if line.startswith(b"##SHOT "):
                # ##SHOT <n> MODE=base|diff
                tok = line.split()
                cur_page = int(tok[1])
                del buf[:nl + 1]
                continue

            if line.startswith(b"##HDR "):
                _, w, h, stride, cf = line.split()
                w, h, stride, cf = int(w), int(h), int(stride), int(cf)
                need = stride * h
                if len(buf) < nl + 1 + need:
                    break            # 载荷没到齐，等下一轮
                pay = bytes(buf[nl + 1:nl + 1 + need])
                del buf[:nl + 1 + need]
                print(f"  page{cur_page}: {w}x{h} stride={stride} cf={cf} {need} B")
                pages[cur_page] = (w, h, stride, cf, pay)
                continue

            if line.startswith(b"##MASK "):
                need = int(line.split()[1])
                if len(buf) < nl + 1 + need:
                    break
                masks[cur_page] = bytes(buf[nl + 1:nl + 1 + need])
                del buf[:nl + 1 + need]
                continue

            if line.startswith(b"##END"):
                del buf[:nl + 1]
                continue

            # 不是协议行（可能是日志里恰好以 ## 开头）→ 跳过这一行
            del buf[:nl + 1]

    s.close()
    open(args.raw, "wb").write(bytes(buf))
    print(f"\n收到 {len(pages)} 页图像、{len(masks)} 个差异掩码")

    if not pages:
        sys.exit("没收到任何图像 —— 检查：固件是否 SHOT_PROBE=1 构建？串口是否被占用（lsof）？")

    for n in sorted(pages):
        w, h, stride, cf, pay = pages[n]
        # 逐行裁掉 stride padding
        rows = b"".join(pay[y * stride: y * stride + w * 2] for y in range(h))
        img = Image.frombytes("RGB", (w, h), rgb565_to_rgb(rows))
        path = os.path.join(args.out, f"page{n}.png")
        img.save(path)

        # 判据①：非黑像素（纯黑背景下的"画面内容量"）
        nonblack = sum(1 for i in range(0, len(rows), 2)
                       if rows[i] or rows[i + 1])

        # 判据②：与上一页的差异像素（差异=0 → 这页其实没画出东西）
        diffmsg = "n/a (首帧)"
        if n in masks:
            m = masks[n]
            cnt = struct.unpack("<I", m[:4])[0]
            diffmsg = f"{cnt}"
        print(f"page{n}.png  {w}x{h}  非黑像素={nonblack:6d}  与上页差异={diffmsg}")


if __name__ == "__main__":
    main()
