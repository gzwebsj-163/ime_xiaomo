#!/data/data/com.termux/files/usr/bin/bash
# xiaomo-cluster 手机端 Agent (Termux)
# 用途: 在 Android Termux 上部署 xiaomo 并接入 Mac Coordinator
#
# 用法:
#   bash phone_agent.sh setup <CoordinatorIP>   # 拉源码+编译(只需一次)
#   bash phone_agent.sh run    <CoordinatorIP>  # 启动 Agent 循环(常驻)

set -e

COORD=${2:-192.168.0.161}   # Mac 局域网 IP (可改)
PORT=8000                    # 源码下载端口
CTRL_PORT=9100               # Coordinator 控制端口
NODE_ID="android-$(getprop ro.product.model 2>/dev/null || echo phone)"
H=/data/data/com.termux/files/home

log() { echo "[xiaomo-agent] $*"; }

cmd_setup() {
  log "=== 第1步: 安装依赖 ==="
  pkg update -y && pkg install -y clang make wget curl python

  log "=== 第2步: 从 Mac 拉取 xiaomo 源码 ==="
  mkdir -p $H/xiaomo
  cd $H/xiaomo
  if [ ! -f src/main.c ]; then
    wget -q "http://${COORD}:${PORT}/xiaomo-src.tar.gz" -O $H/xiaomo-src.tar.gz
    tar xzf $H/xiaomo-src.tar.gz
    rm -f $H/xiaomo-src.tar.gz
  fi

  log "=== 第3步: 编译 xiaomo (跨平台 stub) ==="
  cd $H/xiaomo
  clang++ -std=c++17 -Iinclude -o xiaomo \
    src/lexer.c src/parser.c src/ast.c src/vm.c src/vm_core.c \
    src/vm_stack.c src/mo2kbc.c src/tensor.c src/nd_tensor.c \
    src/weights.c src/main.c src/hw/hw_direct.c src/hw/hw_demo.c -lpthread

  log "=== 第4步: 验证编译 ==="
  ./xiaomo run examples/hello.mo

  log "=== ✅ 部署完成! 本机节点: $NODE_ID ==="
  log "=== 运行 'bash phone_agent.sh run ${COORD}' 启动 Agent ==="
}

cmd_run() {
  log "=== 启动 Agent 循环 (Coordinator: ${COORD}, 节点: ${NODE_ID}) ==="
  while true; do
    # 每轮循环前幂等注册一次 (保障 Coordinator 重启后自动重连)
    curl -s -m 10 \
      -X POST "http://${COORD}:${CTRL_PORT}/node/register" \
      -H "Content-Type: application/json" \
      -d "{\"node_id\":\"${NODE_ID}\",\"arch\":\"aarch64\",\"mem_mb\":7000,\"load\":0}" \
      > /dev/null 2>&1 || true
    log "等待 Coordinator 任务..."
    TASK=$(curl -s -m 15 \
      -X POST "http://${COORD}:${CTRL_PORT}/task/next" \
      -H "Content-Type: application/json" \
      -d "{\"node_id\":\"${NODE_ID}\"}" 2>/dev/null || echo "")

    if echo "$TASK" | grep -q '"status"[[:space:]]*:[[:space:]]*"ok"'; then
      TASK_ID=$(echo "$TASK" | python -c "import sys,json; print(json.load(sys.stdin).get('task_id',''))" 2>/dev/null)
      log "收到任务: ${TASK_ID}"

      # 写临时 .mo 文件到 $HOME (Termux 的 /tmp 无写权限)
      echo "$TASK" | python -c "
import sys, json
data = json.load(sys.stdin)
open('$H/task.mo','w').write(data.get('code',''))
" 2>/dev/null

      if [ -f $H/task.mo ]; then
        OUTPUT=$(cd $H/xiaomo && ./xiaomo run $H/task.mo 2>&1 || echo "RUN_ERROR")
        # 用 python 构造 JSON 汇报 (避免引号嵌套问题)
        python3 -c "
import json, sys
payload = {'node_id': '$NODE_ID', 'task_id': '$TASK_ID', 'output': open('$H/task.out').read() if False else '''$OUTPUT'''}
" 2>/dev/null || true
        # 简化: 直接输出到文件再读
        echo "$OUTPUT" > $H/task.out
        python3 - "$NODE_ID" "$TASK_ID" "$H/task.out" "$COORD" "$CTRL_PORT" << 'PYEOF'
import json, sys, urllib.request
node_id, task_id, outfile, coord, port = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
output = open(outfile).read()
payload = json.dumps({"node_id": node_id, "task_id": task_id, "output": output}).encode()
req = urllib.request.Request(f"http://{coord}:{port}/task/result", data=payload, headers={"Content-Type": "application/json"})
try:
    urllib.request.urlopen(req, timeout=30)
    print("[xiaomo-agent] 任务完成并回报")
except Exception as e:
    print(f"[xiaomo-agent] 回报失败: {e}")
PYEOF
      fi
    else
      sleep 10
    fi
  done
}

case "$1" in
  setup) cmd_setup ;;
  run)   cmd_run ;;
  *) echo "用法: bash phone_agent.sh {setup|run} [CoordinatorIP]"; exit 1 ;;
esac
