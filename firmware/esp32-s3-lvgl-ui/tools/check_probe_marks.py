#!/usr/bin/env python3
"""
check_probe_marks.py — 产品固件产物内探针标记检查

为什么必须有这个工具（而不是一条 strings 命令）：
  `strings` 只认可打印 ASCII。本工程的探针标记既有中文也有长串，
  `strings | grep` 对它们**零命中** —— 看起来像"清理干净了"，
  实际什么都没查。这是「验证工具的静默失败」典型形态：
  工具给出一个自信的答案，而那个答案是无意义的。

所以这里一律按**字节**统计，并把 10 个 CACHE BOOL 旋钮 + 产物内标记
一并列出。阴性对照要求：产品固件必须全部 0 命中。

用法：
  python3 tools/check_probe_marks.py build_v3/esp32-s3-lvgl-ui.bin

退出码：0 = 全部 0 命中（通过）；1 = 有命中（产品固件里混进了探针）
"""

import sys

# main/CMakeLists.txt 里的 10 个 CACHE BOOL 旋钮。
# ⚠️ BTN_SELFTEST 是硬编码在 CMakeLists.txt:26 的（不是 CACHE），
#    所以它不会出现在 CMakeCache.txt 里，但**会**以字符串形式出现在产物里
#    （btn.c 的自检文案），因此在这里单列。
CACHE_FLAGS = [
    "RACE_AMPLIFY",
    "PIN_PROBE",
    "PROBE_TEST_FREE_PIN",
    "SHOT_PROBE",
    "CJK_SMOKETEST",
    "UI_CFG_SELFTEST",
    "UI_CFG_PERSIST_TEST",
    "PROG_SELFTEST",
    "WIFI_PASS_PROBE",
    "PROG_UI_TEST",
]

# 产物内应为零命中的标记串（探针日志文案 / 阳性对照）
#
# ⚠️ 每一条都必须是**足够specific**的串。早先用过裸 "persist"，结果对产品固件
#    报出 2 处命中 —— 追下去发现来源是 lwIP 自带的 TCP 慢启动日志
#    "tcp_slow_start: persist ticking with in-flight data"，与本工程无关。
#    这就是「验证工具的假阳性」：工具自信地给出一个失败，而那个失败没有意义。
#    教训：子串匹配必须挑**带前缀/带上下文的**串（如 "[persist]"），
#    否则任何第三方库的字符串都会让判据变成随机数。
PRODUCT_MARKS = [
    b"SELFTEST",
    b"selftest",
    b"SHOT_PROBE",
    b"##SHOT",
    b"##DONE",
    b"[persist]",             # ui_cfg_persist_probe 的日志标记（不用裸 "persist"）
    b"UI_CFG_PERSIST_TEST",
    b"RACE_AMPLIFY",
    b"PROG_UI_TEST",
    b"WIFI_PASS_PROBE",
    b"CJK_SMOKETEST",
    b"password_probe",
    b"candidate",             # WIFI_PASS_PROBE 的候选探测日志
]

# 阳性对照：这些串**必须**命中。
# 若连它们都 0 命中，说明本工具自己坏了（镜像不是本工程产物 / 被压缩），
# 不能据此宣称"探针清理干净"。
POSITIVE_CONTROLS = [
    b"LVGL",           # lcd_hw.c
    b"ui_cfg",         # ui_cfg.c 的 NVS 命名空间
    b"backend up",     # prog_api.c
    b"jumper",         # prog_api.c 的诚实失败提示
]


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2

    path = argv[1]
    try:
        with open(path, "rb") as f:
            blob = f.read()
    except OSError as e:
        print("无法读取: %s (%s)" % (path, e))
        return 2

    print("镜像: %s  (%d 字节)" % (path, len(blob)))
    print()

    # ---- 阳性对照先行 ----
    ctrl_ok = True
    print("── 阳性对照（必须命中，否则本工具不可信）──")
    for m in POSITIVE_CONTROLS:
        n = blob.count(m)
        ok = n > 0
        ctrl_ok = ctrl_ok and ok
        print("  %-14s %5d  %s" % (m.decode(), n, "OK" if ok else "❌ 0 命中 —— 工具或镜像有问题"))
    if not ctrl_ok:
        print("\n❌ 阳性对照未通过：本工具无法对本镜像下结论，停止。")
        return 1
    print()

    # ---- 探针标记 ----
    bad = 0
    print("── 探针标记（产品固件必须全部 0）──")
    for m in PRODUCT_MARKS:
        n = blob.count(m)
        if n:
            bad += 1
        print("  %-14s %5d  %s" % (m.decode(), n, "OK" if n == 0 else "❌ 探针残留"))
    print()

    # ---- CMake 侧提醒（顺带核 CACHE 是否粘值）----
    print("── 提醒 ──")
    print("  CMake 侧另需人工核对：")
    print("    grep -E '^(%s):BOOL' <build_dir>/CMakeCache.txt"
          % "|".join(CACHE_FLAGS))
    print("  期望 10 个全部 =0。BTN_SELFTEST 不在 CMakeCache（硬编码），靠上面的产物字节查。")
    print()

    if bad:
        print("❌ 结论：%d 个探针标记有残留，本镜像不是干净的产品固件。" % bad)
        return 1
    print("✅ 结论：产物内探针标记全部 0 命中（阳性对照已通过）。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
