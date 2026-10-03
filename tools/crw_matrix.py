#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
crw_matrix.py —— hw_dmc_crw 模块的变异测试矩阵 (锚点 J: V-V 验证工具本身的验证)
================================================================================
【为什么变异打在源码副本上, 而不是测试文件上】
  家族坑 #45/#47: 全绿的测试不证明测试有效。而"变异"若打在 selftest 自己的
  期望值上, 被测模块一个字节没动 —— 那是测试自己改口, 零证据。

  本脚本的变异全部改【src/hw/hw_dmc_crw.c 的临时副本】, selftest 一个字不动,
  重编后必须变红。变红 = 该守卫真承重。

【为什么改副本而不是改仓库里的原文件再还原】
  锚点 K 的修法: 【结构性消除隐患, 而不是事后打补丁】。原地改 + trap 还原, 一旦
  中途被 Ctrl-C / 断电 / 异常退出, 仓库就留下一个带变异的源文件, 而校验器 / git
  status 未必第一时间发现。改副本 ⇒ 仓库零写入, 污染在结构上不可能发生。

【阴性对照】
  第 1 步先跑【未变异基线】, 必须绿。若基线本身就红, 后面"变异被杀掉"就毫无意义
  (无法区分"变异被抓住"与"本来就红"), 脚本直接中止退出。
  判"被杀掉"用的是 selftest 的【真实退出码】, 不是 grep 输出 —— 避免又一个
  "看起来抓到了"的口径。

【归档零改动】
  全程额外校验 src/hw/hw_dmc_base.c 的 md5 前后一致 —— 本模块是归档之外的新
  文件, 归档一个字都不许动。
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
INC = os.path.join(ROOT, "include")
MODULE = os.path.join(ROOT, "src", "hw", "hw_dmc_crw.c")
SELFTEST = os.path.join(ROOT, "tests", "crw_selftest.c")
ARCHIVE = os.path.join(ROOT, "src", "hw", "hw_dmc_base.c")

# ---------------------------------------------------------------------------
# 变异表: 每条 = (id, 说明, 原文, 变异后, 预期被哪几条断言咬死)
# 原文串在源文件里必须【唯一】(脚本会断言替换次数恰为 1)。
# ---------------------------------------------------------------------------
MUTANTS = [
    ("M1", "crw_reg_bit 的 bit>=32 越界守卫",
     "if (bit >= 32U) return CRW_ERR_PARAM;   /* 越界直接判负, 不静默截断 */",
     "if (0)             return CRW_ERR_PARAM;   /* MUTANT M1 */",
     "[3a][3b][3c] 越界 bit 必须判负 (原码 >>2193 在 x86 上退化成 >>17)"),

    ("M2", "crw_cpt_push 的槽位上界守卫",
     "if (n >= (uint16_t)CRW_SLOT_MAX) return CRW_ERR_FULL;   /* 上界, 不静默回绕 */",
     "if (0)                              return CRW_ERR_FULL;   /* MUTANT M2 */",
     "[9d] 第 33 次 push 必须报 FULL, 不得静默回绕覆盖槽0"),

    ("M3", "crw_wait_trig 的 max_polls==0 守卫",
     "if (max_polls == 0U) return CRW_ERR_PARAM;",
     "if (0)               return CRW_ERR_PARAM;   /* MUTANT M3 */",
     "[10d] 零次轮询必须判负, 防'没轮询却看似跑了'的假通过"),

    ("M4", "crw_dump 的 NULL 守卫",
     "if (v == NULL) return CRW_ERR_PARAM;\n"
     "    return (int)(v->size & 0x7FFFFFFFU);",
     "if (0)          return CRW_ERR_PARAM;\n"
     "    return (int)(v->size & 0x7FFFFFFFU);   /* MUTANT M4 */",
     "[6b] crw_dump(NULL) 必须判负而不是解引用空指针"),

    ("M5", "crw_put 的非法入口哨兵检查",
     "if (point == DMC_ILLEGAL_ENTRY) return CRW_ERR_PARAM;",
     "if (0)                               return CRW_ERR_PARAM;   /* MUTANT M5 */",
     "[7a] 非法入口哨兵必须判负 (原码写成 = 赋值, 根本不是比较)"),

    ("M6", "crw_reg_read 的无 BSP 守卫",
     "if (g_reg_read == NULL) return 0U;",
     "if (0)                    return 0U;   /* MUTANT M6 */",
     "[1c] 无 BSP 时必须返回 0, 而不是调用空函数指针 (原码是裸解引用猜测地址)"),
]

CC = os.environ.get("CC", "cc")
CC_FLAGS = ["-std=c11", "-Wall", "-Wextra", "-Werror"]


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def build_and_run(workdir, module_src, label):
    """在【全新目录】里编译并运行 —— 陈旧 .o 在结构上不可能被复用。"""
    exe = os.path.join(workdir, "selftest")
    cproc = subprocess.run(
        [CC] + CC_FLAGS + ["-I", INC, "-o", exe, SELFTEST, module_src],
        capture_output=True, text=True)
    if cproc.returncode != 0:
        return ("BUILD_FAIL", cproc.stderr.strip()[:400], -999)
    try:
        rproc = subprocess.run([exe], capture_output=True, text=True, timeout=10)
    except subprocess.TimeoutExpired:
        return ("TIMEOUT", "selftest 运行超过 10s 未返回 (疑变异引入了死循环)", -998)
    out = (rproc.stdout or "") + (rproc.stderr or "")
    return ("RAN", out, rproc.returncode)


def main():
    for p in (MODULE, SELFTEST, ARCHIVE):
        if not os.path.isfile(p):
            print("FATAL 缺少文件: %s" % p)
            return 2

    arch_md5_before = md5(ARCHIVE)
    module_md5_before = md5(MODULE)
    orig_src = open(MODULE, "rb").read()

    killed, survived, broken = 0, [], []

    # ---------------- 阴性对照: 未变异基线必须绿 ----------------
    with tempfile.TemporaryDirectory() as td:
        m = os.path.join(td, "mut.c")
        with open(m, "wb") as f:
            f.write(orig_src)
        kind, out, rc = build_and_run(td, m, "baseline")
    if kind != "RAN" or rc != 0 or "ALL PASS" not in out:
        print("FATAL 基线本身就红 (kind=%s rc=%s) —— 变异矩阵无意义, 中止。" % (kind, rc))
        print("  --- 原始输出 ---")
        print(out)
        return 2
    n_assert = 0
    for tok in out.split("("):
        if tok.startswith("selftest:"):
            for d in tok:
                if d.isdigit():
                    n_assert = int("".join(
                        c for c in tok[:tok.find(")")] if c.isdigit())) or 0
                    break
    print("[baseline] 未变异基线绿 (rc=0, ALL PASS) —— 阴性对照成立, 继续")
    print("[baseline] 断言数: %s" % (n_assert if n_assert else "见输出"))
    print()

    # ---------------- 逐条变异 ----------------
    for mid, name, old, new, kills in MUTANTS:
        cnt = orig_src.count(old.encode("utf-8"))
        if cnt != 1:
            print("[%s] SKIP  原文串在模块里出现 %d 次 (要求恰好 1 次) —— "
                  "变异无法定位, 不算数。" % (mid, cnt))
            survived.append(mid + "(定位失败)")
            print()
            continue

        with tempfile.TemporaryDirectory() as td:
            m = os.path.join(td, "mut.c")
            with open(m, "wb") as f:
                f.write(orig_src.replace(old.encode("utf-8"),
                                         new.encode("utf-8"), 1))
            kind, out, rc = build_and_run(td, m, mid)

        if kind == "BUILD_FAIL":
            verdict, why = "KILLED(build)", "变异引入了编译错误, 被构建闸门拦下"
        elif kind == "TIMEOUT":
            verdict, why = "KILLED(hang)", "selftest 挂死, 被超时闸门拦下"
        elif rc == 0 and "ALL PASS" in out:
            verdict, why = "SURVIVED ❌", "变异后 selftest 仍然全绿 —— 这个守卫是假的"
            survived.append(mid)
        elif rc < 0:
            # 区分"被断言咬死"与"被信号打死" —— 两者都算杀掉, 但证据强度不同,
            # 必须如实标注, 不能让读者以为都是断言生效了。
            verdict = "KILLED(sig%s)" % (-rc,)
            why = "变异引发了信号 %d (段错误/总线错误等)" % (-rc)
            killed += 1
        else:
            verdict, why = "KILLED(assert)", "selftest 变红 (rc=%d)" % rc
            killed += 1

        fails = [ln for ln in out.splitlines() if "[FAIL]" in ln]
        if fails:
            detail = " ; ".join(f.strip() for f in fails[:3])
        elif kind == "TIMEOUT":
            detail = "(无输出, 进程被判定挂死)"
        elif rc < 0:
            detail = "(无 [FAIL] 行 —— 进程被信号打死, 守卫失守导致崩溃)"
        else:
            last = [l for l in out.strip().splitlines() if l.strip()]
            detail = last[-1].strip() if last else "(空输出)"
        print("[%s] %-6s %s" % (mid, verdict, name))
        print("      应被咬死的断言: %s" % kills)
        print("      实际: %s" % detail)
        if verdict.startswith("SURVIVED"):
            print("      ⚠️  %s" % why)
        print()

    # ---------------- 收尾: 校验仓库零写入 ----------------
    arch_md5_after = md5(ARCHIVE)
    module_md5_after = md5(MODULE)
    ok_arch = (arch_md5_before == arch_md5_after)
    ok_mod = (module_md5_before == module_md5_after)

    print("=" * 78)
    print("变异矩阵: 杀掉 %d / %d" % (killed, killed + len(survived)))
    if survived:
        print("存活变异(裁判有盲点, 必须补断言或承认该守卫无法被测): %s" %
              ", ".join(survived))
    print("归档 hw_dmc_base.c md5 %s  (前=%s 后=%s)" %
          ("未改动 ✅" if ok_arch else "被改动 ❌", arch_md5_before, arch_md5_after))
    print("模块 hw_dmc_crw.c   md5 %s" % ("未改动 ✅" if ok_mod else "被改动 ❌"))
    print("RESULT: %s" % ("ALL KILLED" if not survived and ok_arch and ok_mod
                          else "FAIL"))
    return 0 if (not survived and ok_arch and ok_mod) else 1


if __name__ == "__main__":
    sys.exit(main())
