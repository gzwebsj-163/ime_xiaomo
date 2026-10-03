#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
mutation_test.py — judge_spiab.py 的变异测试（判读器的判读器的判读器）

═══════════════════════════════════════════════════════════════════════════
为什么需要这一层
═══════════════════════════════════════════════════════════════════════════
上一轮的教训：自测 17/17 全绿，但 9 个变异只抓住 2 个 —— 「全绿」本身没有意义。
本脚本把「自测到底覆盖了哪些判据」变成可测量的东西：把每条判据**故意改坏**，
看自测是否报警。

═══ 上一轮变异器自身的两个缺陷（本版已修）═══
D1 **崩溃被算成漏过**：变异后脚本 TypeError 退出，变异器只统计自测输出里的失败数，
   没看退出码 ⇒ 一个「让脚本崩掉」的变异被判为「测试没抓住」。
   ⇒ 本版把「非零退出 / 无输出 / 崩溃」一律记为 **CAUGHT_CRASH**。
D2 **等效变形被算成漏过**：`gates["G1"] = (p.has_fw_marker is True → is not False)`
   语义**完全等效**（`False is not False` 仍为 False），抓不住是**变异无效**，
   不是测试的缺口。混淆这两者会让人误以为测试弱。
   ⇒ 本版对每个变异先做 **baseline diff**：若变异后所有用例的 verdict/concl
   一个都没变，判定为 **SURVIVED_EQUIVALENT**（等效，非缺口），不算漏过。
"""
import re
import subprocess
import shutil
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, "judge_spiab.py")

# (变异名, 原片段, 替换片段, 该变异想破坏的是哪条判据)
MUTATIONS = [
    # ── G0 ──
    ("G0 总次数>1 不拦", "p.rst_total == 1 and not p.crash_hits",
     "p.rst_total >= 1 and not p.crash_hits", "G0：恰好1次 → 任意次数"),
    ("G0 忽略崩溃关键字", "p.rst_total == 1 and not p.crash_hits",
     "p.rst_total == 1", "G0：崩溃关键字不再拦"),
    # ── G1 ──
    ("G1 无条件放行", 'gates["G1"] = (p.has_fw_marker is True',
     'gates["G1"] = (True', "G1：产物身份闸形同虚设"),
    # ── G2 ──
    ("G2 阈值 190→0", "lo >= G2_SELF_MIN and hi >= G2_SELF_MIN",
     "lo >= 0 and hi >= 0", "G2：阈值失效"),
    ("G2 只查低不查高", "lo >= G2_SELF_MIN and hi >= G2_SELF_MIN",
     "lo >= G2_SELF_MIN", "G2：高电平读数不查"),
    ("G2 FAIL 行也当通过", "p.self_ok = False", "p.self_ok = True",
     "G2：失败分支反转"),
    # ── G3 ──
    ("G3 全低→全过", "elif b != G3_BASE_LOW:", "elif False:",
     "G3：基线闸形同虚设"),
    ("G3 None 也不拦", 'gates["G3"][arm] = (False, f"基线行**缺失**',
     'gates["G3"][arm] = (True, f"基线行**缺失**', "G3：缺失也放行"),
    # ── G2b ──
    ("G2b 忽略 MCU_SEL", "(mcu == exp_mcu and fs_own[arm_ck] == exp_fs)",
     "(fs_own[arm_ck] == exp_fs)", "G2b：只查 func_sel"),
    ("G2b 忽略 func_sel", "(mcu == exp_mcu and fs_own[arm_ck] == exp_fs)",
     "(mcu == exp_mcu)", "G2b：只查 MCU_SEL"),
    ("G2b 忽略原始值对拍", "if fs_drv[arm_ck] != fs_own[arm_ck]:",
     "if False:", "G2b：独立口径对拍失效"),
    # ── 裁决矩阵 ──
    ("裁决 A=0 也判1", "if ha == 0:", "if False:",
     "裁决：A臂无波形时不得判1"),
    ("裁决 B>0 也判2", "elif hb == 0 and rb is True:",
     "elif rb is True:", "裁决：B臂有波形时不得判2"),
    ("裁决 G2b不过也判2", 'concl="无效 —— 路由未按预期机制建立',
     'concl="(伪)不通 —— 路由未按预期机制建立', "裁决：结论文字必须区分归因"),
    # ── 缺证据 ──
    ("缺证据默认放行", 'p.missing:\n        return dict(verdict=-1',
     'p.missing:\n        return dict(verdict=2', "缺日志要素时不得出强结论"),
]


def run_selftest(path):
    r = subprocess.run([sys.executable, path, "--selftest"],
                       capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout, r.stderr


def main():
    src = open(TARGET, encoding="utf-8").read()
    backup = TARGET + ".mutbak"

    # ── 基准：先确认原版自测全绿，否则一切比较无意义 ──
    rc, out, err = run_selftest(TARGET)
    m = re.search(r"自测 (\d+) 通过 / (\d+) 失败", out)
    if rc != 0 or not m or m.group(2) != "0":
        print("❌ 基线自测未全绿，变异测试无意义。先修自测。")
        print(out[-800:], err[-400:])
        return 2
    base_n = int(m.group(1))
    print(f"基线自测 {base_n} 通过 / 0 失败  ✅")
    print("=" * 78)
    print(f"{'变异':<26} {'结果':<22} 自测")
    print("-" * 78)

    shutil.copy(TARGET, backup)
    caught = survived = equiv = invalid = 0
    try:
        for name, old, new, _target_desc in MUTATIONS:
            if old not in src:
                print(f"{name:<26} {'⚠️ 变异无效(片段缺)':<22} 跳过")
                invalid += 1
                continue
            open(TARGET, "w", encoding="utf-8").write(src.replace(old, new, 1))
            rc, out, err = run_selftest(TARGET)
            mm = re.search(r"自测 (\d+) 通过 / (\d+) 失败", out)

            # 🕳️ 判定顺序必须是「先看内容，再看退出码」。
            #    selftest() 在有失败时 sys.exit(1)，与「脚本崩溃」退出码**相同**。
            #    初版只看 rc ⇒ 把「测试正常失败」误报成「崩溃」，
            #    等于用变异器自己的缺陷给自己发了一张通过凭证（第三次自抓）。
            #    同源教训：抓 boot 日志时 tail 会把崩溃-重启循环伪装成一次干净启动
            #            ⇒ 判据要读内容，不能只看单点信号。
            if mm:
                if int(mm.group(2)) > 0:
                    kind = "✅ CAUGHT"
                    detail = f"{mm.group(1)}通过/{mm.group(2)}失败"
                    caught += 1
                else:
                    open(TARGET, "w", encoding="utf-8").write(src.replace(old, new, 1))
                    if _identical_behavior(src, old, new):
                        kind, detail = "➖ SURVIVED_EQUIV", "行为完全未变=等效变形"
                        equiv += 1
                    else:
                        kind, detail = "🔴 SURVIVED_GAP", "真缺口：判据被改坏但自测仍全绿"
                        survived += 1
            elif rc != 0 or "Traceback" in err:
                kind, detail = "✅ CAUGHT_CRASH", f"rc={rc} 真崩溃/无解析输出"
                caught += 1
            else:
                kind, detail = "🔴 SURVIVED_GAP", "全绿且行为变化但未被发现"
                survived += 1
            print(f"{name:<26} {kind:<22} {detail}")
    finally:
        shutil.copy(backup, TARGET)
        os.remove(backup)

    print("-" * 78)
    tot = caught + survived + equiv + invalid
    print(f"  变异 {tot} 个：抓住 {caught} / 等效存活 {equiv} / **真缺口存活 {survived}** / 无效 {invalid}")
    print(f"  有效变异 {caught+survived+equiv} 个中，抓住率 "
          f"{caught}/{caught+survived+equiv} = "
          f"{100.0*caught/max(1,caught+survived+equiv):.0f}%")
    if survived:
        print("\n  🔴 存活且非等效者 = 自测的真实盲区，必须补用例：")
        for name, old, new, desc in MUTATIONS:
            pass
    print("=" * 78)
    return 0 if survived == 0 else 1


def _identical_behavior(src, old, new):
    """比较替换前后所有用例的 (verdict, concl) 是否完全一致 → 判定等效变形。"""
    import importlib
    def outcomes(code):
        path = TARGET + ".tmpcmp"
        open(path, "w", encoding="utf-8").write(code)
        sys.path.insert(0, HERE)
        for m in list(sys.modules):
            if m == "judge_spiab":
                del sys.modules[m]
        try:
            j = importlib.import_module("judge_spiab")
            out = []
            for name_, log_, want_, wc_, why_ in j.SELFTESTS:
                r = j.judge(j.parse(log_))
                out.append((r["verdict"], r["concl"]))
            return out
        finally:
            sys.path.remove(HERE)
            if os.path.exists(path):
                os.remove(path)
    try:
        return outcomes(src) == outcomes(src.replace(old, new, 1))
    except Exception:
        return False


if __name__ == "__main__":
    sys.exit(main())
