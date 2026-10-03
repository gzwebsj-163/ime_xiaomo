"""Core module for OBF Engine — 轻量版(惰性导出)。"""
__all__ = ["OBFError", "ObfuscationError", "ConfigurationError", "VMError"]
def __getattr__(name):
    if name in ("OBFError", "ObfuscationError", "ConfigurationError", "VMError"):
        from .exceptions import OBFError, ObfuscationError, ConfigurationError, VMError
        return {"OBFError": OBFError, "ObfuscationError": ObfuscationError,
                "ConfigurationError": ConfigurationError, "VMError": VMError}[name]
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
