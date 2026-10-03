# ESP32-S3 边缘 VM（PoC 交付）

在你的 ESP32-S3 上**本地运行服务器私有 IR 字节码**，实现分布式边缘推理。

## ✅ 已完成的验证（PoC 全链路跑通）

| 验证项 | 结果 |
|---|---|
| 反汇编 mlp.bc | ✅ ESP32版解析器与服务器产出一致（24条指令）|
| 本地推理结果 | ✅ `[0.613014, 0.386986]`，与**服务器VM逐位一致** |
| XOR解密(0x5A) | ✅ 加密/解密对称，通过 |
| ulab 加速 | ✅ 有则用，无则纯Python回退 |

## 📁 交付文件

| 文件 | 说明 |
|---|---|
| `edge_vm.py` | **核心**：MicroPython 版私有IR字节码解析器 + 解释器（纯标准库，可选ulab加速）|
| `mlp.bc` | 从服务器拉取的示例字节码（386字节）|
| `demo_main.py` | ESP32-S3 板上演示/开机自启动脚本 |

## 🚀 部署到 ESP32-S3

1. **准备**：板子已刷 MicroPython（含 `urequests`）+ 可选 `ulab`
2. **上传文件**（任选其一）：
   - Thonny / mpremote / rshell / ampy 上传 `edge_vm.py` 和 `mlp.bc` 到板子根目录
   - 示例（mpremote）：
     ```
     mpremote cp edge_vm.py :edge_vm.py
     mpremote cp mlp.bc :mlp.bc
     mpremote cp demo_main.py :main.py   # 设为开机自启
     ```
3. **配置**：`demo_main.py` 顶部填入你的 `WIFI_SSID` / `WIFI_PASS`
4. **运行**：`mpremote run demo_main.py` 或重启板子

## ⚙️ edge_vm.py 用法

```python
from edge_vm import EdgeVM, disassemble

bc = open('/mlp.bc','rb').read()
print(disassemble(bc))                      # 反汇编查看

vm = EdgeVM(bc, params=[x, w1, b1, w2, b2]) # 加载 + 注入参数(权重)
result = vm.run()                           # 本地推理
```

## 📐 架构（分布式边缘推理）

```
┌── ESP32-S3 ─────────────────────────────┐
│  edge_vm.py 解析 .bc + 本地解释执行推理    │
│  (小模型本地算, 分担服务器)               │
└────────────▲────────────────────────────┘
             │ 服务器下发 .bc + 权重 + 输入
┌────────────▼────────────────────────────┐
│  服务器 VM (MoCode 私有IR)               │
│  编译函数/图→.bc, 分发任务, 收集结果       │
└─────────────────────────────────────────┘
```

## 🔧 已知约束
- **无 PSRAM（空闲 RAM ~138KB）** → 只能跑小模型（MLP/小RNN）。大模型需**模型切分/量化**后才能边缘加载
- 矩阵乘法纯Python较慢 → **建议装 ulab** 加速（`import ulab`），edge_vm 会自动用
- 只实现了**推理指令子集**；完整的混淆/栈/通用控制流指令未移植（推理不需要）

## 🎯 下一步（可选）
1. 在板上实测 `demo_main.py`，确认时序和 RAM 占用
2. 对接现有 `bridge.vm()`：服务器自动分发 .bc + 权重，ESP32 本地推理后上报结果
3. RNN 模型（rnn.bc 2.8KB）也验证一遍
4. 大模型切分：量化为 int8 + 分块下发
