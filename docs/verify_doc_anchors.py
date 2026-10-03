#!/usr/bin/env python3
"""校验 docs/ 里「行号锚点」是否还对得上源码。

## 为什么需要这个

hw 系列的解析文档大量使用 `src/hw/hw_pin.c:737` 这种**行号锚点**,
读者会照着跳转核对。问题是: 任何人改一行代码, 锚点就集体漂移,
而**文档本身不会发出任何错误**——它照常渲染、照常可读, 只是内容已经对不上。
这与「验证工具静默失败」同源: 错的不是产品, 是那份让人以为自己在核对的东西。

本脚本把「过期」从静默状态变成**可机械检出的显式状态**。

## 用法

    python3 docs/verify_doc_anchors.py                # 查当前 HEAD
    python3 docs/verify_doc_anchors.py --ref 2c16940  # 查某个历史提交
    python3 docs/verify_doc_anchors.py --verbose      # 列出每一处(默认只报失效的)

## 退出码

    0 = 全部有效
    1 = 有失效锚点

## 注意(锚点F 的教训: 检索工具自身也会静默失败)

- 用 `git show` 取历史内容, **不用** checkout(不污染工作区)。
- 解析失败/文件不存在一律**计数并显式报告**, 绝不静默跳过——
  「没找到锚点」和「锚点有效」必须能在输出上区分开。
"""
import argparse
import os
import re
import subprocess
import sys

DOCS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(DOCS_DIR)

# 两种写法都要认:
#   带目录  `src/hw/hw_pin.c:737`
#   裸文件名 `hw_pin.c:737`   ← 文档里用得更多, 早期只认带目录的 ⇒ 漏检 22/31
ANCHOR = re.compile(
    r'(?<![\w/.-])((?:[A-Za-z0-9_.-]+/)*[A-Za-z0-9_-]+\.(?:c|h|cpp|py|sh|mo|js|vue|md)):(\d+)(?:[-,](\d+))?'
)

# 仓库内所有候选源文件(用于把裸文件名解析成真实路径)
SRCS = ('.c', '.h', '.py', '.sh', '.mo', '.js', '.vue')

# 🆕 兄弟仓库: 文档会引用 xiaomo-flasher / xiaomo-flash-box, 它们不在本仓库内。
# 实测 /Users/root1/cow/xiaomo-flasher **没有 .git** ⇒ 只能读磁盘, 不能 git show。
COW = os.path.expanduser('~/cow')
EXTRA_ROOTS = [os.path.join(COW, n) for n in ('xiaomo-flasher', 'xiaomo-flash-box')
               if os.path.isdir(os.path.join(COW, n))]

# 符号允许的漂移窗口。太大 → 抓不住(符号在文件里到处都有);
# 太小 → 正常的代码排版微调就误报。3 行是实测折中。
WINDOW = 3

IDENT = re.compile(r'`([^`\n]{3,})`|\b([A-Za-z_][A-Za-z0-9_]{3,})\s*\(')
# 明显不是源码符号的词, 抽出来只会制造假漂移
STOP = {'include', 'return', 'const', 'static', 'struct', 'int', 'void', 'char',
        'if', 'for', 'while', 'sizeof', 'uint32', 'uint8', 'uint16', 'true', 'false',
        'note', 'warning', 'todo', 'https', 'http', 'null', 'None', 'self', 'args',
        'int16', 'int32', 'int64', 'int8', 'uint', 'void', 'char', 'long', 'short'}


def nearby_symbols(text, anchor_start, anchor_end):
    """从文档该行里、锚点附近抽取「文档声称这一行是什么」的符号。

    ⚠️ 必须**剔除锚点自身的跨度**。v2 忘了这一步, 于是从路径字符串
    `hw_flash.h:1` 里抽出符号 `hw_flash`, 再去源码里找 —— 而源码文件名
    本来就不出现在文件内容里 ⇒ 100% 报假 DRIFT。抽取器把自己当成了证据,
    这是「自证工具与被测物共享盲点」的又一个小变体。

    🕳️ 另一个教训(判别实验逼出来的): 符号常写成 `g_vpp_mv[HW_PIN_VPP_MAX]`
    这种**含下标/参数的复合形式**, 所以反引号组必须匹配**跨度**再切根标识符,
    不能要求整串都是标识符字符 —— 否则一个 `[` 就让整条静默退化成
    「只验行号范围」= 我们已经抓出来过的假通过。"""
    masked = text[:anchor_start] + ' ' * (anchor_end - anchor_start) + text[anchor_end:]
    out = []

    def harvest(chunk):
        for m in IDENT.finditer(chunk):
            raw = m.group(1) or m.group(2)
            if not raw:
                continue
            # 跨度过长多半是整句中文或代码块, 抽出来的「符号」纯属噪声
            if m.group(1) and ('\n' in raw or len(raw) > 40):
                continue
            root = re.search(r'[A-Za-z_][A-Za-z0-9_]{3,}', raw)
            sym = root.group(0) if root else None
            if sym and sym not in STOP:
                out.append(sym)

    # 优先看锚点**之前** 60 字符(文档习惯把符号写在锚点前), 不够再扫整行
    harvest(masked[max(0, anchor_start - 60):anchor_start])
    if not out:
        # 兜底扫**整行**: 表格行里符号常在别的格子, 而长句子里符号也常在
        # 60 字符窗口之外(实测 pin.md:147 的 `HW_PIN_FLASH_JEDEC` 距锚点 90+ 字符)。
        # v3 忘了把起点放宽回行首, 于是这个「兜底」实际仍只扫了 60 字符 —— 又一次
        # 静默退化成 RANGE-ONLY。宁可吵也不要静默放过(锚点F 的教训)。
        end = masked.find('\n', anchor_start)
        harvest(masked[:(len(masked) if end == -1 else end)])
    seen, uniq = set(), []
    for s in out:
        if s not in seen:
            seen.add(s)
            uniq.append(s)
    return uniq


def resolve(path):
    """把文档里的路径解析成真实路径(可跨仓库)。
    返回 (绝对路径, 相对显示名, 状态); 状态 ∈ {'ok','ambiguous','notfound'}。
    🕳️ 歧义必须显式报出 —— 随便挑一个会让「校验通过」变成假通过。"""
    # 1) 就在本仓库里
    p = os.path.join(REPO, path)
    if os.path.isfile(p):
        return p, path, 'ok'
    # 2) 裸文件名 → 在本仓库里按 basename 找
    if '/' not in path:
        hits = []
        for root, dirs, files in os.walk(REPO):
            dirs[:] = [d for d in dirs if d not in ('.git', 'node_modules', 'build')]
            for fn in files:
                if fn == path and fn.endswith(SRCS):
                    hits.append(os.path.join(root, fn))
        if len(hits) == 1:
            return hits[0], os.path.relpath(hits[0], REPO), 'ok'
        if len(hits) > 1:
            return path, path, 'ambiguous:' + '|'.join(
                sorted(os.path.relpath(h, REPO) for h in hits)[:3])
    # 3) 🆕 兄弟仓库 —— 文档会引用 xiaomo-flasher / xiaomo-flash-box,
    #    它们在 ~/cow 下且**不是本 git 仓库**(实测 xiaomo-flasher 无 .git),
    #    所以既不在 REPO 里、也不能用 git show, 必须单独搜文件系统。
    for ext in EXTRA_ROOTS:
        cand = os.path.join(ext, path)
        if os.path.isfile(cand):
            return cand, os.path.relpath(cand, os.path.dirname(ext)), 'ok'
    return path, path, 'notfound'


def git_show(ref, path):
    """取某 ref 下的文件内容; 不存在返回 None。
    🆕 兄弟仓库(不在本 git 索引里)直接读磁盘 —— 它们没有 .git,
       对它们跑 `git show` 只会静默返回空, 那正是锚点F 说的那种假阴性。"""
    if not os.path.abspath(path).startswith(REPO):
        try:
            with open(path, encoding='utf-8', errors='replace') as f:
                return f.read().splitlines()
        except OSError:
            return None
    p = subprocess.run(['git', 'show', f'{ref}:{os.path.relpath(path, REPO)}'],
                       cwd=REPO, capture_output=True, text=True)
    if p.returncode != 0:
        return None
    return p.stdout.splitlines()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ref', default='HEAD')
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    docs = [f for f in sorted(os.listdir(DOCS_DIR)) if f.endswith('.md')]
    cache = {}
    total = ok = bad = unresolved = weak = 0
    problems = []

    for doc in docs:
        path = os.path.join(DOCS_DIR, doc)
        with open(path, encoding='utf-8') as f:
            lines = f.readlines()
        for lineno, line in enumerate(lines, 1):
            for m in ANCHOR.finditer(line):
                fpath, l1, l2 = m.group(1), int(m.group(2)), m.group(3)
                if fpath not in cache:
                    rpath, disp, status = resolve(fpath)
                    cache[fpath] = (disp, status,
                                    None if status != 'ok' else git_show(args.ref, rpath))
                disp, status, content = cache[fpath]
                if status.startswith('ambiguous'):
                    # 🕳️ 歧义不是「有效」也不是「失效」, 是「查不了」——第三种状态必须单列
                    unresolved += 1
                    problems.append((doc, lineno, f'{fpath}:{l1}', f'AMBIGUOUS({status[10:]})'))
                    continue
                if content is None:
                    # 🕳️ 锚点F: 解析不了必须显式说, 不能当成「有效」也不能当成「失效」混在一起
                    unresolved += 1
                    problems.append((doc, lineno, f'{fpath}:{l1}', 'FILE-NOT-FOUND'))
                    continue
                for ln in filter(None, [l1, l2 and int(l2)]):
                    total += 1
                    if 1 <= ln <= len(content):
                        # 🕳️🕳️ 只查「行号没越界」是**假通过**: 行还在, 内容早移位了。
                        #   实测 hw_pin.c:737 —— 越界检查说 [ok], 实际那行是
                        #   `b = g_isp_out[0];`, 文档要的 g_vpp_mv 早已被 vex 修复顶下去。
                        #   所以必须**再查符号是否真在这一行附近**, 漂移才抓得到。
                        syms = nearby_symbols(line, m.start(), m.end())
                        hit = any(sym in '\n'.join(content[max(0, ln - 1 - WINDOW):
                                                      min(len(content), ln + WINDOW)])
                                  for sym in syms)
                        if syms and hit:
                            ok += 1
                            if args.verbose:
                                print(f'  [ok]   {doc}:{lineno}  {fpath}:{ln}  ({",".join(syms[:2])})')
                        elif syms:
                            # 弱信号: 行号在范围内, 但附近找不到文档提到的符号 ⇒ 高度疑似漂移
                            weak += 1
                            problems.append((doc, lineno, f'{fpath}:{ln}',
                                             f'DRIFT? 附近无 {",".join(syms[:2])} (行号在范围内但内容已移位)'))
                        else:
                            # 抽不出符号 = 只能验范围, 结论天然弱, 必须单列而不是混进「有效」
                            weak += 1
                            problems.append((doc, lineno, f'{fpath}:{ln}',
                                             'RANGE-ONLY 抽不出符号, 只验了行号范围'))
                    else:
                        bad += 1
                        problems.append((doc, lineno, f'{fpath}:{ln}',
                                         f'OUT-OF-RANGE(文件 {len(content)} 行)'))

    if problems:
        print(f'\n=== 失效锚点 ({args.ref}) ===')
        for doc, lineno, anchor, why in problems:
            print(f'  [BAD]  {doc}:{lineno}  ->  {anchor}   {why}')
    print(f'\n锚点 {total} 处: 有效 {ok} / 越界 {bad} / 查不了 {unresolved}')
    if bad or unresolved or weak:
        print(f'⇒ {len(problems)} 处需修。行号是「相对某个 commit」才有意义的, '
              f'文档应标注所依据的 commit; 或用 --ref 指定该 commit 复核。')
        return 1
    print('⇒ 全部有效 ✔')
    return 0


if __name__ == '__main__':
    sys.exit(main())
