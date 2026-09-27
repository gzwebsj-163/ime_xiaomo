#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""kbc 意图推理 e2e 对拍: 98 条样本逐一 ./xiaomo infer, 对比 INTENT= 与 label"""
import json, subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))  # ~/Desktop/xiaomo
ts = json.load(open(os.path.join(HERE, "intent_testset.json")))["samples"]

ok = bad = 0
fails = []
for s in ts:
    r = subprocess.run([os.path.join(ROOT, "xiaomo"), "infer",
                        os.path.join(HERE, "intent.kbc"), "text", s["text"]],
                       capture_output=True, text=True, timeout=30, cwd=ROOT)
    got = None
    for line in r.stdout.splitlines():
        if line.startswith("INTENT="):
            got = int(line.split("=")[1])
    if got == s["label"]:
        ok += 1
    else:
        bad += 1
        fails.append((s["text"], s["label"], got, r.stdout.strip(), r.stderr.strip()[:120]))

print(f"[e2e] kbc 推理 vs Python 标签: {ok}/{len(ts)} 通过, {bad} 失败")
for t, l, g, out, err in fails:
    print(f"  FAIL [{t}] 期望{l} 得到{g}\n    out: {out}\n    err: {err}")
sys.exit(1 if bad else 0)
