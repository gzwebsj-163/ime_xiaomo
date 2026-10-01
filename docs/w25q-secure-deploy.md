# W25Q 安全核心部署方案（Secure Core Deployment）

> **一句话**：把 xiaomo 核心运行时烧进 W25Q SPI Flash，ESP32 开发板插上这片 W25Q 才能跑 xiaomo 架构——
> 核心镜像经过签名验证、令牌与双硬件指纹（W25Q UID × ESP32 eFuse MAC）绑定，
> **换一片 Flash 或换一块板子都跑不起来**。W25Q 从「存储芯片」升级为「带锁的核心保险箱 + 设备身份证」。

| | |
|---|---|
| 文档版本 | v0.1（设计稿，待评审） |
| 日期 | 2026-09-28 |
| 对应缺口 | roadmap G6「设备授权落地：无授权协议/量产烧录工具」 |
| 复用资产 | hw_token（10 域令牌）、hw_oem（熔丝签名/R127）、serial_hw 烧录器、hw_fault（BSP 回调模式） |
| 新增模块 | `src/hw/hw_w25q.c` + `include/hw_w25q.h`（hw 家族第 12 个成员） |
| 目标硬件 | ESP32-C3/C6/S3 DevKit + W25Q32/64/128（4/8/16MB，3.3V 直连） |

---

## 1. 背景与动机

### 1.1 现有授权原语的边界

| 原语 | 已有能力 | 缺什么 |
|---|---|---|
| hw_token | 10 域令牌派生、seal/open 文本通道、黄金表锁定、六模式逐位一致 | 令牌只活在**单体设备的内存里**，没有跨设备的物理载体 |
| hw_oem | 熔丝签名 `XIAOMO01` 写入 R127、写一次保护、越权计数 | 签名是**全局常量**（所有设备同值），不构成"每台设备唯一"的授权凭据 |
| 二者共同短板 | — | 镜像被整片拷贝后，在任意一块板子上照样运行；**没有"这片存储+这块板子"的双向绑定** |

### 1.2 商业诉求

business-value.md 收入线③「运行时按设备授权（0.5-2 元/台）」要求：

1. **可发货**：授权能随一块物理芯片（W25Q）交付给 OEM 客户；
2. **不可拷贝**：客户拿 W25Q 镜像 dump 出来，烧到自己的 Flash 上**跑不通**；
3. **可计量**：每片授权对应唯一的 W25Q UID，出货量=授权量，天然对账；
4. **可吊销/分级**：token 域机制延续，买的哪个域跑哪个域。

### 1.3 两种产品形态（都基于同一套验证协议）

| 形态 | 接法 | 场景 | 说明 |
|---|---|---|---|
| **A · Dongle 模式** | W25Q 焊在客户板上的一个插槽/排针，ESP32 侧只留验证器 | 把 xiaomo 架构授权给**第三方硬件** | 没插"授权片"→ 板子上的 bootloader 直接不进入 xiaomo 环境。**这是"必须通过我 W25Q 里的核心验证才能用我的架构"的直接实现** |
| **B · 模组模式** | W25Q 与 ESP32 同板焊接，作为核心+应用的唯一存储 | 自家量产硬件 | 省去模组内 ESP32 自带 Flash 的成本，同时获得授权绑定 |

> MVP 从 **形态 A（Dongle）** 切入：外挂一颗 1.5 元的 W25Q32，验证逻辑改动最小，商业演示最直观。

---

## 2. 总体架构

### 2.1 硬件拓扑（ESP32-C3 DevKit + W25Q32，形态 A）

```
ESP32-C3                        W25Q32 (SPI Flash)
┌─────────────────┐            ┌──────────────────┐
│            GPIO6 ──CLK──────►  SCK   (pin 6)     │
│            GPIO7 ──MOSI──────►  DI    (pin 5)     │
│            GPIO2 ──MISO◄─────  DO    (pin 2)     │
│           GPIO10 ──CS▼───────  CS#   (pin 1)     │
│                 3V3 ─────────  VCC   (pin 8)     │
│                 GND ─────────  GND   (pin 4)      │
└─────────────────┘            WP#(3)/HOLD#(7)→3.3V┘
                               ↑
              引脚 1..8 与 hw_fault 的 10 脚诊断表对齐
              （P4=SCL↔CLK / P5=SDA↔MOSI/MISO 复用诊断类）
```

- SPI Mode 0，时钟起步 10MHz（验证期）/40MHz（运行期，C3 上限内）；
- WP#/HOLD# 上拉到 3.3V（不用写保护脚，防误写入靠指令层+安全寄存器锁存）；
- **W25Q 的 8 脚表直接收进 hw_fault 引脚诊断表**——授权片插没插好、虚焊，先由 hw_fault 的 `hw_w25q` 域扫描报告（PWR/GND/CTRL/DATA 分类已具备）。

### 2.2 信任链（本文档的灵魂）

```
                    ┌──────────────────────────────────┐
                    │  发证方（你，唯一持有 ROOT KEY）   │
                    │  xiaomo provision 工具            │
                    └───────┬──────────────────────────┘
                            │ 签核心镜像 + 写域令牌(绑定双指纹) + 写设备证书
                            ▼
┌─────────────────────────── W25Q（授权+核心载体）──────────────────────────┐
│ [头部]  [核心镜像(签名覆盖)]  [证书区(安全寄存器,可锁死)]  [令牌区] [应用区] │
└───────────────────────────────────┬──────────────────────────────────────┘
                                    │ SPI 读取 + 周期挑战
┌───────────────────────────────────▼──────────────────────────────────────┐
│ ESP32 开发板                                                              │
│                                                                           │
│  ROM → bootloader(内嵌验证器 hw_w25q_verify) ← 信任锚在此，不在 W25Q!  │
│          │  L1 验镜像签名（防篡改）                                        │
│          │  L2 验域令牌绑定 = f(域盐, W25Q_UID, ESP32_MAC)（防拷贝）      │
│          ▼                                                                │
│      xiaomo 核心运行（VM+推理+hw_fault 全家）                              │
│          │  L3 运行期周期挑战应答（防"验证后拔片"）                         │
│          ▼                                                                │
│      .kbc 应用字节码                                                       │
└───────────────────────────────────────────────────────────────────────────┘
```

**关键设计决策——信任锚必须一半在 ESP32、一半在 W25Q，缺一半即失效：**

> 验证器代码与根公钥/根密钥摘要放在 ESP32 侧（bootloader，量产可再由 ESP-IDF Secure Boot V2 保护）；
> 被验证的核心镜像与绑定令牌放在 W25Q 侧。
> 若把验证器也放进 W25Q，攻击者整片拷贝即可自洽通过，信任链形同虚设。

### 2.3 三级验证协议总览

| 级 | 时机 | 验什么 | 用什么原语 | 挡住什么攻击 |
|---|---|---|---|---|
| **L1 镜像级** | 上电 bootloader | 核心镜像 SHA-256 + 签名 | 新增 `hw_w25q` 验签 + mbedTLS（ESP）/自研 SHA256（全跨） | 篡改镜像、夹带私货 |
| **L2 设备级** | bootloader 尾声 + 核心启动时 | 域令牌值 == f(域盐, **W25Q 64bit UID**, **ESP32 eFuse MAC**) | hw_token 派生扩展 + W25Q UID 指令(0x4B) + esp_read_mac | **换片、换板、整片克隆** |
| **L3 运行期** | 核心运行中每 N 秒/敏感操作前 | 向 W25Q 发随机挑战，期待应答 = 派生值(UID, 挑战) | hw_token derive + W25Q 随机数（挑战值由 ESP32 侧 TRNG 出） | 验证后拔片、运行中偷换 |

L2 是防拷贝的命门：**拷贝者能复制 Flash 的每一个字节，但复制不了 W25Q 出厂固化在硅片里的 64bit UID，也复制不了另一块 ESP32 的 eFuse MAC**。令牌的派生输入同时含两者，物理上无解。

---

## 3. W25Q 镜像布局（W25Q32 = 4MB 为例）

```
偏移          大小        区域                内容
─────────────────────────────────────────────────────────────────────────
0x000_0000    4 KB        头部区              魔数 "XMW1"(0x584D5731)
                                             版本 u16 / 核心入口 u32
                                             分区表(4条: 偏移+长度+CRC32)
0x001_0000    512 KB      核心镜像区          xiaomo runtime 镜像(.kbc 承载
                                             VM 内核+FFI+hw 家族, 见 §6.2)
0x009_0000    32 B×2      签名区              核心镜像 SHA-256 摘要 + HMAC
                                             (或 Ed25519 sig, 见 §5.3 决策)
0x009_1000    64 B        令牌区              hw_token seal 文本行 × 10 域
                                             ("TK1.07.03.0001.<hex>.<cksum>")
0x00A_0000    256 B×3     证书区              W25Q 安全寄存器×3(Sec.Reg#0-2):
                                             #0 设备证书(UID/MAC/域位图/签发序列)
                                             #1 备用(吊销列表 CRC)
                                             #2 留空
                                             ※ 写入后置 LB 位永久锁存
0x010_0000    ~3.5 MB     应用区              .kbc 字节码 / 模型 / 资源
0x3F0_0000    1 MB        A/B 备份区          核心热更新用(P2 阶段)
```

要点：

- **头部区永远明文可读**（bootloader 要靠它定位一切），但头部内所有偏移在 L1 验签范围内——改头部=签名失效；
- **令牌区用 hw_token 现成的 seal 文本格式**，串口/网络/Flash 三种通道流转同一格式，零新协议；
- **证书区用 W25Q 硬件安全寄存器**（Erase 0xB4 / Program 0x30 / Read 0x48，每片 3×256B）：
  烧完置状态寄存器-2 的 LB1/LB2/LB3 位 → **芯片级永久只读**，连发证方自己都改不了，
  是 W25Q 上唯一"写一次锁死"的区域，天然放设备证书（对齐 hw_oem 熔丝的哲学：一次性、不可撤销）；
- 4KB 对齐与 C3 的 flash 映射缓存行对齐，避免跨 cache line 取指的边界坑。

---

## 4. 核心验证协议详细设计

### 4.1 L1 镜像签验（防篡改）

```
bootloader:
  1. SPI 读 W25Q 头部(4KB) → 校验魔数 XMW1 / 版本合法
  2. 按分区表读 [核心镜像区] 全量 → SHA-256（C3/C6/S3 硬件 SHA 加速）
  3. 读 [签名区] → 验证 digest 签名
     - MVP 路线: HMAC-SHA256(key=设备级派生密钥, msg=镜像)
       其中 key = hw_token 派生链: derive(HW_OEM) ⊕ W25Q_UID
       → 签名与设备绑定, 拷到别的片上连 L1 都过不了
     - 量产路线(可选升级): Ed25519(ROOT 私钥签镜像, bootloader 只存公钥)
       逆向 bootloader 也提不出"能签新镜像"的东西
  4. 任一步失败 → 点灯报错码(L1-FAIL) + 拒载核心, 停在验证器
```

注意逆向 hw_token 的派生算法是公开可读的（源码在手），所以 L1 的 HMAC **密钥来源必须含片外拿不到的两个因子**（W25Q UID + ESP32 MAC）——这正是 L1/L2 分层但密钥链贯穿的设计。

### 4.2 L2 双指纹绑定（防拷贝——本方案的商业护城河）

```
发证时（xiaomo provision）:
  expect = hw_token_derive_ext(type, w25q_uid, esp32_mac)   ← 派生扩展原语
  把 seal(expect) 写入 W25Q 令牌区

运行时（bootloader 尾声 + 核心启动各验一次）:
  实读: W25Q 芯片 UID (指令 0x4B, 64bit, 出厂硅级唯一)
        ESP32 eFuse MAC (esp_read_mac / esp_efuse_mac_get_default)
  复算: derive_ext(type, uid, mac) 与令牌区值比对
  不等 → L2-FAIL, 拒入 xiaomo 架构
```

对 hw_token 的扩展（保持全跨式铁律）：

```c
/* hw_token.h 新增 —— 派生扩展: 与 hw_token_derive 同款纯整数运算 */
uint64_t hw_token_derive_ext(uint8_t type,
                             uint64_t w25q_uid,    /* 0 表示无外挂, 退化为普通派生 */
                             uint64_t esp32_mac);  /* 0 同上, 宿主模拟用 */
```

- 调用点兼容：`hw_token_derive(type)` ≡ `hw_token_derive_ext(type, 0, 0)`，旧黄金表不破；
- 新黄金表：`derive_ext` 增补一张（10 域 × 3 组典型 UID/MAC 样本），Python 对拍锁定，selftest 断言；
- **校验 UID/MAC 读数合法性**：全 0/全 F 的 UID（读线虚焊时的典型值）直接判 L2-FAIL，防止"拔了 W25Q 让 MISO 悬空读出恒 0 恰好撞上未绑定派生"的边角。

### 4.3 L3 运行期挑战应答（防"验证后拔片"）

```
核心运行中（每 30s / 每次 hw_dev 敏感命令前）:
  1. ESP32 TRNG 出 64bit nonce → 经"挑战寄存器"写入 W25Q 应用区头部
     （W25Q 无真 OTP 寄存器, 借 4KB 挑战页, 磨损在 W25Q 寿命内可忽略:
      4KB 页 100k 次擦写 / 30s 一次 ≈ 34 天连续运行耗尽 1 页,
      故挑战页轮转 8 页 → 9 个月, 或挑战频率降为 60s → 2 年）
  2. 期待应答 = derive_ext(HW_SELF_TOKEN, uid, mac) ⊕ nonce
  3. 下次 SPI 事务读回应答并校验; 拔片/换片 → 读回值错误或超时
  4. 连续 2 次失败 → 核心自杀: 清 RAM 关键表 + 复位回 bootloader
```

> L3 的哲学：让"拔片继续跑"的收益窗口缩短到秒级，且无法离线伪造——挑战值来自 ESP32 TRNG，应答需要知道双指纹派生的密钥。

### 4.4 失败行为矩阵

| 验证失败 | 现象 | 商业语义 |
|---|---|---|
| L1-FAIL | 点灯长亮 + 串口打印 `W25Q:L1 BADSIG` | "这镜像不是我发的" |
| L2-FAIL | 点灯双闪 + `W25Q:L2 TOKEN MISMATCH <期望域>` | "这片授权不属于这台设备" |
| L3-FAIL | 运行中复位回 bootloader + `W25Q:L3 CHALLENGE LOST` | "授权片被移走了" |
| 无 W25Q | hw_fault 扫描 `UNPLUGGED` + 验证器直接停 | "没买授权" |

---

## 5. 推送与烧录工具链

### 5.1 `xiaomo provision`（发证方工具，只在你的机器上跑）

```
xiaomo provision --board esp32c3 \
                 --w25q W25Q32 \
                 --domains esp32,point \        # 授权哪些域
                 --mac AA:BB:CC:DD:EE:FF \      # 绑定客户板(可后补: 见"空白令牌二次绑定")
                 --out auth-flash.img

产出:
  auth-flash.img   ← 可直接用编程器/经 ESP32 桥写到 W25Q 的完整镜像
  记账行 UID=0x... 域=.. MAC=.. seq=.. → 追加 provision-ledger.tsv（出货台账）
```

空白令牌二次绑定（OEM 先拿片、后绑板子的真实流程）：
provision 时 MAC 填 0 → 发"待激活片"；客户第一次插上任意板子时，由 bootloader 里的一次性激活例程把当前 MAC 写入并重算令牌（只在 seq=0 未激活时允许，激活后令牌区锁定）。**激活过程不涉及你的私钥离机**，L1 依然要求 HMAC，而 HMAC 密钥派生链此时才完整。

### 5.2 `xiaomo push`（推送链路，三条通道按现场选）

| 通道 | 路径 | 适用 |
|---|---|---|
| **P1 经板桥**（推荐 MVP） | 宿主 →esptool→ ESP32 RAM 内 flasher 代理 →SPI→ W25Q | 已有串口和板子，零额外硬件 |
| P2 直连编程器 | 宿主 →CH340/FT232→ W25Q（或 TL866 等编程器） | 产线批量发证 |
| P3 网络推送 | 宿主 →WiFi/TCP:9000→ ESP32 →SPI→ W25Q | 远程更新应用区（.kbc 热更） |

P1 复用两块已验证的资产：esptool ROM 协议（模拟器烧录战役已把 reset 时序踩平）+ serial_hw 的分片/重组器。W25Q 写入指令序列：Write Enable(0x06) → Page Program(0x02) → 轮询 Status(0x05) Bit0；擦除 Sector(0x20)/Block(0xD8) 前 Read(0x03) 校验空白。

### 5.3 密码学选型决策（需要你拍板）

| 方案 | 优点 | 缺点 | 建议 |
|---|---|---|---|
| HMAC-SHA256（对称） | 纯 C 自研可全跨式、无 mbedTLS 依赖、256B 内搞定 | bootloader 里躺着"能签任何镜像"的密钥材料（虽经派生混淆） | **MVP 采用**（快出 demo） |
| Ed25519（非对称） | bootloader 只存公钥，被逆向也签不了新镜像 | 需要 Ed25519 实现（ESP 上有 mbedTLS，宿主有；但全跨式=自研或引入 monocypher 一类） | P2 量产升级 |
| 纯 hw_token 派生（无独立签名层） | 零新代码 | 镜像本身不防篡改（令牌只绑身份不绑内容） | ❌ 不够，仅作 L2/L3 |

MVP→量产迁移时签名区格式预留双槽（32B digest+32B HMAC / 64B Ed25519），头部版本位区分。

---

## 6. 新模块：`hw_w25q`（hw 家族第 12 命名纪律成员）

### 6.1 定位与分层

```
include/hw_w25q.h      对外 API：驱动 + 验证协议 + selftest + CLI
src/hw/hw_w25q.c        实现：三段(常量/驱动/协议)，全跨式铁律
                        (纯整数运算+最少 libc: memset/snprintf/strncmp)
src/hw/hw_w25q_flasher.c  推送代理(仅 ESP 模式编译): RAM flasher 命令循环
```

### 6.2 API 草案

```c
/* ---- 驱动层(BSP 注入, 与 hw_fault 同款回调模式, 默认=模拟器) ---- */
typedef struct {
    /* 最小集: 三条原语, 平台只须实现这三个 */
    int  (*spi_xfer)(const uint8_t* tx, uint8_t* rx, uint32_t n);
    void (*delay_ms)(uint32_t ms);
    int  (*trng)(uint8_t* out, uint32_t n);       /* L3 用, 可为 NULL(降级) */
} hw_w25q_bsp_t;
int  hw_w25q_bsp_install(const hw_w25q_bsp_t* b);

/* ---- 芯片层 ---- */
int   hw_w25q_probe(void);                 /* JEDEC 0x9F: 在线=0xEF40xx */
uint64_t hw_w25q_uid(void);                /* 0x4B: 64bit 硅级唯一号 */
int   hw_w25q_read (uint32_t addr, uint8_t* buf, uint32_t n);
int   hw_w25q_prog (uint32_t addr, const uint8_t* buf, uint32_t n);
int   hw_w25q_erase(uint32_t addr, uint8_t granularity /*4K/32K/64K*/);
int   hw_w25q_sec_reg_wr(uint8_t idx, const uint8_t* d256);  /* 0x30/0xB4 */
int   hw_w25q_sec_reg_lock(uint8_t idx);   /* LB 位, 永久锁存! */

/* ---- 验证协议层(三级, 各自独立可测) ---- */
int   hw_w25q_verify_image(void);          /* L1: 镜像 HMAC/签名 */
int   hw_w25q_verify_token(uint8_t type);  /* L2: 双指纹绑定令牌 */
int   hw_w25q_challenge(void);             /* L3: 随机挑战应答 */
int   hw_w25q_boot(void);                   /* L1→L2→载核 顺序编排 */
int   hw_w25q_selftest(hw_w25q_puts_fn putf);  /* NULL=静默, 真机可跑 */
int   hw_w25q_cli(void);                    /* ./xiaomo w25q 子命令 */
```

### 6.3 与 VM/编译器的接线（沿用 hw_fault 六层模式）

1. **Makefile**：收编 `src/hw/hw_w25q.c`（macOS + Makefile.linux）；
2. **VM 内核**：`OP_HW_W25Q_CALL` opcode + kvm_run 上电在 hw_oem_protect 之后追加 `hw_w25q_boot()` 失败即拒跑（**CORE_GATE 硬闸门**，这正是"必须通过验证才能用我的架构"的代码化）；
3. **mo2kbc**：内置 `hw_w25q("cmd")` → 字节码侧可查 `hw_w25q("uid")` / `("domain")` / `("challenge")`；
4. **CLI**：`./xiaomo w25q [probe/uid/verify/boot/inject/selftest]`；
5. **examples**：`w25q_secure_boot.mo` 演示"拔片→L3 失败→复位"；
6. **tests/run_tests.sh**：五连测（黄金派生/验签通过/篡改拒绝/换 UID 拒绝/挑战回环）。

### 6.4 全跨式铁律（对齐 hw_token/hw_fault 的验收标准）

- 派生/验签计算**只用无符号整型移位乘异或**（模 2^64 回绕），不用浮点/时序/指针值；
- 六模式（HOST/LINUX/KELL/ESP32/ESP8266/TEST）编译零警告，`-DHW_W25Q_MODE_OVERRIDE` 强指；
- freestanding 依赖面：`memset/snprintf/strncmp`（外加 BSP 注入平台 SPI）；
- 黄金表：UID×MAC×type 的派生矩阵由独立 Python 对拍锁定，selftest 断言逐位相等；
- 模拟器形态：默认 BSP 无真 SPI 时，UID 用 `0xW25Q0001` 类常量顶替，全协议路径仍可回归。

---

## 7. 安全分析（诚实清单）

### 7.1 本方案挡得住

| 攻击 | 挡在哪 |
|---|---|
| 整片拷贝 W25Q 镜像到自购 Flash | L2：UID 不同 → 令牌失配 |
| 把授权片借给别的板子用 | L2：eFuse MAC 不同 → 失配 |
| 验证通过后拔片，程序继续跑 | L3：周期挑战丢失 → 核心自杀 |
| 篡改/夹带核心镜像 | L1：HMAC 失效 |
| 伪造令牌区文本 | hw_token 三重验证（结构/域值/校验和）+ 新黄金表 |
| 普通用户软破解（改 app 跳过验证） | 验证器在 bootloader/VM 硬闸门层，app 够不着 |

### 7.2 本方案挡不住（诚实边界，需要商业条款兜底）

| 攻击 | 缓解 |
|---|---|
| 专业实验室 chip-off 读 eFuse MAC + SPI 总线中间人（探针级） | 成本远超 0.5-2 元/台的授权费；授权协议中约定拆解即违约 |
| bootloader 自身被逆向后重写（只有形态 A 且客户板未开 Secure Boot 时） | 量产建议 OEM 板开 ESP Secure Boot V2 护住 bootloader；或形态 B 模组自己焊死 |
| ESP8266 无 eFuse MAC 可靠读取路径 | ESP8266 MAC 在 Flash 里的系统参数区，可伪造 → 8266 域降级为"仅 UID 绑定"并在文档/合同注明 |
| 一次性激活（空白令牌二次绑定）环节的冒领 | 激活需同时持有发证方签发的 seq=0 原片，冒领等价于偷片 |

### 7.3 风险登记

- **R1 挑战页磨损**：见 §4.3，页轮转已缓解，仍需在 hw_fault 诊断里加"挑战页剩余寿命"读数；
- **R2 SPI 速率与启动时长**：4MB 全量 SHA-256 在 C3 @160MHz 软实现约 1-2s（硬件 SHA 约 200ms）→ bootloader 里只验"核心镜像区 512KB"，应用区用分区 CRC32 抽验；
- **R3 令牌区明文可读**（seal 格式本就为跨模式流转设计）——泄露的只是"期望值"，重放无门（密钥在派生输入里，不在文本里）。

---

## 8. 与现有资产的复用矩阵

| 资产 | 在本方案中的角色 | 改动量 |
|---|---|---|
| hw_token | L2/L3 的派生原语（新增 `derive_ext` + 新黄金表） | +1 函数 +1 表 |
| hw_oem | 熔丝签名常量 `XIAOMO01` 直接作为 HMAC 派生链根；R127 语义升级为"已过 CORE_GATE"标志位 | ~0 |
| hw_fault | W25Q 8 脚进诊断表（UNPLUGGED/短路先暴露）；BSP 回调模式被 hw_w25q 沿用 | +1 脚表条目 |
| serial_hw 烧录器 + esptool ROM 协议经验 | P1 推送通道的复位时序/分片重组直接复制 | 配置级 |
| hw_w25q（新） | 驱动+协议+selftest+CLI 六层接入 | 全新 ~1.5k 行 C |
| business-value 收入线③ / roadmap G6 | 本方案即其载体，完成后 G6 关账 | 文档引用 |

---

## 9. 实施路线图

| 阶段 | 内容 | 验收 | 工作量 |
|---|---|---|---|
| **M1 驱动先行**（1 周） | hw_w25q 驱动层 + ESP32 C3 BSP + 真机 probe/UID 读取 | 真机串口打出 `JEDEC=EF4016 UID=0x...` | ~2 天代码 + 摸板 |
| **M2 令牌绑定**（1 周） | hw_token_derive_ext + Python 对拍黄金表 + L2 验证 + 验证失败矩阵 | 换一片 W25Q → L2-FAIL 实测复现（防拷贝演示片） | ~3 天 |
| **M3 CORE_GATE 闸门**（1 周） | OP_HW_W25Q_CALL + kvm_run 硬闸门 + `./xiaomo w25q` CLI + 六层接线 | 拔片跑 .mo → 拒跑；插回 → 正常；`make test` 全绿 | ~3 天 |
| **M4 推送链路**（1-2 周） | provision 工具 + P1 板桥推送 + 台账 TSV | 从裸片到"插上即跑 xiaomo"≤5 分钟 | ~5 天 |
| **M5 签名升级**（P2 量产期） | Ed25519 双槽签名 + A/B 双分区热更 + 激活二次绑定 | 重签镜像在旧 bootloader 上被拒 | 与量产节奏合拍 |
| **M6 演示物料** | 防拷贝三连演示（克隆片/借板/拔片）视频 + 授权报价页 | 商业谈判道具 | 1 天 |

关键顺序：**M2 的"换片 L2-FAIL 实测"是整个商业故事的实证基石**，优先级压过一切工程美化。

---

## 10. 开放问题（需要决策）

1. **签名选型**：MVP 走 HMAC-SHA256（§5.3 建议）你确认吗？还是直接上 Ed25519 一步到位？
2. **8266 降级**：ESP8266 域退化为仅 UID 绑定，商业报价是否区分？
3. **激活模式**：空白令牌"客户自行激活"（§5.1）流程要不要进 MVP？不进则发证前必须拿到客户板 MAC。
4. **授权粒度**：域位图（买了 point+esp32 域却想用 infer 域）的越权行为=硬拒还是降级+打点？建议硬拒+hw_fault 记 DTC 码。
5. **挑战周期**：30s/60s/可配置？（把 §4.3 的磨损测算带进报价）

---

## 附录 A · W25Q 指令速查（本方案用到的）

| 指令 | 码 | 用途 |
|---|---|---|
| Read JEDEC ID | 0x9F | probe：应答 `EF 40 xx`（Winbond 专属前缀） |
| Read Unique ID | 0x4B | 64bit 硅级序列号，**L2 绑定核心** |
| Read Data | 0x03 | 基本读（≤50MHz） |
| Fast Read | 0x0B | 高速读（dummy byte），运行期用 |
| Write Enable | 0x06 | 每次编程/擦除前置 |
| Page Program | 0x02 | 256B 页编程 |
| Sector/Block Erase | 0x20 / 0xD8 | 4KB / 64KB 擦除 |
| Read Status | 0x05 | 轮询 BUSY(Bit0)/WEL(Bit1) |
| Read/Write Status-2 | 0x35 / 0x31 | LB1-3 位（安全寄存器锁存）在此 |
| Erase/Program/Read Security Reg | 0xB4 / 0x30 / 0x48 | 3×256B OTP 区（证书区） |

## 附录 B · 术语表

- **CORE_GATE**：VM 内核里的硬闸门——W25Q 三级验证不过，kvm_run 拒绝执行任何 .kbc；
- **发证（provision）**：用你的根密钥生成一片"授权 W25Q"完整镜像的动作；
- **双指纹**：W25Q 64bit UID × ESP32 eFuse MAC，令牌派生的两个物理因子；
- **Dongle 形态**：授权片可插拔，验证器留在板上的产品形态（本方案 MVP）。
