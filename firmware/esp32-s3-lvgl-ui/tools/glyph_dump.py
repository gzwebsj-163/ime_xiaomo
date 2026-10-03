#!/usr/bin/env python3
"""glyph_dump.py — 把抓屏 PNG 里的汉字按像素 dump 成 ASCII，判断「真字形 vs 豆腐块」。

═══ 为什么要这个工具（2026-10-01 P0 收尾）═══
中文上屏这件事，我先后用错了三个判据，每个都不可靠：

  ① 「非黑像素数 > 0」   → 豆腐块也是非黑像素，恒真，零判别力
  ② 「OCR 读出汉字」     → 16px 点阵中文在 macOS Vision 上识别率极低，
                            实测真字上屏却读成 "DORFEC A1ABESZPO" 这类拉丁乱码
                            ⇒ **判据工具坏了，不是产品坏了**
  ③ 「看着像字」         → 我肉眼也会把乱码看成「有内容」，无法自证

真正能判别的是**结构**，而且必须在「单字」尺度上判 —— 我第一版犯的错就是算在整行上，
一行里 4 个方框之间的空隙把方差撑高，合成豆腐块被判成了「真字形」，检测器当场失效。

改用更硬的判别原理：**豆腐块是空心矩形**（四边框满、内部空），
真汉字是实心笔画（内部有大量墨）。于是按列把一行切成单字格，逐格算：

  F1 边框墨迹率 border —— 豆腐块 ≈ 1.0（四条边都满）
  F2 中心墨迹率 core   —— 豆腐块 ≈ 0.0（空心）；真字显著 > 0
  判定 BOX ⇔ border > 0.70 且 core < 0.06

这个判据有天然的对照：合成一张豆腐块图必须被抓出来，
否则说明检测器坏了，此时对真图的「未检出」没有任何证明力。

用法：
  ./tools/glyph_dump.py <png> [y0] [y1] [x0] [x1]     # 指定区域 dump
  ./tools/glyph_dump.py <png> --lines                  # 自动切行 + 逐字判定
"""
import sys
from PIL import Image

INK = 30          # 亮度阈值：RGB 之和 < 30 算背景
GAP = 4           # 行间距超过这个值视为换行


def ink_map(im):
    """返回二维 0/1 墨迹图。"""
    px = im.load()
    W, H = im.size
    return [[1 if sum(px[x, y]) > INK else 0 for x in range(W)] for y in range(H)]


def features(rows):
    """给一组像素行算三个可判别特征。"""
    if not rows:
        return None
    h = len(rows)
    w = max(len(r) for r in rows)
    total = sum(sum(r) for r in rows)
    per_row = [sum(r) for r in rows]
    # F1 墨迹密度
    density = total / (h * w)
    # F2 每行是否有墨（真字通常首尾行墨迹少，豆腐块每行都有一条边）
    nonzero_rows = sum(1 for n in per_row if n > 0)
    # F3 逐行墨迹数的方差（豆腐块≈0，真字起伏大）
    mean = total / h
    var = sum((n - mean) ** 2 for n in per_row) / h
    return dict(h=h, w=w, total=total, density=density,
                nonzero_rows=nonzero_rows, var=var, per_row=per_row)


def split_cells(row, gap=1):
    """按列把一行墨迹切成单字格。相邻字间隔 <= gap 视为同一格。"""
    cells, start = [], None
    blank = 0
    for i, v in enumerate(row):
        if v:
            if start is None:
                start = i
            blank = 0
        else:
            if start is not None:
                blank += 1
                if blank > gap:
                    cells.append((start, i - blank))
                    start = None
                    blank = 0
    if start is not None:
        cells.append((start, len(row) - 1))
    return cells


def cell_features(cells_rows):
    """cells_rows = [[v,...], ...]（只含该格覆盖的列）。返回 border/core 墨迹率。"""
    h = len(cells_rows)
    w = len(cells_rows[0]) if h else 0
    if h < 3 or w < 3:
        return None
    # 边框：最上/最下行 + 最左/最右列
    border = []
    for y in range(h):
        border.append(cells_rows[y][0])
        border.append(cells_rows[y][w - 1])
    for x in range(w):
        border.append(cells_rows[0][x])
        border.append(cells_rows[h - 1][x])
    # 中心：内缩 50% 的区域
    y0, y1 = int(h * 0.25), max(int(h * 0.25) + 1, int(h * 0.75))
    x0, x1 = int(w * 0.25), max(int(w * 0.25) + 1, int(w * 0.75))
    core = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            core.append(cells_rows[y][x])
    return dict(border=sum(border) / len(border),
                core=sum(core) / len(core),
                w=w, h=h)


def cell_verdict(f):
    if f is None:
        return '太窄/太矮(可能是标点或空白)'
    if f['border'] > 0.70 and f['core'] < 0.06:
        return '豆腐块(BOX)'
    return '字形(GLYPH)'


def render(rows, x0, w):
    for y, r in enumerate(rows):
        line = ''.join('#' if r[i] else ' ' for i in range(min(w, len(r))))
        print(f'{y:3d}|{line}|')


def verdict(f):
    """豆腐块特征：低方差 + 中等密度 + 几乎每行都有墨。
    真汉字：方差大（笔画横竖疏密不均）。"""
    v = f['var']
    d = f['density']
    # 经验阈值：豆腐块的 per_row 方差接近 0
    is_box = (v < 6.0) and (0.10 < d < 0.30) and (f['nonzero_rows'] >= f['h'] - 1)
    if is_box:
        return '豆腐块(BOX)', '方差极低+空心框特征'
    if d < 0.05:
        return '空/无墨', '密度过低'
    return '真字形(GLYPH)', f'方差{v:.1f} 密度{d:.2f}'


def analyze_row(m, y0, y1):
    """把 [y0,y1] 行切成单字格，逐格判定。返回 (各格详情, 豆腐块数, 字形数)。"""
    rows = [m[y] for y in range(y0, y1 + 1)]
    cells = split_cells(rows[0])
    out, nbox, nglyph = [], 0, 0
    for (a, b) in cells:
        cr = [r[a:b + 1] for r in rows]
        f = cell_features(cr)
        v = cell_verdict(f)
        if 'BOX' in v:
            nbox += 1
        elif 'GLYPH' in v:
            nglyph += 1
        out.append((a, b, v, f))
    return out, nbox, nglyph


def dump_region(im, y0, y1, x0, x1, label='', show=True):
    m = ink_map(im)
    rows = [m[y][x0:x1] for y in range(y0, y1 + 1)]
    total = sum(sum(r) for r in rows)
    print(f'\n=== {label}  y={y0}..{y1} x={x0}..{x1}  墨迹={total} ===')
    cells, nbox, nglyph = analyze_row(m, y0, y1)
    for (a, b, v, f) in cells:
        if f:
            print(f'    x={x0+a:3d}..{x0+b:3d} w={f["w"]:2d} '
                  f'border={f["border"]:.2f} core={f["core"]:.2f} -> {v}')
        else:
            print(f'    x={x0+a:3d}..{x0+b:3d} (太窄) -> {v}')
    print(f'    -- 本行: 字形 {nglyph}, 豆腐块 {nbox} --')
    if show:
        render(rows, x0, x1 - x0)
    return nbox, nglyph


def auto_lines(im):
    """自动切出文字行：逐行统计墨迹，间隔 > GAP 视为新行。"""
    m = ink_map(im)
    H, W = len(m), len(m[0])
    counts = [sum(r) for r in m]
    lines, start = [], None
    for y in range(H):
        if counts[y] > 0 and start is None:
            start = y
        elif counts[y] == 0 and start is not None:
            if y - start >= 4:
                lines.append((start, y - 1))
            start = None
    if start is not None and H - start >= 4:
        lines.append((start, H - 1))
    return lines


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    img = Image.open(sys.argv[1]).convert('RGB')
    if '--lines' in sys.argv:
        lines = auto_lines(img)
        print(f'检出 {len(lines)} 个文字行')
        T = B = G = 0
        for (y0, y1) in lines:
            nb, ng = dump_region(img, y0, y1, 0, img.size[0],
                                 f'行 y={y0}..{y1}', show=False)
            B += nb
            G += ng
        print(f'\n=== 汇总: 字形 {G}, 豆腐块 {B} ===')
        if B:
            print('=> 存在豆腐块：字体缺字或解码失败')
            return 2
        print('=> 无豆腐块：所有字格都有实心笔画')
        return 0
    y0, y1, x0, x1 = (int(a) for a in sys.argv[2:6])
    dump_region(img, y0, y1, x0, x1, '指定区域')
    return 0


if __name__ == '__main__':
    sys.exit(main())
