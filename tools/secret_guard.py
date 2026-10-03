#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
secret_guard.py —— 提交前秘密扫描 (装进 .git/hooks/pre-commit)
================================================================================
【为什么必须有它】
  2026-10-03 事故链: wifi_cfg.h 里的明文 WiFi 密码随工程并入仓库被 push。
  事后补救是 .gitignore —— 但 .gitignore 只能挡住"没被跟踪的文件", 挡不住:
    ① git add -f 显式强制添加
    ② 文件曾经被跟踪过, 再改回来时 ignore 规则对它无效
    ③ 已经进了索引, 后续任何 commit 都会继续带着它
  也就是说: .gitignore 是【意图的记录】, 不是【执行的闸门】。
  本脚本才是闸门 —— 它在 commit 真正发生前扫描暂存区, 命中即拒绝。

【纪律来源】(我自己写下的, 这里执行它)
  "凡结论写成『因为 X 所以安全』时, X 必须是受控且会报警的
   (如 .gitignore + 提交前扫描), 不能靠人工记忆。"
  —— 当初只做了前半句, 结论就悬空了。本脚本是后半句。

【扫描范围】只扫【暂存区内容】(git show :file), 不扫工作区也不扫历史:
  - 工作区没被 add 的改动不该阻塞别人的提交
  - 扫描 diff 文本会引入 diff 格式噪音; 直接读暂存 blob 最干净
  - 历史已经泄露的部分由 rewrite 处理, 本守卫不负责, 也不假装负责

【退出码】0 = 干净可提交; 1 = 命中, 已阻止提交。
"""
import os
import re
import subprocess
import sys

# ---------------------------------------------------------------------------
# 规则表: (规则名, 正则, 说明)
# 正则一律在【字节】上匹配 —— 家族教训: 含 ANSI 转义/控制字节的文件会让 grep
# 判成 binary 而"不输出匹配行", 看起来像"没命中"。此处直接读 bytes, 绕开该坑。
# ---------------------------------------------------------------------------
RULES = [
    ("WIFI 明文密码",
     rb'(?:WIFI_CFG_PASS|WIFI_PASS|WIFI_PASSWORD)\s*"?\s*["\']([^"\'\n]{1,})["\']',
     "WiFi 密码明文。编译需要请走 sdkconfig/NVS 或环境变量, 不要写进被跟踪的 .h"),
    ("明文密钥",
     rb'(?i)\b(?:api[_-]?key|secret[_-]?key|access[_-]?token|auth[_-]?token)'
     rb'\s*[:=]\s*["\']([^"\'\n]{8,})["\']',
     "疑似硬编码 API key / token"),
    ("私钥块",
     rb'-----BEGIN (?:RSA |EC |OPENSSH |PGP )?PRIVATE KEY-----',
     "私钥文件绝不可入库"),
    ("连接串口令",
     rb'(?i)\b(?:mysql|postgres(?:ql)?|mongodb(?:\+srv)?|redis)://[^\s:@/]+:[^\s:@/]+@',
     "数据库连接串里带口令"),
]

# 允许出现的场景: 规则名 -> (行首子串集合, 命中行以其中之一开头则放行)
#
# 🕳️ 这里踩过一次假探针的坑, 记下来:
#   初版把 "#" 也放进放行集合, 本意是放过 .gitignore 里"含明文 WiFi 密码"
#   那句说明, 结果 `#define WIFI_CFG_PASS "xxx"` 这一行也以 "#" 开头 →
#   被当成注释跳过 → 守卫【一声不响】地放行了明文密码, 而守卫本身看着完全正常。
#   阳性对照一跑就露馅(commit 居然成功了)。教训: 白名单要按【语法角色】分,
#   不能按【首字符】分 —— "#" 既是注释标记(C 系), 也是预处理指令的开头。
#   现在只放行真正的注释行首, 预处理行交给正则自己判(它要求关键字后跟带引号的
#   值, #if/#ifdef 这类自然不会命中)。
ALLOWLINE = {
    "WIFI 明文密码": ("//", "*", "/*"),
    "明文密钥": ("//", "*", "/*"),
}

SKIP_SUFFIX = (".png", ".jpg", ".jpeg", ".gif", ".ico", ".bin", ".elf", ".o",
               ".a", ".pdf", ".zip", ".gz", ".xz", ".woff", ".woff2", ".ttf")


def sh(args):
    return subprocess.run(args, capture_output=True)


def staged_files():
    r = sh(["git", "diff", "--cached", "--name-only", "--diff-filter=ACMRT"])
    if r.returncode != 0:
        return None
    return [x for x in r.stdout.decode("utf-8", "replace").split("\n") if x.strip()]


def staged_blob(path):
    """读暂存区内容(不是工作区)。文件不存在于暂存区 -> None。"""
    r = sh(["git", "show", ":" + path])
    if r.returncode != 0:
        return None
    return r.stdout


def scan(path, data):
    hits = []
    if data is None:
        return hits
    if path.lower().endswith(SKIP_SUFFIX):
        return hits
    for lineno, raw in enumerate(data.split(b"\n"), 1):
        for rname, pat, advice in RULES:
            for m in re.finditer(pat, raw):
                val = m.group(m.lastindex) if m.lastindex else b""
                # 排除明显是占位符/空/掩码的值
                vs = val.decode("utf-8", "replace").strip()
                if not vs or vs in ("", "xxx", "your_password", "<passwd>", "***"):
                    continue
                if len(vs) <= 3 and set(vs) <= set("*#."):
                    continue
                stripped = raw.lstrip()
                if any(stripped.startswith(p.encode())
                       for p in ALLOWLINE.get(rname, ())):
                    continue          # 注释行, 放行
                hits.append((lineno, rname, advice, vs[:6] + "***"))
                break
    return hits


def main():
    files = staged_files()
    if files is None:
        sys.stderr.write("[secret-guard] 无法读取暂存区, 拒绝放行(不静默通过)\n")
        return 1
    if not files:
        return 0

    allhits = []
    for f in files:
        for h in scan(f, staged_blob(f)):
            allhits.append((f,) + h)

    if not allhits:
        return 0

    sys.stderr.write("\n" + "=" * 74 + "\n")
    sys.stderr.write("[secret-guard] ⛔ 提交被阻止: 暂存区内检出疑似明文凭据\n")
    sys.stderr.write("=" * 74 + "\n")
    for f, lineno, rname, advice, mask in allhits:
        sys.stderr.write("  %s:%d\n" % (f, lineno))
        sys.stderr.write("      规则: %s   值: %s\n" % (rname, mask))
        sys.stderr.write("      建议: %s\n" % advice)
    sys.stderr.write("-" * 74 + "\n")
    sys.stderr.write("  这不是误报就说明确实有东西会被上传。\n")
    sys.stderr.write("  若确认要提交, 请改用 NVS / 环境变量 / .gitignore 排除该文件,\n")
    sys.stderr.write("  不要用 --no-verify 绕过 —— 那正是当初出事时的做法。\n")
    sys.stderr.write("=" * 74 + "\n")
    return 1


if __name__ == "__main__":
    sys.exit(main())
