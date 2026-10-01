# xiaomo 下一步开发方案

> 版本：2026-09-28 · 配套文档：[商业价值分析报告](business-value.md)
> 基线：`make test` 33/33 全绿 · 177 文件 6 万行 C · 六端移植实测完毕

---

## 0. 现状基线与缺口分析

### 0.1 能力基线（已验证 ✅）

| 域 | 状态 |
|---|---|
| 语言工具链 | 声明/if/else/while/break/continue/fn/递归/一二维数组/字符串模板 `${}` |
| 双路径执行 | 解释器与 mo2kbc 编译链 6 用例逐行一致 |
| 推理 | MLP/CNN/意图识别 .kbc 端到端；Q16 定点近无损（PSNR 49dB+） |
| 硬件层 | hw_direct(13API)/hw_dev/hw_oem 熔丝/hw_token/hw_fault(待真机烧录) |
| 语音 | hw_asr 离线命令词 + 在线转写双引擎 |
| 跨端 | macOS/Linux/kickpi/phone/ESP32-C3·C6·S3/8266 全编译或实测 |

### 0.2 缺口（离「可售卖产品」还差的东西）

| # | 缺口 | 影响 | 优先级 |
|---|---|---|---|
| G1 | 语言残缺：`for` 循环、`dict`、真赋值语义（现靠重复声明）、字符串方法 | 教育客户第一个 demo 就会卡住 | P0 |
| G2 | 无一键工具链：训练/编译/烧录是三个手动步骤 | 无法交付给非开发者 | P0 |
| G3 | 无文档站、教程只有 README 级 | 传播与采购评估的直接障碍 | P0 |
| G4 | 无 CI：六端编译全靠手工验证 | 跨端回归随时可能破 | P0 |
| G5 | hw_fault 未真机烧录验证 | 商用中间件的最后一公里 | P0 |
| G6 | hw_oem 只有熔丝原语，无授权协议/量产烧录工具 | 设备授权收费模式的载体缺失（**已有总体设计：[W25Q 安全核心部署方案](w25q-secure-deploy.md)——双指纹绑定防拷贝 + CORE_GATE 闸门 + provision 发证工具链**） | P1 |
| G7 | 训练管线是本地 Python 脚本 | 云端 SaaS 化的前提 | P1 |
| G8 | 无版本管理规范（无 tag、无 CHANGELOG） | 商业交付的基本信任状 | P1 |

---

## 1. 总路线：三阶段

```
Phase 1 「开发者可用」        Phase 2 「可交付产品」        Phase 3 「可收费商业」
(第 1-6 周)                   (第 7-16 周)                   (第 17-26 周)
────────────────→             ────────────────→              ────────────────→
语言补全 G1                    hw_fault 独立打包 G5✓          云端 SaaS MVP G7
xiaomo-cli G2                  BSP 官方板支持                  设备授权落地 G6
文档站+教程 G3                  双许可结构                      2 个付费案例
CI 六端矩阵 G4                  教育套件首单（现金流）            社区冷启动
v0.9.0 tag                     v1.0.0 tag                     v1.x 迭代
```

**排期原则**：Phase 1 全是减法与收口（不接新架构特性），保证单人 6 周可完成；Phase 2 引入第一笔现金流后才开放 Phase 3 的重投入。

---

## 2. Phase 1 · 开发者可用（第 1-6 周）

### 2.1 语言补全（G1）—— 3 周

| 任务 | 规格 | 验收标准 |
|---|---|---|
| **for 循环** | `for i in [0, 10]:` 与 `for i in arr:` 两形态；lexer 已有 `for` 关键字，补 parser/AST/mo2kbc | 新增 3 个示例 + 回归测试；双路径一致 |
| **真赋值语义** | `x = x + 1` 糖衣到现有重复声明；保持 `void x : int = ...` 向后兼容 | 旧 33 测试零破坏 + 新 5 测试 |
| **dict 字典** | `void m : map = {"a": 1}` + `m["a"]` + `keys()`；VM 侧用数组对存 (k,v)，不新增 opcode | 3 测试双路径一致 |
| **字符串方法** | `.len()` `.upper()` `.slice(a,b)`，编译期 FFI 实现，不动内核 | 3 测试 |

> ⚠️ 技术决策 D1：**不做 class**。lexer 虽有 `class` 关键字，但类系统是 6 周+++ 的深坑且教育场景非必需，明确推迟到 v1.1+。

### 2.2 xiaomo-cli 一键工具链（G2）—— 2 周

```
xiaomo init      # 生成 app/ 工程骨架（main.mo + model/ + board.json）
xiaomo train     # 调 Python 管线（意图/MLP 模板）→ model/model.kbc
xiaomo build     # .mo → .kbc 编译 + 资源打包 → firmware.uf2/bin
xiaomo flash     # esptool 烧录（复用 IDE 的 serial_hw 烧录器代码）
xiaomo monitor   # 串口监视 + hw_fault 巡检报告
```

实现选型：**C 内置于现有 xiaomo 二进制**（不引入新语言/依赖），Python 训练部分用 `scripts/` 子进程调起。
验收：`init→train→build→flash→monitor` 在 ESP32-C3 DevKit 上 10 分钟内走通全流程，全程无手工编辑文件。

### 2.3 文档站 + 教程（G3）—— 2 周（可与 2.2 并行）

- 工具：**mdBook**（Rust 单二进制，无 Node 依赖，与项目零依赖哲学一致）；
- 结构：`docs/book/` — ① 快速上手(15min) ② 语言参考(.mo 全语法+REPL 演示) ③ 硬件 API(hw_* 全家族) ④ 教程 10 篇(从点灯到意图识别) ⑤ .kbc 格式规范；
- 内容源：`docs/architecture-devdoc.md`(81KB 已有) 拆章节 + 新写给用户视角的 API 部分；
- 部署：GitHub Pages（`gzwebsj-163/ime_xiaomo` 仓库 Action 自动发布）。

验收：新人按快速上手文档 15 分钟内在浏览器模拟器跑起 hello.mo（需嵌入 xiaomo WASM 版——见决策 D2）。

> ⚠️ 技术决策 D2：**浏览器体验 = 文档站自带 WebAssembly 版 VM**。g++ 源码已零依赖，Emscripten 移植预计 1 周内（对标六端移植的既有经验）。这是传播的放大器，放 Phase 1 末尾做，若超期滑入 Phase 2。

### 2.4 CI 六端编译矩阵（G4）—— 3 天

`.github/workflows/ci.yml`：
- **build 矩阵**：macos(g++) / ubuntu(g++ + Makefile.linux) / **riscv32-esp-elf freestanding** / **xtensa-esp-elf**（用 espressif/esp-idf docker 镜像）；
- **test job**：ubuntu 上跑 33 项回归，fail 即 block；
- **hw_fault 六模式**：`-DHW_FAULT_MODE_OVERRIDE=1..5` 全编译断言（现有 selftest 扩展）。

### 2.5 hw_fault 真机收官（G5）—— 1 天（代码已就绪）

- 烧录 `esp32-c3-tft-gpio-test/gpio_test.bin` 到 C3 真机；
- T48 屏故意拔线/短接，采集 DTC 报告与实际故障对照表（≥5 种故障注入实测）；
- 结果回填 `docs/hw_fault-fieldguide.md`（现场排障手册，商用中间件的附件）。

### 2.6 版本规约（G8）—— 1 天

- 打 tag **v0.9.0** + CHANGELOG.md（对齐 git log 的 8 个 feature 批次）；
- 语义化版本承诺：0.x 期间语言语法可能破坏，1.0 冻结语法。

**Phase 1 出口条件（gate）**：33+15 项测试全绿 · CLI 真机 10 分钟全流程 · 文档站上线 · CI 六端绿灯 · v0.9.0 tag。

---

## 3. Phase 2 · 可交付产品（第 7-16 周）

### 3.1 hw_fault 独立中间件化 —— 3 周 ⭐第一变现物

```
hwfault-sdk/
├── libhwfault.a / hwfault.h     # 独立静态库（剥离 VM 依赖，仅 libc）
├── bsp_template.c               # 客户板级回调模板
├── docs/fieldguide.md           # 现场排障手册（Phase1 产出）
└── examples/{esp32c3, stm32}/   # 双平台 demo 固件
```

- 关键工作：hw_fault 已经是 freestanding 友好（7 个标准函数），主要是**剥离对 vm_core.h 的 include 依赖**（当前 OP_HW_FAULT_CALL 集成在 VM，独立版只保留库入口）；
- 定价试点：开源 demo + 商业授权（含 STM32 移植与定制故障规则）。

### 3.2 BSP 板级支持包 —— 3 周

`boards/` 目录：esp32c3-devkit / esp32c6-devkit / esp32s3-devkit 三块官方板；
每板：引脚映射表（对齐 hw_fault 的 10 脚模型）+ hw_direct I2C/PWM 配置 + 点亮示例；
验收：`xiaomo init --board esp32c3-devkit` 后 flash 即点灯。

### 3.3 双许可结构落地 —— 2 周（法务+工程各半）

- LICENSE 拆分：核心 `LICENSE-APACHE` ；`src/hw/hw_oem.c hw_token.c` 标注 `LICENSE-COMMERCIAL`；
- Makefile 增加 `make community / make oem` 双目标（社区版无授权层符号表）；
- 授权协议文本：委托律师起草（预算 ~5k）。

### 3.4 教育套件首单（现金流）—— 5 周，与 3.1-3.3 穿插

- 组合现成资产：hw_asr（语音指令）+ intent.kbc（意图分类）+ hw_fault（学生接线排障）+ TFT 点阵反馈；
- 交付物：一套「AI 语音教育套件」参考设计（固件+教案+排障手册）；
- 商务动作：携 demo 接触 3 家教育硬件厂商（本地 STEAM 品牌优先）；
- 目标：签下 1 单定制开发（5-20 万区间）。

### 3.5 云端训练原型 —— 3 周（可延后至 Phase 3 初）

- FastAPI 包装 train_intent.py（服务器已有部署经验）；
- 上传 CSV → 训练 → 下载 .kbc 的最小 Web 页；
- 复用 hk-server 或阿里云 8.163.46.174 现有资源。

**Phase 2 出口条件**：hwfault-sdk 可独立编译交付 · 三板 BSP 点灯 · 双许可 Makefile 双产物 · ≥1 商务合同签署 · v1.0.0 tag。

---

## 4. Phase 3 · 可收费商业（第 17-26 周）

| 事项 | 说明 | 指标 |
|---|---|---|
| 云端 SaaS MVP | 训练管线 Web 化 + .kbc OTA 下发（VM 已有序列化能力） | 20 个注册用户/月 |
| 设备授权落地 | hw_oem 量产烧录工具（批量熔丝写入+token 签发） | 1 个 OEM 试点 |
| 案例研究 ×2 | 教育套件 + 工业诊断各 1 篇对外案例 | 用于商务物料 |
| 社区冷启动 | GitHub 置顶 README 优化、issue 模板、Discord/Gitee 镜像 | 500 star / 10 PR |
| v1.1 语法扩展评估 | class/异常处理（lexer 已有 catch 关键字）按需启动 | 视用户反馈 |

---

## 5. 关键技术决策记录

| # | 决策 | 理由 | 替代方案（已否） |
|---|---|---|---|
| D1 | 不做 class | 6 周+ 深坑，教育场景非必需 | 做→挤占 Phase1（否） |
| D2 | WASM 版 VM 进文档站 | 零依赖源码移植成本低，传播放大器 | 视频教程（制作更慢） |
| D3 | CLI 用 C 而非 Python | 与零依赖哲学一致，复用单一二进制 | Python CLI（多运行时依赖，否） |
| D4 | hw_fault 双形态（VM opcode + 独立库） | 商用独立、VM 集成演示双保险 | 只留 opcode（商用障碍，否） |
| D5 | Phase 1 禁止新架构特性 | 单人带宽保护 | — |

---

## 6. 本周即可启动的任务清单（Top 5）

1. ☐ hw_fault C3 真机烧录 + 5 种故障注入实测（G5，代码就绪只差烧录）
2. ☐ `for` 循环三件套（lexer 已有 token，补 parser/AST/mo2kbc + 3 测试）
3. ☐ CI 骨架：ubuntu 33 测试 + macOS 编译两项先跑起来（G4 最小闭环）
4. ☐ mdBook 文档站骨架 + 快速上手章节（G3 起步）
5. ☐ v0.9.0 tag + CHANGELOG（G8，半天内完成）

---

## 7. 度量（怎么算成了）

| 阶段 | 北星指标 | 辅助指标 |
|---|---|---|
| Phase 1 | 新人上手时间 < 15 分钟 | 测试数 ≥48 · CI 六端绿灯率 100% |
| Phase 2 | 首笔合同金额 | hwfault-sdk 下载量 · BSP 使用数 |
| Phase 3 | MRR（云端订阅+授权费） | GitHub star · 外部 PR 数 |

> 所有技术资产均已就位。接下来的 26 周里，**每一步都是把已验证的技术翻译成别人看得懂、买得到的东西**——这件事比再写 6 万行代码更重要。
