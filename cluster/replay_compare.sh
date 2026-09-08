#!/usr/bin/env bash
# replay_compare.sh — 三端对拍: 把 mlp_trained.mo 钉派到指定节点, 输出 md5
# 用法: bash replay_compare.sh <coordinator_ip> <port> <node_id>
# 例:   bash replay_compare.sh 127.0.0.1 9100 mac-worker
#       bash replay_compare.sh 127.0.0.1 9100 server-worker
set +e
COORD=${1:?用法: replay_compare.sh <coordinator_ip> <port> <node_id>}
PORT=${2:-9100}
NODE=${3:?需要 node_id (mac-worker / kickpi-worker / server-worker)}
MO_FILE=${MO_FILE:-/Users/root1/Desktop/xiaomo/examples/mlp_trained.mo}
W_FILE=${W_FILE:-/Users/root1/Desktop/xiaomo/examples/mlp_trained_weights.json}

TID=$(python3 - "$COORD" "$PORT" "$NODE" <<'PYEOF'
import sys, json, urllib.request
coord, port, node = sys.argv[1:4]
code = open("/Users/root1/Desktop/xiaomo/examples/mlp_trained.mo", encoding="utf-8").read()
weights = open("/Users/root1/Desktop/xiaomo/examples/mlp_trained_weights.json", encoding="utf-8").read()
data = json.dumps({
    "type": "mo_run",
    "payload": {"code": code, "weights": weights, "weights_path": "examples/mlp_trained_weights.json"},
    "timeout": 60, "require": node
}).encode()
req = urllib.request.Request(f"http://{coord}:{port}/wp/submit", data=data,
                             headers={"Content-Type": "application/json"}, method="POST")
r = json.loads(urllib.request.urlopen(req, timeout=15).read().decode())
print(r.get("task_id", ""))
PYEOF
)
[ -z "$TID" ] && { echo "提交失败"; exit 1; }
echo "[replay] 已提交 $TID -> 钉派 $NODE"

for i in $(seq 1 30); do
  sleep 1
  OUT=$(curl -s "http://$COORD:$PORT/wp/$TID" 2>/dev/null)
  STATUS=$(echo "$OUT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('task',{}).get('status',''))" 2>/dev/null)
  ASSIGNED=$(echo "$OUT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('task',{}).get('assigned_to',''))" 2>/dev/null)
  if [ "$STATUS" = "done" ]; then
    echo "$OUT" | python3 -c "import sys,json; print(json.load(sys.stdin)['task']['output'])" > /tmp/replay_out.txt
    echo "[replay] ✅ done | 实际执行节点: $ASSIGNED"
    echo "[replay] md5: $(python3 -c "import hashlib;print(hashlib.md5(open('/tmp/replay_out.txt','rb').read()).hexdigest())")"
    echo "[replay] P(类1): $(grep -o '0\.999988' /tmp/replay_out.txt | head -1)"
    exit 0
  elif [ "$STATUS" = "" ]; then
    echo "[replay] 查询失败, 退出"; exit 1
  fi
done
echo "[replay] ⏰ 超时未完成"
exit 1