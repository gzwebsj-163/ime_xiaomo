#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
esp32vm_variant.py — esp32-vm 变异验证: 拿掉任一检查, selftest_handlers.py 必须变红。

为什么必须有它(锚点 J: 全绿的测试不证明测试有效, 要把裁判承重先称裁判):
  selftest_handlers.py 是 2026-10-04 新写的, 覆盖 open_envelope 重放窗口 /
  path 绑定 / AAD 绑定 与 handle_vm 分发。新写的守卫如果从没被变异打过,
  任何一条都可能【活着但没区分力】—— 测绿了却什么也没证明。

做法: 复制被测源码到临时目录 → 施加一处最小变异 → 真跑自检 → 判红。
  全程【不碰真实工程文件】(临时目录跑), 避免变异残留污染工作树(已中过一次招:
  之前 dmc 哨兵事件, 丢的标记行让 15/0 全绿)。
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
VM = os.path.abspath(os.path.join(HERE, '..'))
SELFTEST = os.path.join(VM, 'selftest_handlers.py')

# (变异名, 文件, 旧片段, 新片段, 期望被哪个用例抓到)
# 旧片段必须唯一匹配, 否则变异没生效 = 假存活(会在下面 assert 里炸)。
MUTANTS = [
    # 1) 拿掉重放窗口检查 → t_expired_envelope 必须红
    ('drop replay window',
     'esp32_bridge.py',
     "    if abs(now - ts) > REPLAY_WINDOW_S:\n        raise ValueError('envelope expired (replay window)')",
     "    if False:\n        raise ValueError('envelope expired (replay window)')"),

    # 2) 拿掉跨接口 path 绑定 → t_cross_path / t_handler_cross_endpoint_blocked 必须红
    ('drop path binding',
     'esp32_bridge.py',
     "    if expected_path and path != expected_path:\n        raise ValueError('path mismatch (cross-interface replay blocked)')",
     "    if False:\n        raise ValueError('path mismatch (cross-interface replay blocked)')"),

    # 3) 拿掉 AEAD tag 校验 → 三个篡改用例必须红
    ('drop tag verify',
     'esp32_bridge.py',
     "        raise ValueError('AEAD tag mismatch')",
     "        return pt"),

    # 4) AAD 不再绑定 dev/ts/path(只用 path) → t_tampered_dev_binds_aad 必须红
    ('aad drops dev/ts',
     'esp32_bridge.py',
     "    aad = ('%s|%d|%s' % (dev, ts, path)).encode()   # AAD 用信封原始 ts, 与设备端一致",
     "    aad = (path).encode()"),

    # 5) ts 不取原值而取当前时间 → AAD 与 seal 时不符, 正向往返必须红
    ('ts rewritten before aad',
     'esp32_bridge.py',
     "    aad = ('%s|%d|%s' % (dev, ts, path)).encode()   # AAD 用信封原始 ts, 与设备端一致",
     "    ts = now\n    aad = ('%s|%d|%s' % (dev, ts, path)).encode()"),
]


def run_selftest(workdir):
    """真跑自检, 返回 (rc, stdout)。rc=0 且含 ALL PASS 才算绿。"""
    p = subprocess.run([sys.executable, os.path.join(workdir, 'selftest_handlers.py')],
                       cwd=workdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=120)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def main():
    if not os.path.isfile(SELFTEST):
        print('selftest_handlers.py not found: %s' % SELFTEST)
        return 2

    # 基线: 未变异必须全绿, 否则后面每个"变红"都没意义
    rc, out = run_selftest(VM)
    if rc != 0 or 'ALL PASS' not in out:
        # ⚠️ 无论如何都先打一行汇总(2026-10-04 自查): 否则上层 run_tests.sh 的
        #   sed 取不到数字, 标签会显示成 "杀掉 ?/?, 存活 ?" —— 而调用方
        #   分不清"没跑"和"跑了没抓到", 这就是 M10 说的误导性失败原因。
        # ⚠️ 措辞必须与成功路径【逐字同形】(含 ", N survived"),
        #   否则 sed 的模式匹配不上, 问号照样漏出来 —— 修了一次才发现。
        print('esp32vm variant: 0/%d killed, 0 survived, 0 invalid (基线未绿, 未施加变异)'
              % len(MUTANTS))
        print('BASELINE NOT GREEN (rc=%d) — 变异验证无意义, 先修自检本身' % rc)
        print(out[-2000:])
        return 2
    print('baseline: ALL PASS (变异前)')

    killed = 0
    survived = []
    invalid = []

    for name, rel, old, new in MUTANTS:
        tmp = tempfile.mkdtemp(prefix='esp32vm_mut_')
        try:
            # 只拷被自检需要的文件, 别把整个工程复制走
            for f in ('esp32_bridge.py', 'esp32_handlers.py', 'selftest_handlers.py'):
                shutil.copy2(os.path.join(VM, f), os.path.join(tmp, f))

            target = os.path.join(tmp, rel)
            src = open(target, encoding='utf-8').read()
            n = src.count(old)
            if n != 1:
                # 变异没生效(片段不存在或多处匹配) —— 这是变异脚本自身的错,
                # 必须报出来, 不能算"存活"(锚点 K 的镜像: 假探针)
                invalid.append((name, 'anchor matched %d times (want exactly 1)' % n))
                continue
            open(target, 'w', encoding='utf-8').write(src.replace(old, new, 1))

            mrc, mout = run_selftest(tmp)
            if mrc != 0 or 'ALL PASS' not in mout:
                killed += 1
                why = [l for l in mout.splitlines() if 'FAIL' in l or 'NO RAISE' in l]
                print('  KILLED   %-26s <- %s' % (name, (why[0][:70] if why else 'rc=%d' % mrc)))
            else:
                survived.append(name)
                print('  SURVIVED %-26s <<< 测试没咬住它' % name)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    print()
    print('esp32vm variant: %d/%d killed, %d survived, %d invalid'
          % (killed, len(MUTANTS), len(survived), len(invalid)))
    for s in survived:
        print('  survived: %s' % s)
    for n, w in invalid:
        print('  INVALID(变异未生效): %s -- %s' % (n, w))

    # 存活 或 变异未生效 都算本轮不收官 —— 不允许"看起来变异过了"
    return 0 if (not survived and not invalid) else 1


if __name__ == '__main__':
    sys.exit(main())
