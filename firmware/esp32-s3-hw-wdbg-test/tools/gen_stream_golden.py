#!/usr/bin/env python3
"""gen_stream_golden.py — 从独立模型生成 panel_lcd.c 用的 C 黄金表

用法：  python3 tools/gen_stream_golden.py          # 打印 C 表
        python3 tools/gen_stream_golden.py --patch  # 直接就地替换 panel_lcd.c 里的表

黄金值来自 panel_stream_golden.py（独立 Python 模型），C 侧只消费不生产，
因此 C 与 Python 的差异一定是 bug，不会被"抄一遍"掩盖。

⚠️ 改了 init 表 / 分辨率 / 偏移 / 旋转规则 / 像素字节序 之后，必须重跑本脚本，
   否则真机自检会立刻报 !! expect/got 不一致（这是设计意图，不是误报）。
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
C_FILE = os.path.join(HERE, "..", "main", "panel_lcd.c")


def load_model():
    path = os.path.join(HERE, "panel_stream_golden.py")
    spec = importlib.util.spec_from_file_location("stream_model", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


BEGIN = "/* ⚠️ 本表由 tools/gen_stream_golden.py 生成，请勿手改（改了就重跑脚本）。"
END = "static void PANEL_UNUSED smismatch"   # 表后面紧跟的函数，用作结束锚点


def build_table() -> str:
    g = load_model()
    L = []
    L.append(BEGIN)
    L.append(" * 来源：tools/panel_stream_golden.py（独立模型）经 tools/gen_stream_golden.py 导出。 */")
    L.append("typedef struct {")
    L.append("    const char *id;")
    L.append("    uint32_t    blen, wlen, flen;          /* boot/window/fill 三段流长度")
    L.append("                                            * ⚠️ fill 段可达 15 万+ 字节，")
    L.append("                                            *    **不可**用 uint16 装（会静默截断） */")
    L.append("    uint32_t    boot[4], win[4], fill[4];  /* 对应 rot 0/90/180/270 的 FNV-1a-32 */")
    L.append("} panel_stream_golden_t;")
    L.append("")
    L.append("static const panel_stream_golden_t STREAM_GOLDEN[PANEL_COUNT] = {")
    for i, (pid, p) in enumerate(g.PANELS.items()):
        bl = wl = fl = None
        boots, wins, fills = [], [], []
        for rot in (0, 90, 180, 270):
            stages, w, h, xo, yo = g.expected(p, rot)
            for nm, ln, cs in stages:
                if nm == "boot":
                    bl = ln; boots.append(cs)
                if nm == "window":
                    wl = ln; wins.append(cs)
                if nm == "fill":
                    fl = ln; fills.append(cs)
        fmt = lambda a: ", ".join("0x%08Xu" % v for v in a)
        L.append(f'    /* {i} */ {{ "{pid}", {bl}, {wl}, {fl},')
        L.append(f'            {{ {fmt(boots)} }},')
        L.append(f'            {{ {fmt(wins)} }},')
        L.append(f'            {{ {fmt(fills)} }} }},')
    L.append("};")
    L.append("")
    L.append("")
    return "\n".join(L)


def main():
    table = build_table()
    if "--patch" not in sys.argv:
        print(table)
        return 0

    src = open(C_FILE).read()
    i = src.find(BEGIN)
    if i < 0:
        print("找不到表起始标记，请检查 " + C_FILE, file=sys.stderr)
        return 1
    j = src.find(END, i)
    if j < 0:
        print("找不到表结束标记", file=sys.stderr)
        return 1
    new = src[:i] + table + src[j:]
    if new == src:
        print("表内容无变化")
    else:
        open(C_FILE, "w").write(new)
        print(f"已更新 {os.path.relpath(C_FILE)} ({len(table)} 字节)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
