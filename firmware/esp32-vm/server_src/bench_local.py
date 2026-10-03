# -*- coding: utf-8 -*-
"""在本地用服务器源码跑 mlp.bc，验证能跑通 + baseline 性能"""
import sys, time
sys.path.insert(0, '.')
from obf_engine.layers.layer5_private_ir.bytecode import ByteCode
from obf_engine.layers.layer5_private_ir.interpreter import VMInterpreter

bc = ByteCode.deserialize(open('mlp.bc','rb').read())

# 与 demo_main 一致
x = [1.0, 0.0, -1.0]
w1 = [[0.2, 0.0], [0.5, 0.1], [-0.3, 0.4]]
b1 = [0.1, 0.2]
w2 = [[0.4, -0.2], [0.3, 0.1]]
b2 = [0.0, -0.1]
params = [x, w1, b1, w2, b2]

vm = VMInterpreter()
vm.load_bytecode(bc)

# warmup
vm.call(*params)
print("result:", vm.call(*params))

# benchmark
N = 2000
t0 = time.perf_counter()
for _ in range(N):
    vm.call(*params)
t1 = time.perf_counter()
dt = (t1-t0)/N*1000
print(f"avg per call: {dt:.4f} ms   ({N} iters)")
print("stats:", vm.get_stats())
