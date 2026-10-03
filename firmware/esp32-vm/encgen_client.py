# -*- coding: utf-8 -*-
"""encgen_client.py — 通过加密信封 HTTP 调用 /api/esp32/vm 的 generate action。"""
import json, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
# 复用本地 esp32_bridge 的信封 seal(PSK 全零, 与服务器一致)
import importlib.util
spec = importlib.util.spec_from_file_location(
    "eb", os.path.join(os.path.dirname(os.path.abspath(__file__)), "esp32_bridge.py"))
eb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(eb)
import urllib.request

VM_PATH = "/api/esp32/vm"
BASE = "http://8.163.46.174:9899"

def call(action, payload, dev="esp32s3-01"):
    plain = {"type": "vm", "action": action, "payload": payload}
    env = eb.seal(plain, VM_PATH, dev=dev)
    req = urllib.request.Request(
        BASE + VM_PATH,
        data=json.dumps(env).encode(),
        headers={"Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read().decode())

def call_dec(action, payload, dev="esp32s3-01"):
    """调用并用 open_envelope 解密内层响应信封。"""
    plain = {"type": "vm", "action": action, "payload": payload}
    env = eb.seal(plain, VM_PATH, dev=dev)
    req = urllib.request.Request(
        BASE + VM_PATH,
        data=json.dumps(env).encode(),
        headers={"Content-Type": "application/json"},
        method="POST")
    with urllib.request.urlopen(req, timeout=60) as r:
        outer = json.loads(r.read().decode())
    if outer.get("status") != "success":
        return outer
    inner_env = outer["data"]
    inner_plain, meta = eb.open_envelope(inner_env, expected_path=VM_PATH)
    return {"status": "success", "meta": meta, "data": inner_plain}

if __name__ == "__main__":
    use_dec = "--dec" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--dec"]
    action = args[0] if len(args) > 0 else "generate"
    payload = json.loads(args[1]) if len(args) > 1 else {"input": "我", "max_len": 40, "mode": "greedy"}
    try:
        if use_dec:
            resp = call_dec(action, payload)
        else:
            resp = call(action, payload)
        print(json.dumps(resp, ensure_ascii=False, indent=2)[:5000])
    except Exception as e:
        print("ERROR:", e)

