#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
coordinator.py v2 - xiaomo-cluster 分布式调度器 (工作包模型, exo 风格)
====================================================================
v2 核心升级 (相对旧版"传 .mo 代码"):
  * 工作包任务模型: mo_run / cnn_infer / video_chunk, 统一 payload+blobs
  * 节点能力注册: caps(mo/cnn/video) + cores + bandwidth(lan/wan) + has_ffmpeg
  * 拓扑感知派发: 大包(video)只派 lan 节点; 按 cores/load 加权选最闲节点
  * blob 分发: 大对象(权重JSON/元数据)走 /blob/<id> 文件下载, 不内联进任务
  * 兼容旧 API: /task/next /task/result 映射为 mo_run, 手机旧 agent 仍可用

API (HTTP):
  POST /node/register  {node_id, arch, mem_mb, cores, caps, bandwidth, has_ffmpeg, load}
  POST /wp/submit      {type, payload, priority, timeout, require} -> {task_id}
  POST /wp/next        {node_id} -> {task_id, type, payload, blob_urls}
  POST /wp/result      {node_id, task_id, output, error}
  GET  /wp/<id>        查询任务
  POST /blob/upload    上传 blob -> {blob_id}
  GET  /blob/<id>      下载 blob
  GET  /nodes /tasks /health
  POST /v1/chat/completions  (兼容)
"""
import argparse
import json
import os
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, HTTPServer
try:
    from http.server import ThreadingHTTPServer
except ImportError:
    # Python < 3.7 兼容: ThreadingHTTPServer 是 3.7 才从 socketserver 提出
    from socketserver import ThreadingMixIn
    class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
        daemon_threads = True

# 任务类型 -> 需要的节点能力
TYPE_CAP = {
    "mo_run":      "mo",
    "cnn_infer":   "cnn",
    "video_chunk": "video",
}
# 任务类型 -> 是否大包(大包只派局域网)
TYPE_HEAVY = {"video_chunk": True}

DEFAULT_TIMEOUT = 120      # 普通任务超时(秒)
VIDEO_TIMEOUT   = 600      # 视频分片超时(秒)
HEARTBEAT_TTL   = 180      # 节点心跳过期秒数

# ---------- 状态 ----------
class Cluster:
    def __init__(self, blob_dir="/tmp/xiaomo-blobs"):
        self.lock = threading.Lock()
        self.nodes = {}          # node_id -> 节点信息
        self.tasks = {}          # task_id -> 任务
        self.queue = []          # [(priority, created, task_id)]
        self.blobs = {}          # blob_id -> bytes (内存, 小对象)
        self.blob_dir = blob_dir
        os.makedirs(blob_dir, exist_ok=True)

    # ---------- 节点 ----------
    def register_node(self, node_id, arch="unknown", mem_mb=0, cores=1,
                      caps=None, bandwidth="lan", has_ffmpeg=False, load=0):
        if caps is None:
            caps = ["mo"]
        with self.lock:
            self.nodes[node_id] = {
                "arch": arch, "mem_mb": mem_mb, "cores": max(1, cores),
                "caps": list(caps), "bandwidth": bandwidth,
                "has_ffmpeg": has_ffmpeg, "load": load,
                "last_seen": time.time(), "status": "online",
            }
        print(f"[coordinator] 节点注册: {node_id} ({arch}, {cores}核, "
              f"{mem_mb}MB, caps={caps}, {bandwidth}, ffmpeg={has_ffmpeg})")
        return True

    def _node_checkin(self, node_id):
        with self.lock:
            if node_id in self.nodes:
                self.nodes[node_id]["last_seen"] = time.time()
                self.nodes[node_id]["status"] = "online"

    def _online_nodes(self):
        now = time.time()
        return {n: info for n, info in self.nodes.items()
                if info["status"] == "online" and now - info["last_seen"] < HEARTBEAT_TTL}

    # ---------- 任务 ----------
    def submit_task(self, type_, payload, priority=0, timeout=None,
                    require=None, labels=None):
        """提交工作包. 返回 task_id
        type_: mo_run / cnn_infer / video_chunk
        """
        task_id = f"task-{uuid.uuid4().hex[:8]}"
        if timeout is None:
            timeout = VIDEO_TIMEOUT if TYPE_HEAVY.get(type_) else DEFAULT_TIMEOUT
        with self.lock:
            self.tasks[task_id] = {
                "type": type_, "payload": payload, "status": "queued",
                "priority": priority, "timeout": timeout,
                "require": require, "labels": labels or {},
                "assigned_to": None, "output": None, "error": None,
                "created": time.time(), "started": None,
            }
            self.queue.append((priority, time.time(), task_id))
        print(f"[coordinator] 新任务: {task_id} [{type_}] 排队中 (timeout={timeout}s)")
        return task_id

    def submit_mo(self, code, priority=0):
        """旧版兼容: .mo 代码 -> mo_run 工作包"""
        return self.submit_task("mo_run", {"code": code}, priority=priority)

    def _pick_node(self, type_, require=None):
        """拓扑感知派发:
        1) 只考虑有对应 cap + 心跳在线的节点
        2) video 大包只派 bandwidth=lan 节点
        3) 候选按 (load/cores) 归一化负载升序, 同负载取核多者
        """
        need = TYPE_CAP.get(type_, "mo")
        with self.lock:
            online = self._online_nodes()
            cands = []
            for n, info in online.items():
                if need not in info["caps"]:
                    continue
                if TYPE_HEAVY.get(type_) and info["bandwidth"] != "lan":
                    continue
                if info.get("has_ffmpeg") is False and type_ == "video_chunk":
                    # video 需要 ffmpeg; 没声明时允许尝试(可能内置处理)
                    pass
                # 归一化负载: 已分配任务数 / 核数, 越低越闲
                norm = info["load"] / max(1, info["cores"])
                cands.append((norm, -info["cores"], n, info))
            if not cands:
                return None
            cands.sort()
            return cands[0][2]  # node_id

    def next_task(self, node_id):
        """工作包派发: 节点取任务. 返回 dict 或 None
        - 超时任务回退重新排队
        - 返回 {task_id, type, payload, blob_urls, timeout}
        """
        self._node_checkin(node_id)
        with self.lock:
            now = time.time()
            # 超时回退: running 超过 timeout 且无结果 -> 重新排队
            for tid in list(self.tasks.keys()):
                t = self.tasks[tid]
                if t["status"] == "running" and t["output"] is None \
                        and now - t["created"] > t.get("timeout", DEFAULT_TIMEOUT):
                    t["status"] = "queued"
                    t["assigned_to"] = None
                    self.queue.append((t["priority"], now, tid))
                    print(f"[coordinator] 任务超时回退: {tid} ({t['type']})")
            # 队列按 (priority, created) 排序, 选一个可给当前节点的
            self.queue.sort(key=lambda x: (-x[0], x[1]))
            node_info = self.nodes.get(node_id)
            for prio, created, tid in self.queue[:]:
                t = self.tasks[tid]
                if t["status"] != "queued":
                    continue
                # require 过滤: 支持 指定node_id钉派 或 cap能力过滤
                req = t.get("require")
                # 防御: 历史遗留 dict 格式 {"node_id": ...} 归一化为 node_id
                if isinstance(req, dict):
                    req = req.get("node_id") or req.get("cap") or None
                if req:
                    if req in self.nodes:
                        # 钉派: 只给指定节点
                        if node_id != req:
                            continue
                    elif node_info and req not in node_info.get("caps", []):
                        continue
                # 节点能力校验
                need = TYPE_CAP.get(t["type"], "mo")
                if node_info and need not in node_info.get("caps", []):
                    continue
                if TYPE_HEAVY.get(t["type"]) and node_info and \
                        node_info.get("bandwidth") != "lan":
                    continue
                # 派发
                self.queue.remove((prio, created, tid))
                t["status"] = "running"
                t["assigned_to"] = node_id
                t["started"] = time.time()
                if node_id in self.nodes:
                    self.nodes[node_id]["load"] += 1
                return {"task_id": tid, "type": t["type"],
                        "payload": t["payload"], "timeout": t.get("timeout", 120)}
        return None

    def report_result(self, node_id, task_id, output, error=None):
        with self.lock:
            if node_id in self.nodes:
                self.nodes[node_id]["load"] = max(0, self.nodes[node_id]["load"] - 1)
            t = self.tasks.get(task_id)
            if not t:
                return False
            t["status"] = "done"
            t["output"] = output
            t["error"] = error
            t["done_at"] = time.time()
            print(f"[coordinator] 任务完成: {task_id} <- {node_id} "
                  f"({'OK' if not error else 'ERR: ' + str(error)[:80]})")
            return True

    def get_task(self, task_id):
        with self.lock:
            t = self.tasks.get(task_id)
            if not t:
                return None
            return json.loads(json.dumps(t, default=str))

    # ---------- blob ----------
    def blob_put(self, data, ext=".bin"):
        blob_id = uuid.uuid4().hex[:16] + ext
        with open(os.path.join(self.blob_dir, blob_id), "wb") as f:
            f.write(data)
        return blob_id

    def blob_get(self, blob_id):
        path = os.path.join(self.blob_dir, blob_id)
        if not os.path.exists(path):
            return None
        with open(path, "rb") as f:
            return f.read()

    # ---------- 快照 ----------
    def snapshot(self):
        with self.lock:
            return {
                "nodes": self.nodes,
                "tasks": self.tasks,
            }

cluster = Cluster()

# ---------- HTTP Handler ----------
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass  # 静默

    def _json(self, obj, code=200):
        body = json.dumps(obj, default=str).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        if not length:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw)
        except Exception:
            return {"_raw": raw.decode("utf-8", "replace")}

    def do_POST(self):
        path = self.path.split("?")[0]
        body = self._read_body()
        try:
            if path == "/node/register":
                cluster.register_node(
                    body.get("node_id", "unknown"),
                    body.get("arch", "unknown"),
                    body.get("mem_mb", 0),
                    body.get("cores", 1),
                    body.get("caps", ["mo"]),
                    body.get("bandwidth", "lan"),
                    body.get("has_ffmpeg", False),
                    body.get("load", 0))
                self._json({"status": "ok"})
            elif path == "/wp/submit":
                payload = body.get("payload", {})
                # 兼容: 顶层直接给 code/weights_file 时自动归并进 payload
                if not payload and body.get("code"):
                    payload = {"code": body.get("code")}
                if body.get("weights_file") and "weights_file" not in payload:
                    payload["weights_file"] = body["weights_file"]
                if body.get("weights") and "weights" not in payload:
                    payload["weights"] = body["weights"]
                # weights_file 是服务器本地路径 -> 读内容内联进 payload.weights
                if payload.get("weights_file") and not payload.get("weights"):
                    wf = payload["weights_file"]
                    try:
                        with open(wf, "r", encoding="utf-8") as _f:
                            payload["weights"] = _f.read()
                    except Exception as e:
                        return self._json({"error": f"weights_file 读取失败: {e}"}, 500)
                # require 规范化: 兼容 {"node_id": ...} 对象格式 与 纯字符串
                req = body.get("require")
                if isinstance(req, dict):
                    req = req.get("node_id") or req.get("cap") or None
                tid = cluster.submit_task(
                    body.get("type", "mo_run"),
                    payload,
                    priority=body.get("priority", 0),
                    timeout=body.get("timeout"),
                    require=req,
                    labels=body.get("labels"))
                self._json({"status": "ok", "task_id": tid})
            elif path == "/wp/next":
                node_id = body.get("node_id", "unknown")
                t = cluster.next_task(node_id)
                if t:
                    self._json({"status": "ok", **t})
                else:
                    self._json({"status": "idle"})
            elif path == "/wp/result":
                ok = cluster.report_result(
                    body.get("node_id", ""),
                    body.get("task_id", ""),
                    body.get("output"),
                    body.get("error"))
                self._json({"status": "ok" if ok else "not_found"})
            elif path == "/wp/status":
                t = cluster.get_task(body.get("task_id", ""))
                if t:
                    self._json({"status": "ok", "task": t})
                else:
                    self._json({"status": "not_found"}, 404)
            elif path == "/blob/upload":
                tid = cluster.blob_put(body.get("data", "").encode("utf-8"))
                self._json({"status": "ok", "blob_id": tid})
            elif path == "/task/next":
                # 旧兼容: 手机 agent 取 .mo 任务
                node_id = body.get("node_id", "unknown")
                t = cluster.next_task(node_id)
                if t:
                    self._json({"status": "ok", "task_id": t["task_id"],
                                "file": "task.mo", "code": t["payload"].get("code", "")})
                else:
                    self._json({"status": "idle"})
            elif path == "/task/result":
                ok = cluster.report_result(
                    body.get("node_id", ""), body.get("task_id", ""), body.get("output", ""))
                self._json({"status": "ok" if ok else "not_found"})
            elif path == "/v1/chat/completions":
                messages = body.get("messages", [])
                user_msg = next((m["content"] for m in messages if m["role"] == "user"), "")
                safe_msg = user_msg.replace('"', '').replace(chr(10), ' ')[:50]
                code = '>> print >> "hello from cluster: ' + safe_msg + '"'
                tid = cluster.submit_mo(code)
                out = None
                for _ in range(60):
                    time.sleep(1)
                    t = cluster.get_task(tid)
                    if t and t["status"] == "done":
                        out = t["output"]
                        break
                self._json({
                    "id": tid, "object": "chat.completion",
                    "choices": [{"index": 0, "message": {"role": "assistant",
                                                          "content": out or "任务还在跑"}}]})
            else:
                self._json({"error": f"unknown path {path}"}, 404)
        except Exception as e:
            self._json({"error": str(e)}, 500)

    def do_GET(self):
        path = self.path.split("?")[0]
        try:
            if path == "/health":
                self._json({"status": "ok", "nodes": len(cluster.nodes)})
            elif path == "/nodes":
                self._json(cluster.snapshot()["nodes"])
            elif path == "/tasks":
                self._json(cluster.snapshot()["tasks"])
            elif path == "/submit":
                # 测试: 提交一个 mo_run 求和任务
                code = """
void x : int = 0
void i : int = 0
while ${i} < 10:
    void x : int = ${x} + ${i}
    void i : int = ${i} + 1
>> print >> ${x}
>> print >> "hello from xiaomo cluster!"
"""
                tid = cluster.submit_mo(code)
                self._json({"task_id": tid})
            elif path.startswith("/wp/"):
                tid = path.split("/")[2]
                t = cluster.get_task(tid)
                if t:
                    self._json({"status": "ok", "task": t})
                else:
                    self._json({"status": "not_found"}, 404)
            elif path.startswith("/blob/"):
                blob_id = path.split("/")[2]
                data = cluster.blob_get(blob_id)
                if data:
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    self.wfile.write(data)
                else:
                    self._json({"error": "blob not found"}, 404)
            else:
                self._json({"error": "not found"}, 404)
        except Exception as e:
            self._json({"error": str(e)}, 500)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--dl-port", type=int, default=8000, help="源码下载端口")
    ap.add_argument("--blob-dir", type=str, default="/tmp/xiaomo-blobs")
    args = ap.parse_args()
    cluster.blob_dir = args.blob_dir
    os.makedirs(args.blob_dir, exist_ok=True)

    print("=" * 58)
    print("  xiaomo-cluster Coordinator v2 (工作包模型)")
    print(f"  API:      http://0.0.0.0:{args.port}")
    print(f"  源码下载: http://0.0.0.0:{args.dl_port}/xiaomo-src.tar.gz")
    print(f"  blob 存储: {args.blob_dir}")
    print("=" * 58)

    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()

    import functools
    from http.server import SimpleHTTPRequestHandler
    dl_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "dl")
    if not os.path.isdir(dl_dir):
        os.makedirs(dl_dir, exist_ok=True)
    # Python 3.6 兼容: SimpleHTTPRequestHandler.directory 参数是 3.7+ 才有,
    # 用 chdir 到 dl_dir 代替 (主 server 已在 daemon 线程运行, 不受影响)
    os.chdir(dl_dir)
    handler = SimpleHTTPRequestHandler
    dl = ThreadingHTTPServer(("0.0.0.0", args.dl_port), handler)
    dl.serve_forever()

if __name__ == "__main__":
    main()
