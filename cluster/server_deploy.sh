#!/usr/bin/env bash
# server_deploy.sh — 服务器端一键部署 xiaomo + 接入集群
# 用法: bash server_deploy.sh <coordinator_ip> [port]
set +e
COORD=${1:?用法: server_deploy.sh <coordinator_ip> [port]}
PORT=${2:-9100}
SRC=/root/xiaomo-src.tar.gz
DST=/root/xiaomo

echo "=== [1/4] 解压源码 ==="
[ -d "$DST" ] || mkdir -p "$DST"
tar xzf "$SRC" -C "$DST" 2>/dev/null || tar xzf "$SRC" -C "$DST" --strip-components=0 || { echo "解压失败"; exit 1; }
# 若包内含 ./, 解压后目录可能错误, 兜底检查
[ -f "$DST/Makefile.linux" ] || { echo "Makefile.linux 不存在, 尝试 strip"; rm -rf "$DST"; mkdir -p "$DST"; tar xzf "$SRC" -C "$DST" --transform 's/^\.\///' 2>/dev/null || tar xzf "$SRC" -C "$DST" --strip-components=1; }
ls "$DST/Makefile.linux" || { echo "仍无 Makefile.linux"; exit 1; }

echo "=== [2/4] 清理跨架构 .o ==="
find "$DST" -name "*.o" -delete 2>/dev/null

echo "=== [3/4] devtoolset-9 编译 (x86_64) ==="
cd "$DST" || exit 1
if [ -x /opt/rh/devtoolset-9/root/usr/bin/g++ ]; then
  scl enable devtoolset-9 "make -f Makefile.linux -j4" > /root/build.log 2>&1
  RC=$?
else
  make -f Makefile.linux -j4 > /root/build.log 2>&1
  RC=$?
fi
if [ "$RC" -ne 0 ] || [ ! -x "$DST/xiaomo" ]; then
  echo "编译失败, 看日志尾部:"; tail -20 /root/build.log; exit 1
fi
echo "编译成功: $(file "$DST/xiaomo" | cut -d: -f2)"
"$DST/xiaomo" --version 2>/dev/null | head -1

echo "=== [4/4] 启动 worker 接入 coordinator ==="
pkill -f "worker.sh $COORD" 2>/dev/null
# worker.sh 需提前上传到 /root/xiaomo/worker.sh
[ -f "$DST/worker.sh" ] || cp /root/worker.sh "$DST/worker.sh" 2>/dev/null
mkdir -p /root/xiaomo/cluster
nohup bash "$DST/worker.sh" "$COORD" "$PORT" server-worker mo >> /root/xiaomo/cluster/worker.log 2>&1 < /dev/null &
sleep 3
echo "worker.log:"; tail -5 /root/xiaomo/cluster/worker.log 2>/dev/null
echo "=== 部署完成 ==="