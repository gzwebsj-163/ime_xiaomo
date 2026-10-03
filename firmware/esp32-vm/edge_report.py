# -*- coding: utf-8 -*-
"""
edge_report.py — ESP32-S3 边缘推理 + 结果上报服务器 一体化脚本
在板上本地运行 EdgeVM 执行私有IR字节码(.bc)，把推理结果经
ChaCha20-Poly1305 加密通道上报到服务器 /api/esp32/vm (action=report)。

依赖(板上): edge_vm.py, bridge.py, secure_channel.py, chacha20_poly1305.py, config.py
用法:
  import edge_report
  edge_report.run()          # 默认跑 mlp.bc 上报
  edge_report.report_all()   # 遍历 / 下 *.bc 逐个跑并上报
"""

import gc
import time

# ---- 与 demo_main.py 一致的 mlp 测试输入 ----
MLP_INPUTS = {
    'x':  [1.0, 0.0, -1.0],
    'w1': [[0.2, 0.0], [0.5, 0.1], [-0.3, 0.4]],
    'b1': [0.1, 0.2],
    'w2': [[0.4, -0.2], [0.3, 0.1]],
    'b2': [0.0, -0.1],
}
MLP_ORDER = ['x', 'w1', 'b1', 'w2', 'b2']   # 与 mlp.bc input_names 顺序一致


def _sync_time():
    try:
        import ntptime
        ntptime.host = 'ntp.aliyun.com'
        for _ in range(5):
            try:
                ntptime.settime()
                return True
            except Exception:
                time.sleep(1)
    except Exception:
        pass
    return False


def local_infer(bc_bytes, params):
    """板上本地运行 EdgeVM, 返回 (result, elapsed_ms, ram_free)"""
    from edge_vm import EdgeVM
    gc.collect()
    t0 = time.ticks_ms()
    vm = EdgeVM(bc_bytes, params=params)
    result = vm.run()
    dt = time.ticks_diff(time.ticks_ms(), t0)
    ram = gc.mem_free()
    return result, dt, ram


def run(bc_path='/mlp.bc', params=None, graph='two_layer_mlp', device='esp32s3-01'):
    _sync_time()
    import bridge
    try:
        with open(bc_path, 'rb') as f:
            bc = f.read()
    except OSError:
        print('[-] 找不到 %s' % bc_path)
        return None
    if params is None:
        params = [MLP_INPUTS[k] for k in MLP_ORDER]
    result, dt, ram = local_infer(bc, params)
    print('[+] 本地推理 %s: %s  (%d ms, RAM %d)' % (bc_path, result, dt, ram))
    # 加密上报
    resp = bridge.vm('report', {
        'device': device,
        'graph': graph,
        'result': result,
        'local_infer_ms': dt,
        'ram_free': ram,
    })
    print('[+] 上报结果: accepted=%s seq=%s' % (resp.get('accepted'), resp.get('seq')))
    return result


def report_all():
    """遍历板子根目录所有 *.bc, 逐个本地推理并上报"""
    import os
    _sync_time()
    import bridge
    for name in os.listdir('/'):
        if not name.endswith('.bc'):
            continue
        path = '/%s' % name
        try:
            with open(path, 'rb') as f:
                bc = f.read()
            result, dt, ram = local_infer(bc, [])
            resp = bridge.vm('report', {
                'device': 'esp32s3-01',
                'graph': name.replace('.bc', ''),
                'result': result,
                'local_infer_ms': dt,
                'ram_free': ram,
            })
            print('[+] %s -> accepted=%s' % (name, resp.get('accepted')))
        except Exception as e:
            print('[-] %s ERR: %s' % (name, e))
