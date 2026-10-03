#!/usr/bin/env python3
"""看每个泄漏帧**前面那一行**是什么文本。
若帧总是跟在同一类行后面 => 落点由「哪一行刚写完」决定, 而非随机字节位置。
"""
import re, sys

ANSI = re.compile(rb'\x1b\[[0-9;]*[A-Za-z]')
path = sys.argv[1]
raw = open(path, 'rb').read()
clean = ANSI.sub(b'', raw)          # 注意: 偏移会变, 只能在 clean 上做行级分析

# 逐行切, 同时记录每行内是否存在 AA55+DMCP
lines, cur, off = [], bytearray(), 0
for b in clean:
    cur.append(b)
    if b == 0x0A:
        lines.append((off, bytes(cur)))
        off += len(cur)
        cur = bytearray()
if cur:
    lines.append((off, bytes(cur)))

from collections import Counter
pat = Counter()
n = 0
for idx, (o, ln) in enumerate(lines):
    if b'\xaa\x55' in ln and b'DMCP' in ln:
        n += 1
        prev = lines[idx-1][1].decode('utf-8', 'replace').strip() if idx else '<BOF>'
        # 抽掉数字, 只留结构
        key = re.sub(r'\d+', '#', prev)[-90:]
        pat[key] += 1

print(f"含帧的行数 = {n}")
print(f"紧邻前一行的结构种类 = {len(pat)}\n")
for k, v in pat.most_common():
    print(f"  x{v:2d}  {k}")
