#!/bin/bash
# 用法: ssh_retry.sh <cmd>  — 多次重试直到成功
CMD="$*"
for i in $(seq 1 8); do
  OUT=$(python3 ssh_helper.py run "$CMD" 2>&1 | grep -iv "deprecat\|default_backend\|paramiko/transport")
  if ! echo "$OUT" | grep -q "socket.timeout\|Auth failed\|traceback"; then
    echo "$OUT"
    exit 0
  fi
  sleep 3
done
echo "FAILED after retries"; exit 1
