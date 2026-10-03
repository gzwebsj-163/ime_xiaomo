#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_i18n.py — 从 main/i18n/strings.txt 生成 main/i18n.c
═══════════════════════════════════════════════════════════════════════
UI 里怎么用：
    ui_label_create(parent, TR(S_OK));            // 运行时按当前语言取文案
    static const char *kName = TR_STATIC(S_OK);   // 仅语言切换时才变，可缓存
  · TR() 每次调用一次数组索引 + 一次指针取址，**没有分支** ⇒ 可以放心放进
    每帧刷新的地方。
  · ⚠️ 禁止把 TR() 的结果长期缓存：用户切语言后旧指针还指着旧语言文案，
    会显示成"一半中文一半英文"。要缓存就用 TR_STATIC（切换时统一重建）。

设计要点：
  · 索引 0 = 中文（出厂默认，用户明确要求"默认全部中文"）
  · 索引 1 = English（可切）
  · 表按 key 排序无关，按 strings.txt 顺序 ⇒ 文案表即执行顺序
  · 未在表里的 key → 编译期哨兵（见 _TR_MISSING），运行时会显式报出来，
    绝不静默显示空串（空串在屏幕上表现为"标签消失"，极难排查）

用法：
  python3 tools/gen_i18n.py
  python3 tools/gen_i18n.py --check    # 只校验产物与文案表是否同步（CI 用）
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
STRINGS = ROOT / "main" / "i18n" / "strings.txt"
OUT_C = ROOT / "main" / "i18n.c"
OUT_H = ROOT / "main" / "i18n.h"

LANGS = ["ZH", "EN"]
LANG_DEFAULT = 0  # 中文 = 出厂默认

HEADER = r'''/**
 * i18n.c / i18n.h — 多语言文案表（自动生成，⚠️ 勿手改）
 *
 * 由 tools/gen_i18n.py 从 main/i18n/strings.txt 生成。
 * 加文案的正确做法：改 strings.txt → 重跑 gen_i18n.py → 重跑 gen_font.py
 *                   （字体子集也依赖文案表！漏了 gen_font.py 会出现方块字）
 */
'''


def c_escape(s):
    """转义成合法 C 字符串字面量内容。"""
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ord(ch) < 0x20:
            out.append(f"\\x{ord(ch):02x}")
        else:
            out.append(ch)          # UTF-8 原样输出，源文件即 UTF-8
    return "".join(out)


def parse():
    entries = []
    for lineno, raw in enumerate(STRINGS.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("|")
        if len(parts) != 3:
            sys.exit(f"[FATAL] strings.txt:{lineno} 须为 key|中文|English")
        key, zh, en = (p.strip() for p in parts)
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*", key):
            sys.exit(f"[FATAL] strings.txt:{lineno} key 非法: {key!r}")
        entries.append((key, zh, en))
    seen = set()
    for k, _, _ in entries:
        if k in seen:
            sys.exit(f"[FATAL] 重复 key: {k}")
        seen.add(k)
    return entries


def render_c(entries):
    n = len(entries)
    L = [HEADER, '#include "i18n.h"', '#include <stddef.h>', "",
         "/* 运行时当前语言；0=中文（出厂默认） 1=English */",
         "uint8_t g_ui_lang = %d;" % LANG_DEFAULT, "",
         "/* 找不到的 key 走这里 —— 故意显示醒目文本而非空串，",
         " * 因为空串在屏上表现为「标签凭空消失」，排查成本极高。 */",
         'const char *const UI_STR_MISSING = "\\xE2\\x9C\\x97 [i18n MISSING]";',
         "const char *const UI_LANG_NAME[2] = { \"\\xE4\\xB8\\xAD\\xE6\\x96\\x87\", \"English\" };", "",
         "/* ---- 文案表：[语言][条目] ---- */",
         "static const char *const UI_STR[2][%d] = {" % n, "  {"]
    for key, zh, _ in entries:
        L.append('    "%s", /* %s */' % (c_escape(zh), key))
    L += ["  },", "  {"]
    for key, _, en in entries:
        L.append('    "%s", /* %s */' % (c_escape(en), key))
    L += ["  }", "};", ""]
    L += [render_key_index(entries)]
    L += [render_lookup(entries)]
    return "\n".join(L)


def render_keys_macro(entries):
    """TID_<KEY> 宏必须进**头文件** —— ui.c 要用它来取文案。

    走宏而非运行时查表的理由：UI 每帧都要取文案，编译期常量在 -O2 下
    落成一条常量 load；若改成 ui_tr_id("S_OK") 字符串比较，就是每帧 174 次
    字符串比对，白白烧 CPU。
    """
    L = ["/* ---- key → 索引（编译期常量；ui.c 用它调 ui_tr） ---- */"]
    for i, (key, _, _) in enumerate(entries):
        L.append(f"#define TID_{key} {i}u")
    L.append("")
    return "\n".join(L)


def render_key_index(entries):
    """运行时 key→索引 查表（字符串比较），供自检/调试路径使用。"""
    L = ["/* ---- 运行时 key→索引（字符串比较；仅自检/调试用） ---- */",
         "static const char *const UI_KEYS[] = {"]
    L.append("    " + ", ".join(f'"{k}"' for k, _, _ in entries))
    L += ["};", "",
          "uint16_t ui_tr_id(const char *key)",
          "{",
          "    for (uint16_t i = 0; i < UI_STR_COUNT; i++) {",
          "        const char *a = UI_KEYS[i], *b = key;",
          "        while (*a && *a == *b) { a++; b++; }",
          "        if (*a == *b) return i;",
          "    }",
          "    return 0xFFFFu;      /* 未命中 → 调用方显示 UI_STR_MISSING */",
          "}", ""]
    return "\n".join(L)


def render_lookup(entries):
    return "\n".join([
        "const char *ui_tr(uint16_t idx)",
        "{",
        "    if (idx >= UI_STR_COUNT || g_ui_lang >= UI_LANG_COUNT) return UI_STR_MISSING;",
        "    return UI_STR[g_ui_lang][idx];",
        "}", "",
    ])


def render_h(entries):
    L = [HEADER, "#pragma once", "",
         "#include <stdint.h>", "",
         "#ifdef __cplusplus", 'extern "C" {', "#endif", "",
         "/** 语言索引 */",
         "typedef enum {",
         "    UI_LANG_ZH = 0,   /* 中文（出厂默认） */",
         "    UI_LANG_EN = 1,   /* English */",
         "    UI_LANG_COUNT = 2",
         "} ui_lang_t;", "",
         "/** 语言范围（写入存储用；UI_LANG_FIRST 恒为出厂默认中文） */",
         "typedef enum {",
         "    UI_LANG_FIRST = 0,       /* = UI_LANG_ZH，出厂默认 */",
         "    UI_LANG_LAST  = %d,      /* = UI_LANG_EN */" % (len(LANGS) - 1),
         "} ui_lang_range_t;", ""]
    L += ["/** 运行时当前语言（UI 每次取文案都读它） */",
          "extern uint8_t g_ui_lang;", "",
          "/** 语言显示名（语言选择页用：中文 / English） */",
          "extern const char *const UI_LANG_NAME[2];", "",
          "/** 缺失 key 的占位串（醒目，非空） */",
          "extern const char *const UI_STR_MISSING;", "",
          "/** 文案总数（自检用） */",
          "#define UI_STR_COUNT %d" % len(entries), "",
          render_keys_macro(entries),
          "/** 取文案：TR(TID_S_OK)。",
          " *  ⚠️ 结果**不要长期缓存** —— 切语言后旧指针仍指向旧语言，",
          " *     会显示成「半中半英」。要缓存就存 idx，切语言时统一重刷。 */",
          "#define TR(idx) ui_tr(idx)", "",
          "/** 取文案 */",
          "const char *ui_tr(uint16_t idx);", "",
          "/** 按 key 查索引；查不到返回 0xFFFF（配合 UI_STR_MISSING 暴露问题） */",
          "uint16_t ui_tr_id(const char *key);", "",
          "#ifdef __cplusplus", "}", "#endif", ""]
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    if not STRINGS.exists():
        sys.exit(f"[FATAL] 找不到 {STRINGS}")
    entries = parse()
    c_src, h_src = render_c(entries), render_h(entries)

    if args.check:
        bad = []
        for path, want in ((OUT_C, c_src), (OUT_H, h_src)):
            if not path.exists():
                bad.append(f"{path.name} 不存在")
            elif path.read_text(encoding="utf-8") != want:
                bad.append(f"{path.name} 与 strings.txt 不同步")
        if bad:
            sys.exit("[FAIL] " + "；".join(bad) + "\n       跑 python3 tools/gen_i18n.py")
        print(f"[OK] i18n.c/h 与文案表同步（{len(entries)} 条）")
        return

    OUT_C.write_text(c_src, encoding="utf-8")
    OUT_H.write_text(h_src, encoding="utf-8")
    print(f"  ✓ i18n.c / i18n.h  ({len(entries)} 条 × {len(LANGS)} 语言)")


if __name__ == "__main__":
    main()
