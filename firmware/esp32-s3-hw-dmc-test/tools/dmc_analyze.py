#!/usr/bin/env python3
"""剥 ANSI 后按结构统计 DMC 日志。
🕳️ 纪律: 字节级/去转义一律走 .py, 不走 bash heredoc（shell 吞 \\x 转义）。
用法: dmc_analyze.py <log> [更多log...]
"""
import re, sys

ANSI = re.compile(rb'\x1b\[[0-9;]*[A-Za-z]')

for path in sys.argv[1:]:
    raw = open(path, 'rb').read()
    clean = ANSI.sub(b'', raw).decode('utf-8', errors='replace')
    lines = clean.splitlines()
    print(f"===== {path} =====")
    print(f"raw={len(raw)}B  clean={len(clean)}B  lines={len(lines)}")

    # 复位次数（避免 tail/tail 二次伤害：崩溃循环伪装成一次干净启动）
    rst = [l for l in lines if 'rst:0x' in l]
    print(f"rst 次数 = {len(rst)}  -> {rst}")

    for kw in ['rst:0x', 'Guru', 'panic', 'abort', 'Backtrace', 'Watchdog',
               'Master', 'Slave', 'slave', 'master', 'link', 'LINK',
               'loopback', 'backoff', 'PH_UP', 'ok=', 'crc', 'UART']:
        hits = [l for l in lines if kw in l]
        if not hits:
            continue
        # 去重（同文折叠），保留首次出现顺序
        seen, uniq = set(), []
        for h in hits:
            k = re.sub(r'\d+', '#', h)
            if k not in seen:
                seen.add(k); uniq.append(h)
        print(f"\n-- [{kw}] {len(hits)} 行 / {len(uniq)} 种 --")
        for u in uniq[:12]:
            print("   ", u.strip()[:150])
    print()
