#!/usr/bin/env python3
"""分析 DMC 帧泄漏进 USB-Serial-JTAG 日志流的现象。

要回答的问题(OPEN 项): 泄漏帧是否系统性落在换行符之后?
判据: 找 0xAA 0x55 头, 校验其后 4 字节是否含 'DMCP' 魔数,
      再看该头的前一个字节是不是 0x0A。
输出 阳性/阴性 两组, 便于做**镜像内双向对照**(锚点 H: 要有阴阳两侧)。
"""
import re, sys, glob

HDR = b'\xaa\x55'

def scan(path):
    raw = open(path, 'rb').read()
    # 日志自身是文本流, 帧是二进制, 直接按字节找
    frames = []
    i = 0
    while True:
        i = raw.find(HDR, i)
        if i < 0:
            break
        win = raw[i:i+12]
        if b'DMCP' in win:
            # 前一字节: 是换行/回车 => "落在行尾后"
            prev = raw[i-1] if i > 0 else None
            prev2 = raw[i-2] if i > 1 else None
            after_nl = (prev in (0x0A, 0x0D))
            # 后一字节: 帧结束处后面跟的文本起始
            frames.append((i, win.hex(' '), after_nl, prev, prev2))
        i += 1
    return raw, frames

for path in sys.argv[1:]:
    raw, fr = scan(path)
    total = len(raw)
    print(f"===== {path}  raw={total}B =====")
    print(f"命中 AA55+DMCP 帧数 = {len(fr)}")
    nl = [f for f in fr if f[2]]
    nn = [f for f in fr if not f[2]]
    print(f"  前一字节是换行/回车 : {len(nl)}")
    print(f"  前一字节不是换行     : {len(nn)}")
    for f in fr:
        print(f"   @{f[0]:6d} after_nl={f[2]!s:5} prev={f[3]!r} win={f[1]}")
    print()
