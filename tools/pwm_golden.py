#!/usr/bin/env python3
"""
hw_pwm 黄金值独立对拍 (锚点 A: 独立口径)
------------------------------------------------------------------------------
为什么必须独立写一遍:
  C 侧 selftest 断言的是 pwm_fnv32_reg_raw() == PWM_GOLDEN_REG,
  两端同源。若实现和期望同时被改错, 断言恒成立 —— 这是"自算恒等"的死断言。
  本文件从 FNV-1a-32 规范重新实现, 且**不 include 任何 C 代码**, 只从 include/hw_pwm.h
  里正则抽取常量字面量 —— 即便 C 侧全错, 本脚本仍会算出真值并报差异。

判据: 四个黄金值逐个比对, 任一不等即 FAIL(退出码非 0)。
用法: python3 tools/pwm_golden.py [--verbose]
"""
import re
import sys
import os

FNV_OFF = 2166136261
FNV_PRM = 16777619
M32 = 0xFFFFFFFF
OUT_MASK = 0x7FFFFFFF


def fnv_feed(h: int, b: int) -> int:
    h = (h ^ (b & 0xFF)) & M32
    return (h * FNV_PRM) & M32


def fnv_u32(h: int, v: int) -> int:
    """小端 4 字节喂入 (C 侧 pwm_fnv32_u32 写死小端, 端序必须一致)"""
    for i in range(4):
        h = fnv_feed(h, (v >> (8 * i)) & 0xFF)
    return h


def duty_encode(pct: int) -> int:
    """(pct*65535 + 50) / 100 四舍五入, 与 C 同款算式"""
    assert 0 <= pct <= 100
    return (pct * 65535 + 50) // 100


def extract_consts(header_path: str) -> dict:
    """从真头抽取常量字面量 —— 真头才是被测物的真相源, 不是 .c 里的副本"""
    src = open(header_path, "r", encoding="utf-8", errors="replace").read()
    # 只认带 U/u 后缀或纯十六进制的大写宏, 避免误抓注释
    consts = {}
    for m in re.finditer(
        r"^\s*#define\s+(HW_PWM_\w+|PWM_(?:REG_MAX|PIN_MAX|CH_MAX|DUTY_MAX|DUTY_SCALE))\s+"
        r"(0[xX][0-9A-Fa-f]+|\d+)[uU]?\b",
        src,
        re.M,
    ):
        consts[m.group(1)] = int(m.group(2), 0)
    # 黄金值单列一条 —— 上一版把 GOLDEN 混进上面的 alternation 时漏抽,
    # 而漏抽会让"期望值"变成 None, 判据自己瞎掉却不报警。
    for m in re.finditer(
        r"^\s*#define\s+(PWM_GOLDEN_\w+)\s+(0[xX][0-9A-Fa-f]+|\d+)[uU]?\b",
        src,
        re.M,
    ):
        consts[m.group(1)] = int(m.group(2), 0)
    return consts


def main() -> int:
    verbose = "--verbose" in sys.argv
    # 路径必须用 abspath: 直接拿 __file__ 做 rsplit("/tools/") 时,
    # 若以相对路径 `python3 tools/pwm_golden.py` 启动, split 不中, root 会
    # 退化成整个脚本路径 -> NotADirectoryError。上轮就是这么白跑一轮的。
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    header = os.path.join(root, "include", "hw_pwm.h")
    c = extract_consts(header)

    need = ["HW_PWM_FREQUENCY", "HW_PWM_DC", "HW_PWM_DDC", "HW_PWM_OEP",
            "HW_PWM_EMC", "HW_PWM_OPEN", "HW_PWM_PUT", "HW_PWM_POST",
            "HW_PWM_VPP", "HW_PWM_GND", "PWM_REG_MAX", "PWM_PIN_MAX"]
    missing = [k for k in need if k not in c]
    if missing:
        print("[FAIL] 真头里抽不到常量: %s" % missing)
        print("       -> 抽取口径失效, 这不是'黄金值错', 是工具瞎了 (锚点 F)")
        return 2

    reg = [c["HW_PWM_DC"], c["HW_PWM_DDC"], c["HW_PWM_OEP"], c["HW_PWM_EMC"],
           c["HW_PWM_OPEN"], c["HW_PWM_PUT"], c["HW_PWM_POST"]]
    pin = [c["HW_PWM_VPP"], c["HW_PWM_GND"]]

    if len(reg) != c["PWM_REG_MAX"]:
        print("[FAIL] reg 表长度 %d != PWM_REG_MAX %d" % (len(reg), c["PWM_REG_MAX"]))
        return 2
    if len(pin) != c["PWM_PIN_MAX"]:
        print("[FAIL] pin 表长度 %d != PWM_PIN_MAX %d" % (len(pin), c["PWM_PIN_MAX"]))
        return 2

    # 四个黄金, 口径与 C 侧逐条对应
    h = FNV_OFF
    for v in reg:
        h = fnv_u32(h, v)
    calc_reg = h

    h = FNV_OFF
    for v in pin:
        h = fnv_u32(h, v)
    calc_pin = h

    h = FNV_OFF
    for pct in (0, 25, 50, 75, 100):
        h = fnv_u32(h, duty_encode(pct))
    calc_vec = h

    h = FNV_OFF
    for v in reg:
        h = fnv_u32(h, v)
    for v in pin:
        h = fnv_u32(h, v)
    h = fnv_u32(h, c["HW_PWM_FREQUENCY"])
    calc_all = h

    expect = {
        "REG": (calc_reg, c.get("PWM_GOLDEN_REG")),
        "PIN": (calc_pin, c.get("PWM_GOLDEN_PIN")),
        "VEC": (calc_vec, c.get("PWM_GOLDEN_VEC")),
        "ALL": (calc_all, c.get("PWM_GOLDEN_ALL")),
    }

    # 期望值也要从真头抽, 抽不到就是工具问题而不是判 FAIL
    for k, (calc, rec) in expect.items():
        if rec is None:
            print("[FAIL] 真头里抽不到 PWM_GOLDEN_%s" % k)
            return 2

    ok = True
    for k in ("REG", "PIN", "VEC", "ALL"):
        calc, rec = expect[k]
        good = (calc == rec)
        ok = ok and good
        if verbose or not good:
            print("  %-4s 独立算得=0x%08X  头里记录=0x%08X  %s"
                  % (k, calc, rec, "OK" if good else "<<< 不一致"))
        # 掩码出口: 记录值 > 2^31 时, 公共出口应被截断
        if rec & 0x80000000:
            if verbose:
                print("       出口(带 mask)=0x%08X  (记录跨 2^31, 掩码非空操作)"
                      % (rec & OUT_MASK))

    # 独立口径的意义之一: 自己算的值必须【不等于】被测实现才叫对拍。
    # 这里再加一道自检 —— 若把 FNV 初始值改错, 本脚本应该"算出一个不同的值",
    # 说明它真的在算而不是在复读常量。
    sentinel = fnv_u32(FNV_OFF, reg[0])
    if sentinel == calc_reg:
        print("[FAIL] 自检: 单元素 FNV 等于全表 FNV, 计算逻辑可疑")
        return 2

    if not ok:
        print("[FAIL] 黄金值独立对拍不一致 -> C 侧实现或记录有错")
        return 1
    print("[PASS] 4 个黄金值独立对拍一致 (REG/PIN/VEC/ALL)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
