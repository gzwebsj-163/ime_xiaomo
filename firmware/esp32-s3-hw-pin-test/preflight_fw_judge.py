#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
固件↔判读器 一致性预检（刷板之前必须先跑，全绿才允许刷）

为什么需要这个：
  判读器 judge_spiab.py 的正则，是照着**源码里的 printf 格式串**手写的。
  两者可能悄悄对不上（改过固件忘了改判读器 / 改过判读器忘了改固件 /
  空格数变了 / ANSI 转义插在中间），而这种不一致**在真机上才会暴露**：
  表现为「日志要素缺失」，很容易被误判成「采样法失灵」或「刷错固件」，
  白烧一轮板子。

🔴 方法论要点（本脚本的核心纪律）：
  **不要用人工编造的字符串去验判读器。**
  必须从**固件二进制本体**抽出真实格式串再喂给判读器的正则 ——
  否则验的是「我以为固件长什么样」，不是「固件真的长什么样」。
  2026-10-03 本轮就出现过：先手工造串验过 26/0 全绿，
  换成从固件抽真串后才发现格式串里是**两个空格**、且带 ANSI 转义。

🔴 检索纪律：判断「某串在不在固件里」**一律用字节口径** `b.count(s.encode())`。
  固件含大量非文本字节，`decode(errors='replace')` 会插入 U+FFFD 改变字符数，
  用它做 count 会得到与字节口径不一致的结果（锚点 F 第四形态）。
"""
import hashlib
import os
import re
import re as _re
import subprocess
import sys

FIRMWARE = "/Users/root1/cow/esp32-s3-hw-pin-test/build_spiab5/hw_pin_s3_test.bin"
EXPECT_MD5 = "82cea3f46e0b83ac8a461cfb146844e6"

# ── 判读器里的关键常量（必须与 judge_spiab.py 保持同步）──
G2_SELF_MIN = 190
# 判读器第 90 行的正则，逐字照抄（改判读器时这里必须同步改）
RE_G2_OK = re.compile(
    r"\[AB0\] 采样法自证 OK\s+驱动低读到低\s+(\d+)/200,\s*驱动高读到高\s+(\d+)/200")
RE_G2_FAIL = re.compile(
    r"\[AB0\] 采样法自证 FAIL\s+驱动低读到低\s+(\d+)/200,\s*驱动高读到高\s+(\d+)/200")


def extract_arm_formats(b):
    """从固件二进制里抽出含 [AB0]/[AB1]/[AB2] 的真实 printf 格式串片段。
    只用于**人眼显示**：固件含非文本字节，这里宽松解码。"""
    out = {}
    for tag in (b"[AB0]", b"[AB1]", b"[AB2]"):
        for m in re.finditer(re.escape(tag), b):
            disp = b[m.start():m.start() + 130].decode("utf-8", "ignore")
            out.setdefault(tag.decode(), []).append(disp)
    return out


def main():
    print("=" * 74)
    print("固件↔判读器 一致性预检 · 刷板前闸门")
    print("=" * 74)

    # ── 闸 A：产物指纹（认指纹不认文件名）──
    b = open(FIRMWARE, "rb").read()
    got = hashlib.md5(b).hexdigest()
    if got != EXPECT_MD5:
        print(f"\n❌ 闸A 产物指纹不符：{got}（期望 {EXPECT_MD5}）")
        print("   固件已变 ⇒ 判读器可能已与它对不上。**先重跑一致性检查**。")
        return 2
    print(f"\n[闸A] 产物指纹 {got[:12]}… {len(b)}B ✅")

    # ── 闸 B：G1 所需字面量在固件内可解析（全部字节口径）──
    print("\n[闸B] G1 产物身份标记（字节口径）")
    g1_items = [
        ("AB0 标记", "[AB0]"),
        ("中文标题", "SPI 侧 GPIO Matrix A/B 实测"),
        ("反例 [C1]", "[C1]"),            # 必须 0 命中
        ("反例 [B9]", "[B9]"),            # 必须 0 命中
        ("反例 honest-fail", "isolated honest-fail"),  # 必须 0 命中
    ]
    ok = True
    for label, s in g1_items:
        n = b.count(s.encode("utf-8"))
        expect_zero = label.startswith("反例")
        good = (n == 0) if expect_zero else (n > 0)
        ok &= good
        print(f"  {label:16s} {n:3d} 次  {'✅' if good else '❌'}"
              f"{'  (须为 0)' if expect_zero else ''}")
    if not ok:
        print("  ❌ 闸B 不通过：G1 无法在本产物上正确判定「是不是刷错固件了」。")
        return 3
    print("  → has_ab0 and not has_phas ⇒ 真机上 G1 应为 True")

    # ── 闸 C：G2 正则 × 固件真实格式串（端到端）──
    print("\n[闸C] G2 正则 × 固件真实格式串（不人工编造）")
    arms = extract_arm_formats(b)
    real_ok = real_fail = None
    for disp in arms.get("[AB0]", []):
        if "采样法自证 OK" in disp:
            real_ok = disp
        if "采样法自证 FAIL" in disp:
            real_fail = disp
    if not real_ok:
        print("  ❌ 固件里找不到 '[AB0] 采样法自证 OK' 格式串 —— 判读器无源可对。")
        return 4
    # 截到 printf 的 %d 处，还原成可 fill 的模板
    real_ok = real_ok.split(" => ")[0] + "  => pad 采样通道是活的, "
    print(f"  固件原串: {real_ok!r}")
    if real_ok.count("%d") != 2:
        print(f"  ❌ 格式串里 %d 出现 {real_ok.count('%d')} 次（须为 2）—— 判读器只捕获 2 个数字组。")
        return 4
    # 断言关键形态：OK 与「驱动低读到低」之间是什么空白
    gap = re.search(r"自证 OK(\s+)驱动低", real_ok).group(1)
    gap_desc = ("空格×%d，判读器用 \\s+ 可容忍 ✅" % len(gap)) if gap.strip() == "" \
        else "含非空白 ❌"
    print(f"  OK 与『驱动低读到低』之间空白 = {len(gap)} 字符 ({gap_desc})")

    cases = [(200, 200, True), (G2_SELF_MIN, G2_SELF_MIN, True),
             (G2_SELF_MIN - 1, 200, False), (200, G2_SELF_MIN - 1, False),
             (150, 150, False), (0, 0, False)]
    ok = True
    for lo, hi, expect in cases:
        s = real_ok % (lo, hi)
        m = RE_G2_OK.search(s)
        got_ = m is not None and int(m.group(1)) >= G2_SELF_MIN and int(m.group(2)) >= G2_SELF_MIN
        good = got_ == expect
        ok &= good
        print(f"    {lo:3d}/{hi:3d}  判定={got_!s:5s} 期望={expect!s:5s} {'✅' if good else '❌'}")
    if not ok:
        print("  ❌ 闸C 不通过：判读器与固件实际输出对不上。**不要刷板**。")
        return 5

    # ── 闸 D：FAIL 分支不得被 OK 正则误吃 ──
    print("\n[闸D] G2 FAIL 分支优先级")
    if real_fail:
        real_fail = real_fail.split(" => ")[0] + "  => 采样法本身失灵, "
    else:
        real_fail = "[AB0] 采样法自证 FAIL  驱动低读到低 %d/200, 驱动高读到高 %d/200"
    s = real_fail % (10, 10)
    mis = RE_G2_OK.search(s) is not None
    hit = RE_G2_FAIL.search(s) is not None
    print(f"  FAIL串 被 OK 正则误匹配 = {mis} (须 False)   被 FAIL 正则匹配 = {hit} (须 True)")
    if mis or not hit:
        print("  ❌ 闸D 不通过：FAIL 会被误判为 OK —— 采样法失灵将被当成通过。")
        return 6

    # ── 闸 E：变异检验（证明这些用例真能区分，不是恒真）──
    print("\n[闸E] 变异检验：抽掉『高』这一路，原判定与变异判定必须出现分歧")
    RE_MUT = re.compile(r"\[AB0\] 采样法自证 OK\s+驱动低读到低\s+(\d+)/200")
    # ── 两类「看起来像变异、其实不是真变异」的坏例子，本闸必须都能识破 ──
    #   ① 等价体：RE_MUT = RE_G2_OK（逻辑完全相同）⇒ 必须**零分歧**
    #   ② 死变异体：RE_MUT 恒不匹配 ⇒ 必须被识破，而非「有分歧就算过」
    # 所以判据不能是「有没有分歧」，而必须是：
    #     真变异 ⇒ 有分歧，且**分歧恰好只出现在『高不达标而低达标』的用例上**
    #     恒不匹配体会在「低也不达标」的用例上同样分歧 ⇒ 结构上不满足 ⇒ 被拦
    diverge = []
    for lo, hi, expect in cases:
        s_ = real_ok % (lo, hi)
        m_o = RE_G2_OK.search(s_)
        orig = m_o is not None and int(m_o.group(1)) >= G2_SELF_MIN and int(m_o.group(2)) >= G2_SELF_MIN
        m_m = RE_MUT.search(s_)
        mut = m_m is not None and int(m_m.group(1)) >= G2_SELF_MIN
        if orig != mut:
            diverge.append((lo, hi, orig, mut, lo >= G2_SELF_MIN, hi >= G2_SELF_MIN))

    print(f"  {len(cases)} 个用例中，原判定与『只看低』变异判定分歧 {len(diverge)} 处：")
    for lo, hi, o, m_, lo_ok, hi_ok in diverge:
        print(f"    {lo:3d}/{hi:3d}  原={o!s:5s} 变异={m_!s:5s}   低达标={lo_ok!s:5s} 高达标={hi_ok!s}")

    # 结构断言 1：必须有分歧（否则用例对该变异不敏感）
    if not diverge:
        print("  ❌ 闸E 结构断言1 失败：原判定与变异体在所有用例上结论一致")
        print("     ⇒ 要么用例对该变异不敏感（验了等于没验），")
        print("     要么「变异体」本身就是等价体（自我变异测试的经典陷阱）。")
        return 7

    # 结构断言 2：每一处分歧都必须**只发生在「低达标但高不达标」的用例上**。
    #   - 真变异（抽掉高）只在这种用例上改变结论 ⇒ 满足
    #   - 恒不匹配体在「低也不达标」等用例上也会分歧 ⇒ 违反 ⇒ 被拦
    bad = [d for d in diverge if not (d[4] and not d[5])]
    if bad:
        print(f"  ❌ 闸E 结构断言2 失败：{len(bad)} 处分歧不符合『仅低达标而高不达标』形态")
        for lo, hi, o, m_, lo_ok, hi_ok in bad:
            print(f"    {lo:3d}/{hi:3d}  低达标={lo_ok} 高达标={hi_ok}  ← 应无分歧却有")
        print("     ⇒ 该「变异体」不是『抽掉高一路』，而是另一个东西（等价体或死体）。")
        return 7
    print("  ✅ 存在分歧，且全部落在『低达标/高不达标』形态 ⇒ 确为『抽掉高一路』的真变异")

    # ── 闸F：选脚合法性（2026-10-03 事故后新增）──────────────────────
    # 上一轮整轮实验作废，根因是 B 臂用了 IO4/5/6/7 —— 本板 LCD 接线。
    # 闸C~E 全是「判读器与固件对不对得上」，**没有一道问「固件选脚对不对」**：
    # 选脚合法性只写在注释里，注释不执行。本闸把它变成会失败的东西。
    #
    # 脚号从**固件源码**现 parse，不从本脚本里手抄 —— 手抄就与源码
    # 退化成同源的两份数字，改脚不改这里就会漏（与上一轮同型事故）。
    print("\n[闸F] 选脚合法性（禁脚表见 pin_free.py，LIVE 段现 parse 活体头文件）")
    PIN_RE = r"(?m)^#define\s+AB_([AB]_(?:MOSI|CK|MISO|CS)|PROBE)\s+(\d+)\s*$"
    order = ["A_MOSI", "A_CK", "A_MISO", "A_CS",
             "B_MOSI", "B_CK", "B_MISO", "B_CS", "PROBE"]

    # 扫 main/ 下所有 .c 找选脚宏，**不猜是哪个文件**。
    # 猜文件名 = 固件名与源文件名不同名时（实际就是：产物 hw_pin_s3_test.bin
    # 的逻辑在 main/pin_spi_ab.c）必然找不到，而「找不到」最容易被写成
    # 「没找到就没问题」—— 那是把「我没在看」当「不存在」。宁可报错。
    main_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main")
    hits = {}          # 宏名 -> (值, 文件)
    for fn in sorted(os.listdir(main_dir)):
        if not fn.endswith(".c"):
            continue
        with open(os.path.join(main_dir, fn), "r", encoding="utf-8") as f:
            for m in _re.finditer(PIN_RE, f.read()):
                nm, val = m.group(1), int(m.group(2))
                if nm in hits and hits[nm][0] != val:
                    print(f"  ❌ 宏 AB_{nm} 在两个文件里值不同："
                          f"{hits[nm][0]}({hits[nm][1]}) vs {val}({fn})")
                    return 8
                hits[nm] = (val, fn)

    missing = [k for k in order if k not in hits]
    if missing:
        print(f"  ❌ 源码里找不到这些选脚宏：{missing}")
        print(f"     实际扫到：{sorted(hits)}")
        print("     ⇒ 宏改名/移文件了。闸F 已失效 —— **不许当成通过**。")
        return 8
    pins = {k: hits[k][0] for k in order}
    for k in order:
        print(f"  AB_{k:8s} = IO{pins[k]:<3d} ({hits[k][1]})")
    if len(set(pins.values())) != len(pins):
        dup = sorted(p for p in pins.values() if list(pins.values()).count(p) > 1)
        print(f"  ❌ 引脚重复：{dup} —— 两处用同一只脚会互相打架")
        return 8

    cmd = [sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "pin_free.py"), "--check"] + \
          [str(pins[k]) for k in order]
    rc = subprocess.call(cmd)
    if rc != 0:
        print(f"  ❌ 闸F 不通过（pin_free rc={rc}）：选脚撞了禁脚，**不要刷板**。")
        return 8
    print(f"  ✅ 闸F 通过：9 只脚全部避开 [HARD] 芯片级 + [LIVE] 活体板载占用")

    print("\n" + "=" * 74)
    print("✅ 闸 A~F 全绿：判读器与本固件一致，选脚合法，可刷板。")
    print("   注意：本脚本只验**格式一致性**与**选脚合法性**，不验硬件行为。")
    print("   真机判读仍须走 judge_spiab.py 的 G0~G3 + G2b 五道闸。")
    print("=" * 74)
    return 0


if __name__ == "__main__":
    sys.exit(main())
