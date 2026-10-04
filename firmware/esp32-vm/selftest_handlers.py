#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
selftest_handlers.py — esp32-vm 信封层 / 指令分发层 / web Handler 的端到端自检

为什么需要它(2026-10-04):
  esp32_bridge.selftest() 只覆盖最底层的 aead_encrypt/aead_decrypt
  (RFC 8439 官方测试向量)。而真正跑生产流量的是它上面三层:
    open_envelope    重放窗口 / 跨接口 path 绑定 / AAD 绑定 dev|ts|path
    handle_vm        5 个 action 的指令分发
    esp32_handlers   web.py 薄封装(POST / GET / OPTIONS)
  这三层此前【零验证】, 且 esp32_handlers.py 无人挂载、连导入都不成立
  (from channel.web import esp32_bridge —— channel 包在本工程不存在)。

设计纪律(本仓库既有, 见 knowledge/concepts/silent-verification-failure.md):
  1) 每个负向用例【只有一条拒绝路径】。例如"过期信封"必须用 AAD 自洽的
     信封(按旧 ts 正确封出)。若直接把 ts 改掉, 拒绝会来自 tag 校验而不是
     重放窗口 —— 测试照样绿, 却【活着但没区分力】。
  2) 判据是【真的抛异常】, 不是"返回了错误字符串"。
  3) 正向期望值来自 RFC 官方向量(独立事实), 不用本测试自己生成的密文
     当期望 —— 那等于自己判自己(锚点 K/L)。

配套变异验证见 tests/esp32vm_variant.py: 拿掉任一检查必须让本文件变红。
"""
import binascii
import json
import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import esp32_bridge
import esp32_handlers

_bridge = esp32_bridge
_h = esp32_handlers

FAILS = []
CHECKS = [0]


def check(name, fn):
    """跑一条断言型用例, 失败记账而不是直接崩 —— 要看到全部结果。"""
    CHECKS[0] += 1
    try:
        fn()
    except Exception as e:
        FAILS.append((name, repr(e)))


def must_raise(name, fn, want_sub=None):
    """负向判据: 必须抛异常(可选校验异常文本片段)。"""
    CHECKS[0] += 1
    try:
        got = fn()
    except ValueError as e:
        if want_sub and want_sub not in str(e):
            FAILS.append((name, 'raised ValueError but text %r lacks %r' % (str(e), want_sub)))
        return
    except Exception as e:
        FAILS.append((name, 'raised %r, want ValueError' % (e,)))
        return
    FAILS.append((name, 'NO RAISE, returned %r' % (got,)))


# ---------------------------------------------------------------- 独立工具
def _b(x):
    """本测试自带的 base64 编码 —— 刻意【不】复用 esp32_bridge._b,
    避免判据与被测物共用同一实现(锚点 K/L: 桩与被测物同型 = 对拍是复印机)。

    用 binascii 而非 base64: binascii 是独立的 C 实现路径,
    两条代码路互相独立, 写错任一边不会同步地"对得上"。

    ⚠️ 踩坑记录(2026-10-04, 自写 bug #1): 初版这里写的是 x.hex() —— 我按
    hex 去编解码, 而信封里的 ct/tag 其实是 base64。首跑 2/18 失败,
    报的是 binascii.Error/ValueError。
    教训同锚点 F 的另一面: 失败先怀疑【用例】, 尤其是"我自己新写的、
    还没跑过第二遍"的代码 —— 报错文本指向解码器, 根因却在我对格式的理解。
    """
    return binascii.b2a_base64(x).rstrip(b'\n').decode('ascii')


def _u(s):
    """base64 解码(独立于 esp32_bridge._u), 供篡改类用例把密文取回字节。"""
    return binascii.a2b_base64(s.encode('ascii'))


def seal_at(plain_obj, path, ts, dev="esp32s3-01"):
    """按【显式】ts/dev 构造 AAD 自洽的信封。

    自洽是全部负向用例的承重点: ts/dev/path 写进 AAD, 所以信封一旦造出来,
    tag 一定通过。此后若 open_envelope 仍拒绝, 唯一可能的来源就是
    我们想测的那条检查(重放窗口 / path 绑定)。
    """
    nonce = os.urandom(12)
    aad = ('%s|%d|%s' % (dev, ts, path)).encode()
    pt = json.dumps(plain_obj, separators=(',', ':')).encode()
    ct, tag = _bridge.aead_encrypt(_bridge._KEY, nonce, pt, aad)
    return {'v': 1, 'dev': dev, 'ts': ts, 'path': path,
            'nonce': _b(nonce), 'ct': _b(ct), 'tag': _b(tag)}


# ------------------------------------------------------- 1. 加密内核(正向)
def t_rfc_vector():
    """RFC 8439 §2.8.2 官方向量 —— 独立于本工程任何代码的期望值。"""
    assert _bridge.selftest() is True


# --------------------------------------------------- 2. 信封层正向往返
def t_roundtrip():
    now = _bridge._now_epoch()
    env = _bridge.seal({'hello': 'esp32', 'n': 42}, _h.VM_PATH)
    plain, meta = _bridge.open_envelope(env, expected_path=_h.VM_PATH)
    assert plain == {'hello': 'esp32', 'n': 42}, plain
    assert meta['path'] == _h.VM_PATH, meta
    assert meta['dev'] == 'esp32s3-01', meta
    assert abs(int(env['ts']) - now) <= 5, (env['ts'], now)


def t_roundtrip_unicode():
    """中文/emoji 走一遍 —— JSON+AEAD 边界。"""
    obj = {'msg': '温度 26.5°C 已上报 ✅', 'dev': 'esp32s3-01'}
    env = _bridge.seal(obj, _h.VM_PATH)
    plain, _ = _bridge.open_envelope(env, expected_path=_h.VM_PATH)
    assert plain == obj, plain


def t_envelope_survives_delay_within_window():
    """【存活变异逼出来的用例】封入后隔一段时间(仍在重放窗口内)必须仍能解。

    变异 'ts rewritten before aad' (open_envelope 里 ts = now) 之所以能活,
    是因为其它正向用例都是"封完立刻开", 同一秒内 ts 被改写成 now 后 AAD
    几乎不变 ⇒ 恒等通过。真实语义却要求: AAD 里的 ts 必须是【信封原始 ts】,
    否则一条合法信封只要在封入后的下一秒才被打开, 就会因 tag 失配被误拒
    —— 而"重放窗口内必可解密"恰恰是重放窗口的契约。

    做法: 打桩 _now_epoch 固定时钟推进(确定性, 不靠 sleep 等真的秒跳),
    再用信封自带的原始 ts 打开。改动后 AAD 必然不同 ⇒ 必须变红。
    """
    env = _bridge.seal({'delayed': True}, _h.VM_PATH)
    sealed_ts = int(env['ts'])
    # 固定假时钟: 比封入时刻晚 7 秒, 远在 REPLAY_WINDOW_S(300) 之内
    real_now = _bridge._now_epoch
    try:
        _bridge._now_epoch = lambda: sealed_ts + 7
        plain, meta = _bridge.open_envelope(env, expected_path=_h.VM_PATH)
    finally:
        _bridge._now_epoch = real_now     # 全局态必须还原(家族第三次同形教训)
    assert plain == {'delayed': True}, plain
    assert meta['ts'] == sealed_ts, ('AAD/元信息应回信封原始 ts', meta, sealed_ts)


# ------------------------------------- 3. 负向: 跨接口重放(只有 path 能挡)
def t_cross_interface_replay():
    """用 /bridge 的信封按 /vm 打开。
    关键: 信封本身 AAD 自洽(用 BRIDGE_PATH 正确封出), 所以解密本来能过,
    唯一拒绝来源就是 expected_path 比对。"""
    now = _bridge._now_epoch()
    env = seal_at({'x': 1}, _h.BRIDGE_PATH, now)
    must_raise('cross-interface replay', lambda: _bridge.open_envelope(env, expected_path=_h.VM_PATH),
               'path mismatch')


# ------------------------------------------- 4. 负向: 重放窗口(只有 ts 能挡)
def t_expired_envelope():
    """超出 REPLAY_WINDOW_S 的旧信封。
    关键: 用旧 ts 正确封出, AAD 自洽 -> 解密本来能过,
    唯一拒绝来源是重放窗口比对。"""
    old = _bridge._now_epoch() - (_bridge.REPLAY_WINDOW_S + 60)
    env = seal_at({'x': 1}, _h.VM_PATH, old)
    must_raise('expired envelope', lambda: _bridge.open_envelope(env, expected_path=_h.VM_PATH),
               'expired')


# --------------------------------------------- 5. 负向: AEAD tag 校验
def t_tampered_ciphertext():
    env = _bridge.seal({'secret': 1}, _h.VM_PATH)
    raw = bytearray(_u(env['ct']))       # base64 解码回字节(见 _u)
    raw[0] ^= 0x01
    env['ct'] = _b(bytes(raw))
    must_raise('tampered ct', lambda: _bridge.open_envelope(env, expected_path=_h.VM_PATH))


def t_tampered_dev_binds_aad():
    """改 dev -> AAD 变了 -> tag 失配。证明 AAD 真的绑定了 dev。"""
    env = _bridge.seal({'x': 1}, _h.VM_PATH)
    env['dev'] = 'attacker-01'
    must_raise('tampered dev', lambda: _bridge.open_envelope(env, expected_path=_h.VM_PATH))


def t_tampered_tag():
    env = _bridge.seal({'x': 1}, _h.VM_PATH)
    raw = bytearray(_u(env['tag']))       # base64 解码回字节(见 _u)
    raw[0] ^= 0x01
    env['tag'] = _b(bytes(raw))
    must_raise('tampered tag', lambda: _bridge.open_envelope(env, expected_path=_h.VM_PATH))


# ------------------------------------------------ 6. 指令分发层 handle_vm
def t_handle_vm_actions():
    r = _bridge.handle_vm({'action': 'list', 'payload': {}})
    assert r['status'] == 'success', r
    assert 'data' in r, r
    # 本机没有 /app/_vm_demo 时应诚实降级, 而不是抛异常
    assert 'bc_root' in r['data'] or 'available' in r['data'], r

    r = _bridge.handle_vm({'action': 'reports', 'payload': {}})
    assert r['status'] == 'success', r

    r = _bridge.handle_vm({'action': 'invoke', 'payload': {'bc': 'nope.bc'}})
    assert r['status'] == 'success', r
    assert 'error' in r['data'], r          # 缺 bc 应诚实失败

    r = _bridge.handle_vm({'action': 'no_such_action'})
    assert r['status'] == 'error', r
    assert 'unknown action' in r['message'], r


def t_handle_vm_default_is_list():
    """不写 action 应回落到 list(与 openclaw/设备端约定一致)。"""
    r = _bridge.handle_vm({})
    assert r['status'] == 'success', r


# --------------------------------------------- 7. web Handler 端到端(假 web)
def _install_fake_web():
    """handlers 内部按需 import web; 注入一个只实现用到接口的替身。"""
    mod = types.ModuleType('web')
    state = {'headers': {}, 'body': b''}

    def header(k, v):
        state['headers'][k] = v
    mod.header = header
    mod.data = lambda: state['body']
    mod._state = state
    sys.modules['web'] = mod
    return mod


def t_handler_post_vm():
    web = _install_fake_web()
    env_in = _bridge.seal({'type': 'vm', 'action': 'list', 'payload': {}}, _h.VM_PATH)
    web._state['body'] = json.dumps(env_in).encode()
    out = json.loads(_h.Esp32VmHandler().POST())
    assert out['status'] == 'success', out
    # 响应必须是【加密信封】而不是明文
    assert 'data' in out and 'ct' in out['data'], out
    plain, meta = _bridge.open_envelope(out['data'], expected_path=_h.VM_PATH)
    assert meta['path'] == _h.VM_PATH
    assert plain['status'] == 'success', plain
    assert web._state['headers'].get('Access-Control-Allow-Origin') == '*', web._state['headers']


def t_handler_get_probe():
    web = _install_fake_web()
    out = json.loads(_h.Esp32VmHandler().GET())
    assert out == {'status': 'ok', 'endpoint': _h.VM_PATH}, out


def t_handler_rejects_bad_envelope():
    """Handler 必须把非法信封变成 error JSON, 而不是 500/未捕获异常。"""
    web = _install_fake_web()
    env = _bridge.seal({'x': 1}, _h.VM_PATH)
    env['ct'] = '00' * 4          # 坏密文
    web._state['body'] = json.dumps(env).encode()
    out = json.loads(_h.Esp32VmHandler().POST())
    assert out['status'] == 'error', out
    assert 'message' in out, out


def t_handler_bridge_endpoint():
    web = _install_fake_web()
    env_in = _bridge.seal({'type': 'ping', 'payload': {'hi': 1}}, _h.BRIDGE_PATH)
    web._state['body'] = json.dumps(env_in).encode()
    out = json.loads(_h.Esp32BridgeHandler().POST())
    assert out['status'] == 'success', out
    plain, meta = _bridge.open_envelope(out['data'], expected_path=_h.BRIDGE_PATH)
    assert meta['path'] == _h.BRIDGE_PATH
    assert plain['echo'] is True and plain['type'] == 'ping', plain


def t_handler_cross_endpoint_blocked():
    """把 /vm 的信封打到 /bridge 端点上, 必须被 path 检查挡下。
    这是端点级隔离的端到端证明(不是只测 open_envelope)。"""
    web = _install_fake_web()
    env_in = _bridge.seal({'type': 'vm', 'action': 'list'}, _h.VM_PATH)
    web._state['body'] = json.dumps(env_in).encode()
    out = json.loads(_h.Esp32BridgeHandler().POST())
    assert out['status'] == 'error', out
    assert 'path mismatch' in out['message'], out


# ------------------------------------------------------------------ 主流程
CASES = [
    ('rfc vector', t_rfc_vector),
    ('envelope roundtrip', t_roundtrip),
    ('roundtrip unicode', t_roundtrip_unicode),
    ('envelope survives delay within window', t_envelope_survives_delay_within_window),
    ('cross-interface replay', t_cross_interface_replay),
    ('expired envelope', t_expired_envelope),
    ('tampered ciphertext', t_tampered_ciphertext),
    ('tampered dev binds aad', t_tampered_dev_binds_aad),
    ('tampered tag', t_tampered_tag),
    ('handle_vm actions', t_handle_vm_actions),
    ('handle_vm default list', t_handle_vm_default_is_list),
    ('handler POST /vm', t_handler_post_vm),
    ('handler GET probe', t_handler_get_probe),
    ('handler bad envelope', t_handler_rejects_bad_envelope),
    ('handler POST /bridge', t_handler_bridge_endpoint),
    ('handler cross-endpoint blocked', t_handler_cross_endpoint_blocked),
]


def main():
    for name, fn in CASES:
        check(name, fn)
    if FAILS:
        print('esp32vm handlers selftest: FAIL (%d/%d)' % (len(FAILS), CHECKS[0]))
        for n, e in FAILS:
            print('  [FAIL] %s: %s' % (n, e))
        return 1
    print('esp32vm handlers selftest: ALL PASS (%d checks, %d cases)'
          % (CHECKS[0], len(CASES)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
