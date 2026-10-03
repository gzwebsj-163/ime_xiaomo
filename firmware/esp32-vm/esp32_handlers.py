  from cryptography.hazmat.backends import default_backend
# -*- coding: utf-8 -*-
"""
esp32_handlers.py — 把 esp32_bridge 挂载到 web.py 路由

为 web_channel.py 提供 Esp32VmHandler / Esp32BridgeHandler，
让服务器 web 容器(9899, 公网开放)对外暴露 /api/esp32/vm 与 /api/esp32/bridge。

Web 容器内 esp32_bridge.py 提供完整能力:
  - open_envelope(env, expected_path)    解密 ESP32 加密信封(ChaCha20)
  - handle_vm(plain)                     action: list/invoke/report/reports
  - seal(plain_obj, path, dev)           构造加密响应信封

这两个 Handler 复用 esp32_bridge 全部逻辑，只做 web.py 的薄封装。
"""
import json
import logging

from channel.web import esp32_bridge

logger = logging.getLogger(__name__)

# 端点路径（用于信封 path 校验 + 响应信封）
VM_PATH = "/api/esp32/vm"
BRIDGE_PATH = "/api/esp32/bridge"


def _cors_headers():
    import web
    web.header("Content-Type", "application/json; charset=utf-8")
    web.header("Access-Control-Allow-Origin", "*")
    web.header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
    web.header("Access-Control-Allow-Headers", "Content-Type")


def _read_envelope():
    """读取并解析请求体为信封 dict。"""
    import web
    raw = web.data() or b"{}"
    try:
        return json.loads(raw.decode("utf-8") or "{}")
    except json.JSONDecodeError as e:
        raise ValueError(f"invalid envelope json: {e}")


class Esp32VmHandler:
    """POST /api/esp32/vm — VM 指令通道 (list / invoke / report / reports)。"""

    def POST(self):
        _cors_headers()
        try:
            env = _read_envelope()
            plain, meta = esp32_bridge.open_envelope(env, expected_path=VM_PATH)
            result = esp32_bridge.handle_vm(plain)
            out = esp32_bridge.seal(result, VM_PATH, dev=meta["dev"])
            return json.dumps({"status": "success", "data": out}, ensure_ascii=False)
        except Exception as e:
            logger.error(f"[esp32/vm] {e}")
            return json.dumps({"status": "error", "message": str(e)}, ensure_ascii=False)

    def GET(self):
        """明文探活"""
        _cors_headers()
        return json.dumps({"status": "ok", "endpoint": VM_PATH})

    def OPTIONS(self):
        _cors_headers()
        return ""


class Esp32BridgeHandler:
    """POST /api/esp32/bridge — OpenAI/Mocode-Lab 对话桥 (占位回显，与 mocode-cli 网关一致)。"""

    def POST(self):
        _cors_headers()
        try:
            env = _read_envelope()
            plain, meta = esp32_bridge.open_envelope(env, expected_path=BRIDGE_PATH)
            result = {
                "echo": True,
                "type": plain.get("type"),
                "payload": plain.get("payload"),
            }
            out = esp32_bridge.seal(result, BRIDGE_PATH, dev=meta["dev"])
            return json.dumps({"status": "success", "data": out}, ensure_ascii=False)
        except Exception as e:
            logger.error(f"[esp32/bridge] {e}")
            return json.dumps({"status": "error", "message": str(e)}, ensure_ascii=False)

    def OPTIONS(self):
        _cors_headers()
        return ""
