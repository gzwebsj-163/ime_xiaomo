# -*- coding: utf-8 -*-
"""check_compiler.py — 在容器内检查独立编译环境与运行环境"""
import sys
print("PY", sys.version)
# 1. 独立编译环境
sys.path.insert(0, "/root/ir_compile")
try:
    from crypto.obf_engine.layers.layer5_private_ir.compiler import IRCompiler
    print("ir_compile: IRCompiler OK ->", IRCompiler)
except Exception as e:
    print("ir_compile: IRCompiler FAIL", repr(e))
# 2. 直接检查 standalone compiler 内容
import importlib.util
spec = importlib.util.spec_from_file_location("cc", "/root/ir_compile/crypto/obf_engine/layers/layer5_private_ir/compiler.py")
cc = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(cc)
    names = [n for n in dir(cc) if not n.startswith('_')]
    print("standalone compiler attrs:", names)
except Exception as e:
    print("standalone compiler load fail", repr(e))
