#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_font.py — 从 i18n/strings.txt 导出中文字体子集
═══════════════════════════════════════════════════════════════════════
为什么要有这个脚本（P0 的核心工程决策）：
  · LVGL 内置 Montserrat 无 CJK，sdkconfig 里 SOURCE_HAN_SANS_SC_*_CJK
    开关默认全关（实测 sdkconfig:2192-2193）。
  · 手工开全量 CJK 14/16 = 2.39MB，占 4MB 分区 60% → 挤掉模块空间，不可接受。
  · 手工把字符集写死在命令行参数里 = 一旦加文案就静默变方块，且没人会发现。
  ⇒ 所以字符集必须**由文案表自动导出**：单一真源，改文案 → 重跑 → 字体自动跟上。

用法：
  python3 tools/gen_font.py                 # 生成全部字号
  python3 tools/gen_font.py --check         # 只校验：文案里有但字库里没有的字符（CI 用）
  python3 tools/gen_font.py --tts <路径>    # 指定 TTF（默认用 LVGL 自带 NotoSansSC）

产物：
  main/fonts/ui_cjk_{14,16}.c
  main/fonts/ui_cjk.h
  tools/font_charsets.json     （记录字符集，供 --check 与文档追溯）
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STRINGS = ROOT / "main" / "i18n" / "strings.txt"
FONT_DIR = ROOT / "main" / "fonts"
TTF_DEFAULT = (ROOT / "managed_components" / "lvgl__lvgl" / "tests" /
               "src" / "test_files" / "fonts" / "noto" / "NotoSansSC-Regular.ttf")
CONV = Path.home() / "cow" / "tmp" / "fontconv" / "node_modules" / ".bin" / "lv_font_conv"
CHARSET_JSON = ROOT / "tools" / "font_charsets.json"

# 字号 → (lv_font_conv size, bpp)
#
# bpp 是本项目唯一需要在体积/清晰度间取舍的旋钮。
# **实测（3886 字全量字库 bpp=2，2026-10-01 从 .map 量出）**：
#     14px → 198.0 KB     16px → 234.1 KB     双字号合计 442.1 KB
# 定版 bpp=2 的理由：
#   · bpp=1 只有开/关两色，14px 中文笔画会粘连，240x320 屏近距离阅读吃力 → 排除
#   · bpp=4 比 bpp=2 多占约 60% 体积，只为多两级灰阶，在 app 分区还剩 2.4MB 时
#     这点空间买不到可感知的清晰度提升 → 不划算
#
# ⚠️⚠️ 体积纪律：**只有 .map 里的数是判据，源文件大小和任何正则估算都不是。**
#    两次踩坑，方向还相反：
#      ① 拿源文件大小当预算 → 高估 12 倍（lv_font_conv 输出 hex 文本，
#        每数据字节占 5 个字符 + 每字形一行注释）
#      ② 改用正则数 `0x..` → 低估 12%（输出是 `0x%X` **不补零**，`0x8` 只有
#        1 位十六进制，要求正好 2 位的正则漏掉所有 0x0–0xf 的值）
#    ⇒ 预算流程固定为：生成 → 编译 → 从 .map 量 glyph_bitmap/glyph_dsc/
#      unicode_list_1..3 四段。gen_font.py 的估算只用来判断「值不值得编译」。
#
# 占用核对：app 分区 4096 KB，含全量字库的探针固件 1663 KB = 40.6%，余 2433 KB。
# 这个余量是留给 P4 那 4187 行模块移植的。
# ⚠️ 若日后 P4 挤爆分区，降级顺序：① --subset 收回子集 ② bpp 降到 1。
SIZES = {14: (14, 2), 16: (16, 2)}

# 永远要包含的基础字符（数字/标点/单位符号，界面里到处都有）
BASE_ASCII = (
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    " !\"#$%&'()*+,-./"
    ":;<=>?@[\\]^_`{|}~"
)
# 界面里确实会出现的非 ASCII 符号
EXTRA_SYMBOLS = "±×÷°Ωμ·—…《》「」、。，；：？！（）√✕→←↑↓★☆●○▲△■□"


def parse_strings():
    """解析 strings.txt → [(key, zh, en)]，同时收集中文字符集。"""
    entries, zh_chars, en_chars = [], set(), set()
    if not STRINGS.exists():
        sys.exit(f"[FATAL] 找不到文案表: {STRINGS}")
    for lineno, raw in enumerate(STRINGS.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("|")
        if len(parts) != 3:
            sys.exit(f"[FATAL] {STRINGS.name}:{lineno} 格式应为 key|中文|English，实得 {len(parts)} 段")
        key, zh, en = (p.strip() for p in parts)
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*", key):
            sys.exit(f"[FATAL] {STRINGS.name}:{lineno} key 非法: {key!r}（须全大写字母数字下划线）")
        entries.append((key, zh, en))
        zh_chars.update(zh)
        en_chars.update(en)
    # 去重校验：重复 key 会导致 C 表里两条同名 → 静默取第一条 = 陷阱
    seen = {}
    for key, _, _ in entries:
        seen[key] = seen.get(key, 0) + 1
    dups = [k for k, n in seen.items() if n > 1]
    if dups:
        sys.exit(f"[FATAL] 重复 key: {', '.join(dups)}")
    return entries, zh_chars, en_chars


def gb2312_level1():
    """枚举 GB2312 一级（常用 3755）汉字。

    ⚠️ 用 Python codec 枚举，不硬编码字表 —— 硬编码的「常用字表」在不同
    上下文里内容不同且无从校验；codec 是标准，枚举结果可复现。
    区位码 16–55 区 = 一级汉字（标准规定），区 56–87 = 二级。

    为什么收录全量：见 build_charset() 上方注释 —— flash 装得下，
    换来的是「加任何中文文案都不会变方块」这一整类故障被消除。
    """
    chars = set()
    for hi in range(0xA1, 0xFE):
        if not (16 <= hi - 0xA0 <= 55):      # 只取一级区
            continue
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if ch.strip() and ord(ch) > 0x2E80:
                chars.add(ch)
    return chars


def build_charset(zh_chars, en_chars, full_cjk=True):
    """完整字符集 = 中文 + 英文 + 基础 ASCII + 符号。"""
    chars = set()
    chars.update(zh_chars)
    # 英文只取 ASCII 部分（英文文案里的非 ASCII 已被 EXTRA_SYMBOLS 覆盖）
    chars.update(c for c in en_chars if ord(c) < 128)
    chars.update(BASE_ASCII)
    chars.update(EXTRA_SYMBOLS)
    # 必须保留的 LVGL 内置符号：LVGL 用私有区码点画箭头/勾/叉等
    chars.update("✓✗✔✘")
    chars.discard("\n")
    chars.discard("\r")
    if full_cjk:
        # ── 全量字库（默认）──
        # 实测双字号 382.6 KB = app 分区 39.2%，余 2489 KB 给 P4 模块移植。
        # 收益：① 加任何中文文案都不会变方块（不必记得重跑本脚本）
        #       ② 探针/模块回读的中文串能直接显示（子集方案的死穴）
        #       ③ 未来接设备型号/错误码不需要为字符集操心
        # 文案表的字仍然并入（保证 GB2312 之外的字，比如「Probe」用的私用区
        # 符号或后加的生僻字，不会因为不在 GB2312 里而丢）。
        chars.update(gb2312_level1())
    return "".join(sorted(chars))


def run_conv(tts, size, bpp, out_c, name, chars):
    if not CONV.exists():
        sys.exit(f"[FATAL] 找不到 lv_font_conv: {CONV}\n"
                 f"        安装：cd {CONV.parent.parent} && npm i lv_font_conv")
    if not tts.exists():
        sys.exit(f"[FATAL] 找不到 TTF: {tts}")
    # ⚠️ 前置条件自检：lv_font_conv 是 `#!/usr/bin/env node` 脚本。
    #    node 不在 PATH 时它会报 "env: node: No such file or directory" 但
    #    退出码仍是 0（env 找不到解释器时的行为）⇒ 不检查就会「静默无产物」。
    #    本项目已踩过一次（node 装在 /usr/local/bin，不在默认 PATH）。
    if not shutil.which("node"):
        found = [p for p in ("/usr/local/bin/node", "/opt/homebrew/bin/node")
                 if Path(p).exists()]
        hint = f"\n        试：export PATH={str(Path(found[0]).parent)}:$PATH" if found else ""
        sys.exit(f"[FATAL] node 不在 PATH —— lv_font_conv 无法运行{hint}")
    cmd = [str(CONV),
           "--font", str(tts),
           "--size", str(size),
           "--bpp", str(bpp),
           "--format", "lvgl",
           "--lv-include", "lvgl.h",
           "--lv-font-name", name,
           "--no-kerning",             # 子集字体关 kerning：省体积，且中文 Kern 本就少
           "-r", "0x20-0x7F",          # ASCII 区间
           "--symbols", chars,         # 关键：文案表导出的非 ASCII 字符
           "-o", str(out_c)]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"[FATAL] lv_font_conv 失败(size={size}):\n{proc.stdout}\n{proc.stderr}")
    # 产物自检：rc=0 也不代表写出了文件（见上面的 node 说明）
    if not out_c.exists() or out_c.stat().st_size < 1024:
        sys.exit(f"[FATAL] lv_font_conv 退出 0 但产物异常 "
                 f"({out_c.name}, {out_c.stat().st_size if out_c.exists() else 0} B)\n"
                 f"        stdout={proc.stdout.strip()}\n"
                 f"        stderr={proc.stderr.strip()}")
    return proc.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tts", default=str(TTF_DEFAULT), help="TTF 路径")
    ap.add_argument("--check", action="store_true", help="只做一致性检查，不写文件")
    ap.add_argument("--only", type=int, default=0, help="只生成某字号（调试用）")
    ap.add_argument("--subset", action="store_true",
                    help="只收文案表里的字，不带 GB2312 全量（应急降体积用）")
    args = ap.parse_args()

    entries, zh_chars, en_chars = parse_strings()
    charset = build_charset(zh_chars, en_chars, full_cjk=not args.subset)
    cjk_only = "".join(sorted(c for c in charset if ord(c) > 127))

    print(f"文案条目 : {len(entries)} 条")
    print(f"字符集   : 共 {len(charset)} 字（非 ASCII {len(cjk_only)}）"
          f"{'  [子集模式]' if args.subset else '  [含 GB2312 一级全量]'}")
    print(f"TTF      : {args.tts}")

    # ── 一致性检查模式 ──
    if args.check:
        if not CHARSET_JSON.exists():
            sys.exit(f"[FAIL] 没有 {CHARSET_JSON.name}，先生成一次")
        old = json.loads(CHARSET_JSON.read_text(encoding="utf-8"))
        missing = sorted(set(cjk_only) - set(old.get("cjk", "")))
        if missing:
            sys.exit(f"[FAIL] 文案表新增了 {len(missing)} 个字但字库未重生成: {''.join(missing)}\n"
                     f"       跑 python3 tools/gen_font.py")
        print("[OK] 字库与文案表一致")
        return

    FONT_DIR.mkdir(parents=True, exist_ok=True)
    tts = Path(args.tts)
    sizes = {args.only: SIZES[args.only]} if args.only else SIZES

    results = []
    for px, (size, bpp) in sizes.items():
        name = f"ui_cjk_{px}"
        out_c = FONT_DIR / f"{name}.c"
        run_conv(tts, size, bpp, out_c, name, charset)
        results.append((px, out_c))
        print(f"  ✓ {out_c.name:22s} 生成完毕（源 {out_c.stat().st_size/1024:.0f} KB）")

    # ── 头文件 ──
    hdr = ["/* 自动生成，勿手改 —— 由 tools/gen_font.py 生成",
           " * 字符集 = main/i18n/strings.txt 的字 ∪ GB2312 一级(3755)常用字 ∪ ASCII ∪ 符号",
           " * 改文案不必重跑本脚本（全量字库已覆盖）；只有换字体/改 bpp 才需要重跑。 */",
           "#pragma once", "",
           '#include "lvgl.h"', "",
           "#ifdef __cplusplus", 'extern "C" {', "#endif", ""]
    for px in sizes:
        hdr.append(f"LV_FONT_DECLARE(ui_cjk_{px});")
    hdr += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    (FONT_DIR / "ui_cjk.h").write_text("\n".join(hdr), encoding="utf-8")
    print(f"  ✓ ui_cjk.h")

    # ── 字符集留档（供 --check 与文档追溯）──
    CHARSET_JSON.write_text(json.dumps(
        {"cjk": cjk_only, "all": charset, "entries": len(entries),
         "full_cjk": not args.subset},
        ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"  ✓ {CHARSET_JSON.name}")

    # ── flash 预算 ──
    # 🕳️ 两次踩坑，都栽在「用源文件反推二进制大小」这件事上：
    #   ① 源文件是 hex 文本，比数据量大 12 倍 —— 拿它当预算会严重【高】估。
    #   ② 但也不能简单地数 `0x..`：lv_font_conv 输出的是 `0x%X`，**不补零**，
    #      所以 `0x8` / `0x2` 只有 1 位十六进制。要求正好 2 位的正则会漏掉
    #      所有 0x0–0xf 的值 —— 本项目因此把 14px 估成 174.9 KB，真值 198.0 KB
    #      （低 12%）。两次都错在方向不同，靠估算做决策就是这么不可靠。
    # ⇒ 本函数的数字只用于「值不值得编译看一眼」，**判据是 .map**。
    print("\nflash 占用预估（链接后，非源文件大小）：")
    total = 0
    for px, path in results:
        src = path.read_text(encoding="utf-8")
        # ⚠️ 必须先剥注释，且 1-2 位十六进制 + 负向前瞻（否则会从更长的值里切一半）
        bm_blk = re.sub(r"/\*.*?\*/", "", re.search(
            r"glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};", src, re.S).group(1), flags=re.S)
        bm = len(re.findall(r"0x[0-9a-fA-F]{1,2}(?![0-9a-fA-F])", bm_blk))
        dsc = re.search(r"glyph_dsc\[\]\s*=\s*\{(.*?)\n\};", src, re.S).group(1).count("adv_w") * 8
        ul = sum(len(re.findall(r"0x[0-9a-fA-F]{4}", re.search(
            nm + r"\[\]\s*=\s*\{(.*?)\n\};", src, re.S).group(1))) * 2
            for nm in ("unicode_list_1", "unicode_list_2", "unicode_list_3")
            if re.search(nm + r"\[\]", src))
        n = len(re.findall(r"^    /\* U\+", src, re.M))
        est = bm + dsc + ul + 44 + 24 + 96          # 44=lv_font_t 24=font_dsc 96=cmaps
        total += est
        print(f"    {path.name:22s} {n:>5} 字形  ≈{est/1024:>7.1f} KB")
    app_kb = 4096
    print(f"  字体合计 ≈{total/1024:.1f} KB")
    print(f"  ⚠️ 估算值，误差量级 ±15%。判据 = 编译后量 .map。")
    print(f"  实测校准（2026-10-01, bpp=2, 3886 字形）: 14px=198.0KB 16px=234.1KB 合计 442.1KB")
    print(f"  → 按固件基线 1258 KB 估算，占比 ≈{(1258 - 33 + total/1024)/app_kb*100:.1f}%，"
          f"余 ≈{app_kb - 1258 + 33 - total/1024:.0f} KB")


if __name__ == "__main__":
    main()
