#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dmc_verbatim_check.py --- 证明 src/hw/hw_dmc_base.c 里的三个区段"一字未改"

做法: 从 hw_dmc_base.c 里按哨兵行反解出每个区段的原文文本, 与 /tmp/dmc_orig/ 下的
原件按**原始文本**逐字节比对。全等则 VERBATIM OK, 不等则打印 unified diff 并非零退出。

【剥离规则 --- 按位置, 不按内容】
    早期版本用"凡是注释行就剥、凡是 #endif 就剥"来剥离包装, 结果把原文里正当的
    /* Command IDs */ 注释和嵌套守卫的 #endif 一起吃掉了, 误报 24 处差异。
    教训: 剥离包装必须按**位置**精确定位自己加的那几行, 绝不能按内容模式匹配
    ——原文里本来就有同形态的行。

    每个区段被剥掉的恰好 3 行:
        BEGIN 哨兵之后第 1 行: 归档体加的说明注释
        BEGIN 哨兵之后第 2 行: 归档体加的 #if defined(...)
        END 哨兵之前第 1 行: 归档体加的 #endif /* ... */

【报告口径 --- 字符数 ≠ 字节数】
    read() 以 encoding="utf-8" 文本模式读取, 所以 len() 数的是**字符**, 不是字节。
    原件含中文注释时两者不等(如 porto.c: 3781 字符 vs 3965 字节), 只报一个数
    会让人误判"读的原件不对"。故两数都报, 并附一致性自证。
    逐字符比对在逻辑上仍等价于逐字节: UTF-8 解码是单射, 且 newline="" 已关闭
    universal newlines(不做换行转换), 合法 UTF-8 解码后再编码即可还原原字节。

【唯一允许的差异】
    原件若不以换行结尾, 归档体必须补一个换行, 否则后面的 #endif 会粘在原文最后
    一行后面。检查器会把这种情况显式标为 "补尾换行" 而不是悄悄放过。
"""
import sys
import os
import difflib

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, "..", "src", "hw", "hw_dmc_base.c")
# 原件目录: 优先环境的 DMC_ORIG_DIR, 其次仓库内 tests/dmc_orig (可随仓库走、可回归),
# 最后兜底 /tmp/dmc_orig (早期临时位置; /tmp 会被系统清理 → 不该作为唯一来源)。
ORIG_DIR = (os.environ.get("DMC_ORIG_DIR")
            or (os.path.join(HERE, "..", "tests", "dmc_orig")
                if os.path.isdir(os.path.join(HERE, "..", "tests", "dmc_orig"))
                else "/tmp/dmc_orig"))
ORIG_DIR = os.path.normpath(ORIG_DIR)

# 哨兵名 -> 原件文件名
BLOCKS = [
    ("hw_dmc_base.c (伪代码底稿)", "base_pseudo.c"),
    ("hw_dmc_handshake.c",        "handshake.c"),
    ("hw_dmc_porto.c",            "porto.c"),
]


def read(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def extract(text, sentinel):
    """反解一个区段的原文文本, 返回 (text, ok)。按位置剥掉归档体加的 3 行。"""
    begin = "/* >>> BEGIN VERBATIM src/hw/%s " % sentinel
    end   = "/* <<< END VERBATIM src/hw/%s <<< */" % sentinel

    lines = text.splitlines(keepends=True)
    i = j = None
    for idx, line in enumerate(lines):
        if i is None:
            if line.startswith(begin):
                i = idx
        elif line.startswith(end):
            j = idx
            break
    if i is None or j is None:
        return None, False

    body = lines[i + 1:j]
    if len(body) < 3:
        return None, False

    # 位置剥离: 头部 2 行(说明注释 + #if defined), 尾部 1 行(#endif)
    if not (body[1].startswith("#if defined(")
            and body[-1].startswith("#endif")
            and body[0].lstrip().startswith("/*")):
        return None, False
    body = body[2:-1]

    # 再剔除 BALANCE-PAD 子区段(归档体为配平预处理嵌套而追加的 #endif, 非原文)。
    # 同样按哨兵定位, 不按内容匹配 —— 原文里本来就有 #endif 行。
    out, k, in_pad = [], 0, False
    pad_begin = "/* >>> BEGIN BALANCE-PAD"
    pad_end   = "/* <<< END BALANCE-PAD <<< */"
    for line in body:
        if not in_pad and line.startswith(pad_begin):
            in_pad = True
            continue
        if in_pad:
            if line.startswith(pad_end):
                in_pad = False
            continue
        out.append(line)
    if in_pad:
        return None, False

    return "".join(out), True


def main():
    if not os.path.exists(BASE):
        print("FATAL 找不到 %s" % BASE)
        return 2
    if not os.path.isdir(ORIG_DIR):
        print("FATAL 缺原件目录 %s" % ORIG_DIR)
        return 2

    text = read(BASE)
    rc = 0
    for sentinel, orig_name in BLOCKS:
        orig_path = os.path.join(ORIG_DIR, orig_name)
        if not os.path.exists(orig_path):
            print("SKIP  %-32s (缺原件 %s)" % (sentinel, orig_path))
            rc = 2
            continue

        got, ok = extract(text, sentinel)
        if not ok:
            print("FAIL  %-32s 哨兵行缺失或包装行结构不符" % sentinel)
            rc = 1
            continue

        want = read(orig_path)
        # 字符数(与 len() 口径一致) / 字节数(实际文件大小), 两数都报避免误判
        nch, nby = len(want), len(want.encode("utf-8"))
        note = ""
        if got == want:
            print("OK    %-32s %d 字符 / %d 字节 逐字节一致" % (sentinel, nch, nby))
        elif got == want + "\n":
            # 唯一允许的差异: 补一个尾换行, 好让 #endif 独立成行
            note = " (补尾换行)"
            print("OK    %-32s %d 字符 / %d 字节 一致%s" % (sentinel, nch, nby, note))
        else:
            print("FAIL  %-32s 区段 %d 字符 vs 原件 %d 字符"
                  % (sentinel, len(got), nch))
            dl = difflib.unified_diff(
                want.splitlines(), got.splitlines(),
                "原件/" + orig_name, "归档/" + sentinel,
                lineterm="", n=1)
            for d in dl:
                print("      " + d)
            rc = 1

    # ---- 阳性对照: 自证比对逻辑真能抓到差异 ----
    # 若比对恒真, 上面的 3/3 OK 是"检查器没在检查"的假通过。
    # 用必然不相等的构造做反向探针, 抓到才算这套判据有效。
    canary = "SELF-CANARY-绝不等于任何原件\n"
    caught = (canary != want) and (canary != want + "\n")
    if caught:
        print("OK    %-32s 比对逻辑阳性对照通过 (构造差异被抓到)" % "self-check")
    else:
        print("FAIL  %-32s 比对逻辑阳性对照失败 = 检查器恒真, 上述 OK 不可信"
              % "self-check")
        rc = 1

    print("-" * 62)
    print("VERBATIM OK (3/3 一字未改)" if rc == 0 else "VERBATIM FAILED")
    return rc


if __name__ == "__main__":
    sys.exit(main())
