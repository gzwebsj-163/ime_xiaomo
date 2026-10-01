#!/bin/bash
# xdebugd 一键入口：确保服务在跑 → 用浏览器打开正确地址
# 双击即用（macOS 会把它交给 Terminal 执行），不要再双击 xdebug/web/index.html
cd "$(dirname "$0")" || exit 1
PORT="${1:-9210}"
URL="http://127.0.0.1:${PORT}/"

echo "🐞 xdebugd 调试工作台"
echo "   项目目录: $(pwd)"

# 服务已在跑？
if curl -s -m 2 -o /dev/null "http://127.0.0.1:${PORT}/api/state"; then
  echo "   ✔ 服务已在运行 (端口 ${PORT})"
else
  if [ ! -x ./xdebugd ]; then
    echo "   ✗ 找不到可执行文件 ./xdebugd，请先执行: make xdebugd"
    read -r -p "   按回车关闭…" _
    exit 1
  fi
  echo "   → 启动服务 (端口 ${PORT}) …"
  mkdir -p tmp
  nohup ./xdebugd "$PORT" > tmp/xdebugd.log 2>&1 &
  # 等它起来
  for _ in $(seq 1 40); do
    curl -s -m 1 -o /dev/null "http://127.0.0.1:${PORT}/api/state" && break
    sleep 0.25
  done
  if curl -s -m 2 -o /dev/null "http://127.0.0.1:${PORT}/api/state"; then
    echo "   ✔ 服务已就绪 (日志: tmp/xdebugd.log)"
  else
    echo "   ✗ 服务启动失败，请看 tmp/xdebugd.log"
    read -r -p "   按回车关闭…" _
    exit 1
  fi
fi

echo "   → 打开浏览器: ${URL}"
open "$URL"
echo
echo "完成。此窗口可以直接关闭（服务在后台运行）。"
echo "停止服务: pkill -f 'xdebugd ${PORT}'"
sleep 1
