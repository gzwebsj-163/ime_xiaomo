# -*- coding: utf-8 -*-
"""编译 zh_rnn 单步前向为 bc（参数注入版，规避常量池 list 序列化 bug 与多输出局限）。
输入(运行时注入): x, h_prev, W_in, W_h, b_h, W_out, b_out
输出: out = concat(prob(91维softmax), hnext(12维tanh))  单向量 103 维
  - out[0:91]  = prob  汉字概率分布（和为1）
  - out[91:103] = hnext 新隐藏状态
权重由服务器 bridge 加载 npz 后组装注入。
"""
import sys
for p in ["/app", "/root/ir_compile"]:
    if p not in sys.path:
        sys.path.insert(0, p)

from crypto.obf_engine.layers.layer5_private_ir.compiler import IRCompiler

graph = {
    "name": "zh_rnn_step",
    "inputs": ["x", "h_prev", "W_in", "W_h", "b_h", "W_out", "b_out"],
    "input_schema": {"x": "vec", "h_prev": "vec", "W_in": "mat",
                     "W_h": "mat", "b_h": "vec", "W_out": "mat", "b_out": "vec"},
    "nodes": [
        {"name": "hp",    "type": "matmul",  "inputs": ["x", "W_in"]},
        {"name": "hx",    "type": "matmul",  "inputs": ["h_prev", "W_h"]},
        {"name": "s1",    "type": "add",     "inputs": ["hp", "hx"]},
        {"name": "s2",    "type": "add",     "inputs": ["s1", "b_h"]},
        {"name": "hnext", "type": "tanh",    "inputs": ["s2"]},
        {"name": "lo1",   "type": "matmul",  "inputs": ["hnext", "W_out"]},
        {"name": "lo2",   "type": "add",     "inputs": ["lo1", "b_out"]},
        {"name": "prob",  "type": "softmax", "inputs": ["lo2"]},
        {"name": "out",   "type": "concat",  "inputs": ["prob", "hnext"], "params": {"count": 2, "axis": 0}},
    ],
    "outputs": ["out"],
}

comp = IRCompiler(optimization_level=1)
bc = comp.compile_inference_graph(graph)
data = bc.serialize(encrypt=False)
open("/app/_vm_demo/zh_rnn_step.bc", "wb").write(data)
print("wrote zh_rnn_step.bc bytes=%d instrs=%d" % (len(data), len(bc.instructions)))
print("input_names:", bc.get_metadata("input_names"))
