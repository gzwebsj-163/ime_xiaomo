#!/usr/bin/env python3
"""panel_stream_golden.py — SPI 字节流黄金（协议层独立对拍）

与 panel_lcd.c 的「流哈希」逐位对拍。**独立实现**：本文件按 ST7789 规范
从零重写一遍「驱动应该吐什么字节」，因此能抓出 C 侧的笔误（命令/参数/字节序/
窗口偏移/每帧像素数），而不只是把 C 的结果抄一遍。

规范（两边必须一致）：
  boot      : 复位(无字节) → init 表逐条 [cmd, d0..d(len-1)] → MADCTL(0x36, v)
  set_window: 0x2A, x0+xoff, x1+xoff (各 2B 大端), 0x2B, y0+yoff, y1+yoff, 0x2C
  fill(rgb) : set_window(0,0,w-1,h-1) + w*h 个像素（每个 2B 大端 rgb565）
  MADCTL    : rot 0→base, 90→base|0x60(MV|MX), 180→base|0xC0(MX|MY), 270→base|0xA0(MV|MY)
  偏移      : rot 90/270 时 x_off/y_off **对调**（MV 把 X/Y 轴换了）
  w/h       : rot 90/270 时对调

流里只含 SDA 上的字节；延时/CS/DC 时序不进流（属示波器范畴）。
哈希 = FNV-1a-32。
"""

FNV_OFFSET = 0x811C9DC5
FNV_PRIME = 0x01000193
MASK32 = 0xFFFFFFFF


def fnv1a32(data: bytes, h: int = FNV_OFFSET) -> int:
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK32
    return h


# ---------------- init 表（独立誊写，笔误即被抓） ----------------
INIT_240x320 = [
    (0x01, []), (0x11, []), (0x3A, [0x05]),
    (0xB2, [0x0C, 0x0C, 0x00, 0x33, 0x33]),
    (0xB7, [0x35]), (0xBB, [0x28]), (0xC0, [0x2C]), (0xC2, [0x01]),
    (0xC3, [0x12]), (0xC4, [0x20]), (0xC6, [0x0F]), (0xD0, [0xA4, 0xA1]),
    (0xE0, [0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23]),
    (0xE1, [0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23]),
    (0x21, []), (0x13, []), (0x29, []),
]

INIT_240x240 = [
    (0x01, []), (0x11, []), (0x3A, [0x05]),
    (0xB2, [0x0C, 0x0C, 0x00, 0x33, 0x33]),
    (0xB7, [0x35]), (0xBB, [0x19]), (0xC0, [0x2C]), (0xC2, [0x01]),
    (0xC3, [0x12]), (0xC4, [0x20]), (0xC6, [0x0F]), (0xD0, [0xA4, 0xA1]),
    (0x21, []), (0x13, []), (0x29, []),
]

# ---------------- 档案（独立誊写） ----------------
PANELS = {
    "GM1020-05-10P": dict(w=240, h=320, x_off=0, y_off=0, madctl=0x00, init=INIT_240x320),
    "GM1020-05-14P": dict(w=240, h=320, x_off=0, y_off=0, madctl=0x00, init=INIT_240x320),
    "GM13-240x240":  dict(w=240, h=240, x_off=0, y_off=80, madctl=0x00, init=INIT_240x240),
}

RED = 0xF800  # RGB565 纯红


def madctl_of(base: int, rot: int) -> int:
    return {0: base, 90: base | 0x60, 180: base | 0xC0, 270: base | 0xA0}[rot]


def be16(v: int) -> bytes:
    return bytes(((v >> 8) & 0xFF, v & 0xFF))


class Stream:
    """驱动应该吐出的字节流（独立模型）"""

    def __init__(self):
        self.buf = bytearray()

    def cmd(self, c):
        self.buf.append(c & 0xFF)

    def dat8(self, d):
        self.buf.append(d & 0xFF)

    def dat16(self, v):
        self.buf += be16(v)

    # 与 C 侧 panel_lcd_set_window 对应
    def set_window(self, x0, y0, x1, y1, xoff, yoff):
        self.cmd(0x2A)
        self.dat16(x0 + xoff)
        self.dat16(x1 + xoff)
        self.cmd(0x2B)
        self.dat16(y0 + yoff)
        self.dat16(y1 + yoff)
        self.cmd(0x2C)

    def boot(self, p, rot):
        for c, data in p["init"]:
            self.cmd(c)
            for d in data:
                self.dat8(d)
        self.cmd(0x36)
        self.dat8(madctl_of(p["madctl"], rot))

    def fill(self, rgb, w, h, xoff, yoff):
        self.set_window(0, 0, w - 1, h - 1, xoff, yoff)
        px = be16(rgb)
        self.buf += px * (w * h)


def expected(p, rot):
    """返回 (boot, window, fill) 三个阶段的 (len, csum)"""
    swap = rot in (90, 270)
    xoff = p["y_off"] if swap else p["x_off"]
    yoff = p["x_off"] if swap else p["y_off"]
    w = p["h"] if swap else p["w"]
    h = p["w"] if swap else p["h"]

    s = Stream()
    s.boot(p, rot)
    r = [("boot", len(s.buf), fnv1a32(bytes(s.buf)))]

    s.set_window(0, 0, w - 1, h - 1, xoff, yoff)
    r.append(("window", len(s.buf), fnv1a32(bytes(s.buf))))

    s.fill(RED, w, h, xoff, yoff)
    r.append(("fill", len(s.buf), fnv1a32(bytes(s.buf))))
    return r, w, h, xoff, yoff


def main():
    print("== panel_stream_golden.py (独立 Python 模型) ==")
    n = 0
    for pid, p in PANELS.items():
        for rot in (0, 90, 180, 270):
            stages, w, h, xoff, yoff = expected(p, rot)
            print(f"ARCHIVE {pid:14s} rot {rot:3d}  w={w} h={h}")
            for nm, ln, cs in stages:
                print(f"  {nm:7s} len={ln} csum=0x{cs:08X}")
            n += 1
    print(f"STREAM GOLDEN: {n} cases")


if __name__ == "__main__":
    main()
