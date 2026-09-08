#!/usr/bin/env bash
# xiaomo-cluster 通用 Worker (节点侧执行器) v2
# 用法:
#   bash worker.sh <coordinator_ip> [port] [node_id] [caps...]
#   例: bash worker.sh 192.168.0.161 9100 kickpi-x86 mo cnn video
#   例: bash worker.sh 127.0.0.1 9100 mac-local mo cnn video
#
# 环境变量(可选):
#   XIAOMO=<路径>         xiaomo 二进制 (默认自动探测)
#   FFMPEG=<路径>         ffmpeg 二进制 (video 用)
#   MEM_KB=<kb>           资源隔离: 内存上限 (默认不限)
#   WORKER_NICE=<n>       资源隔离: nice 值 (默认 19)
#   COORD_PORT=<port>     覆盖端口

# 注意: 不用 set -e! 集群 worker 必须永不因任务失败而退出.
# 任务失败要回报 error 给 coordinator, 然后继续轮询.
set +e
COORD=${1:?用法: worker.sh <coordinator_ip> [port] [node_id] [caps...]}
PORT=${2:-9100}
NODE_ID=${3:-$(hostname)-$(uname -m)}
CAPS=${4:-mo}
shift 4 2>/dev/null || true
# 剩余参数作为附加 caps
for c in "$@"; do CAPS="$CAPS $c"; done

# ---------- 环境探测 ----------
[ -n "$XIAOMO" ] || XIAOMO="./xiaomo"
# 找 xiaomo 二进制 (逐级探测: Mac/kickpi/服务器)
if [ ! -x "$XIAOMO" ]; then
  for cand in ~/Desktop/xiaomo/xiaomo ~/xiaomo/xiaomo ~/xiaomo/build/xiaomo /home/kickpi/xiaomo/xiaomo /root/xiaomo/xiaomo ./xiaomo; do
    if [ -x "$cand" ]; then XIAOMO="$cand"; break; fi
  done
fi
if [ ! -x "$XIAOMO" ]; then
  echo "[worker:$NODE_ID] 错误: 找不到 xiaomo 二进制, 请设置 XIAOMO=<路径>"
  exit 1
fi
FFMPEG_BIN=${FFMPEG:-$(command -v ffmpeg || echo "")}
ARCH=$(uname -m)
MEM_MB=$( [ "$(uname)" = "Darwin" ] && sysctl -n hw.memsize 2>/dev/null | awk '{print int($1/1024/1024)}' || grep MemTotal /proc/meminfo 2>/dev/null | awk '{print int($2/1024)}' )
CORES=$( [ "$(uname)" = "Darwin" ] && sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null )
[ -n "$MEM_MB" ] || MEM_MB=0
[ -n "$CORES" ] || CORES=1
HAS_FF=$( [ -n "$FFMPEG_BIN" ] && echo true || echo false )

log() { echo "[worker:$NODE_ID] $*"; }

# ---------- 资源隔离执行器 ----------
# ⚠️ 关键坑: 看门狗子壳必须重定向 >/dev/null 2>&1 !
# 若在 $(...) 命令替换里调用, 后台子壳会继承输出管道 fd,
# 命令替换要等所有持有管道的进程退出才结束 -> 任务白等满 timeout_s.
run_isolated() {
  local mem_kb=${MEM_KB:-0}
  local nice_v=${WORKER_NICE:-19}
  local timeout_s=$1; shift
  ( 
    if [ "$mem_kb" -gt 0 ]; then ulimit -v "$mem_kb" 2>/dev/null || true; fi
    if [ "$nice_v" -ne 0 ]; then nice -n "$nice_v" "$@"; else "$@"; fi
  ) &
  local pid=$!
  ( sleep "$timeout_s"; kill -9 "$pid" 2>/dev/null ) >/dev/null 2>&1 &
  local watch=$!
  wait "$pid"
  local rc=$?
  kill "$watch" 2>/dev/null
  return $rc
}

# ---------- 任务执行: 按类型 ----------
run_task() {
  local type=$1 payload_json=$2 timeout_s=$3
  case "$type" in
    mo_run)
      local code=$(printf '%s' "$payload_json" | python3 -c "import sys,json;print(json.load(sys.stdin).get('code',''))")
      # 权重随任务分发: payload.weights 内联 JSON -> 写到 .mo 期望的相对路径
      local weights=$(printf '%s' "$payload_json" | python3 -c "import sys,json;print(json.load(sys.stdin).get('weights',''))" 2>/dev/null)
      local wpath=$(printf '%s' "$payload_json" | python3 -c "import sys,json;print(json.load(sys.stdin).get('weights_path','examples/mlp_trained_weights.json'))" 2>/dev/null)
      if [ -n "$weights" ] && [ -n "$wpath" ]; then
        mkdir -p "$(dirname "$wpath")" 2>/dev/null
        printf '%s' "$weights" > "$wpath"
        log "权重已落盘: $wpath ($(printf '%s' "$weights" | wc -c) 字节)"
      fi
      local mo="$HOME/.xiaomo_task_$NODE_ID.mo"
      printf '%s' "$code" > "$mo"
      log "执行 mo_run: $(printf '%s' "$code" | head -1)..."
      local out
      out=$(run_isolated "$timeout_s" "$XIAOMO" run "$mo" 2>&1 || echo "RUN_ERROR rc=$?")
      echo "---TASK_OUTPUT_START---"
      echo "$out"
      echo "---TASK_OUTPUT_END---"
      ;;
    cnn_infer)
      # payload: {weights_file 或 weights_json, input} -> 生成 .mo 调用内核推理
      log "cnn_infer 待实现, 先用 mo_run 兜底"
      local code=">> print >> \"cnn_infer placeholder\""
      local mo="$HOME/xiaomo_task.mo"
      printf '%s' "$code" > "$mo"
      run_isolated "$timeout_s" "$XIAOMO" run "$mo" 2>&1 || echo "RUN_ERROR"
      ;;
    video_chunk)
      log "video_chunk 待实现"
      echo "video_chunk placeholder"
      ;;
    *)
      echo "unknown type: $type"
      ;;
  esac
}

# ---------- HTTP 辅助 (用 python3 保 JSON 正确) ----------
api() { # api <method> <path> <json_data>
  local method=$1 path=$2 data=$3
  python3 - "$method" "$path" "$data" "$COORD" "$PORT" <<'PYEOF'
import sys, json, urllib.request
method, path, data, coord, port = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
url = f"http://{coord}:{port}" + path
req = urllib.request.Request(url, data=data.encode() if data else None,
                             headers={"Content-Type": "application/json"}, method=method)
try:
    r = urllib.request.urlopen(req, timeout=30)
    print(r.read().decode())
except Exception as e:
    print(f"{{'err': '{e}'}}")
PYEOF
}

# 主循环
register() {
  python3 - "$COORD" "$PORT" "$NODE_ID" "$ARCH" "$MEM_MB" "$CORES" "$CAPS" "$HAS_FF" <<'PYEOF'
import sys, json, urllib.request
coord, port, node, arch, mem, cores, caps, hff = sys.argv[1:9]
caps = caps.split()
data = json.dumps({"node_id": node, "arch": arch, "mem_mb": int(float(mem)),
                   "cores": int(cores), "caps": caps, "bandwidth": "lan",
                   "has_ffmpeg": hff == "true", "load": 0}).encode()
req = urllib.request.Request(f"http://{coord}:{port}/node/register", data=data,
                             headers={"Content-Type": "application/json"}, method="POST")
try: urllib.request.urlopen(req, timeout=15)
except Exception as e: print(f"register err: {e}")
PYEOF
}

log "=== xiaomo Worker 启动 ==="
log "节点: $NODE_ID ($ARCH, ${MEM_MB}MB, ${CORES}核, caps=[$CAPS], ffmpeg=$HAS_FF)"
log "xiaomo: $XIAOMO | coordinator: $COORD:$PORT"

while true; do
  register
  # 取任务 (长轮询: 空则等 3s)
  resp=$(api POST /wp/next "{\"node_id\":\"$NODE_ID\"}")
  if printf '%s' "$resp" | grep -q '"status"[[:space:]]*:[[:space:]]*"ok"'; then
    TID=$(printf '%s' "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin).get('task_id',''))" 2>/dev/null)
    TYPE=$(printf '%s' "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin).get('type','mo_run'))" 2>/dev/null)
    TIMEOUT=$(printf '%s' "$resp" | python3 -c "import sys,json;print(json.load(sys.stdin).get('timeout',120))" 2>/dev/null)
    PAYLOAD=$(printf '%s' "$resp" | python3 -c "import sys,json;print(json.dumps(json.load(sys.stdin).get('payload',{})))" 2>/dev/null)
    [ -n "$TID" ] || continue
    log "收到任务: $TID ($TYPE, timeout=${TIMEOUT}s)"
    # 提取输出
    OUT=$(run_task "$TYPE" "$PAYLOAD" "$TIMEOUT" 2>&1)
    # 去掉 run_task 内部的标记行, 仅保留真实输出
    OUT=$(echo "$OUT" | sed -n '/---TASK_OUTPUT_START---/,/---TASK_OUTPUT_END---/p' | sed '1d;$d')
    # 构造回报 JSON (用 python 保正确)
    python3 - "$COORD" "$PORT" "$NODE_ID" "$TID" "$OUT" <<'PYEOF'
import sys, json, urllib.request
coord, port, node, tid, out = sys.argv[1:6]
data = json.dumps({"node_id": node, "task_id": tid, "output": out}).encode()
req = urllib.request.Request(f"http://{coord}:{port}/wp/result", data=data,
                             headers={"Content-Type": "application/json"}, method="POST")
try:
    urllib.request.urlopen(req, timeout=30)
    print("[worker] 回报成功")
except Exception as e:
    print(f"[worker] 回报失败: {e}")
PYEOF
  else
    sleep 3
  fi
done
