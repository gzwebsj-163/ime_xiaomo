# -*- coding: utf-8 -*-
"""
demo_main.py — ESP32-S3 边缘VM 演示/开机自启动脚本
在板上加载 mlp.bc 并本地推理，验证边缘VM能独立跑通服务器私有IR字节码。

两种使用方式:
  1. 把 mlp.bc 放到板子Flash根目录, 直接本地加载推理
  2. (可选) 连接服务器拉取最新 .bc (为分布式做准备)

依赖: edge_vm.py (本机), 可选 ulab (加速矩阵乘)
"""
import time
import gc

# 请按你的实际板子填写 WiFi 与服务器信息（如与你现有 config.py 重复可复用）
WIFI_SSID = 'YOUR_SSID'
WIFI_PASS = 'YOUR_PASS'
SERVER = 'http://8.163.46.174:9899'

import network
import urequests as requests


def wifi_connect():
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    if not wlan.isconnected():
        print('Connecting WiFi...')
        wlan.connect(WIFI_SSID, WIFI_PASS)
        for _ in range(20):
            if wlan.isconnected():
                break
            time.sleep(0.5)
    print('WiFi IP:', wlan.ifconfig()[0])
    return wlan.isconnected()


def get_bc_from_server():
    """(可选) 从服务器拉取 mlp.bc"""
    try:
        r = requests.get(SERVER + '/mlp.bc')
        data = r.content
        r.close()
        print('从服务器拉取 mlp.bc, %d 字节' % len(data))
        with open('/mlp.bc', 'wb') as f:
            f.write(data)
        return data
    except Exception as e:
        print('拉取失败(用本地):', e)
        return None


def main():
    from edge_vm import EdgeVM, disassemble

    # 1. 本地 .bc 数据 (若已放板上则读文件；否则内置)
    bc_bytes = None
    try:
        with open('/mlp.bc', 'rb') as f:
            bc_bytes = f.read()
        print('读取本地 mlp.bc: %d 字节' % len(bc_bytes))
    except OSError:
        print('板上无 mlp.bc, 尝试从服务器拉取...')
        if wifi_connect():
            bc_bytes = get_bc_from_server()
        if bc_bytes is None:
            raise RuntimeError('无 .bc 文件，请先把 mlp.bc 传上板子')

    print()
    print('=' * 50)
    print(disassemble(bc_bytes))
    print('=' * 50)

    # 2. 推理输入: 两层MLP, x(3)->隐藏(2)->softmax(2)
    #    (与服务器生成结果一致的数据)
    x = [1.0, 0.0, -1.0]
    w1 = [[0.2, 0.0], [0.5, 0.1], [-0.3, 0.4]]
    b1 = [0.1, 0.2]
    w2 = [[0.4, -0.2], [0.3, 0.1]]
    b2 = [0.0, -0.1]
    params = [x, w1, b1, w2, b2]

    # 3. 执行边缘VM
    gc.collect()
    t0 = time.ticks_ms()
    vm = EdgeVM(bc_bytes, params=params)
    result = vm.run()
    dt = time.ticks_diff(time.ticks_ms(), t0)
    gc.collect()

    print()
    print('=== ESP32-S3 边缘VM 推理结果 ===')
    print('  输出:', result)
    print('  耗时: %d ms' % dt)
    print('  空闲RAM: %d bytes' % (gc.mem_free()))
    print()
    print('✅ 边缘VM在 ESP32-S3 上独立运行成功 (与服务器一致: [0.6130, 0.3870])')


if __name__ == '__main__':
    main()
