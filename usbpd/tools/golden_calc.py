# golden_calc.py -- 【独立口径】重算 FNV-1a-32 黄金值
# 为什么需要它: 自校验自己算自己 = 自证不成立 (锚点 J / 独立口径)。
# 本脚本用 Python 独立实现 FNV-1a-32, 对同一份档案结构重新计算,
# 两边不一致就说明至少有一边错。
import struct, sys

FNV_OFFSET = 0x811C9DC5
FNV_PRIME  = 0x01000193
TAP_NAME   = "19k/10k"
TAP_RATIO  = 2960
TAP_RAIL   = 2850
TAP_QUIET  = 30

# 口径: 只哈希【标量字段】ratio/rail/quiet, 共 12 字节 <Iii 小端>。
# 与 C 侧 hw_usbpd_selftest() [11] 完全同口径 => 两边必须相等。
# 🕳️ 早期 C 侧哈希整个 struct (含 name 指针), 指针受 ASLR 影响 =>
#    指纹每次都变 = 实际上没有黄金值。这是本脚本存在的理由。
payload = struct.pack('<Iii', TAP_RATIO, TAP_RAIL, TAP_QUIET)

h = FNV_OFFSET
for b in payload:
    h ^= b
    h = (h * FNV_PRIME) & 0xFFFFFFFF

print("name       =", TAP_NAME)
print("ratio_milli=", TAP_RATIO)
print("raw_rail   =", TAP_RAIL)
print("quiet_mv   =", TAP_QUIET)
print("FNV-1a-32 over <Iii> = 0x%08X" % h)
print("期望 (HW_USBPD_GOLDEN) = 0xC481F6E5")
ok = (h == 0xC481F6E5)
print("独立口径比对:", "MATCH ✅" if ok else "MISMATCH ❌")
sys.exit(0 if ok else 1)
