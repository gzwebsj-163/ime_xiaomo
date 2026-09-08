#!/usr/bin/env bash
# xiaomo-cluster 一键启动/停止 (Mac 端 Coordinator)
# 用法:
#   ./launch.sh start   启动 (screen 后台常驻, :9100 + :8000)
#   ./launch.sh stop    停止
#   ./launch.sh status  查看状态
#   ./launch.sh log     查看日志
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
SESSION="xiaomo-cluster"
LOG="$DIR/coordinator.log"

cmd_start() {
  if screen -ls | grep -q "$SESSION"; then
    echo "⚠️  Coordinator 已在运行 (screen: $SESSION)"
    return 0
  fi
  echo "🚀 启动 Coordinator (screen: $SESSION)..."
  echo "======= $(date '+%F %T') =======" >> "$LOG"
  screen -dmS "$SESSION" bash -c "cd '$DIR' && python3 coordinator.py --port 9100 --dl-port 8000 >> '$LOG' 2>&1"
  sleep 1.5
  curl -s -m 3 http://127.0.0.1:9100/health && echo ""
  echo "✅ 已启动: API :9100 | 源码下载 :8000 (日志: $LOG)"
}

cmd_stop() {
  if screen -ls | grep -q "$SESSION"; then
    screen -S "$SESSION" -X quit
    echo "🛑 已停止 Coordinator"
  else
    echo "ℹ️  Coordinator 未在运行"
  fi
}

cmd_status() {
  echo "=== screen 会话 ==="
  screen -ls | grep "$SESSION" || echo "未运行"
  echo "=== /health ==="
  curl -s -m 3 http://127.0.0.1:9100/health || echo "无响应"
  echo ""
  echo "=== 节点 ==="
  curl -s -m 3 http://127.0.0.1:9100/nodes || echo "无响应"
}

case "$1" in
  start)  cmd_start ;;
  stop)   cmd_stop ;;
  status) cmd_status ;;
  log)    tail -50 "$LOG" ;;
  *) echo "用法: $0 {start|stop|status|log}"; exit 1 ;;
esac
