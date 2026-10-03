# hw 全家桶开发文档

> **定位**：`src/hw/` 下 13 个硬件抽象模块的开发手册。回答「家族怎么组织、每个模块干什么、怎么加新成员、怎么验证」。
>
> **数据来源**：本文所有数字与结论均于 **2026-10-02** 从磁盘源码 + 实跑二进制采集，**不含凭记忆书写的部分**。凡是实测与既有记录冲突的，本文以实测为准并在文中标注。
>
> **前置阅读**：`docs/architecture-devdoc.md`（C 源码全量解析）、`docs/hw_isa.md`（指令集）。

---

## 目录

- [第 0 章 家族全景](#第-0-章-家族全景)
- [第 1 章 家族公约（四条铁律）](#第-1-章-家族公约四条铁律)
- [第 2 章 模块总表](#第-2-章-模块总表)
- [第 3 章 公共基础：注册表 hw_main](#第-3-章-公共基础注册表-hw_main)
- [第 4 章 模块详解](#第-4-章-模块详解)
- [第 5 章 六层接入检查表](#第-5-章-六层接入检查表)
- [第 6 章 验证体系](#第-6-章-验证体系)
- [第 7 章 如何新增一个 hw 模块](#第-7-章-如何新增一个-hw-模块)
- [第 8 章 坑位档案](#第-8-章-坑位档案)
- [附录 A 权威常数总表](#附录-a-权威常数总表)
- [附录 B 命令速查](#附录-b-命令速查)

---

## 第 0 章 家族全景

### 0.1 一句话

`hw_*` 是 xiaomo 的**硬件抽象层家族**：把「跟具体硬件打交道」的所有事情，从 VM 内核里隔离出来，做成可跨平台编译、可用真机 BSP 替换、行为逐位可回归的模块。

### 0.2 规模（实测）

| 项 | 数值 |
|---|---|
| 模块源码 | **13,208 行**（`wc -l src/hw/*.c`，不含 `.o`） |
| 模块文件 | 17 个 `.c` + 17 个 `.h`（`src/hw/` 与 `include/` 各一半） |
| 最大模块 | `hw_pin.c` 1,757 行（占家族 13.3%） |
| 挂注册表 | **8 类** |
| 有 VM opcode | **7 个**（`core`/`dev`/`fault`/`flash`/`pin`/`dc`/`dmc`/`wdbg` 中有 7 个含 `OP_HW_*_CALL`） |
| 有 BSP 注入 | **6 个**（`fault`/`flash`/`pin`/`dc`/`dmc`/`wdbg`） |

### 0.3 分层：家族不是扁平的

```
┌─ L3 设备身份 / 安全 ────────────────────────────────┐
│  hw_token  跨模式域令牌    hw_oem   熔丝写保护签名   │
│  hw_core   内核 DNA 编码                              │
├─ L2 协议层（帧 / 命令 / 状态机）──────────────────┤
│  hw_flash  ESP32 ROM 下载协议    hw_dmc  主从半双工  │
│  hw_dc     DC 电源信号 + DCPP    hw_dev  设备命令分发│
├─ L1 引脚 / 物理层 ────────────────────────────────┤
│  hw_pin    引脚档案 + 双模驱动 + 编程电压            │
│  hw_direct 底层直访（真 termios/spidev/sysfs）        │
│  hw_wdbg   无线调试器信号面                          │
├─ L0 工具 / 诊断 ─────────────────────────────────┤
│  hw_hex    十六进制      hw_fault  故障诊断          │
│  hw_asr    语音识别      hw_demo   演示             │
└─ 家族总线 ───────────────────────────────────────┘
                     hw_main（8 类注册表 + 三态探针）
```

> ⚠️ **这张分层图是文档作者的归纳，不是代码里的显式声明。** 代码里没有层级元数据，L0~L3 只存在于本文件的分类和 `docs/` 既有描述里。**不要把分层当约束用**——代码不阻止你把新模块放任何位置。

### 0.4 🔴 两个"家族数字"分属两个不同的东西

hw 相关代码里有**两个都会被叫作"家族数字"的值**，极易混淆：

| 值 | 归属 | 谁算的 | 怎么查 |
|---|---|---|---|
| `0xA57E74DF` | **xiaomo hw 家族**总线注册表 | `hw_main_checksum()` | `./xiaomo main sum` |
| `0x116FD00A` | **烧录器门面** `xfl_hw.c` 聚合指纹 | 门面运行时聚合 16 条探针 | `~/cow/xiaomo-flasher/xiaomo-flash hw` |

**`0x116FD00A` 不是编译期常量**——它在 `xfl_hw.c` 和 `include/*.h` 里 `grep` 零命中，因为它是**运行时把 16 条探针的实测值聚合出来的**。门面登记表见 `xiaomo-flasher/src/xfl_hw.c:64` 的 `g_mods[]`。

**"14 模块"的真实构成**（门面视角，非 xiaomo 家族视角）：

```
9 个 hw_main 注册模块（asr/core/dev/direct/fault/hex/oem/token/usbpd）
  + 6 个新模块及子表项（pin/pin.ISP/flash/dc/dc.proto/wdbg）
  = 14 条登记，16 条探针
```

其中 `hw_oem` 和 `infer` 标 **NOLINK**（刻意不编入，依赖 VM 内核），`hw_dev`/`hw_token`/`hw_hex`/`hw_direct`/`hw_asr` 标 **SKIP**（无 FNV 黄金值可探，**不假装探过**）。

**实跑复核**（2026-10-02）：

```
$ ~/cow/xiaomo-flasher/xiaomo-flash hw
  hw 全家桶 14 模块 · 模式 HOST · 指纹 0x116FD00A · 型号库 61 条 (0xE6CD7913)
  ...
  家族指纹 0x116FD00A   已检 10 项, 失败 0
```

> ⚠️ **本节记录一次真实的自我误判**。写本文时我曾断言 `0x116FD00A` "源码零命中 = 已废弃"，**这是错的**——正确结论是"它是运行时聚合值，静态 grep 本就查不到"。**教训：静态检索零命中只能证明"不是编译期常量"，不能证明"不存在"。** 判断一个值是否有效，必须找到**产出它的那次运行**，而不是只搜字面量。

---

## 第 1 章 家族公约（四条铁律）

这四条是全部 13 个模块共享的约定。改代码前先确认没有违反它们。

### 铁律 1 · 六模式编译

每个模块必须能在 **6 种模式**下编译通过，行为一致：

| 模式 | 宏 | 用途 |
|---|---|---|
| HOST | 宿主 Mac/Linux | 桌面开发、回归测试 |
| LINUX | 目标 Linux | 服务器部署 |
| KELL | 内核态 | Ring0 特权层 |
| ESP32 | 目标 ESP32 | 真机固件 |
| ESP8266 | 目标 ESP8266 | 轻量真机 |
| TEST | 测试桩 | 单元测试 |

判定标准：**同一份源码，六种模式各编一次，产物行为逐位一致。**

### 铁律 2 · BSP 注入（硬件可换，逻辑不动）

模块本体**不直接碰硬件**。真机固件通过 `hw_xxx_bsp_install()` 注入回调，模块逻辑零改动：

```c
void hw_dc_bsp_install(const hw_dc_bsp_t* bsp);  /* NULL = 卸载回模拟 */
```

这条是「一套逻辑，六处运行」的关键。**新增模块若绕过 BSP 直接调 `termios`/寄存器，就破坏了整个家族的移植性。**

> 🔥 **实测踩过的坑（hw_dc）**：`hw_dc_init()` 曾在初始化时**静默抹掉已装的 BSP**。表现是 HUD 显示模拟值、一切"正常"，但真机硬件绑定已丢失——**头号假成功陷阱**。已修为 init 只复位信号表不动 BSP，并在 selftest 加 `[11]` 回归护栏。
>
> **规律**：任何 `init`/`reset` 函数碰 BSP 指针，都必须有一条 selftest 断言守着。

### 铁律 3 · 黄金值锁死

每个模块有一个 `HW_XXX_GOLDEN` 宏 = 关键表（命令表 / 档案表 / 信号表）的 **FNV-1a-32** 校验和。改动表结构**必须**同步更新黄金值，且**必须**用独立实现（Python）重算对拍。

> 🔥 **黄金值必须挂在公共出口上**，不能只在 selftest 内部算。实测教训：某模块的出口做了 `& 0x7FFFFFFF` 掩码，selftest 内部没掩 → **selftest 全绿而 CLI 全错**。

### 铁律 4 · 六层接入

每个模块必须接满 6 层，缺一层就是死代码。详见[第 5 章](#第-5-章-六层接入检查表)。

---

## 第 2 章 模块总表

**实测数据**，按源码行数降序：

| # | 模块 | 行数 | 职责 | 类别 | opcode | BSP |
|---|---|---|---|---|---|---|
| 1 | `hw_pin` | **1,757** | 引脚档案 + 双模驱动 + 编程电压 | L1 | ✅ | ✅ |
| 2 | `hw_dmc` | 1,390 | 主从半双工字节协议 | L2 | ✅ | ✅ |
| 3 | `hw_dc` | 1,327 | DC 电源信号 + DCPP 供电协议 | L2 | ✅ | ✅ |
| 4 | `hw_flash` | 1,117 | ESP32 ROM 下载协议（烧录） | L2 | ✅ | ✅ |
| 5 | `hw_asr` | 1,113 | 双引擎语音识别（本地+云） | L0 | — | — |
| 6 | `hw_wdbg` | 1,058 | 无线调试器信号面 | L1 | ✅ | ✅ |
| 7 | `hw_direct` | 958 | 底层硬件直访（真 termios/spidev） | L1 | — | — |
| 8 | `hw_dmc_base` | 860 | DMC 帧编解码 + CRC | L2(内) | — | — |
| 9 | `hw_dmc_handshake` | 577 | DMC 握手/重传状态机 | L2(内) | — | — |
| 10 | `hw_main` | 548 | 家族总线 + 三态探针 | 总线 | — | — |
| 11 | `hw_fault` | 512 | TFT 模组故障诊断 | L0 | ✅ | ✅ |
| 12 | `hw_dev` | 481 | 设备命令分发 | L2 | ✅ | — |
| 13 | `hw_token` | 455 | 跨模式域令牌（10 域） | L3 | — | — |
| 14 | `hw_core` | 452 | 内核 DNA 编码 | L3 | ✅ | — |
| 15 | `hw_oem` | 234 | 熔丝写保护签名 | L3 | — | — |
| 16 | `hw_demo` | 166 | 演示（非功能模块） | L0 | — | — |
| 17 | `hw_dmc_porto` | 164 | 8051 移植遗留（归档） | 归档 | — | — |
| — | `hw_hex` | 39 | 十六进制工具 | L0 | — | — |
| | **合计** | **13,208** | | | | |

**关于 `hw_dmc_*` 三个内部件**：`base`（帧层）+ `handshake`（状态机）+ `porto`（8051 归档）合起来构成一个功能模块 `hw_dmc`。算模块数时是 **1 个**，算文件数时是 **3 个**。

---

## 第 3 章 公共基础：注册表 hw_main

`hw_main.c`（548 行）是**家族总线**：把 9 个早期模块挂进统一注册表，提供统一的健康探针和命令分发。

### 3.1 注册表实测内容

```
$ ./xiaomo main card
=== hw_main 类注册表 (hw 家族总调度) ===
count  : 8
mode   : HOST (0)
golden : 0xA57E74DF OK
  [0] ASR    cls=0x0058  probe=OK
  [1] CORE   cls=0x0098  probe=OK
  [2] DEV    cls=0x0089  probe=OK
  [3] DIRECT cls=0x01EF  probe=OK
  [4] FAULT  cls=0x07DE  probe=OK
  [5] HEX    cls=0x0DEF  probe=OK
  [6] OEM    cls=0x0FFD  probe=OK
  [7] TOKEN  cls=0x0EFD  probe=OK
```

**总线类 ID（用户定版，勿改）**：

| 序 | 类名 | cls ID | 探针函数 |
|---|---|---|---|
| 0 | `ASR` | `0x0058` | `main_probe_asr` |
| 1 | `CORE` | `0x0098` | `main_probe_core` |
| 2 | `DEV` | `0x0089` | `main_probe_dev` |
| 3 | `DIRECT` | `0x01EF` | `main_probe_direct` |
| 4 | `FAULT` | `0x07DE` | `main_probe_fault` |
| 5 | `HEX` | `0x0DEF` | `main_probe_hex` |
| 6 | `OEM` | `0x0FFD` | `main_probe_oem` |
| 7 | `TOKEN` | `0x0EFD` | `main_probe_token` |

### 3.2 🔴 关键发现：注册表只覆盖 8/13 个模块

实测：新 5 个模块（`flash`/`pin`/`dc`/`dmc`/`wdbg`）在 `hw_main.c` 里**零引用**。

```
$ for m in flash pin dc dmc wdbg; do grep -c "hw_$m" src/hw/hw_main.c; done
flash : 0
pin   : 0
dc    : 0
dmc   : 0
wdbg  : 0
```

**含义**：
- `hw_main count` 报 9，**不代表家族只有 9 个模块**，只代表挂了 9 个。
- 新模块走**自己的 CLI + 自己的 opcode**，不进注册表。这是**当前的设计现状，不是 bug**——但它意味着 `hw_main probeall` **测不到新 5 模块**。
- 想让全家族统一体检，得挨个跑 6 个模块的 selftest。

> 📌 **是否该把新 5 模块补进注册表，取决于你的目标**：
> - **补**的好处：`probeall` 一次看全家族健康；坏处：要动已定版的类 ID 表和黄金值 `0xA57E74DF`。
> - **不补**的现状：新模块各自 `card`/`selftest`，已能覆盖，只是没有统一入口。
>
> **本文只记录现状，不替你做这个决定。**

### 3.3 三态探针（家族最有价值的设计）

`hw_main_probe_one()` 返回三态，而不是简单的 成功/失败：

| 返回 | 含义 | 解读 |
|---|---|---|
| `1` | **OK** | 强符号存在，模块能跑 |
| `0` | **NOLINK** | 没链接进来（该模式下不编） |
| `-1` | **BAD** | 链进来了但坏了 |

为什么重要：宿主上 `nolink` 和 `bad` 都表现为"没输出"，但**含义完全相反**——一个是这平台不部署，一个是部署了却坏了。区分开才能在真机上判故障。

**实现**：纯 `extern` 弱声明探针。模块符号强链接进来则探到，强链接缺失则 NOLINK，探到但调用失败则 BAD。

### 3.4 家族黄金值

```
$ ./xiaomo main sum
0xA57E74DF
```

`HW_MAIN_GOLDEN = 0xA57E74DFu`，= 类表 FNV-1a-32。改动类表必须重算。

---

## 第 4 章 模块详解

以下按「职责 → 关键结构 → 实测输出 → 注意事项」组织。**实跑输出均为 2026-10-02 采集。**

---

### 4.1 `hw_pin` — 引脚档案层（1,757 行，家族最大）

**职责**：GPIO 级真实烧录器的地基。换目标芯片 = 换一条档案。

**四层结构**：

| 层 | 内容 |
|---|---|
| **L2** | `hw_pin_profile_t` — 9 信号（MOSI/MISO/CK/CS/TX/RX/RST/VPP/VCC）→ GPIO 映射，4 条档案 |
| **L1** | 双模驱动：`HW` 模式（GPIO Matrix 外设映射）/ `BB` 模式（纯 bit-bang，CPOL/CPHA 四模式） |
| **L0** | 电压层：VPP 5/12/12.5/21V + ADC 回读 |
| **L3** | 协议插件：25xx SPI Flash（0x9F/0x03/0x02/0x20/0x06/0x05）+ STM32 UART ISP（AN3155） |

**实跑**：

```
$ ./xiaomo pin card
pin: L3 plugins: flash(25xx SPI RDID=EF4018) + isp(AN3155 UART cmdsum=0xD2A9A924)
pin: profile=esp32-9p drv=BB spi_mode=0 spi_hz=1000000 baud=115200 vpp=0
pin: MOSI=IO23 MISO=IO19 CK=IO18 CS=IO5 TX=IO17 RX=IO16 GND=--
```

**黄金值**：`HW_PIN_GOLDEN = 0x9E0F10FA`（档案表）、`HW_PIN_ISP_GOLDEN = 0xD2A9A924`（ISP 命令表 11 条）。

**关键决策**：
- **BB 模式是保底**。实测发现 **ESP32-S3 的 GPIO Matrix 对 TX 出向不可靠**（详见坑位 #1），所以 S3 上做 SPI 烧录**第一刀必须用 IOMUX 专属脚**。
- 档案有 VPP ⇒ 定位是**通用编程器**（能烧 EPROM/OTP），不是单纯下载器。

**改它要注意**：
- 动 `hw_pin_profile_t` ⇒ 必须重算 `HW_PIN_GOLDEN`。
- 动 ISP 命令表 ⇒ 必须重算 `HW_PIN_ISP_GOLDEN`。
- `bb_spi_byte` 的 `use_dev=(g_bsp.gpio_read==NULL)` 是 **SIM/REAL 的结构性分界**，别改这个判定式。

---

### 4.2 `hw_flash` — ESP32 ROM 下载协议（1,117 行）

**职责**：esptool 的 C 实现。SLIP RFC1055 帧 + 9 命令 + MD5 校验。

**实跑**：

```
$ ./xiaomo flash card
  0x13  FLASH_MD5
  0x14  SEC_INFO
  cmds : card|mode|slip|md5|sync|chip|run N|verify|stat|selftest
```

**黄金值**：`HW_FLASH_GOLDEN = 0xAF05978A`（ROM 命令表 FNV-1a-32）。

**关键点**：
- S3 ROM 的 `FLASH_DATA` 块 = **1KB**。
- ROM 模式**没有 `FLASH_END`**，MD5 就是最后一条命令。
- `GET_SECURITY_INFO` 应答 20 字节，`chip_id=9` 才是 S3。

---

### 4.3 `hw_dc` — DC 电源信号 + DCPP 协议（1,327 行）

**职责**：两层。信号层（8 路 IN/OUT/MAX/MIN/HILITE/LOLITE）+ 协议层（DCPP 供电握手帧）。

**DCPP 帧格式**：

```
START(80 EF 02) | VER | SIDE | CMD | LEN | PAYLOAD | CKSUM8 | END(ED FF 0D)
9 命令：PING / COUNT / GET / SET / BASE / RANGE / DATA / OPEN / CLOSE
会话语义：OPEN..CLOSE 门禁，未知命令 ERR_CMD 先于会话判断
```

**黄金值**：`HW_DC_GOLDEN = 0x028ECE6`（信号层）、`HW_DCPP_GOLDEN = 0x9432D6A5`（协议层）。

**实跑**：`./xiaomo dc card` / `dc proto card` / `dc proto loop`

---

### 4.4 `hw_dmc` — 主从半双工字节协议（1,390 行）

**职责**：主从链路。帧 `AA55|LEN|CMD|PAYLOAD|CRC16(小端)`，9 命令 + 7 状态。

**由三份历史件整合而成**：

| 件 | 行数 | 状态 |
|---|---|---|
| `hw_dmc_base.c` | 860 | 帧编解码 + CRC-16/CCITT-FALSE（向量 `0x29B1`） |
| `hw_dmc_handshake.c` | 577 | 握手/超时/序号重传状态机 |
| `hw_dmc_porto.c` | 164 | 8051 移植遗留，**算法已吸走，本体归档** |

> ⚠️ **整合前三份文件没有一份能编译**。`porto` 是唯一能跑但栈越界的，`handshake` 逻辑完整但 CRC 与 `porto` 不兼容 + 裸内存映射 SIGSEGV。

**黄金值**：`HW_DMC_GOLDEN = 0x169A603E`、帧 FNV `0xB3DFA0D8`。

**🔴 上界缺陷（2026-10-01 宿主镜像刷板前抓获）**：
初版 `MAX_PAYLOAD=252 → FRAME=258`，而 LEN 只有 1 字节 ⇒ `(uint8_t)258` **静默回绕成 2** ⇒ 载荷 ≥250 的帧结构性无法解出。
根因是 selftest `[13]` 只断言 pack 返回长度、**从不 round-trip**，完全遮住。
修：`252→249`、`258→255` + 护栏 `total>255→ERR_PARAM` + `[13]` 改真 round-trip（黄金值不变）。

---

### 4.5 `hw_wdbg` — 无线调试器信号面（1,058 行）

**职责**：复刻立创开源「AI 远程调试器」。串口桥接 / PWM 双沿 / SPI·I2C 事务环 / 四协议线序可改。

**实跑**：

```
$ ./xiaomo wdbg card
  stat -> [status: HOST baud=115200 tx=2 rx=6 spill=0 spi=1 i2c=1 pwm=1000Hz/250 ev=11]
```

**黄金值**：`HW_WDBG_GOLDEN = 0xCD91F641`（默认线序表 FNV-1a-32）。

**规模**：SPI/I2C 事务环 **50 条 × 64 字节**；波特率 4 档白名单；Mode 0~3 / 7bit 地址。

---

### 4.6 `hw_asr` — 双引擎语音识别（1,113 行）

**职责**：本地离线（纯 C：WAV→MFCC→DTW，零依赖）+ 线上（OpenAI 兼容转写，curl/socket 双通道）。

**注意**：无 opcode、无 BSP。属功能模块而非硬件抽象层。

---

### 4.7 `hw_direct` — 底层硬件直访（958 行）

**职责**：真实 `termios`（第 821 行起）+ `spidev` + `sysfs GPIO`。

**与 `hw_flash` 的关系**：`hw_flash.c` 里 `grep termios` **零命中**——真 termios 一直在这里。
`hw_flash_run` 走的是 `fl_tx`/`fl_rx` **BSP 包装**，所以装了真 BSP 就全家族生效。

---

### 4.8 `hw_fault` — 故障诊断（512 行）

**职责**：TFT 模组故障码注入与诊断。

**CLI**：`hwfault card/scan/pin N/inject N CODE/clear/mode/selftest`

**已修的历史 bug**：selftest 期间安装的 BSP 未在结尾恢复，导致宿主 39/39 通过但**真机 13 fails**。根因是数学验证出来的：`g_inject` 注入表只影响模拟器 BSP 路径，真机 BSP 直读真实 GPIO 对它不可见 ⇒ `[5]×10 + [6]×1 + [7]×2 = 恰 13`。修法是 selftest 开头保存 + 结尾卸载 BSP。

---

### 4.9 `hw_dev` / `hw_core` / `hw_token` / `hw_oem` / `hw_hex` / `hw_demo`

| 模块 | 职责 | 要点 |
|---|---|---|
| `hw_dev` | 设备命令分发 | 8 命令 `\n\r ` 尾缀前缀分发 + 动态注册 `dev_bin` |
| `hw_core` | 内核 DNA 编码 | 四基类 × 10 项 DNA 表；黄金 `0xA5618C4A` |
| `hw_token` | 跨模式域令牌 | 10 域 × 6 模式；域盐 `XIAOMO01` × 黄金比率 → fmix64 → XOR 0x5A |
| `hw_oem` | 熔丝写保护 | 签名 `0x5849414F4D4F3031`("XIAOMO01") → R127 只读 |
| `hw_hex` | 十六进制工具 | 39 行，被 `hw_main` 探针引用 |
| `hw_demo` | 演示 | 非功能模块 |

---

## 第 5 章 六层接入检查表

**新增模块时逐项打勾。漏任一层 = 死代码。**

| # | 层 | 改什么 | 验证方式 |
|---|---|---|---|
| 1 | **Makefile** | `HW_SRCS` 加 `src/hw/hw_xxx.c` | `make test` 不报缺符号 |
| 2 | **VM opcode** | `vm_core.h` 加 `OP_HW_XXX_CALL`（**追加到枚举尾**） | 反汇编 `kvm` 能看到新指令 |
| 3 | **mo2kbc** | `src/mo2kbc.c` 内置函数分派 | `.mo` 里能直接调 `hw_xxx("...")` |
| 4 | **CLI** | `main.c` 加子命令 | `./xiaomo xxx card` 有输出 |
| 5 | **示例** | `examples/xxx_test.mo` | `make test` 跑通 |
| 6 | **测试** | `tests/` 加回归块 | 回归数 +N |

### 5.1 实测接入状态

| 模块 | 1 Makefile | 2 opcode | 3 mo2kbc | 4 CLI | 5 示例 | 6 测试 |
|---|---|---|---|---|---|---|
| `asr` | ✅ | — | ✅ | ✅ | — | — |
| `core` | ✅ | ✅ | ✅ | ✅ | 20 处 | 5 处 |
| `dev` | ✅ | ✅ | ✅ | ✅ | 13 处 | 7 处 |
| `direct` | ✅ | — | ✅ | ✅ | — | — |
| `fault` | ✅ | ✅ | ✅ | ✅ | 15 处 | 5 处 |
| `hex` | ✅ | — | ✅ | ✅ | — | — |
| `oem` | ✅ | — | ✅ | ✅ | — | 1 处 |
| `token` | ✅ | — | ✅ | ✅ | — | — |
| `flash` | ✅ | ✅ | ✅ | ✅ | 18 处 | 7 处 |
| `pin` | ✅ | ✅ | ✅ | ✅ | 22 处 | 6 处 |
| `dc` | ✅ | ✅ | ✅ | ✅ | 49 处 | 10 处 |
| `dmc` | ✅ | ✅ | ✅ | ✅ | 23 处 | 6 处 |
| `wdbg` | ✅ | ✅ | ✅ | ✅ | 37 处 | 8 处 |

> `asr`/`hex`/`token`/`oem` 无 opcode 是**有意为之**：它们不通过 VM 调用，靠 CLI 和 C API 入口。`oem` 有 1 处测试。

### 5.2 接入时的三个高频坑

1. **枚举必须追加到尾部**。插在中间会改变已有 opcode 编号 ⇒ 所有已编译的 `.kbc` 全部失效。
2. **双参编码**。`hw_xxx("fmt", 数值)` 要把数值先求值进 `R_TMP`，用 `imm=R_TMP+1` 编码动态拼接。只取 `argv[2]` 会让 `load w25q` 截成 `load` 后静默 exit 1。
3. **示例对拍循环要进 SKIP 清单**。`.mo` 的数组声明不能带尺寸、字符串不转义，扫描 examples 时会误报。

---

## 第 6 章 验证体系

### 6.1 三级验证

```
L1  宿主回归        make test                    ← 每次改动必跑
L2  跨模式矩阵      六模式各编一次 + 产物 md5 对拍  ← 改模块本体必跑
L3  真机 PASS       Phase A 确定性 + Phase B 真硅  ← 动 BSP/时序必跑
```

**L1 挡不住什么**（实测教训）：宿主 8MB 栈掩盖栈帧问题（`hw_flash` selftest 16.5KB 链在真机 IDLE 栈溢出）；C11 分支从无编译器校验（`#else` 死代码）；桩模型归属错误时其它用例照过。

### 6.2 黄金值对拍纪律

**独立实现**重算，不能用模块自己的函数算——自己算自己永远一致。

```bash
# 家族黄金
./xiaomo main sum          # 应 0xA57E74DF
# 各模块黄金
./xiaomo dmc golden
./xiaomo flash md5
./xiaomo pin profiles
```

**改表 → 重算 → 对拍 → 才算通过**。跳过对拍等于没验证。

### 6.3 现有工具

| 工具 | 用途 |
|---|---|
| `tools/dmc_matrix.sh` | DMC 六模式验证矩阵 |
| `tools/dmc_verbatim_check.py` | DMC 三份归档件逐字节对拍 |
| `tools/dmc_golden.txt` | DMC 黄金值归档 |
| `scripts/xdbg_board.py` | 真机串口调试（四命令） |

### 6.4 真机工程对照

| 工程 | 状态 |
|---|---|
| `~/cow/esp32-s3-hw-flash-test/` | ✅ PASS |
| `~/cow/esp32-s3-hw-dc-test/` | ✅ PASS（含 DCPP 帧层） |
| `~/cow/esp32-s3-hw-wdbg-test/` | ✅ PASS |
| `~/cow/esp32-s3-hw-pin-test/` | ⚠️ Phase C 未收口（见坑位 #1） |
| `~/cow/esp32-s3-lvgl-ui/` | 共享板，测完必须回烧产品固件 |

> ⚠️ **共享板铁律**：`esp32-s3-lvgl-ui` 是唯一带屏固件。每轮测试固件收尾**必须回烧产品版三段**（`0x0`/`0x8000`/`0x10000`），NVS `0x9000` 不动以保凭据。

---

## 第 7 章 如何新增一个 hw 模块

### 7.1 骨架

> ⚠️ 下面的 `hw_xxx` / `xxx_test.mo` 是**占位模板名，不是磁盘上真实存在的文件**。新增模块时把它换成你的模块名。

```
src/hw/hw_xxx.c          模块本体
include/hw_xxx.h         公开 API
```

头文件必须写清楚四件事（照抄任一现有模块的结构）：

1. **职责与分层位置**
2. **BSP 注入方式**（有硬件依赖就必须有）
3. **黄金值说明**（哪张表的 FNV-1a-32）
4. **六模式说明**

### 7.2 步骤

**Step 1 · 写本体**

```c
/* 铁律 2：BSP 注入，模块本体不碰硬件 */
typedef struct {
    int (*read)(int ch, void* buf, int n);   /* 你的硬件读 */
    int (*write)(const void* buf, int n);    /* 你的硬件写 */
} hw_xxx_bsp_t;

void hw_xxx_bsp_install(const hw_xxx_bsp_t* bsp);  /* NULL = 卸载回模拟 */

/* 铁律 1：六模式 */
#if defined(HW_ESP32) || defined(HW_ESP8266)
    /* 真机路径：走 bsp 回调 */
#elif defined(HW_HOST) || defined(HW_LINUX)
    /* 宿主路径：走桩 */
#endif
```

**Step 2 · 定黄金值**

```c
#define HW_XXX_GOLDEN   0x........u   /* 表 FNV-1a-32 */
uint32_t hw_xxx_checksum(void);      /* 必须挂在公共出口上 */
```

用**独立 Python** 重算这个值，两边对拍。

**Step 3 · 接六层**（照[第 5 章](#第-5-章-六层接入检查表)表逐项）

**Step 4 · 跑验证**

```bash
make clean && make test           # L1
./xiaomo xxx card                 # 看输出对不对
./xiaomo xxx selftest             # fails=0
./xiaomo xxx golden               # == HW_XXX_GOLDEN
```

**Step 5 · 考虑要不要进注册表**

见 [3.2](#32--关键发现注册表只覆盖-813-个模块)。要进的话：加类 ID（**追加到尾部**）+ 加探针 + 重算 `HW_MAIN_GOLDEN`。

### 7.3 新模块自检清单

```
□ 本体 BSP 指针在 init/reset 时不被清（否则重蹈 hw_dc 覆辙）
□ 黄金值挂在公共出口，不只在 selftest 内部
□ selftest 有"负向用例"——坏数据必须真的报错
□ 负向用例取"首个"负返回码并立即 break（状态机复位会覆盖真因）
□ opcode 追加到枚举尾
□ 双参 imm 编码正确
□ 示例进对拍循环的 SKIP 清单
□ 六模式各编一次零警告
□ 改表后黄金值用独立实现重算
```

---

## 第 8 章 坑位档案

按"坑住我的严重程度"排序。每条都是**实测**，不是推测。

### 坑 #1 🔴 ESP32-S3 的 GPIO Matrix 对 TX 出向不可靠

**现象**：UART TX 走 matrix 脚无效。IO17 → 1974/7000（有波形），IO9 → 0/7000（完全没信号）。
**A/B 设计**：同芯片、同 UART1、同 64B 突发、同样采样函数，**唯一变量是引脚**。
**源码硬证据**：`U2TXD_GPIO_NUM = (-1)`，S3 的 `gpio_struct.h` 无经典 `out_sig_map[]`。

**影响范围**：🔴 **SPI 是否同样失效尚未测试**——底层是同一个 `gpio_matrix_out`，列为高风险待验，**不得当结论**。

**结论**：S3 做 SPI 烧录，**第一刀用 IOMUX 专属脚**；BB 模式不受影响。

**教训**：「S3 GPIO Matrix 可任意路由 = 一等公民红利」这个假设是错的，而且**错得很贵**——它支撑了一整套架构决策。**平台能力要实测，不能从 ESP32 的经验外推到 S3。**

### 坑 #2 🔴 上界回绕（`hw_dmc`）

`MAX_PAYLOAD=252 → FRAME=258`，LEN 只有 1 字节 ⇒ `(uint8_t)258` 静默回绕成 `2`。
**为什么没被测出来**：selftest 只断言 pack 返回长度，**从不 round-trip**。
**修**：改 249/255 + 加护栏 + selftest 改真 round-trip。

> **通用教训**：**只测"能不能生成"不测"能不能解回来"，上界类 bug 100% 漏过。**

### 坑 #3 🔴 `init` 静默抹掉 BSP（`hw_dc`）

`hw_dc_init()` 抹掉已装 BSP ⇒ HUD 显示模拟值、一切"正常"，真机绑定已丢。
**修**：init 只复位信号表 + selftest `[11]` 回归护栏。

### 坑 #4 🔴 桩模型归属必须逐槽位打标

本端发和对端来的字节在**同一个环形缓冲里交错**。区间/上界/计数断言全部失效。
前两版都能编译、能跑、其它用例全过——**必须专门补交错顺序用例**。

### 坑 #5 🟡 黄金值打错位置

出口掩 `0x7FFFFFFF`、selftest 内部没掩 ⇒ **selftest 全绿而 CLI 全错**。

### 坑 #6 🟡 交叉编译器上 `uint32_t == long unsigned int`

`printf("%08X")` 触发 `-Wformat`。**宿主 clang 不报，只有交叉编译暴露。**
修：显式 `(unsigned)`。

### 坑 #7 🟡 栈帧炸弹（宿主永远测不出）

`hw_flash` selftest 16.5KB + run 6.3KB + 链 ≈ **25KB** → 真机 IDLE 栈溢出复位。
**宿主 8MB 栈完全掩盖**。修：5 处改 `static` + diag 栈 8192→16384。
用 `xtensa -fstack-usage` 实测量化 25KB → ≈1.0KB。

### 坑 #8 🟡 哨兵注释丢失而矩阵全绿

`hw_dmc_base.c` 归档区段标记（5/106·108/689·691/859）丢失，`check` FAIL 而**矩阵 15/0 全绿**。
**机理**：丢的只是标记行，预处理嵌套始终平衡（`#if`=16/`#endif`=16 净 0）⇒ 代码层完好 ⇒ 编译/行为矩阵**天然隐形**，只有完整性检查器看得见。

> **通用教训**：**行为测试测不出"注释/标记丢了"这类问题**，必须配独立的结构完整性检查器。

### 坑 #9 🟡 `init` 期间安装的 BSP 未恢复（`hw_fault`）

宿主 39/39 通过，真机 **13 fails**。数学验证：`[5]×10 + [6]×1 + [7]×2 = 恰 13`（注入表只影响模拟器 BSP，真机直读真实 GPIO 对它不可见）。
修：selftest 开头保存 BSP + 结尾卸载恢复。

### 坑 #10 🟡 CRC 向量不一致

`handshake` 用 `0x8408`、`porto` 用另一套。合并时统一为 **CRC-16/CCITT-FALSE（向量 `0x29B1`）**。
（另：把 CRC 叫 "MODBUS" 是撒谎——MODBUS 向量是 `0x4B37`。）

### 坑 #11 ⚪ `w25q` 加载被截断

CLI 只取 `argv[2]` ⇒ `load w25q` 截成 `load` 后**静默 exit 1**。家族坑 #2 的变体。

### 坑 #12 ⚪ 弱内部上下拉在 ADC 脚上不生效

三态读数 25/24/25mV 近乎相同。**强线索：唯一区别是"该脚是否被配置过"。**
⇒ 「本脚自驱动自测 ADC」结构上不可能，必须外部激励。
（早期 ≈400mV 的"分离"是采样噪声假阳性，靠对照组当场抓获。）

### 坑 #13 ⚪ 家族级数字可能不存在于源码

见 [0.4](#04--两个家族数字分属两个不同的东西)。

### 坑 #14 🔴 静态 grep 零命中 ≠ 不存在

**本轮真实误判**：断言 `0x116FD00A` "源码零命中 = 已废弃"，**实际它完全有效**。

**为什么错**：`0x116FD00A` 是烧录器门面在**运行时聚合 16 条探针实测值**算出来的，不是编译期常量。`grep` 静态搜字面量，**结构上永远搜不到**。

**正确判别法**：
- 判"常量是否存在" → 静态 `grep`
- 判"值是否有效" → **必须找到产出它的那次运行**

```
❌ grep -rn "0x116FD00A" src/ include/     → 零命中，什么都证明不了
✅ xiaomo-flash hw                          → 指纹 0x116FD00A 已检 10 项, 失败 0
```

> **归属教训**：我把这个数**记成了 xiaomo 家族指纹**，其实它属于 `xiaomo-flasher` 门面。**一个数字对，但归属错，照样会导致错误决策**（比如据此以为 xiaomo 仓库该有这个常量而去"修"）。
>
> **纪律**：引用任何聚合/运行时数值时，**连同它的产出命令一起记录**。

---

## 附录 A 权威常数总表

> 下列「定义位置」列可直接跳转核对。**注意：`HW_MAIN_GOLDEN` 和 `HW_DC_GOLDEN` 定义在 `.c` 里而非 `.h`**——`grep include/*.h` 查不到它们。

| 宏 | 值 | 含义 | 定义位置 | 实跑复核 |
|---|---|---|---|---|
| `HW_MAIN_GOLDEN` | `0xA57E74DF` | 家族类表 FNV-1a-32 | `src/hw/hw_main.c:34` | `main sum` |
| `HW_FLASH_GOLDEN` | `0xAF05978A` | ROM 命令表 FNV-1a-32 | `include/hw_flash.h:104` | `flash md5` |
| `HW_PIN_GOLDEN` | `0x9E0F10FA` | 引脚档案表 FNV-1a-32 | `include/hw_pin.h:180` | `pin card` |
| `HW_PIN_ISP_GOLDEN` | `0xD2A9A924` | ISP 命令表（11 条）FNV-1a-32 | `include/hw_pin.h:181` | `pin card` |
| `HW_DC_GOLDEN` | `0x028EECE6` | DC 信号层 FNV-1a-32 | ⚠️ `src/hw/hw_dc.c:30` | `dc sig 0` |
| `HW_DCPP_GOLDEN` | `0x9432D6A5` | DCPP 协议层 FNV-1a-32 | `include/hw_dc.h:143` | `dc proto card` |
| `HW_DMC_GOLDEN` | `0x169A603E` | DMC FNV-1a-32 | `include/hw_dmc.h:287` | `dmc golden` |
| （DMC 帧） | `0xB3DFA0D8` | 帧 FNV-1a-32 | `include/hw_dmc.h` | `dmc frame` |
| `HW_WDBG_GOLDEN` | `0xCD91F641` | 默认线序表 FNV-1a-32 | `include/hw_wdbg.h:80` | `wdbg card` |
| `HW_CORE_GOLDEN` | `0xA5618C4A` | DNA 表 FNV-1a-32 | `include/hw_core.h` | `core card` |
| （OEM 签名） | `0x5849414F4D4F3031` | "XIAOMO01" → R127 | `src/hw/hw_oem.c` | `xiaomo sig` |
| （CRC 向量） | `0x29B1` | CRC-16/CCITT-FALSE | `src/hw/hw_dmc_base.c` | `dmc crcvec` |
| `HW_FAULT_*_GOLDEN` | `0x6AFD1876` | 故障注入表 FNV-1a-32 | 门面 `XFL_G_FAULT` | `xiaomo-flash hw -v` |

**门面聚合指纹**（**非编译期常量，静态 grep 查不到**）：

| 值 | 含义 | 产出者 |
|---|---|---|
| `0x116FD00A` | 烧录器门面 16 条探针聚合 | `~/cow/xiaomo-flasher/xiaomo-flash hw` |
| `0xE6CD7913` | 芯片型号库 61 条 | 同上 |

**本次实跑复核结果**（2026-10-02）：
```
$ ./xiaomo main sum        → 0xA57E74DF          ✅
$ ./xiaomo dmc golden      → golden=0x169A603E expect=0x169A603E OK  ✅
$ ./xiaomo dc sig 0        → sig cksum = 0x028EECE6 (golden OK)      ✅
$ xiaomo-flash hw -v       → 家族指纹 0x116FD00A 已检 10 项, 失败 0    ✅
```

**总线类 ID**：`ASR 0x0058` / `CORE 0x0098` / `DEV 0x0089` / `DIRECT 0x01EF` / `FAULT 0x07DE` / `HEX 0x0DEF` / `OEM 0x0FFD` / `TOKEN 0x0EFD`

---

## 附录 B 命令速查

```bash
# 家族总线
./xiaomo main card          # 注册表卡片
./xiaomo main selftest      # 家族自检
./xiaomo main sum           # 家族黄金 → 0xA57E74DF
./xiaomo main probeall      # 全注册表探针
./xiaomo main find 152      # cls → 表序

# 各模块
./xiaomo pin card | profiles | load <名> | id | wtest | vpp <档> | selftest
./xiaomo pin isp info | erase | read | go | wtest | chk
./xiaomo flash card | mode | slip | md5 | sync | chip | run <N> | verify | stat
./xiaomo dc card | sig N | set N V | base N | range N | data N
./xiaomo dc proto card | proto loop
./xiaomo dmc count | cmds | states | errs | golden | frame | crcvec | hello
./xiaomo wdbg card | selftest
./xiaomo hwfault card | scan | pin N | inject N CODE | clear
./xiaomo core dna | idx N | slot N CODE | free N | slots
./xiaomo token | asr | hwdev | sig | sigprobe
```

> **CLI 输出陷阱**：`./xiaomo dmc card` → `unknown cmd 'card'`（dmc 用 `count` 不是 `card`）。**先跑 `--help` 看子命令，别照抄别处的命令名。**

---

## 文档维护

- **改模块行数/结构** → 更新[第 2 章](#第-2-章-模块总表)
- **改类表** → 更新[附录 A](#附录-a-权威常数总表) + 重算 `HW_MAIN_GOLDEN`
- **踩新坑** → 追加[第 8 章](#第-8-章-坑位档案)，**必须标注是实测还是推测**
- **发现本文与源码不符** → **以源码为准**，并修正本文

> **本文档的最后一条纪律**：本文所有结论都应能通过「打开对应文件/跑对应命令」复核。**发现任何无法复核的陈述，视为缺陷，删掉或改成可验证的写法。**
