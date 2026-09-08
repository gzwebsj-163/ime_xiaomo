# xiaomo-cluster — 手机分布式 xiaomo 集群 (exo 风格)

Mac 作 Coordinator 调度 → Android 手机 (Termux) 跑 xiaomo VM → 结果回传。
架构参考 exo (分布式 AI 执行)。

## 📂 位置 (正式目录, 2026-08-23 固化)

```
~/Desktop/xiaomo/cluster/
├── coordinator.py     # Mac 端调度器 (:9100) + 源码下载 (:8000)
├── phone_agent.sh     # 手机 Agent (Termux)
├── launch.sh          # 一键 start/stop/status/log (screen 常驻)
├── README.md
└── dl/
    └── xiaomo-src.tar.gz   # 手机拉源码用 (含跨平台 stub 适配)
```

## 🚀 快速启动 (Mac)

```bash
cd ~/Desktop/xiaomo/cluster
./launch.sh start    # 一键后台启动
./launch.sh status   # 查看节点/健康
./launch.sh stop     # 停止
./launch.sh log      # 看日志
```

## 📱 手机 Agent (phone_agent.sh)

```bash
bash phone_agent.sh setup <MacIP>   # 首次: 拉源码+编译 (MacIP=192.168.0.161)
bash phone_agent.sh run    <MacIP>   # 启动 Agent (注册+10s轮询, 常驻)
```

## 🏗️ 拓扑

```
Mac (Coordinator, :9100)
   │  任务提交 / 结果查询
   ▼
Android 手机 (Termux, android-SM-G9910)   ← xiaomo VM 执行节点
   aarch64 / 7GB RAM
```

## 📡 服务端口 (Mac)
- `:9100`  Coordinator (任务调度 + OpenAI 兼容 API)
- `:8000`  源码下载服务

## 🔌 API
- `GET  /health`          健康检查
- `POST /node/register`   节点注册 `{node_id, arch, mem_mb, load}`
- `GET  /nodes`           节点列表
- `GET  /submit`          提交示例任务 (返回 task_id)
- `POST /task/next`       节点取任务 `{node_id}` → `{task_id, code}`
- `POST /task/result`     节点回报结果
- `GET  /tasks`           任务列表
- `POST /v1/chat/completions`  OpenAI 兼容接口 (提交→执行→返回)

## 📱 手机 Agent (phone_agent.sh)
```bash
bash phone_agent.sh setup <MacIP>   # 首次: 拉源码+编译
bash phone_agent.sh run    <MacIP>   # 启动 Agent (注册+轮询)
```

## ✅ 已验证
- 手机 aarch64 原生编译 xiaomo (202KB) 成功
- 跨平台适配: `hw_direct.h/c` 非 macOS 编译 stub (extern "C")
- 节点注册 + 任务分发 + 执行 + 回报全链路 done
- OpenAI 兼容接口返回手机执行结果

## ⚠️ 关键坑
1. `.mo` 是自定义语法 (非 Python): `void x : int`, `>> print >>`, `while ${i}<10:`
2. Termux `/tmp` 无写权限 → 临时文件用 `$HOME`
3. JSON 响应有空格 `"status": "ok"` → grep 要 `"status"[[:space:]]*:...`
4. scp 假成功 → 用 base64 管道写文件
5. 任务长轮询竞态 → Coordinator 90s 超时回退 + agent 10s 轮询
