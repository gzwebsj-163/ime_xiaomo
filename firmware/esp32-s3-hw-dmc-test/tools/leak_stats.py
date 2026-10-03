#!/usr/bin/env python3
"""检验「泄漏帧 100% 落在 0x0A 之后」是不是巧合。
做法: 算出日志字节流里 0x0A 的占比 p, 再算 19 次全部命中的随机概率 p^19。
若 p^19 极小 => 落点有结构性, 不是随机电气串扰该有的样子。
"""
import sys

path = sys.argv[1]
raw = open(path, 'rb').read()

HDR = b'\xaa\x55'
frames = []
i = 0
while True:
    i = raw.find(HDR, i)
    if i < 0:
        break
    if b'DMCP' in raw[i:i+12]:
        frames.append(i)
    i += 1

# 帧占用的字节从日志里扣除, 得到"纯文本部分"
occupied = set()
for s in frames:
    for k in range(s, min(s + 20, len(raw))):
        occupied.add(k)
text_bytes = [raw[k] for k in range(len(raw)) if k not in occupied]

n = len(text_bytes)
cnt_lf = text_bytes.count(0x0A)
p = cnt_lf / n if n else 0

# 帧长是否恒定(交织是「按 write 边界」还是「按字节随机」的关键旁证)
lens = []
for s in frames:
    ln = raw[s + 2] if s + 2 < len(raw) else None
    lens.append(ln)
print(f"文件 {path}")
print(f"  总字节        = {len(raw)}")
print(f"  泄漏帧数      = {len(frames)}")
print(f"  纯文本字节    = {n}   其中 0x0A 个数 = {cnt_lf}")
print(f"  0x0A 占比 p   = {p:.5f}  (约 1/{1/p:.0f})")
print(f"  随机全命中概率 = p^{len(frames)} = {p**len(frames):.3e}")
print()
print(f"  帧头 LEN 字段取值集合 = {sorted(set(lens))}  (恒定 => 帧长固定 20B)")
seqs = [raw[s+4] for s in frames]
print(f"  帧序号 = {seqs}")
print(f"  序号连续无缺口 = {seqs == list(range(seqs[0], seqs[0]+len(seqs)))}")
