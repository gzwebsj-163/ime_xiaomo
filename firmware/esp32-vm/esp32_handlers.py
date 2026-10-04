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

logger = logging.getLogger(__name__)

# 两种上下文都要能导入(2026-10-04):
#   容器内: 本文件与 esp32_bridge.py 同处 channel/web/, 容器根在 sys.path
#   本工程: 本文件与 esp32_bridge.py 同处 firmware/esp32-vm/
# 先试容器路径(生产语义), 失败才回退同目录(自测)。
#
# ✅ 事实核查(2026-10-04, 只读登录 8.163.46.174 实测, 非推断):
#   1) 容器 `mocode` (mocode-web:latest, 9899) 内 `/app/app/channel/web/` 【真实存在】,
#      含 web_channel.py(493KB)/chat.html/static/dialog_monitor.js 等 —— 布局假设成立。
#      但 channel/ 与 channel/web/ 均【无 __init__.py】, 故容器 import 走 PEP-420 命名空间包,
#      依赖容器根在 sys.path。
#   2) `esp32_bridge.py` 与 `esp32_handlers.py` 在【任何容器内都搜不到】(全盘 find) ——
#      本仓库这两个文件【从未部署】。
#   3) web_channel.py 里【零】esp32 路由 / 零 Handler 引用(8 处 "esp32" 全是无关的
#      数据处理文案) —— 这两个 Handler 【没有被挂载】。
#   4) esp32_bridge.py 里 `sys.path.insert(0,"/app")` 想 import 的 `crypto.obf_engine`
#      实际位于【另一个容器】`mocode-cli:/app/crypto/obf_engine`, 不在 `mocode` —— 跨容器错配。
# ==> 真正跑在生产路径上的实现是【另一份】: `mocode-cli:/app/esp32_vm_gateway.py`
#     (154 行, ThreadingHTTPServer, 端口 9897, 同一套信封协议, PSK 全零,
#      import cli.mcp.vm_tools + chacha20_poly1305)。接口与本文件不同:
#      prod `handle_vm(action,payload)` / `seal(obj,dev,path)`  vs
#      本文件 `handle_vm(plain)` / `seal(obj,path,dev)`(注意 dev/path 次序相反)。
#     ⚠️ 且该网关当前【没有运行】(全机无 9897 监听、无进程、端口未映射)。
# ==> 结论: 本文件是同一协议的一份【孤儿替代实现】; 它假设的部署路径(塞进 9899 web 容器的
#     channel/web)【并非生产做法】。本文件在全仓库零引用, 为其补的自检只证明
#     "代码可导入且行为可验证", 不证明它已在生产跑通。
try:
    from channel.web import esp32_bridge
except ImportError:  # pragma: no cover - 仅本工程自测走到
    import esp32_bridge  # type: ignore

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
