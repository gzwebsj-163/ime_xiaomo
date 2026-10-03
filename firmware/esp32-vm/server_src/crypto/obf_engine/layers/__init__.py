"""Obfuscation layers module — 轻量版(惰性导出)。"""
__all__ = ["PrivateIRObfuscator"]
def __getattr__(name):
    if name == "PrivateIRObfuscator":
        from .layer5_private_ir import PrivateIRObfuscator
        return PrivateIRObfuscator
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
