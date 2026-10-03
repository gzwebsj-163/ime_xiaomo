#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
judge_spiab.py — SPI 侧 GPIO Matrix A/B 实验的**外部独立判读器**

═══════════════════════════════════════════════════════════════════════════
为什么必须有一个独立于固件的判读器（本脚本存在的理由，不是冗余）
═══════════════════════════════════════════════════════════════════════════
1. **G0 只有外部能做**：固件在物理上无法自检「rst:0x 恰好 1 次」。
   一次崩溃-重启循环刷出来的日志，照样含完整的 A/B 两臂数据，
   固件会照常裁决出 verdict=2 这样的**强结论**。只有外部读全量日志能拦下。
2. **G3 固件漏实现**：`ab_run_arm()` 采了空载基线（pin_spi_ab.c:364-372）并打印，
   但裁决段（:453-478）**完全没有引用该值**。判据表 G3 明写「基线非全低 → 作废该臂」。
   ⇒ 固件在「该脚被外部拉高」时会给出不可信的 verdict=1/2。
3. **锚点 F（检索工具静默失败）**：ESP-IDF 串口日志含 ANSI 转义序列，
   经 shell `grep` 会被判为 binary 而**不输出匹配行**（`-c` 仍吐 0，像给了个正常答案）。
   ⇒ 本脚本**一律先剥 ANSI 再匹配**，且全部用 Python，不用 shell 文本工具。
4. **两个裁判必须交叉比对**：固件自带裁决逻辑。若本脚本与固件 verdict 不一致，
   本脚本会**显式报警而非静默取其一**。

═══════════════════════════════════════════════════════════════════════════
判据来源：VERDICT_RUBRIC_267c1ca9.md（r2.1，取数据前定稿）
本脚本是那份判据表的**独立第二实现**，不替代它；两者不一致时以判据表为准并报警。
═══════════════════════════════════════════════════════════════════════════
"""
import re
import os
import sys
import argparse

# ── 判据表常量（与 VERDICT_RUBRIC_267c1ca9.md 严格一致，改一处必须同步改另一处）──
G2_SELF_MIN      = 190      # /200
G3_BASE_POINTS   = 500      # 基线采样点数
G3_BASE_LOW      = 500      # 必须全低
EXP_MCU_A, EXP_FS_A = 4, 256      # A 臂(IOMUX): MCU_SEL=SPI2_FUNC_NUM(4), func_sel=SIG_GPIO_OUT_IDX(256)
# 🕳️ 2026-10-03 修正：曾写 `EXP_MCU_B, EXP_FS_B = 0, 101` 并注释「MCU_SEL=0」。
#    权威依据 components/soc/esp32s3/include/soc/io_mux_reg.h:140 → #define PIN_FUNC_GPIO 1
#    ⇒ S3 上走 GPIO Matrix 的 FSEL 是 **1**，0 不是 GPIO（该头文件无 PIN_FUNC_RESERVED=0）。
#    真机实测 B 臂 MCU_SEL=1、func_sel=101=FSPICLK_OUT_IDX —— 路由本就是对的，
#    是**这份期望值**把它判成不通过。与固件 pin_spi_ab.c 的字面量 `0u` 是同一份错误的两个副本。
EXP_MCU_B, EXP_FS_B = 1, 101      # B 臂(matrix): MCU_SEL=PIN_FUNC_GPIO(1), func_sel=FSPICLK_OUT_IDX(101)

CRASH_KEYWORDS = [
    "Guru Meditation", "panic", "abort()", "assert failed",
    "Backtrace:", "Illegal instruction", "LoadProhibited", "StoreProhibited",
]


def strip_ansi(s: str) -> str:
    """剥掉 ANSI 转义序列。必须先剥再匹配，否则含转义的日志会被当 binary 静默失配。"""
    return re.sub(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])", "", s)


class Parsed:
    """从 boot 日志中抽出判读所需的全部事实。任何一项抽不到 = None（不是 0）。"""
    def __init__(self):
        self.rst_total = None       # 总出现次数
        self.rst_unique = None      # 去重后的值个数
        self.crash_hits = []
        self.has_fw_marker = None      # True=本固件 / False=刷错 / None=无证据
        self.self_ok = None            # G2
        self.base = {}                 # G3: arm -> low count
        self.route = {}                # G2b: arm -> (mcu_sel, func_sel, ok)
        self.high = {}                 # arm -> high count
        self.fw_verdict = None         # 固件自己的 verdict（交叉比对用）
        self.missing = []


def parse(text: str) -> Parsed:
    p = Parsed()
    t = strip_ansi(text)

    # ── G0：重启次数。必须同时给「总次数」与「去重值个数」——
    #    判据表原话是「rst:0x 恰好 1 次」= 总次数语义，不是去重语义。
    #    panic 后 rst 可能是 0x1 也可能变成 0x3，两种都必须拦下。
    #    🕳️ 2026-10-03 修正：初版只算 len(set(...))，导致「同值重复 2 次」漏判，
    #    而配套自测用例 T6 恰好生成两行相同 rst:0x1 ⇒ 假通过（被 crash 分支误拦）。
    #    教训同「失败的阳性对照先怀疑用例」：这次是**通过的用例其实没覆盖目标路径**。
    rst = re.findall(r"rst:(0x[0-9a-fA-F]+)", t)
    p.rst_total = len(rst)
    p.rst_unique = len(set(rst))

    low_t = t.lower()
    p.crash_hits = [k for k in CRASH_KEYWORDS if k.lower() in low_t]

    # ── G1：产物身份。正例与反例都要证据，缺证据 = None（不猜） ──
    has_ab0 = "[AB0]" in t and "SPI 侧 GPIO Matrix A/B 实测" in t
    has_phas = ("isolated honest-fail" in t) or ("[C1]" in t) or ("[B9]" in t)
    if has_ab0 and not has_phas:
        p.has_fw_marker = True
    elif has_phas:
        p.has_fw_marker = False
    # else: 保持 None

    # ── G2 ──
    m = re.search(r"\[AB0\] 采样法自证 OK\s+驱动低读到低\s+(\d+)/200,\s*驱动高读到高\s+(\d+)/200", t)
    if m:
        lo, hi = int(m.group(1)), int(m.group(2))
        p.self_ok = (lo >= G2_SELF_MIN and hi >= G2_SELF_MIN)
        p.self_lo, p.self_hi = lo, hi
    m = re.search(r"\[AB0\] 采样法自证 FAIL\s+驱动低读到低\s+(\d+)/200,\s*驱动高读到高\s+(\d+)/200", t)
    if m:
        p.self_ok = False
        p.self_lo, p.self_hi = int(m.group(1)), int(m.group(2))

    # ── G3 空载基线 ──
    for m in re.finditer(
        r"\[AB\]\s*(\S+)\s*臂 发送前基线: 采500点 低电平=(\d+)/500", t
    ):
        p.base[m.group(1)] = int(m.group(2))

    # ── G2b 路由实况（寄存器读数：MCU_SEL + func_sel） ──
    for m in re.finditer(
        r"\[AB-G\]\s*CK=IO(\d+)\s*寄存器实况:\s*IO_MUX=0x[0-9a-fA-F]+"
        r"\s*\(FUN_IE=(\d+)\s+MCU_SEL=(\d+)\)\s*OE=(\d+)", t
    ):
        p.route.setdefault("_raw", []).append((int(m.group(1)), int(m.group(3))))
        p.route.setdefault("_oe", []).append((int(m.group(1)), int(m.group(4))))
    for m in re.finditer(
        r"\[AB-G\]\s*CK=IO(\d+)\s*寄存器实况:.*?FUNCx_OUT_SEL=0x([0-9a-fA-F]+)"
        r"\s*\(func_sel=(\d+)\)", t
    ):
        # 🕳️ 真相修正（2026-10-03，源码 pin_spi_ab.c:244-245 坐实）：
        #     固件里 out_func_sel = out_sel_raw & 0x1FFu，
        #     即「func_sel=」这个字段**恒等于** FUNCx_OUT_SEL=0x… 的低 9 位。
        #     ⇒ 本对拍在**真实硬件上永不触发**，只可能被伪造日志（T14）触发。
        #     它**不是**对 func_sel 的独立验证；它真正的价值仅是：
        #     若两字段对不上，说明**日志行被截断/串行化错位/解析失配**（锚点 F）。
        #     ⚠️ 不得据此对外宣称「func_sel 有两个独立口径」。
        #     （G2b 本身仍成立：MCU_SEL 与 func_sel 出自两个不同寄存器，
        #       且与 pad 电平属不同硬件路径。）
        raw = int(m.group(2), 16)
        p.route.setdefault("_fs", []).append((int(m.group(1)), int(m.group(3))))
        p.route.setdefault("_fs_raw", []).append((int(m.group(1)), raw & 0x1FF))

    # ── 高电平采样数（波形事实） ──
    for m in re.finditer(
        r"\[AB\]\s*(\S+)\s*臂 CK=IO(\d+)\s*传输中采\s*(\d+)\s*点,\s*高电平点数=(\d+)", t
    ):
        p.high[m.group(1)] = int(m.group(4))

    # 汇总行是更可靠的来源（两臂都在一行里）
    for m in re.finditer(
        r"A 臂 IOMUX\s+\(CK=IO\d+\):\s*高电平\s+(-?\d+)\s*/\s*(\d+)\s+路由闸=(\S+)", t
    ):
        p.high["A(IOMUX)"] = int(m.group(1))
    for m in re.finditer(
        r"B 臂 matrix\s+\(CK=IO\d+\):\s*高电平\s+(-?\d+)\s*/\s*(\d+)\s+路由闸=(\S+)", t
    ):
        p.high["B(matrix)"] = int(m.group(1))

    # ── 固件自报 verdict（交叉比对） ──
    m = re.search(r"\[AB\]\s*fails=(\d+)\s+verdict=(-?\d+)", t)
    if m:
        p.fw_verdict = int(m.group(2))

    # ── 完整性：判读所需项是否齐 ──
    need = {
        "rst_total": p.rst_total is not None,
        "G2": p.self_ok is not None,
        "G2b_raw": bool(p.route.get("_raw")),
        "G2b_fs": bool(p.route.get("_fs")),
        "G3_A": "A(IOMUX)" in p.base or any(k.startswith("A") for k in p.base),
        "G3_B": "B(matrix)" in p.base or any(k.startswith("B") for k in p.base),
        "AB1": "A(IOMUX)" in p.high,
        "AB2": "B(matrix)" in p.high,
    }
    for k, v in need.items():
        if not v:
            p.missing.append(k)
    return p


def route_check(p: Parsed, arm_ck: int, exp_mcu: int, exp_fs: int):
    """按 A/B 臂各自的 CK 脚号配对 MCU_SEL 与 func_sel。

    func_sel 用**独立口径**：从 FUNCx_OUT_SEL 原始寄存器值自行算 bits0-8，
    而不是直接采信驱动打印的 func_sel= 字段。两者不一致 → 返回 ok=None（判不可用）并记警告。
    """
    mcus = dict(p.route.get("_raw", []))
    fs_drv = dict(p.route.get("_fs", []))
    fs_own = dict(p.route.get("_fs_raw", []))
    if arm_ck not in mcus or arm_ck not in fs_drv or arm_ck not in fs_own:
        return None, (mcus.get(arm_ck), fs_drv.get(arm_ck), fs_own.get(arm_ck))
    mcu = mcus[arm_ck]
    if fs_drv[arm_ck] != fs_own[arm_ck]:
        # 解析链自相矛盾 → 不产出结论，只报矛盾
        return None, (mcu, fs_drv[arm_ck], fs_own[arm_ck])
    return (mcu == exp_mcu and fs_own[arm_ck] == exp_fs), (mcu, fs_drv[arm_ck], fs_own[arm_ck])


# ── 两只臂的 CK 脚号：现 parse 固件源码，不写死 ──────────────────────
# 🕳️ 2026-10-03 真 bug（锚点 E：常量归属与固件脱钩）：
#     本行曾写死 `CK_A, CK_B = 12, 5`，而固件当时 B 臂 CK=5。
#     换脚后固件 B 臂 CK=16，这里**没跟着改** ⇒ route_check(16) 查不到
#     ⇒ 返回 (None,(None,None,None)) ⇒ 面板打出 `MCU_SEL=? func_sel=None`，
#     而裁决矩阵第 1 行 `if hb > 0: verdict=1` **不看 G2b** ⇒ 照样出强结论。
#     即：判据表一个常量没更新，就让整条 G2b 闸门静默失效，26 项 selftest 全绿也没抓到
#     （T13 用的还是旧脚号的自造日志，自洽但不真实）。
#     修法：与 pin_free.py 同一原则 —— 数值不进判据器，每次现 parse 固件源码。
FW_SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "main", "pin_spi_ab.c")


def _parse_ck_from_fw():
    """从 main/pin_spi_ab.c 现 parse A/B 臂 CK 脚。读不到 → 抛错，绝不静默默认值。"""
    if not os.path.isfile(FW_SRC):
        raise FileNotFoundError(
            f"固件源码缺失：{FW_SRC}\n"
            f"  判读器需要知道两只臂的 CK 脚号才能配对路由寄存器。\n"
            f"  **不允许退回写死的默认值** —— 那正是本脚本上一次静默失效的机制。")
    txt = open(FW_SRC, "r", encoding="utf-8", errors="replace").read()
    out = {}
    for key, macro in (("A", "AB_A_CK"), ("B", "AB_B_CK")):
        m = re.search(r"^#define\s+" + macro + r"\s+(\d+)", txt, re.M)
        if not m:
            raise ValueError(f"固件源码里找不到 #define {macro} —— 宏改名了，先弄清楚新名字")
        out[key] = int(m.group(1))
    return out["A"], out["B"]


CK_A, CK_B = _parse_ck_from_fw()
# 每次运行都自证这两个数与固件一致（阳性对照：解析器确实在工作）
_FW_CK_TEXT = f"CK_A/CK_B 现 parse 自 {FW_SRC} = {CK_A}/{CK_B}"


def judge(p: Parsed) -> dict:
    """严格按判据表裁决。返回 {verdict, 结论, 理由, 闸明细}。"""
    gates = {}
    verdict, why = -1, []

    # ── 前置闸：不满足则后面全部作废 ──
    # ── G0：按判据表原话「恰好 1 次」= 总次数语义。同时打印去重值数供诊断。 ──
    gates["G0"] = (p.rst_total == 1 and not p.crash_hits,
                   f"rst:0x 总次数 {p.rst_total} 次(须=1)／去重值 {p.rst_unique} 个，"
                   f"崩溃关键字 {p.crash_hits or '无'}")
    if p.has_fw_marker is False:
        return dict(verdict=-1, why=["G1 失败：日志含 Phase C / honest-fail 行 = 刷错固件，本轮作废"],
                    concl="实验环境不可信（刷错固件）", gates=gates)
    gates["G1"] = (p.has_fw_marker is True, f"产物标记 {'有' if p.has_fw_marker else '无'}")
    gates["G2"] = (p.self_ok is True,
                   f"驱动低 {getattr(p,'self_lo','?')}/200 高 {getattr(p,'self_hi','?')}/200 (须各≥{G2_SELF_MIN})")
    gates["G3"] = {}
    for arm, ck in (("A(IOMUX)", CK_A), ("B(matrix)", CK_B)):
        b = p.base.get(arm)
        # 🕳️ 必须显式区分「缺失(None)」与「非全低」：两者都作废，但诊断含义不同。
        # 写成 b == G3_BASE_LOW 时 None 会被静默当成「不等于500」，输出误导性的「基线低 None/500」。
        if b is None:
            gates["G3"][arm] = (False, f"基线行**缺失**（日志里找不到该臂发送前基线）→ 无法判该臂")
        elif b != G3_BASE_LOW:
            gates["G3"][arm] = (False, f"基线低 {b}/{G3_BASE_POINTS} ≠ {G3_BASE_LOW} → 该脚存在外部驱动/占用，作废该臂")
        else:
            gates["G3"][arm] = (True, f"基线低 {b}/{G3_BASE_POINTS}")
    gates["G2b"] = {}
    for arm, ck, em, ef in (("A(IOMUX)", CK_A, EXP_MCU_A, EXP_FS_A),
                            ("B(matrix)", CK_B, EXP_MCU_B, EXP_FS_B)):
        ok, val = route_check(p, ck, em, ef)
        gates["G2b"][arm] = (ok is True, f"MCU_SEL={val[0]} func_sel={val[1]} (须 {em}/{ef})")

    for g in ("G0", "G1", "G2"):
        if gates[g][0] is not True:
            return dict(verdict=-1, why=[f"{g} 未通过：{gates[g][1]}"],
                        concl="实验环境不可信 / 采样法失灵 → 本轮不产出任何结论", gates=gates)

    if p.missing:
        return dict(verdict=-1, why=[f"日志要素缺失：{p.missing}（无法完成判读）"],
                    concl="日志不完整 → 本轮无效", gates=gates)

    ha, hb = p.high.get("A(IOMUX)"), p.high.get("B(matrix)")
    g3a, g3b = gates["G3"]["A(IOMUX)"][0], gates["G3"]["B(matrix)"][0]
    ra, rb = gates["G2b"]["A(IOMUX)"][0], gates["G2b"]["B(matrix)"][0]

    # ── 裁决矩阵（判据表 §二，五行） ──
    if ha is not None and hb is not None and ha < 0 or hb < 0:
        return dict(verdict=-1, why=["某臂启动失败（负计数）→ 结构性失败，查 rc"],
                    concl="结构性失败", gates=gates)

    if not g3a or not g3b:
        bad = [a for a, v in (("A(IOMUX)", g3a), ("B(matrix)", g3b)) if not v]
        return dict(verdict=-1,
                    why=[f"G3 空闲基线未通过（{', '.join(bad)}）→ 该臂存在外部驱动/占用，作废该臂"],
                    concl="无效 —— 基线非全低，该脚被外部占用，电平读数不可信", gates=gates)

    if ha == 0:
        return dict(verdict=-1,
                    why=["A 臂（阳性对照臂，IOMUX 专属脚）也未采到波形"],
                    concl="无效 —— 阳性对照失效，不得据此说 matrix 不通", gates=gates)

    if hb > 0:
        # 🕳️ 2026-10-03 真 bug 修正（这才是元凶）：此处原为无条件
        #     `if hb > 0: verdict = 1`，**完全不检查 B 臂 G2b 路由闸**。
        #     后果是整条 G2b 闸门在「有波形」分支上形同虚设 —— 上一轮固件自己打了
        #     「✗ 路由闸不通过」照样输出 verdict=1；CJ 判读器更是 parse 失败打出
        #     `MCU_SEL=? func_sel=None` 也照样给 +1。
        #     ⇒ 一条闸门「在某个分支上不被检查」，比「没有这条闸门」更危险：
        #       它还给人「闸门已覆盖」的错觉。
        #     现在 B 臂 G2b 成为 verdict=1 的**必要条件**，且 None(解析失败) 明确拦截。
        if rb is not True:
            return dict(verdict=-1,
                        why=[f"G2b(B臂) 未通过却采到波形：{gates['G2b']['B(matrix)'][1]}"
                             f"（None=解析链失败或路由与预期机制不符）—— 证据不自洽，不出强结论"],
                        concl="无效 —— 采到波形但路由闸未通过，两个证据互相矛盾，本轮不可裁决",
                        gates=gates)
        verdict, concl = 1, "SPI matrix 出向通 —— FSPI 可路由到任意 GPIO（与 UART 侧不同）"
    elif hb == 0 and rb is True:
        verdict, concl = 2, "SPI matrix 出向不通 —— 寄存器已独立证实 B 臂路由确实建立，仍无波形"
    elif hb == 0 and rb is not True:
        return dict(verdict=-1,
                    why=[f"G2b(B臂) 未通过：{gates['G2b']['B(matrix)'][1]}"],
                    concl="无效 —— 路由未按预期机制建立，读 0 不可归因于 matrix 不通", gates=gates)
    else:
        return dict(verdict=-1, why=["未知组合"], concl="无效", gates=gates)

    extra = []
    if verdict == 1 and ra is not True:
        extra.append("⚠️ A 臂 G2b 未通过但仍判 1：B 臂有波形本身即强证据，判据表 §二第一行未对 A 臂设 G2b 门槛")
    return dict(verdict=verdict, why=extra, concl=concl, gates=gates)


# ═══════════════════════════════════════════════════════════════════════
# 阴性/阳性对照自测 —— 本脚本自己也必须先被验证
# ═══════════════════════════════════════════════════════════════════════
GOOD_TAIL = """
rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
[AB0] 采样法自证 OK  驱动低读到低 200/200, 驱动高读到高 200/200
[AB-G] CK=IO12 寄存器实况: IO_MUX=0x60009034 (FUN_IE=1 MCU_SEL=4) func_sel=256
[AB-G] CK=IO12 ✓ 路由实况与该臂预期机制相符
[AB-G] CK=IO5 寄存器实况: IO_MUX=0x60009018 (FUN_IE=1 MCU_SEL=0) func_sel=101
[AB-G] CK=IO5 ✓ 路由实况与该臂预期机制相符
[AB] A(IOMUX) 臂 发送前基线: 采500点 低电平=500/500 (无下拉, 靠 CPOL=0 天然低)
[AB] B(matrix) 臂 发送前基线: 采500点 低电平=500/500 (无下拉, 靠 CPOL=0 天然低)
[AB] A(IOMUX) 臂 CK=IO12 传输中采 7000 点, 高电平点数=HA
[AB] B(matrix) 臂 CK=IO5 传输中采 7000 点, 高电平点数=HB
  A 臂 IOMUX  (CK=IO12): 高电平 HA / 7000   路由闸=通过
  B 臂 matrix (CK=IO5): 高电平 HB / 7000   路由闸=通过
[AB] fails=0 verdict=FV
"""
HEAD = "I (316) boot: ESP32-S3\n===== SPI 侧 GPIO Matrix A/B 实测 =====\n"


def mk(high_a, high_b, *, rst=1, crash=False, mcu_b=EXP_MCU_B, fs_b=EXP_FS_B,
       mcu_a=EXP_MCU_A, fs_a=EXP_FS_A, base_a=500, base_b=500, self_lo=200, self_hi=200,
       self_line="OK", phase_c=False, self_gone=False, fs_b_lie=None):
    """rst 可传 int（同值重复该次数）或 list（不同值序列，用于测 panic 后 rst 变值的真实情形）。
    fs_b_lie: 故意让 FUNCx_OUT_SEL 原始值算出的 func_sel 与驱动打印的 func_sel= 不一致，
              用于验证「解析链自相矛盾 → 判不可用」这条路径真能触发。
    🕳️ 2026-10-03：本函数原写死 `abg(5, ...)` 与 `mcu_b=0`。这两个都是**判据副本**——
         固件换脚到 CK=16、且 EXP_MCU_B 已按 io_mux_reg.h 更正为 1 之后，本函数仍在
         造旧脚号+旧期望的日志 ⇒ route_check(CK_B=16) 查不到 ⇒ 7 条用例集体假失败。
         教训：**造用例的函数也是副本**，副本与真源脱钩时，失败的是用例不是被测物。
         现在全部改用真源常量（CK_A/CK_B/EXP_*），换脚或改期望不必再动这里。"""
    t = HEAD
    if phase_c:
        t += "[C1] isolated honest-fail (Phase C)\n"
    vals = [f"0x1"] * rst if isinstance(rst, int) else list(rst)
    for v in vals:
        t += f"rst:{v} (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\n"
    if crash:
        t += "Guru Meditation Error: Core 0 panic'ed (LoadProhibited)\n"
    if not self_gone:
        t += f"[AB0] 采样法自证 {self_line}  驱动低读到低 {self_lo}/200, 驱动高读到高 {self_hi}/200\n"
    # 下面两行严格照抄 pin_spi_ab.c:257-260 的 C 字符串字面量拼接后的真实形态
    # "[AB-G] CK=IO%d 寄存器实况: IO_MUX=0x%08x (FUN_IE=%u MCU_SEL=%u) OE=%u  FUNCx_OUT_SEL=0x%08x (func_sel=%u)"
    def abg(ck, iomux, mcu, oe, fs_raw, fs_field):
        return (f"[AB-G] CK=IO{ck} 寄存器实况: IO_MUX=0x{iomux:08x} "
                f"(FUN_IE=1 MCU_SEL={mcu}) OE={oe}  FUNCx_OUT_SEL=0x{fs_raw:08x} (func_sel={fs_field})\n")
    t += abg(CK_A, 0x60009034, mcu_a, 1, fs_a, fs_a)
    t += abg(CK_B, 0x60009018, mcu_b, 1,
             (fs_b_lie if fs_b_lie is not None else fs_b), fs_b)
    t += f"[AB] A(IOMUX) 臂 发送前基线: 采500点 低电平={base_a}/500 (无下拉, 靠 CPOL=0 天然低)\n"
    t += f"[AB] B(matrix) 臂 发送前基线: 采500点 低电平={base_b}/500 (无下拉, 靠 CPOL=0 天然低)\n"
    t += f"[AB] A(IOMUX) 臂 CK=IO{CK_A} 传输中采 7000 点, 高电平点数={high_a}\n"
    t += f"[AB] B(matrix) 臂 CK=IO{CK_B} 传输中采 7000 点, 高电平点数={high_b}\n"
    t += f"  A 臂 IOMUX  (CK=IO{CK_A}): 高电平 {high_a} / 7000   路由闸=通过\n"
    t += f"  B 臂 matrix (CK=IO{CK_B}): 高电平 {high_b} / 7000   路由闸=通过\n"
    t += "[AB] fails=0 verdict=1\n"
    return t


NEVER = "\x00NEVER\x00"   # 哨兵：必须显式给出期望结论，不允许留空（默认跳过＝未检查）

# 🕳️ 2026-10-03：逐行照抄真机 boot1.log（去掉时间戳与行号，保留 ANSI 与全部后缀）。
#    数字一个没改：A 3592 / B 3599、基线各 500/500、采样法 200/200、
#    A 臂 MCU_SEL=4/func_sel=256，B 臂 MCU_SEL=1/func_sel=101。
#    唯一改动：B 臂的 ✗ 行换成 ✓ 行（真机那行是固件拿**写错的期望值 0** 去比才出的 ✗，
#    而判据表已按 io_mux_reg.h 更正为 1 ⇒ 真值下本就该通过）。
#    留着 ✗ 行会让本条恒判 -1，那就不是测「格式能否解析」而是测「已知错误」了。
REAL_BOOT_TAIL = (
    HEAD
    + "rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\n"   # ← 首版漏了这行，G0 判失败
    + "\x1b[0;32mI (364) spi_ab: [AB0] 采样法自证 OK  驱动低读到低 200/200, 驱动高读到高 200/200"
      "  => pad 采样通道是活的, 后续电平读数可作证据\x1b[0m\n"
    + f"\x1b[0;32mI (384) spi_ab: [AB-G] CK=IO{CK_A} 寄存器实况: IO_MUX=0x00004a02 (FUN_IE=1 MCU_SEL=4)"
      f" OE=0  FUNCx_OUT_SEL=0x00000100 (func_sel=256)\x1b[0m\n"
    + f"\x1b[0;32mI (394) spi_ab: [AB-G] CK=IO{CK_A} ✓ 路由实况与该臂预期机制相符"
      f" (MCU_SEL=4 期望{EXP_MCU_A}, func_sel=256 期望{EXP_FS_A}) => 驱动确实按预期路径布了路由\x1b[0m\n"
    + f"[AB] A(IOMUX) 臂 发送前基线: 采500点 低电平=500/500 (无下拉, 靠 CPOL=0 天然低)\n"
    + f"[AB] A(IOMUX) 臂 CK=IO{CK_A} 传输中采 7000 点, 高电平点数=3592"
      f"  => ★外设确实在驱动这个脚★\n"
    + f"\x1b[0;32mI (1994) spi_ab: [AB-G] CK=IO{CK_B} 寄存器实况: IO_MUX=0x00001a02 (FUN_IE=1 MCU_SEL=1)"
      f" OE=1  FUNCx_OUT_SEL=0x00000065 (func_sel=101)\x1b[0m\n"
    + f"\x1b[0;32mI (1994) spi_ab: [AB-G] CK=IO{CK_B} ✓ 路由实况与该臂预期机制相符"
      f" (MCU_SEL=1 期望{EXP_MCU_B}, func_sel=101 期望{EXP_FS_B}) => 驱动确实按预期路径布了路由\x1b[0m\n"
    + f"[AB] B(matrix) 臂 发送前基线: 采500点 低电平=500/500 (无下拉, 靠 CPOL=0 天然低)\n"
    + f"[AB] B(matrix) 臂 CK=IO{CK_B} 传输中采 7000 点, 高电平点数=3599"
      f"  => ★外设确实在驱动这个脚★\n"
    + "\x1b[0;33mW (3334) spi_ab: [AB] fails=0 verdict=1 (1=matrix通 2=matrix不通 -1=无效)\x1b[0m\n"
)

SELFTESTS = [
    # (名称, 日志, 期望verdict, 期望结论须含的片段, 这条在防什么)
    ("T1 matrix 通（A/B 都有波形）",            mk(3500, 3200), 1,  "出向通",
                                                    "阳性对照：唯一能判 1 的场景必须真能判 1"),
    ("T2 matrix 不通（A有 B无 + B臂G2b通过）",   mk(3500, 0),    2,  "不通",
                                                    "r2 核心场景：寄存器证实路由已建仍无波形"),
    ("T3 陷阱 B臂G2b双变量不符 → -1",           mk(3500, 0, mcu_b=4, fs_b=256), -1, "无效 —— 路由未按预期机制建立",
                                                    "r2 收紧的核心：路由没建 ≠ matrix 不通"),
    ("T4 陷阱 G2打印FAIL → 必须 -1",            mk(3500, 0, self_line="FAIL", self_lo=100, self_hi=100), -1, "采样法",
                                                    "CX1/CX2 共享盲点：采样法瞎了不得出结论"),
    ("T5 陷阱 A臂也无波形 → 必须 -1",            mk(0, 0),      -1, "阳性对照失效",
                                                    "阳性对照臂失效时绝不能判 2"),
    ("T6 陷阱 崩溃重启(rst×2 同值) → 必须 -1",    mk(3500, 0, rst=2), -1, "环境不可信",
                                                    "G0 核心：同值重复2次，去重=1 仍须拦下(初版漏判)"),
    ("T6b 陷阱 panic后rst变值(0x1→0x3) → -1",     mk(3500, 0, rst=["0x1", "0x3"]), -1, "环境不可信",
                                                    "G0 另一真实形态：去重=2，总次数=2"),
    ("T6c 崩溃关键字命中(rst×1) → 必须 -1",        mk(3500, 0, crash=True), -1, "环境不可信",
                                                    "G0 第二半：单次启动但含 panic 关键字仍须拦"),
    ("T6d 对照 rst×1 无崩溃 → 不得误拦",           mk(3500, 0), 2, "不通",
                                                    "防过度拦截：正常单次启动必须能通过 G0"),
    ("T7 陷阱 B臂基线非全低 → 必须 -1",         mk(3500, 0, base_b=120), -1, "基线",
                                                    "G3 固件漏实现：脚被外部拉高时固件会误判"),
    ("T8 陷阱 A臂基线非全低 → 必须 -1",         mk(3500, 3200, base_a=60), -1, "基线", "G3 对 A 臂同样生效"),
    ("T8b 陷阱 基线行完全缺失 → 必须 -1 且不崩",
        "\n".join(l for l in mk(3500, 0).split("\n") if "发送前基线" not in l), -1, "日志不完整",
                                                    "G3 修复：None 与 非全低 必须区分，且不得 TypeError"),
    ("T9 陷阱 刷错固件(含Phase C) → -1",         mk(3500, 0, phase_c=True), -1, "刷错固件", "G1 产物身份闸"),
    ("T10 陷阱 AB0行缺失 → 必须 -1",            mk(3500, 0, self_gone=True), -1, "环境不可信",
                                                    "缺证据≠通过：不能默认闸门通过"),
    ("T11 空日志 → 必须 -1",                     "",          -1, "环境不可信",  "零输入不得产出任何结论"),
    ("T12 ANSI污染仍能正确判 2",
        mk(3500, 0).replace("[AB0]", "\x1b[0;32m[AB0]\x1b[0m"), 2, "不通",
        "锚点F：含 ANSI 的日志不得导致静默失配"),
    ("T13 源码真实格式(含OE/FUNCx_OUT_SEL) → 判 2",
        mk(3500, 0), 2, "不通",
        "打破自证循环：日志形态严格照抄 pin_spi_ab.c:257-260 的 C 字符串拼接结果"),
    ("T14 陷阱 func_sel解析链自相矛盾 → -1",
        mk(3500, 0, fs_b=101, fs_b_lie=0), -1, "无效 —— 路由未按预期机制建立",
        "独立口径对拍：驱动打印 func_sel=101 但原始寄存器算出=0 ⇒ 解析链有问题，不得出结论"),
    # ── 以下三条是变异测试暴露的真缺口补齐（2026-10-03） ──
    ("T15 陷阱 G2打印OK但数值不达标 → -1",
        mk(3500, 0, self_line="OK", self_lo=100, self_hi=100), -1, "采样法",
        "真缺口：源码 205/208 是两条独立 ESP_LOG 路径，只测 FAIL 那条等于没测阈值"),
    ("T20 陷阱 G2 低达标/高不达标 → -1",
        mk(3500, 0, self_line="OK", self_lo=200, self_hi=100), -1, "采样法",
        "真缺口：旧用例 lo 与 hi 总是同时不达标 ⇒ 抓不住『只查低不查高』的变异"),
    ("T21 对照 G2 低199(临界下) → 必须过闸",
        mk(3500, 0, self_line="OK", self_lo=189, self_hi=200), -1, "采样法",
        "阈值边界对照：189<190 须拦下（防把 >= 写成 >）"),
    ("T22 对照 G2 低190(恰临界) → 须放行并判2",
        mk(3500, 0, self_line="OK", self_lo=190, self_hi=190), 2, "不通",
        "阈值边界对照：190>=190 须放行（防把 >= 写成 >）"),
    ("T16 陷阱 G2b 单变量：仅 MCU_SEL 错 → -1",
        mk(3500, 0, mcu_b=4, fs_b=101), -1, "无效 —— 路由未按预期机制建立",
        "真缺口：T3 同时改两个字段，改一个时另一个仍不匹配 ⇒ 旧用例抓不住『忽略 MCU_SEL』的变异"),
    ("T17 陷阱 G2b 单变量：仅 func_sel 错 → -1",
        mk(3500, 0, mcu_b=0, fs_b=256), -1, "无效 —— 路由未按预期机制建立",
        "真缺口：同上，对称地覆盖『忽略 func_sel』的变异"),
    ("T19 陷阱 基线行臂名对不上 → 走G3的None分支 → -1",
        mk(3500, 0).replace("[AB] A(IOMUX) 臂 发送前基线", "[AB] AXIOMUX 臂 发送前基线"),
        -1, "基线",
        "专打 G3 显式三分支里的 None 分支（缺失≠非全低），T8b 走的是 p.missing 不是这条"),
    ("T18 对照 A臂G2b不符但B臂有波形 → 判1且须带告警",
        mk(3500, 3200, mcu_a=0, fs_a=101), 1, "出向通",
        ("防过度拦截：判据表第一行未对A臂设G2b门槛，判1但须显式告警。"
         "🕳️ 本条原传 mcu_a=EXP_MCU_A, fs_a=EXP_FS_A（即 A 臂的**正确**值），"
         "名字说『A臂G2b不符』实际却让它符合 ⇒ 这条用例自己什么都没防。"
         "典型：**用例名与用例实际做的事不一致**，比没有用例更难发现。",
         "A 臂 G2b 未通过但仍判 1")),
    # ── T23：直接吃真机 boot1.log 的形态（不是 mk() 造的） ──
    # 🕳️ 为什么要这条：T1~T22 全部经 mk() 生成，而 mk() 是**我自己写的日志生成器**。
    #    若 parse 的正则与固件实际输出格式有偏差，mk() 造的日志同样带这个偏差 ⇒
    #    自测永远绿，真机日志却解析不出来。这正是 T13 想防的「自证循环」的完整形态。
    #    本条用 spiab_run_20261003/boot1.log 的**真实行**拼装（A臂OK/B臂期望值错），
    #    数字与真机完全一致：A 3592 / B 3599、MCU_SEL A=4 B=1、func_sel A=256 B=101。
    #    期望 B 臂 G2b 通过 + 有波形 ⇒ verdict=1。
    ("T23 真机日志形态(A3592/B3599, B臂MCU_SEL=1) → 判 1",
        REAL_BOOT_TAIL, 1, "出向通",
        "打破自证循环：日志**不是 mk() 造的**，逐行照抄真机 boot1.log。"
        "若 parse 正则与固件实际格式有偏差，这条会假失败而 T1~T22 仍全绿。"),
]


def selftest():
    print("=" * 78)
    print("judge_spiab.py 自测（阴性/阳性对照）—— 判读器自己先被验证")
    print("=" * 78)
    passed = failed = 0
    for name, log, want, want_concl, why in SELFTESTS:
        # 可选的第 6 元：期望 r["why"] 必须含的片段（用于验证「须带告警」这类
        # 落在 why 而不在 concl 的行为——concl 是面向人的结论，why 是诊断，
        # 两者分开，之前「须带告警」只写在名字里，机器不检查 ⇒ 名为测告警，实则没测）。
        want_why = ""
        if isinstance(why, tuple):
            why, want_why = why
        if want_concl == NEVER:
            print(f"  ❌ 用例自身缺陷: {name} 未给出期望结论 —— 结论文字从未被检查过")
            failed += 1
            continue
        r = judge(parse(log))
        got, got_concl = r["verdict"], r["concl"]
        v_ok = (got == want)
        c_ok = (want_concl in got_concl)
        y_ok = (want_why == "") or (want_why in " ".join(r.get("why") or []))
        ok = v_ok and c_ok and y_ok
        passed, failed = passed + ok, failed + (not ok)
        mark = "✅" if ok else ("❌ verdict" if not v_ok else
                               ("❌ concl" if not c_ok else "❌ 告警"))
        print(f"  {mark}  {name}")
        if not ok:
            print(f"        verdict 期望={want:+d} 实得={got:+d}"
                  f"   结论期望含『{want_concl}』实得『{got_concl[:40]}』")
            if not y_ok:
                print(f"        告警 期望含『{want_why}』实得『{' '.join(r.get('why') or [])[:60]}』")
        print(f"        防的是：{why}")
    print("-" * 78)
    print(f"  自测 {passed} 通过 / {failed} 失败"
          f"（verdict 与结论文字**都**通过才算通过）")
    print("=" * 78)
    return failed == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", help="boot 日志文件")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()

    if a.selftest or not a.log:
        sys.exit(0 if selftest() else 1)

    with open(a.log, "r", encoding="utf-8", errors="replace") as f:
        p = parse(f.read())
    r = judge(p)

    print("=" * 78)
    print("SPI matrix A/B · 外部独立判读结果")
    print("=" * 78)
    print(f"  G0 重启统计   : 总次数 {p.rst_total}（须=1）／去重值 {p.rst_unique} 个"
          f"   崩溃关键字: {p.crash_hits or '无'}")
    print(f"  G1 产物标记   : {p.has_fw_marker}")
    print(f"  G2 采样法     : 低 {getattr(p,'self_lo','?')}/200 高 {getattr(p,'self_hi','?')}/200")
    for arm in ("A(IOMUX)", "B(matrix)"):
        rk = p.route.get("_raw", []); fs = p.route.get("_fs", [])
        mcu = dict(rk).get(CK_A if arm.startswith("A") else CK_B)
        fsv = dict(fs).get(CK_A if arm.startswith("A") else CK_B)
        print(f"  G2b {arm:<10}: MCU_SEL={msv if (msv:=mcu) is not None else '?'} func_sel={fsv}")
        print(f"  G3  {arm:<10}: 基线低 {p.base.get(arm,'?')}/500")
        print(f"  波形 {arm:<9}: 高电平 {p.high.get(arm,'?')}/7000")
    print("-" * 78)
    if r["why"]:
        for w in r["why"]:
            print(f"  ⚠️  {w}")
    print(f"  >>> verdict = {r['verdict']:+d}")
    print(f"  >>> {r['concl']}")

    if p.fw_verdict is not None and p.fw_verdict != r["verdict"]:
        print("-" * 78)
        print(f"  🚨 裁判不一致：固件自报 verdict={p.fw_verdict:+d}，本脚本={r['verdict']:+d}")
        print(f"     以判据表为准，并须查清固件裁决逻辑（:453-478）何处偏离。")
    elif p.fw_verdict is not None:
        print(f"  交叉比对：与固件自报 verdict={p.fw_verdict:+d} 一致 ✓")
    print("=" * 78)
    print("\n  结论适用范围（判据表 §四，不可外推）：")
    print("    ❌ 不可说「S3 GPIO Matrix 完全不能用」/「对所有外设都不可行」/「读不到=信号不存在」")
    print("    🔴 残余风险（替代解释 5）始终开放：G2b 只证路由已建，不证 pad 能读回经矩阵驱动的信号。")
    print("       闭合需示波器或物理跳线；本轮不具备条件，不得假装已排除。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
