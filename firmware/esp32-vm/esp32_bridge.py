# -*- coding: utf-8 -*-
"""
esp32_bridge.py — ESP32-S3 <-> 服务器 二次加密信封桥
与设备端 secure_channel.py 完全兼容 (ChaCha20-Poly1305 AEAD, RFC 8439)。

端点(由 web_channel.py 挂载):
  POST /api/esp32/bridge  -> OpenAI/Mocode-Lab 对话桥 (占位)
  POST /api/esp32/vm      -> VM 指令通道 (list / invoke)

信封协议:
  请求: {v, dev, ts, path, nonce, ct, tag}
  校验: AAD = "{dev}|{ts}|{path}"
  响应: {"status":"success","data":<同格式信封>}  或 {"status":"error","message":...}
"""
import json
import time
import base64
import struct
import os
import glob
import sys
# 确保 /app 在 sys.path, 以便 import crypto.obf_engine.*
for _p in ("/app", "/app/app/channel/web"):
    if _p not in sys.path:
        sys.path.insert(0, _p)

# ============ ChaCha20-Poly1305 (RFC 8439) ============
_MASK = 0xFFFFFFFF
def _rotl(v, n):
    return ((v << n) | (v >> (32 - n))) & _MASK
def _quarter(s, a, b, c, d):
    s[a] = (s[a] + s[b]) & _MASK; s[d] ^= s[a]; s[d] = _rotl(s[d], 16)
    s[c] = (s[c] + s[d]) & _MASK; s[b] ^= s[c]; s[b] = _rotl(s[b], 12)
    s[a] = (s[a] + s[b]) & _MASK; s[d] ^= s[a]; s[d] = _rotl(s[d], 8)
    s[c] = (s[c] + s[d]) & _MASK; s[b] ^= s[c]; s[b] = _rotl(s[b], 7)
def _chacha_block(key, counter, nonce):
    st = [0x61707865, 0x3320646e, 0x79622d32, 0x6b206574]
    st += list(struct.unpack('<8I', key))
    st.append(counter & _MASK)
    st += list(struct.unpack('<3I', nonce))
    w = st[:]
    for _ in range(10):
        _quarter(w, 0, 4, 8, 12); _quarter(w, 1, 5, 9, 13)
        _quarter(w, 2, 6, 10, 14); _quarter(w, 3, 7, 11, 15)
        _quarter(w, 0, 5, 10, 15); _quarter(w, 1, 6, 11, 12)
        _quarter(w, 2, 7, 8, 13); _quarter(w, 3, 4, 9, 14)
    return b''.join(struct.pack('<I', (w[i] + st[i]) & _MASK) for i in range(16))
def chacha20_xor(key, nonce, counter, data):
    out = bytearray()
    for i in range(0, len(data), 64):
        ks = _chacha_block(key, counter + i // 64, nonce)
        chunk = data[i:i + 64]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
    return bytes(out)
def _clamp(r):
    return r & 0x0ffffffc0ffffffc0ffffffc0fffffff
def _poly1305_mac(key32, msg):
    r = _clamp(int.from_bytes(key32[:16], 'little'))
    s = int.from_bytes(key32[16:], 'little')
    p = (1 << 130) - 5
    acc = 0
    for i in range(0, len(msg), 16):
        blk = msg[i:i + 16]
        n = int.from_bytes(blk, 'little') + (1 << (8 * len(blk)))
        acc = ((acc + n) * r) % p
    return ((acc + s) & ((1 << 128) - 1)).to_bytes(16, 'little')
def _pad16(n):
    return b'\x00' * ((16 - n % 16) % 16)
def aead_encrypt(key, nonce, plaintext, aad=b''):
    otk = _chacha_block(key, 0, nonce)[:32]
    ct = chacha20_xor(key, nonce, 1, plaintext)
    mac_data = (aad + _pad16(len(aad)) + ct + _pad16(len(ct))
                + struct.pack('<Q', len(aad)) + struct.pack('<Q', len(ct)))
    return ct, _poly1305_mac(otk, mac_data)
def aead_decrypt(key, nonce, ct, tag, aad=b''):
    otk = _chacha_block(key, 0, nonce)[:32]
    mac_data = (aad + _pad16(len(aad)) + ct + _pad16(len(ct))
                + struct.pack('<Q', len(aad)) + struct.pack('<Q', len(ct)))
    expect = _poly1305_mac(otk, mac_data)
    if len(expect) != len(tag) or any(a ^ b for a, b in zip(expect, tag)):
        raise ValueError('AEAD tag mismatch')
    return chacha20_xor(key, nonce, 1, ct)

# ============ 信封层 ============
PSK_HEX = '0000000000000000000000000000000000000000000000000000000000000000'
_KEY = bytes.fromhex(PSK_HEX)
if len(_KEY) != 32:
    raise ValueError('PSK must be 32 bytes')
REPLAY_WINDOW_S = 300

def _b(x):
    return base64.b64encode(x).decode()
def _u(s):
    return base64.b64decode(s)

def seal(plain_obj, path, dev="esp32s3-01"):
    nonce = os.urandom(12)
    # 用 2000 基准时间戳, 与 ESP32(MicroPython) time.time() 对齐
    ts = _now_epoch()
    aad = ('%s|%d|%s' % (dev, ts, path)).encode()
    pt = json.dumps(plain_obj, separators=(',', ':')).encode()
    ct, tag = aead_encrypt(_KEY, nonce, pt, aad)
    return {
        'v': 1, 'dev': dev, 'ts': ts, 'path': path,
        'nonce': _b(nonce), 'ct': _b(ct), 'tag': _b(tag),
    }

_EPOCH_DIFF_2000_1970 = 946684800  # 2000-01-01 与 1970-01-01 的秒差

def _now_epoch():
    """返回当前时间(统一到 2000 基准), 用于与设备时间戳比较。"""
    return int(time.time()) - _EPOCH_DIFF_2000_1970

def open_envelope(env, expected_path=None):
    """解密并校验设备信封, 返回 (明文obj, 校验元信息)。

    时间基准约定: 信封 ts 一律采用「2000 基准」(与 ESP32 MicroPython
    time.time() 一致, 服务器在 seal 时用 now-30年 生成)。校验时统一到
    2000 基准比较, 重放窗口内通过。
    """
    now = _now_epoch()             # 2000 基准
    ts = int(env.get('ts', 0))     # 应为 2000 基准
    if abs(now - ts) > REPLAY_WINDOW_S:
        raise ValueError('envelope expired (replay window)')
    path = env.get('path')
    if expected_path and path != expected_path:
        raise ValueError('path mismatch (cross-interface replay blocked)')
    dev = env.get('dev')
    aad = ('%s|%d|%s' % (dev, ts, path)).encode()   # AAD 用信封原始 ts, 与设备端一致
    pt = aead_decrypt(_KEY, _u(env['nonce']), _u(env['ct']), _u(env['tag']), aad)
    return json.loads(pt), {'dev': dev, 'ts': ts, 'path': path}

# ============ VM 处理 ============
# .bc 字节码根目录 (可被主容器访问)
BC_ROOT = '/app/_vm_demo'

def vm_list():
    """列出服务器可用的 .bc 字节码文件"""
    if not os.path.isdir(BC_ROOT):
        return {'bc_root': BC_ROOT, 'available': [], 'error': 'bc_root missing'}
    files = sorted(glob.glob(os.path.join(BC_ROOT, '*.bc')))
    return {'bc_root': BC_ROOT, 'available': [os.path.basename(f) for f in files]}

def vm_invoke(payload):
    """调用服务器 VM 解释器执行 .bc。payload: {bc:<name>|<path>, args:{...}或[...]}"""
    bc_name = payload.get('bc') or (payload.get('payload') or {}).get('bc')
    args = payload.get('args', {})
    if not bc_name:
        return {'error': 'missing bc in payload'}
    if not os.path.isabs(bc_name):
        bc_name = os.path.join(BC_ROOT, os.path.basename(bc_name))
    if not os.path.exists(bc_name):
        return {'error': 'bc not found: %s' % bc_name}
    try:
        from crypto.obf_engine.layers.layer5_private_ir.bytecode import ByteCode
        from crypto.obf_engine.layers.layer5_private_ir.interpreter import VMInterpreter
    except Exception as e:
        return {'error': 'vm module unavailable: %s' % e}
    try:
        data = open(bc_name, 'rb').read()
        bc = None
        for decrypt in (True, False):
            try:
                bc = ByteCode.deserialize(data, decrypt=decrypt)
                break
            except Exception:
                continue
        if bc is None:
            return {'error': 'cannot parse bc'}
        vm = VMInterpreter(max_instructions=2_000_000)
        vm.load_bytecode(bc)
        # args 支持 dict(按 input_names) 或 list(位置参数)
        if isinstance(args, dict):
            input_names = bc.get_metadata('input_names') or []
            call_args = [args.get(n) for n in input_names] if input_names else list(args.values())
        else:
            call_args = list(args)
        result = vm.call(*call_args)
        return {'result': _jsonable(result),
                'graph_name': bc.get_metadata('graph_name'),
                'bc': os.path.basename(bc_name)}
    except Exception as e:
        return {'error': 'vm invoke failed: %s' % (e,)}

def _jsonable(obj):
    try:
        json.dumps(obj)
        return obj
    except (TypeError, ValueError):
        return str(obj)


# ---- 边缘推理结果上报/记录 ----
# 记录同时写入内存(REPORTS)与磁盘(REPORT_FILE), 重启后仍可查。
REPORTS = []
REPORT_MAX = 100
REPORT_FILE = '/app/_vm_reports.jsonl'

def _load_reports_from_disk():
    try:
        if os.path.exists(REPORT_FILE):
            with open(REPORT_FILE, 'r', encoding='utf-8') as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            REPORTS.append(json.loads(line))
                        except Exception:
                            pass
        while len(REPORTS) > REPORT_MAX:
            REPORTS.pop(0)
    except Exception:
        pass

_load_reports_from_disk()

def _vm_report(payload):
    """接收 ESP32 本地 EdgeVM 推理结果并持久化。payload: {result, graph, ...}"""
    try:
        entry = {
            'device': payload.get('device') or 'esp32s3-01',
            'graph': payload.get('graph') or payload.get('graph_name') or 'unknown',
            'result': payload.get('result'),
            'local_infer_ms': payload.get('local_infer_ms'),
            'ram_free': payload.get('ram_free'),
            'arrived': time.strftime('%Y-%m-%d %H:%M:%S', time.localtime()),
            'ts_2000': int(time.time()) - _EPOCH_DIFF_2000_1970,
        }
        REPORTS.append(entry)
        if len(REPORTS) > REPORT_MAX:
            REPORTS.pop(0)
        try:
            with open(REPORT_FILE, 'a', encoding='utf-8') as f:
                f.write(json.dumps(entry, ensure_ascii=False) + '\n')
        except Exception:
            pass
        return {'status': 'success', 'data': {'accepted': True, 'seq': len(REPORTS), 'entry': entry}}
    except Exception as e:
        return {'status': 'error', 'message': 'report failed: %s' % (e,)}

def vm_reports():
    """返回已接收的边缘上报记录(含持久化)"""
    _load_reports_from_disk()
    return {'count': len(REPORTS), 'reports': list(REPORTS)}

def handle_vm(plain):
    """plain = {type:'vm', action:..., payload:...}"""
    action = plain.get('action', 'list')
    payload = plain.get('payload') or {}
    try:
        if action == 'list':
            return {'status': 'success', 'data': vm_list()}
        elif action == 'invoke':
            return {'status': 'success', 'data': vm_invoke(payload)}
        elif action == 'report':
            return _vm_report(payload)
        elif action == 'reports':
            return {'status': 'success', 'data': vm_reports()}
        else:
            return {'status': 'error', 'message': 'unknown action: %s' % action}
    except Exception as e:
        return {'status': 'error', 'message': 'server error: %s' % (e,)}

# ============ 自检 ============
def selftest():
    key = bytes.fromhex('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f')
    nonce = bytes.fromhex('070000004041424344454647')
    aad = bytes.fromhex('50515253c0c1c2c3c4c5c6c7')
    pt = (b"Ladies and Gentlemen of the class of '99: If I could offer you "
          b"only one tip for the future, sunscreen would be it.")
    ct, tag = aead_encrypt(key, nonce, pt, aad)
    assert ct[:16] == bytes.fromhex('d31a8d34648e60db7b86afbc53ef7ec2')
    assert tag == bytes.fromhex('1ae10b594f09e26a7e902ecbd0600691')
    assert aead_decrypt(key, nonce, ct, tag, aad) == pt
    return True

if __name__ == '__main__':
    print('selftest:', selftest())
    print('vm list:', vm_list())
