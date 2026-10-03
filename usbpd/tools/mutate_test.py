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
    r2 = subprocess.run([BIN, "usbpd", "selftest"],
                        capture_output=True, text=True, timeout=60)
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
  "    (void)s_bsp.i2c_read;   /* 显式标注: 未来实现点就在这里 */\n    return HW_USBPD_ERR_UNSUPPORT;   /* PDO 解码未实现 (第③期) —— 绝不编造 PDO */",
  "    (void)s_bsp.i2c_read;\n    pdo_list[0] = 0x0001; *n_out = 1; return HW_USBPD_OK;"),

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

 # ---- M11..M15: 打 [16] "at <mv>" 的新守卫 ----
 # 🕳️ 纪律同 M9/M10: 每加一条守卫就配一个变异, 否则它就是未验证代码。
 #
 # 🔴 M11 的来历是个真实的自我纠错 (2026-10-03):
 #   我原先给"空参数"单写了一句 `if (!*p) return ERR_PARAM;` 专门拦空串。
 #   配的变异把它删掉 → selftest【全绿, 存活】⇒ 那句是冗余死代码, 因为
 #   strtol 的 end==p 已经把空串一起兜住了。
 #   按"没被任何变异杀死的守卫 = 没被依赖"的纪律, 已把那句从源码删掉,
 #   改为只留注释说明真实机制。变异本身也改成打【真正的依赖】:
 #   把 strtol 换成 atoi —— 那才是"空串静默变成 0mV/SDP"真正会发生的地方。
 ("M11 strtol 换 atoi (空串静默变 0mV)", SRC,
  "        mv = strtol(p, &end, 10);\n        if (end == p || *end != '\\0') return HW_USBPD_ERR_PARAM;  /* (a) 空/非法 */",
  "        mv = atoi(p); (void)end;"),

 ("M12 拆掉负线压守卫 (b)", SRC,
  "        if (mv < 0) return HW_USBPD_ERR_PARAM;              /* (b) 负线压 */\n", ""),

 ("M13 拆掉越界守卫 (c)", SRC,
  "        if (mv > (long)s_tap->raw_rail_mv * (long)s_tap->ratio_milli / 1000)\n            return HW_USBPD_ERR_PARAM;                      /* (c) 越界 */\n",
  ""),

 ("M14 cli 退回只读 argv[2] (多词命令不可达)", SRC,
  "        for (i = 2; i < argc; i++) {",
  "        for (i = 2; i < 3; i++) {"),

 # M15: 让容差失效 —— 四档判据若退化成"全落同一档", [16] 的互异断言必须红。
 #   改 classify 的容差比较为恒真 (任何线压都命中第一行 QC_5V)。
 ("M15 判据容差恒真 (四档退化成同一档)", SRC,
  "        if (a <= HW_USBPD_TOL_MV && -a <= HW_USBPD_TOL_MV &&\n            b <= HW_USBPD_TOL_MV && -b <= HW_USBPD_TOL_MV)",
  "        if (1)"),
]

def main():
    backups = {}
    for f in (SRC, HDR):
        backups[f] = open(f, "rb").read()

    rc, out = build_and_run()
    if rc is None:
        print("基线编译失败!\n", out); return 3
    # 🕳️ 基线守卫之二 (锚点 J: 绿色的用例也可能是假的):
    #   仅凭 rc==0 不能证明"selftest 真的跑过了"。驱动 usbpd_host_main 改成
    #   纯转发 hw_usbpd_cli 后, 无参调用会打 usage 并 return 2 —— rc!=0 尚能拦住;
    #   但若哪天的驱动改成"无参也 return 0", 全部 15 个变异会被判"杀死",
    #   而实际上一条断言都没跑 = 极端假通过。故必须确认输出的形状。
    if "selftest:" not in out:
        print("基线输出里找不到 selftest 特征串 => 驱动调用方式失配, 变异结论不可信!\n",
              out[:400]); return 4
    if rc != 0:
        print("基线 selftest 未通过! 先修基线再谈变异。\n", out); return 2
    print("基线: ALL PASS ✅ (且已确认 selftest 真的跑过)\n")

    survived, killed, skipped, crashed = [], [], [], []
    try:
        for name, path, old, new in MUTATIONS:
            txt = backups[path].decode()
            if old not in txt:
                # 🕳️ SKIP 与 SURVIVED 必须分开报 (锚点 F): 锚点文本失配是
                #   【脚本没跟上源码】的工具故障, 不是"产品抓不到该缺陷"。
                #   两者混在一个列表里 ⇒ 会把工具故障说成产品缺陷, 进而
                #   让人去改本来正确的代码 (本轮 M5 就这样混报过一次)。
                print(f"  [SKIP ⚠️ ] {name} -- 锚点文本未命中 (脚本锚点过期, 需重打)")
                skipped.append(name); continue
            open(path, "w").write(txt.replace(old, new, 1))
            rc, out = build_and_run()
            if rc is None:
                print(f"  [BUILD-FAIL] {name} (变异导致编译失败, 编译器抓到)")
                killed.append(name)
            elif rc < 0:
                # 🕳️【成因分类, 2026-10-03 加】Python 里 rc<0 = 被信号打死
                #   (如 SIGSEGV=-11)。这【也】是"被杀", 但它和"断言报红"
                #   完全是两回事:
                #     · 断言杀 = 守卫真的按设计命中了那条路(强证据)
                #     · 崩溃杀 = 变异把程序带到了没设防的地方(弱证据)
                #   混成一句 "KILLED ✅" 会让我以为"我加的 !T 守卫生效了",
                #   而真相可能只是"解引用炸了"—— 守卫【根本没被走到】。
                #   这正是我自己记过的"失败消息硬编码比没有原因更坏"。
                #   纪律: 崩溃杀必须【点名】, 不能计入"守卫已验证"账上。
                print(f"  [KILLED-BY-CRASH ⚠️] {name}  rc={rc} "
                      f"(信号打死, 非断言命中 => 不可据此声称相关守卫已验证)")
                killed.append(name); crashed.append(name)
            elif rc > 0:
                fails = [l.strip() for l in out.splitlines() if "[FAIL]" in l or "[VAR-FAIL]" in l]
                print(f"  [KILLED ✅] {name}  (断言命中, 守卫确实走到了)")
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
    print(f"变异 {len(MUTATIONS)} 个: 杀死 {len(killed)} / 存活 {len(survived)} / 跳过 {len(skipped)}")
    if crashed:
        print(f"⚠️  其中【崩溃杀】{len(crashed)} 个 (非断言命中, 不可据此声称相关守卫已验证):", crashed)
    if survived:
        print("存活变异 (= selftest 抓不到该缺陷, 不能声称'自校验有效'):", survived)
    if skipped:
        print("跳过 (= 工具锚点过期, 不是产品缺陷):", skipped)
    return 1 if (survived or skipped) else 0

sys.exit(main())
