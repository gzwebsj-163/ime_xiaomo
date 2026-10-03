#!/usr/bin/env python3
# mutate_probe.py -- 【探针变异】证明 main_probe_usbpd 不是恒真信号
#
# 🕳️ 为什么需要这个文件 (锚点 H + 锚点 J):
#   上一轮我给 hw_main 登记 USBPD 时, 新写了 main_probe_usbpd 探针, 但
#   没有任何变异打它 —— 这正是我自己刚写进知识页的病: "新写的守卫必须有
#   变异打它, 否则它就是未验证代码 = 假设自己已经修好"。
#   恒真探针比没有探针更险: 它让你"有依据地"放心 (锚点 H: 不具区分力的信号)。
#
#   本脚本把 hw_usbpd 的真表出口打坏, 要求 family 级的
#   `./xiaomo main probeall` 必须【真的报 BAD】。
#   探针在 hw_main.c 里, 判据数据在 hw_usbpd.c 里 —— 所以这个变异必须走
#   家族构建 (make), 不能像 mutate_test.py 那样只编宿主三件套。
#
# 纪律: 每个变异在 finally 还原, 还原后必须复验绿; SKIP 与 SURVIVED 分开报
#       (锚点 F: "我没在看" ≠ "不存在", 混报会把工具故障说成产品缺陷)。

import os, re, shutil, subprocess, sys, tempfile, time

REPO   = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
USBP   = os.path.join(REPO, "usbpd")
USBP_C = os.path.join(USBP, "src", "hw_usbpd.c")
MK     = os.path.join(REPO, "Makefile")
MAIN_C = os.path.join(REPO, "src", "hw", "hw_main.c")
BIN    = os.path.join(REPO, "xiaomo")

RC_OK, RC_BAD, RC_NOLINK = 0, 2, 6          # 与 hw_main.h 的三态约定一致

def bin_mtime():
    try:    return os.path.getmtime(BIN)
    except OSError: return None

def build(force=False):
    """force=True 时全量重编 —— Makefile 变更不在 $(TARGET): $(OBJS) 依赖里,
    不强制就会拿【陈旧二进制】去判变异, 把工具故障读成产品缺陷 (锚点 F)。"""
    cmd = ["make", "-s"] + (["-B"] if force else [])
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr)

def probe_usbpd():
    """跑家族 probeall, 抽 USBPD 那一行的 probe= 值"""
    r = subprocess.run([BIN, "main", "probeall"], cwd=REPO,
                       capture_output=True, text=True, timeout=60)
    for line in r.stdout.splitlines():
        if "USBPD" in line and "probe=" in line:
            m = re.search(r"probe=(\w+)", line)
            return (m.group(1) if m else "?"), r.stdout
    return "MISSING", r.stdout           # 锚点 F: 抽不到行 != 不存在, 单独标记

def selftest_fails():
    """跑家族 selftest, 返回 (是否失败, 输出)。供 cmd 出口类变异判定"""
    r = subprocess.run([BIN, "main", "selftest"], cwd=REPO,
                       capture_output=True, text=True, timeout=60)
    return (r.returncode != 0), r.stdout

# ---- 变异: (名称, 目标文件, 旧文本, 新文本, 期望) ----
# PM1/PM2 打【被测物 hw_usbpd 的真表出口】→ 探针应报 BAD
# PM3 打【链接关系】(从 HW_SRCS 摘掉 hw_usbpd)。
#   ⚠️ 2026-10-03 实测修正: 期望不是 NOLINK 而是【链接失败】。
#      macOS ld64 不把 weak undefined 解析为 0 (3 行最小复现实测, 见 wtest),
#      摘掉既有成员 hw_token.c 表现完全相同 ⇒ 这是平台属性, 非 USBPD 引入。
#      推论: 宿主 macOS 构建下 NOLINK 态【不可达】, 家族 9 个探针的
#      `if (sym == 0) return NOLINK` 分支在本平台全是死代码。
#      真机 ESP-IDF 侧 (xtensa-esp32-elf-ld) 行为未实测, 不在此断言。
MUTATIONS = [
 ("PM1 真表出口返回 NULL → 探针须报 BAD", USBP_C,
  "    if (n_rows) *n_rows = QC_N;\n    return QC_TBL;",
  "    if (n_rows) *n_rows = QC_N;\n    return (const hw_usbpd_qc_row_t*)0;",
  "BAD"),

 ("PM2 真表出口谎报行数=1 → 探针须报 BAD", USBP_C,
  "    if (n_rows) *n_rows = QC_N;",
  "    if (n_rows) *n_rows = 1;",
  "BAD"),

 ("PM3 摘掉 hw_usbpd → 须链接失败 (ld64 不解析 weak undef)", MK,
  " usbpd/src/hw_usbpd.c usbpd/src/hw_usbpd_selftest.c",
  "",
  "BUILD-FAIL"),

 # PM4 打本轮【新加的 selftest [8] 双出口一致性护栏】。
 #   纪律来源: "新写的守卫必须有变异打它" —— 否则它就是未验证代码 = 假设自己已修好。
 #   PM4 把 0x7FFFFFFF 掩码塞回去, 新护栏必须立刻报红。
 ("PM4 塞回 0x7FFFFFFF 掩码 → selftest [8] 双出口护栏须报红",
  os.path.join(REPO, "src", "hw", "hw_main.c"),
  "             ? (int)(HW_MAIN_GOLDEN) : -1;",
  "             ? (int)(HW_MAIN_GOLDEN & 0x7FFFFFFFu) : -1;",
  "SELFTEST-RED"),
]

def main():
    backups = {}
    for f in (USBP_C, MK, MAIN_C):
        backups[f] = open(f, "rb").read()

    rc, log = build()
    if rc != 0:
        print("基线构建失败!\n", log[-3000:]); return 2
    st, out = probe_usbpd()
    if st != "OK":
        print(f"基线探针不是 OK (得到 {st}) —— 先修基线再谈变异。\n", out); return 2
    print("基线: USBPD probe=OK ✅  (family build 干净, 9 类)\n")

    killed, survived, skipped = [], [], []
    try:
        for name, path, old, new, want in MUTATIONS:
            txt = backups[path].decode()
            if old not in txt:
                print(f"  [SKIP ⚠️ ] {name}\n              锚点文本未命中 = 变异没打出去, "
                      f"不等于探针没被证伪 (锚点 F)")
                skipped.append(name); continue
            open(path, "w").write(txt.replace(old, new, 1))
            # 一律 -B 强制全量重编。改 Makefile 根本不在 $(TARGET): $(OBJS) 依赖里,
            # 改 .c 也吃过"还原后 mtime 落在同一秒 → make 判定无需重编"的亏,
            # 两者都导致拿【陈旧二进制】判变异 (锚点 F: 我没在看 ≠ 不存在)。
            # 这里用速度换确定性 —— 陈旧产物的静默失败我已栽过多次。
            before = bin_mtime()
            rc, log = build(force=True)
            if rc != 0:
                got = "BUILD-FAIL"
                mark = "✅" if want == "BUILD-FAIL" else "❌"
                print(f"  [BUILD-FAIL {mark}] {name}"
                      + ("" if want == "BUILD-FAIL" else f"  期望 {want}"))
                (killed if want == "BUILD-FAIL" else survived).append(name)
            else:
                # 🕳️ 效应确认: 变异必须真的落到二进制上, 否则下面的判定全是空判
                if bin_mtime() == before:
                    print(f"  [TOOL-FAIL ❌] {name}  二进制 mtime 未变 = 变异没生效, "
                          f"此判定作废 (工具问题, 不是产品缺陷)")
                    skipped.append(name)
                    open(path, "wb").write(backups[path]); continue
                if want == "SELFTEST-RED":
                    red_, out = selftest_fails()
                    if red_:
                        print(f"  [KILLED ✅] {name}  → selftest 报红 ✅")
                        killed.append(name)
                    else:
                        print(f"  [SURVIVED ❌] {name}  selftest 仍全绿 = 新护栏没判别力!")
                        survived.append(name)
                    open(path, "wb").write(backups[path]); continue
                st, out = probe_usbpd()

                if st == "MISSING":
                    print(f"  [MISSING ❌] {name}  probeall 抽不到 USBPD 行 —— "
                          f"工具口径问题, 需人工看\n{out[-600:]}")
                    survived.append(name)
                elif st == want:
                    print(f"  [KILLED ✅] {name}  → probe={st}")
                    killed.append(name)
                else:
                    print(f"  [SURVIVED ❌] {name}  期望 {want} 实得 {st}  <== 探针无区分力!")
                    survived.append(name)
            open(path, "wb").write(backups[path])   # 还原
    finally:
        for f, b in backups.items():
            open(f, "wb").write(b)

    rc, log = build(force=True)
    st, _ = probe_usbpd() if rc == 0 else ("?", "")
    ok = (rc == 0 and st == "OK")
    print(f"\n还原后复验: {'ALL PASS ✅' if ok else 'STILL RED ❌ (build rc=%s, probe=%s)' % (rc, st)}")
    print(f"探针变异 {len(MUTATIONS)} 个: 杀死 {len(killed)} / 存活 {len(survived)} / 跳过 {len(skipped)}")
    if survived:
        print("存活:", survived)
        print("⚠️ 存活 = main_probe_usbpd 对该缺陷无反应, 不能声称探针有效")
    if skipped:
        print("跳过(工具问题, 需重打锚点):", skipped)
    return 1 if (survived or skipped or not ok) else 0

sys.exit(main())
