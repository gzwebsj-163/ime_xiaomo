#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
export_xiaomo_weights.py — 把魔塔 int8 量化权重转成 xiaomo 内核可加载的 float JSON

xiaomo 的 load_weights(path,key) 只返回 float 数组。这里读取魔塔导出的
imggen_quantized.json（int8 整数 + scale），反量化回 float，另存一份
imggen_xiaomo.json，供 xiaomo 脚本 load_weights 加载并跑推理验证。

用法:
  python export_xiaomo_weights.py [input.json] [output.json]

说明: 这等价于「反量化」——验证 xiaomo 能用魔塔量化的权重,
      反向传播/前向与魔塔 int8 重建一致(误差只在 int8 舍入)。
"""
import argparse
import json
import numpy as np

import imggen_quantize as qz


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?", default="output/imggen_quantized.json")
    ap.add_argument("output", nargs="?", default="output/imggen_xiaomo.json")
    args = ap.parse_args()

    payload = qz.load_quantized(args.input)
    q = payload["weights"]
    meta = payload["meta"]

    out = {}
    for k, item in q.items():
        qarr = np.array(item["data"], dtype=np.float64).reshape(item["shape"])
        farr = qarr * item["scale"]                     # 反量化
        # 存成嵌套 list（与 mlp_trained_weights.json 一致格式）
        if farr.ndim == 1:
            out[k] = [float(x) for x in farr.ravel()]
        else:
            out[k] = [[float(x) for x in row] for row in farr]

    with open(args.output, "w") as f:
        json.dump(out, f, indent=2)
    print(f"[save] xiaomo float 权重 -> {args.output}")
    print(f"[info] 关键字={meta.get('keywords')} size={meta.get('size')} "
          f"bits={meta.get('bits')} 层={list(out.keys())}")


if __name__ == "__main__":
    main()
