#!/usr/bin/env python3
# mutate_test.py -- 【变异测试】证明 selftest 真的会红
#
# 锚点 J: 全绿的测试不证明测试有效, 要把裁判承重先称裁判。
# 本脚本对 hw_usbpd.c 逐个注入已知缺陷, 每次重编 + 跑 selftest,
# 要求【必须红】。若某变异存活 = 裁判有盲点, 不是产品通过。
#
# 纪律: 每个变异都在 finally 里还原, 且还原后必须复验绿。

import os, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC  = os.path.join(ROOT, "src", "hw_usbpd.c")
HDR  = os.path.join(ROOT, "include", "hw_usbpd.h")
ST   = os.path.join(ROOT, "src", "hw_usbpd_selftest.c")
MAIN = os.path.join(ROOT, "tools", "usbpd_host_main.c")
BIN  = os.path.join(tempfile.gettempdir(), "usbpd_mut")

def build_and_run():
    r = subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-O2",
                        "-I", os.path.join(ROOT, "include"),
                        SRC, ST, MAIN, "-o", BIN],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, r.stderr
    r2 = subprocess.run([BIN], capture_output=True, text=True, timeout=60)
    return r2.returncode, r2.stdout

# ---- 变异: (名称, 目标文件, 旧文本, 新文本, 期望"必须失败") ----
MUTATIONS = [
 ("M1 容差退回 300mV (c3_adc 原始值)", HDR,
  "#define HW_USBPD_TOL_MV 50", "#define HW_USBPD_TOL_MV 300"),

 ("M2 塞回 c3_adc 的死代码行 {500,0}", SRC,
  "    {  600,    0, HW_USBPD_DCP_BC12  },",
  "    {  600,    0, HW_USBPD_DCP_BC12  },\n    {  500,    0, HW_USBPD_DCP_SHORT },"),

 ("M3 拆掉悬空拾波闸", SRC,
  "    if (dp_raw > s_tap->raw_rail_mv || dm_raw > s_tap->raw_rail_mv) {",
  "    if (0) {"),

 ("M4 BSP 卸载不生效 (分水岭失效)", SRC,
  "    if (bsp) s_bsp = *bsp;\n    else    memset(&s_bsp, 0, sizeof(s_bsp));",
  "    if (bsp) s_bsp = *bsp;"),

 ("M5 无 FUSB302 时编造假 PDO (假成功)", SRC,
  "    if (!s_bsp.i2c_read) return HW_USBPD_ERR_UNSUPPORT;\n    return HW_USBPD_ERR_UNSUPPORT;   /* 驱动待硬件到位后实现 */",
  "    if (!s_bsp.i2c_read) { *n_out = 1; pdo_list[0] = 0x0001; return HW_USBPD_OK; }\n    return HW_USBPD_ERR_UNSUPPORT;"),

 ("M6 未装 BSP 时假装读到 0mV", SRC,
  "        rc_dp = HW_USBPD_ERR_NODEV;\n        rc_dm = HW_USBPD_ERR_NODEV;",
  "        rc_dp = 0; dp_raw = 0;\n        rc_dm = 0; dm_raw = 0;"),

 ("M7 错误码统一压成 ERR_HW (丢失 ERR_NODEV)", SRC,
  "        if (rc_dp == HW_USBPD_ERR_NODEV || rc_dm == HW_USBPD_ERR_NODEV)\n            return HW_USBPD_ERR_NODEV;\n        return HW_USBPD_ERR_HW;",
  "        return HW_USBPD_ERR_HW;"),

 ("M8 改分压系数 (黄金值应失配)", SRC,
  '"19k/10k", 2960u,', '"19k/10k", 2961u,'),

 # ---- M9/M10: 专门打【本轮新加的两条守卫】----
 # 🕳️ 上一轮的教训反过来用: 新写的守卫如果没被任何变异触发, 它就是
 #    未验证代码 = 假设自己已经修好。所以每加一条守卫就配一个变异。
 ("M9 真表出口返回 NULL (打 !T 守卫)", SRC,
  "    if (n_rows) *n_rows = QC_N;\n    return QC_TBL;",
  "    if (n_rows) *n_rows = QC_N;\n    return (const hw_usbpd_qc_row_t*)0;"),

 ("M10 真表出口谎报行数 (打 n<4 守卫)", SRC,
  "    if (n_rows) *n_rows = QC_N;",
  "    if (n_rows) *n_rows = 1;"),
]

def main():
    backups = {}
    for f in (SRC, HDR):
        backups[f] = open(f, "rb").read()

    rc, out = build_and_run()
    if rc != 0:
        print("基线 selftest 未通过! 先修基线再谈变异。\n", out); return 2
    print("基线: ALL PASS ✅\n")

    survived, killed = [], []
    try:
        for name, path, old, new in MUTATIONS:
            txt = backups[path].decode()
            if old not in txt:
                print(f"  [SKIP] {name} -- 锚点文本未命中 (源码变了?)"); survived.append(name); continue
            open(path, "w").write(txt.replace(old, new, 1))
            rc, out = build_and_run()
            if rc is None:
                print(f"  [BUILD-FAIL] {name} (变异导致编译失败, 编译器抓到)")
                killed.append(name)
            elif rc != 0:
                fails = [l.strip() for l in out.splitlines() if "[FAIL]" in l or "[VAR-FAIL]" in l]
                print(f"  [KILLED ✅] {name}")
                for l in fails[:2]:
                    print(f"            {l}")
                killed.append(name)
            else:
                print(f"  [SURVIVED ❌] {name}  <== 裁判盲点!")
                survived.append(name)
            open(path, "wb").write(backups[path])   # 还原
    finally:
        for f, b in backups.items():
            open(f, "wb").write(b)

    rc, out = build_and_run()
    print(f"\n还原后复验: {'ALL PASS ✅' if rc == 0 else 'STILL RED ❌'}")
    print(f"变异 {len(MUTATIONS)} 个: 杀死 {len(killed)} / 存活 {len(survived)}")
    if survived:
        print("存活变异:", survived)
        print("⚠️ 存活 = selftest 抓不到该缺陷, 不能声称'自校验有效'")
    return 1 if survived else 0

sys.exit(main())
