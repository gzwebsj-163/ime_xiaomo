#!/usr/bin/env python3
# 独立实现 FNV-1a-32，给面板档案锁定黄金校验和（与 C 侧 panel_lcd_csum 逐位对拍）
#
# 规范串格式（必须与 panel_lcd.c 的 snprintf 完全一致）：
#   "<id>|<ic>|<W>x<H>|<xoff>,<yoff>|inv<0/1>|0x<COLMOD>|<iface>|<pin_id>:<sig1> <sig2> ..."
def fnv1a32(s: str) -> int:
    h = 0x811C9DC5
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h

SIG_10P = "GND RS CS SCL SDA RESET VDD GND LED+ LED-"
SIG_14P = "NC GND LEDK LEDA GND RESET DC SDA SCL VCC IOVCC CS GND NC"

panels = {
    # ★ 当前实物：2.0" 240x320 竖屏，10 脚（用户定义）
    "GM1020-05-10P": "GM1020-05-10P|ST7789P3|240x320|0,0|inv1|0x05|4wireSPI|10P:" + SIG_10P,
    # 同一块 2.0" 屏的 14 脚插接版（淘宝详情图 OCR，保留对照）
    "GM1020-05-14P": "GM1020-05-14P|ST7789P3|240x320|0,0|inv1|0x05|4wireSPI|14P:" + SIG_14P,
    # 旧 1.3" 240x240 方屏，10 脚，RAM 偏移 0,80
    "GM13-240x240": "GM13-240x240|ST7789V|240x240|0,80|inv1|0x05|4wireSPI|10P:" + SIG_10P,
}

for k, v in panels.items():
    print(f"{k:16s} len={len(v.encode()):3d}  FNV-1a-32 = 0x{fnv1a32(v):08X}")
