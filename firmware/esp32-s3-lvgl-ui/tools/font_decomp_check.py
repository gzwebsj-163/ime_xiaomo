#!/usr/bin/env python3
"""font_decomp_check.py — 独立解码 LVGL 压缩字体的 C 数组，渲染字形为 ASCII。

为什么要有这个（2026-10-01 P0 事故复盘）：
    本次 P0 的根因是「字是压缩格式，但 CONFIG_LV_USE_FONT_COMPRESSED 没开」
    ⇒ 解码器被 #if 掉 ⇒ 每个字形返回 NULL ⇒ 一个字都不上屏。
    而设备端的「字形覆盖自检」却 4118 字次全绿，因为它只查 glyph_dsc 表，
    **不碰解压**。判据选了太窄的一条路，于是 100% 失败被报成 100% 成功。

    本脚本是**独立口径**：不复用 LVGL 任何代码，纯 Python 重实现
    rle_next / prefilter / opa_table，把字形画出来。
    ⇒ 若 Python 能画出「探」，而设备画不出，那问题 100% 在 LVGL 侧配置，
      不在字体数据。这是「不许 C 自己算自己」的应用。

    反向价值同样重要：若哪天 Python 也画不出，说明字体数据本身坏了，
    就不用再花时间查配置。

用法:
    ./tools/font_decomp_check.py                       # 渲染一组默认汉字
    ./tools/font_decomp_check.py 探针 烧录 菜单        # 渲染指定字
"""
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ── LVGL 的解码算法（逐行对照 managed_components/lvgl__lvgl/src/font/fmt_txt/lv_font_fmt_txt.c）──
OPA2 = [0, 85, 170, 255]          # opa2_table，bpp=2

SINGLE, REPEATED, COUNTER = 0, 1, 2


def get_bits(buf, bit_pos, length):
    """lv_font_fmt_txt.c: get_bits()。注意跨字节时读 16 位再移位。"""
    mask = (1 << length) - 1
    byte_pos = bit_pos >> 3
    bit_pos &= 0x7
    if bit_pos + length >= 8:
        in16 = (buf[byte_pos] << 8) + buf[byte_pos + 1]
        return (in16 >> (16 - bit_pos - length)) & mask
    return (buf[byte_pos] >> (8 - bit_pos - length)) & mask


class RLE:
    """lv_font_fmt_txt.c: rle_init() + rle_next() 的逐字翻译。"""

    def __init__(self, buf, bpp):
        self.buf, self.bpp = buf, bpp
        self.state = SINGLE
        self.rdp = 0
        self.prev_v = 0
        self.count = 0

    def next(self):
        ret = 0
        if self.state == SINGLE:
            ret = get_bits(self.buf, self.rdp, self.bpp)
            if self.rdp != 0 and self.prev_v == ret:
                self.count = 0
                self.state = REPEATED
            self.prev_v = ret
            self.rdp += self.bpp
        elif self.state == REPEATED:
            v = get_bits(self.buf, self.rdp, 1)
            self.count += 1
            self.rdp += 1
            if v == 1:
                ret = self.prev_v
                if self.count == 11:
                    self.count = get_bits(self.buf, self.rdp, 6)
                    self.rdp += 6
                    if self.count != 0:
                        self.state = COUNTER
                    else:
                        ret = get_bits(self.buf, self.rdp, self.bpp)
                        self.prev_v = ret
                        self.rdp += self.bpp
                        self.state = SINGLE
            else:
                ret = get_bits(self.buf, self.rdp, self.bpp)
                self.prev_v = ret
                self.rdp += self.bpp
                self.state = SINGLE
        elif self.state == COUNTER:
            ret = self.prev_v
            self.count -= 1
            if self.count == 0:
                ret = get_bits(self.buf, self.rdp, self.bpp)
                self.prev_v = ret
                self.rdp += self.bpp
                self.state = SINGLE
        return ret


def decompress(buf, w, h, bpp=2, prefilter=True):
    """lv_font_fmt_txt.c: decompress()。返回 h 行 × w 列的不透明度值(0..3)。"""
    rle = RLE(buf, bpp)
    line1 = [rle.next() for _ in range(w)]
    rows = [list(line1)]
    for _ in range(1, h):
        line2 = [rle.next() for _ in range(w)]
        if prefilter:
            line1 = [line2[x] ^ line1[x] for x in range(w)]
            rows.append(list(line1))
        else:
            rows.append(line2)
    return rows


# ── 解析 lv_font_conv 生成的 C 数组 ──
def parse_font(path):
    src = open(path, encoding='utf-8').read()

    # glyph_bitmap
    m = re.search(r'glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    bitmap = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(1)))

    # glyph_dsc
    m = re.search(r'glyph_dsc\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    dsc = []
    for line in m.group(1).splitlines():
        mm = re.search(
            r'\{\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*'
            r'\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*'
            r'\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\s*\}', line)
        if mm:
            dsc.append(tuple(int(g) for g in mm.groups()))

    # bpp / bitmap_format
    bpp = int(re.search(r'\.bpp\s*=\s*(\d+)', src).group(1))
    fmt = int(re.search(r'\.bitmap_format\s*=\s*(\d+)', src).group(1))

    # cmaps：unicode_list_N（SPARSE_TINY）
    cmaps = []
    for name, body in re.findall(
            r'static const uint16_t (unicode_list_\d+)\[\]\s*=\s*\{(.*?)\n\};', src, re.S):
        cmaps.append((name, [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]+)', body)]))

    # 区间信息：range_start / range_length / glyph_id_start / list_length
    ranges = []
    for blk in re.findall(r'\{\s*\.range_start.*?\n\s*\}', src, re.S):
        rs = int(re.search(r'\.range_start\s*=\s*(\d+)', blk).group(1))
        rl = int(re.search(r'\.range_length\s*=\s*(\d+)', blk).group(1))
        gs = int(re.search(r'\.glyph_id_start\s*=\s*(\d+)', blk).group(1))
        ll = int(re.search(r'\.list_length\s*=\s*(\d+)', blk).group(1))
        un = re.search(r'unicode_list\s*=\s*(unicode_list_\d+)', blk)
        ranges.append((rs, rl, gs, ll, un.group(1) if un else None))

    return dict(bitmap=bitmap, dsc=dsc, bpp=bpp, fmt=fmt, ranges=ranges, cmaps=dict(cmaps))


def gid_of(cp, f):
    """逐字翻译 lv_font_fmt_txt.c: get_glyph_dsc_id()（v9 语义，2026-10-01 修正）。

    ⚠️ 修正前的实现把 SPARSE_TINY 当成「按码位索引的稠密表、用表里的值当 offset」：
         return gs + lst[cp - rs]
       这是错的。LVGL 实际语义是：
         · unicode_list 是**升序排列的 rcp 值列表**（不是稠密表）
         · 对 rcp 做**二分查找**（lv_utils_bsearch）
         · glyph_id = glyph_id_start + **命中位置的下标**（不是列表里的值）
       旧实现对真实字体必然越界（list_length=3189，而 rcp 可达 35952），
       所以「独立验证器」本身算不出任何汉字 —— 一个坏尺子量不了任何东西。
    """
    if cp == 0:
        return 0
    for rs, rl, gs, ll, uname in f['ranges']:
        rcp = cp - rs
        if rcp >= rl:                       # 与 LVGL 的 `if(rcp >= range_length) continue` 同义
            continue
        if uname is None:                   # FORMAT0_TINY：顺序映射
            return gs + rcp
        # SPARSE_TINY：升序 rcp 列表上二分查找，offset = 下标
        lst = f['cmaps'][uname]
        lo, hi = 0, len(lst)
        while lo < hi:
            mid = (lo + hi) // 2
            if lst[mid] < rcp:
                lo = mid + 1
            else:
                hi = mid
        if lo < len(lst) and lst[lo] == rcp:
            return gs + lo
        return 0                            # 命中区间但无字形（LVGL 同样返回 0）
    return 0


def render(f, ch, pad=0):
    cp = ord(ch)
    gid = gid_of(cp, f)
    if gid == 0 or gid >= len(f['dsc']):
        return None, f'U+{cp:04X} 无字形(gid={gid})'
    bi, adv, bw, bh, ox, oy = f['dsc'][gid]
    if bw == 0 or bh == 0:
        return None, f'U+{cp:04X} gid={gid} 空字形(空白字符)'
    rows = decompress(f['bitmap'][bi:], bw, bh, f['bpp'], f['fmt'] == 1)
    return rows, f'U+{cp:04X} gid={gid} box={bw}x{bh} adv={adv} ofs=({ox},{oy})'


def main():
    args = sys.argv[1:]
    path = os.path.join(ROOT, 'main', 'fonts', 'ui_cjk_16.c')
    f = parse_font(path)
    print(f'字体: {os.path.basename(path)}  bpp={f["bpp"]}  bitmap_format={f["fmt"]}'
          f'  ({"压缩" if f["fmt"] == 1 else "未压缩"})  glyph_dsc={len(f["dsc"])} 项')
    chars = args or list('探针烧录菜单中文')
    bad = 0
    for ch in chars:
        rows, info = render(f, ch)
        print(f'\n── {ch}  {info}')
        if rows is None:
            bad += 1
            continue
        for r in rows:
            print('   ' + ''.join(' .:-=+*#%@'[v] for v in r))
    print(f'\n独立解码结果: {len(chars)-bad}/{len(chars)} 字成功渲染')
    if bad:
        print('⇒ 有字解不出来 → 字体数据本身有问题，去查 gen_font.py')
    else:
        print('⇒ 字体数据完好。设备画不出 ⇒ 问题在 LVGL 侧（配置/解码器），不在字体。')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
