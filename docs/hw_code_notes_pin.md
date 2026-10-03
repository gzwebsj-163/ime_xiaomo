# hw 全家桶 · 逐段代码解析

> **这份文档是什么**：`hw_family_guide.md` 是模块级摘要（每模块约 30 行），
> 本文件是**逐段代码级解析**——按文件的物理段落拆开，每段给出
> 「它在干什么 / 关键数据结构 / 为什么这么写 / 改它要注意什么」。
>
> **诚实声明**：本文件的每一段都来自**实际读取源码**，行号可跳转核对。
> 未读过的段落不写。写完一段即在末尾标注覆盖区间。
>
> **铁律复述**：不要凭印象写解析。**「我以为这段代码在干什么」是本文件头号敌人。**

---

## 阅读方式

| 段落标题含义 | 例子 |
|---|---|
| `### 3. §行号区间` | 直接跳到该行号读原码 |
| 「**在干什么**」 | 一句话职责 |
| 「**关键点**」 | 数据结构 / 状态机 / 边界判定 |
| 「⚠️ 注意」 | 改这段会踩的坑（多数来自源码注释里的实战教训） |

---

# 第一部分 `hw_pin.c`（1,757 行）+ `hw_pin.h`（271 行）

**模块定位**：GPIO 级真实烧录器的地基。**换目标芯片 = 换一条档案。**

四层结构（源码自述，来自 `hw_pin.c:6-11`）：

| 层 | 职责 | 代码位置 |
|---|---|---|
| **L2** 档案层 | 9 个逻辑信号 → 物理 GPIO 的映射表 | §2 档案表 |
| **L1** 驱动层 | 双模：HW（外设映射）/ BB（纯 bit-bang） | §5 驱动层 |
| **L0** 电压层 | VPP 5/12/12.5/21V + ADC 回读 | §6 电压层 |
| **L3** 协议插件 | #1 = 25xx SPI Flash；#2 = STM32 UART ISP(AN3155) | §7 / §7b |

---

## 一、头文件 `hw_pin.h`（271 行）

### 1.1 §1-44 文件头：设计契约

这一段不是废话，是**模块的对外承诺**，逐条对应后面代码里的实现：

| 注释承诺 | 对应实现 |
|---|---|
| 「核心只依赖 stdint+string」 | 真的只 include 了 `<string.h>` `<stdio.h>` `<stdlib.h>`（`hw_pin.c:29-32`） |
| 「引脚读写一律走 BSP 回调注入」 | `g_bsp` 结构体 9 个函数指针，全空 = 模拟器 |
| 「默认 = 确定性模拟」 | §3 两大器件模型（W25Q + AN3155） |
| 「六层接入」 | Makefile / OP_HW_PIN_CALL / mo2kbc / CLI / examples / tests |

**引脚定版（§23-31）** 值得单独记：9 信号 = SPI 四线（MOSI/MISO/CK/CS）+ UART 两线（TX/RX）+ RST + VPP + VCC，GND 固定不参与映射。
**关键设计决策（§30）**：SPI 四线与 UART 两线**物理独立不共用 → 可并行工作**。
这条直接决定了「档案是稀疏的」——`w25q` 档案只填 4 个信号，`mcu-isp` 档案只填 3 个，其余填 `-1`。

### 1.2 §46-55 编译期模式枚举

`HW_PIN_MODE_HOST/LINUX/KELL/ESP32/ESP8266/TEST` 六模式。**注意 `HW_PIN_MODE_MAX = 6` 是数组长度哨兵**，不是合法模式。

### 1.3 §57-69 逻辑信号枚举

```c
HW_PIN_MOSI=0, MISO, CK, CS, TX, RX, RST, VPP, VCC, HW_PIN_SIG_MAX  /* =9 */
```
**这个顺序是全模块的 ABI**：`hw_pin_profile_t.gpio[9]` 的下标、`pin_sig_names[9]` 数组、checksum 的哈希顺序**三者必须一致**。改动枚举 = 三个地方同时改，且必须重算黄金值。

### 1.4 §71-81 驱动模式与 VPP 档位

```c
#define HW_PIN_DRV_HW 0   /* 外设映射 */
#define HW_PIN_DRV_BB 1   /* bit-bang */
```
VPP 五档：`OFF/5V/12V/12V5/21V`，`HW_PIN_VPP_MAX=5`。

**⚠️ 注意**：`g_vpp_mv[HW_PIN_VPP_MAX]`（`hw_pin.c:737`）是 `[0, 5000, 12000, 12500, 21000]`——**下标 0 对应 OFF 档的 0mV**。档位到 mV 的映射是**平行数组**，不是算出来的。

### 1.5 §83-95 返回码

12 个码，`0x00 = OK`，`0x04 = NOFLASH`（目标无应答/JEDEC 非法），`0x09 = NOTGT`（握手超时）。
**⚠️ 注意**：`NOCMD = -1` 和 `HELP = -2` 是**负数**，与业务码空间分离。CLI 入口靠这个区分「业务失败」和「命令不存在」。

### 1.6 §97-106 核心结构 `hw_pin_profile_t`

```c
typedef struct {
    const char* name;                    /* 档案名 */
    int16_t     gpio[HW_PIN_SIG_MAX];    /* 信号 → GPIO，-1 = 未映射 */
    uint32_t    uart_baud, spi_hz;
    uint8_t     spi_mode, drv, vpp_level;
} hw_pin_profile_t;
```

**这是整个模块的灵魂**：换芯片 = 换这个结构体的一个实例，协议代码一行不动（与 `panel_lcd`「换屏 = 换一条档案」同源设计）。

**⚠️ 注意 `int16_t` 而非 `int`**：为了让 checksum 里 `& 0xFFFF` 的两字节哈希有确定语义（`hw_pin.c:153`）。改成 `int` 会改变黄金值。

### 1.7 §108-124 状态快照 `hw_pin_stat_t`

13 个计数器 + 最近值。分两组：SPI Flash 组（`spi_bytes/bb_toggles/vex/flash_id_ok/flash_jedec`）和 ISP 组（`isp_rx/isp_tx/isp_synced/isp_pid/isp_ver`）。
**用途**：`pin stat` 命令的输出源。**这些计数器是「有没有真碰硬件」的间接证据**——`bb_toggles` 在真机和模拟器都会涨（都是真 GPIO 翻转），但 `flash_jedec` 只有真机才有意义。

### 1.8 §126-141 BSP 接口 `hw_pin_bsp_t`

```c
typedef struct {
    int (*gpio_dir)(int pin, int output);
    int (*gpio_write)(int pin, int level);      /* 0=成功 */
    int (*gpio_read)(int pin);                  /* 返回 0/1，<0 失败 */
    int (*delay_us)(uint32_t us);
    int (*spi_xfer)(const hw_pin_profile_t* p, const uint8_t* tx, uint8_t* rx, uint32_t n);
    int (*uart_putc)(int ch);                   /* 0=成功 */
    int (*uart_getc)(void);                     /* 0..255，<0 失败 */
    int (*vpp_set)(int level, uint32_t mv);
    int (*vpp_read)(uint32_t* mv);
} hw_pin_bsp_t;
```

**这是「全跨式」的关键设计**：9 个回调**可以部分注入**（`§130` 明确说「任意回调可置 NULL → 该动作回落模拟器」）。
真机固件只注入 `gpio_*` 就自动走真 SPI bit-bang；只注入 `spi_xfer` 就自动走硬件外设模式。

**⚠️ 陷阱**：`gpio_dir` 存在但**在宿主侧从未被调用**（知识页已记录）——方向必须由 BSP 自己配，模块不管。

### 1.9 §143-191 协议常量

**25xx SPI Flash 六命令**：`WREN 0x06 / RDSR 0x05 / READ 0x03 / PP 0x02 / SE 0x20 / RDID 0x9F`。

**STM32 AN3155 九命令 + 两个扩展**：
```
GET 0x00  GVR 0x01  GID 0x02  RM 0x11  GO 0x21
WM 0x31  ER 0x43  WP 0x63  WRU 0x73
```
**⚠️ 源码里明确标注了一个协议事实**（`hw_pin.h:163-165`）：
> `0x44 扩展擦除在 STM32F1 不存在，只有 0x43 标准页擦除。`

**模拟目标参数**：`BOOTVER 0x31`（v3.1）、`PID 0x0410`（STM32F103x8/B）、`MEM_CAP 8192`（8 页 × 1KB）、`PAGE 1024`。

**关键黄金值**（`hw_pin.h:180-184`）：
```c
#define HW_PIN_GOLDEN        0x9E0F10FA  /* 档案表 FNV-1a-32 */
#define HW_PIN_ISP_GOLDEN    0xD2A9A924  /* ISP 11 条命令表 FNV */
#define HW_PIN_ISP_FLASH_BASE 0x08000000 /* STM32 Flash 起点 */
#define HW_PIN_FLASH_JEDEC   0x001840EF  /* W25Q128: 9F -> EF 40 18 */
#define HW_PIN_VPP_MODEL_MV  5000         /* 模拟器固定 5V */
```

**⚠️ 注意 `HW_PIN_FLASH_JEDEC = 0x001840EF`**：这个字面量的**低字节在前**（EF 是厂商码），即 `0x...EF` 表示「EF 在最低位」。`hw_pin.c:242` 取 `& 0xFF` 得 `0xEF` = 厂商码，读起来是自然的，但**字面量本身是反的**——改这个值时极易搞反。

### 1.10 §193-258 API 清单

分五组：L2 档案层 / L1 驱动层 / L0 电压层 / L3 插件 ×2 / 生命周期 + 命令分发 + 自检 + CLI。

**⚠️ 生命周期注释**（`hw_pin.h:254`）：「`kvm_run` 上电自动调 `init`」。这一句是后面 T18 那个测试存在的全部理由。

---

## 二、`hw_pin.c` §1-3 编译期探测与模式字符串（1-100 行）

### 2.1 §34-63 `pin_mode_probe()` / `hw_pin_mode()`

**探测优先级**（`#if` 链的顺序就是优先级，从上到下）：
```
1. HW_PIN_MODE_OVERRIDE 宏（编译期强指，家族通用测试手段）
2. HW_PIN_KELL（内核嵌入）
3. CONFIG_IDF_TARGET_ESP8266 / __ESP8266__
4. CONFIG_IDF_TARGET_ESP32/S2/S3/C3/C6/H2
5. __linux__
6. __APPLE__ / _WIN32 / __unix__
7. 兜底 TEST
```

**⚠️ 关键设计**：`hw_pin_mode()`（`:58`）用 `static uint8_t cached = 0xFFu` 缓存，**首次调用才真正探测**。因为 `#if` 是编译期的，探测结果永不改变，但缓存避免了每次调用都走一遍分支。

### 2.2 §65-100 名称与返回码查表

三个 `static const char* const[]` 平表 + 一个 switch。`hw_pin_mode_str` / `hw_pin_sig_name` 都做**越界保护**（越界返回 `"?"`），只有 `hw_pin_result_code_str` 用 switch（有 `default: return "?"`）。

**⚠️ 边界判定都是「双条件」**：`mode < HW_PIN_MODE_MAX` 但没判 `mode >= 0`——因为入参是 `uint8_t`，负数不可能。`hw_pin_sig_name` 入参是 `int`，所以判了 `sig >= 0`。**判不判下界取决于入参类型**，这是这类表函数最容易漏的地方。

---

## 三、`hw_pin.c` §102-186 档案表（L2 核心）

### 3.1 §102-113 ⚠️ ESP32-classic 选脚约束注释

这段是**纯硬件知识**，写在代码里而不是文档里：

| 约束 | 内容 |
|---|---|
| GPIO 6~11 | 片内 SPI Flash，**占用不可用** |
| GPIO 0/2/5/12/15 | strapping 脚，上电电平影响启动模式，**避开** |
| GPIO 34~39 | input-only，只可作输入（MISO/RX 可用） |
| GPIO 1/3 | UART0（USB 下载口），**留作上位机通信** |

**这条约束解释了为什么档案全用 23/19/18/5/17/16**——全绕开了 0~15。

### 3.2 §114-120 四条档案（真实数据）

```c
{ "esp32-9p", { 23, 19, 18, 5, 17, 16, -1, -1, -1 }, 115200, 1000000, 0, BB, OFF }
{ "w25q",    { 23, 19, 18, 5, -1, -1, -1, -1, -1 },      0, 1000000, 0, BB, OFF }
{ "mcu-isp", { -1, -1, -1, -1, 17, 16,  4, -1, -1 }, 115200,       0, 0, BB, OFF }
{ "eprom",   { 23, 19, 18, 5, -1, -1, -1, 25, 26 },      0,  500000, 0, BB, 12V }
```

**逐条读出来的设计意图**：
- `esp32-9p` = 全信号（6 个 GPIO 有映射），唯一 UART+SPI 兼有
- `w25q` = **纯 SPI 4 脚**，`TX/RX/RST` 全 `-1`，`uart_baud=0`（不用的字段清零）
- `mcu-isp` = **纯 UART 3 脚**（TX/RX/RST），SPI 三脚全 `-1`
- `eprom` = SPI 4 脚 + **VPP=IO25 / VCC=IO26**，默认 12V 档

**⚠️ `-1` 是「未映射」的哨兵**，不是错误。`hw_pin_gpio_of()` 直接返回它，上层判 `< 0` 就是「本档案没接这根线」。

### 3.3 §142-170 `hw_pin_profile_checksum()` — 黄金值算法

FNV-1a-32（`h=2166136261u`, 乘数 `16777619u`），**哈希的字节序列**：

| 顺序 | 内容 | 字节数 |
|---|---|---|
| 1 | 每条档案的 `name` 字符串（不含 `\0`） | 变长 |
| 2 | 9 个 `gpio[j]` 值，**各 2 字节小端** | 18 |
| 3 | `uart_baud` **4 字节小端** | 4 |
| 4 | `spi_hz` **4 字节小端** | 4 |
| 5 | `spi_mode` / `drv` / `vpp_level` 各 **1 字节** | 3 |

**⚠️ 三个易错点**：
1. `gpio[j]` 是 `int16_t`，代码做 `& 0xFFFF` **再拆两字节**——如果直接 `h ^= g` 会把 32 位全混进去
2. `uart_baud` 和 `spi_hz` 都是完整 4 字节，不是「只哈希低字节」
3. 哈希**包含 4 条档案全部**（循环在外层），改任一档案的任一字段都要重算

**这就是「动 `hw_pin_profile_t` 必须重算黄金值」的确切原因。**

### 3.4 §172-186 `hw_pin_profile_valid()`

四道校验：
1. `!p || !p->name` → NODEV
2. `spi_mode > 3` / `drv > BB` / `vpp_level >= VPP_MAX` → BADARG
3. 任一 `gpio[j] < -1` → BADARG
4. **一个都没映射（`mapped == 0`）** → NODEV

**⚠️ 第 4 条是「语义校验」而非「语法校验」**：档案语法合法但毫无用处，同样拒绝。T10 用例测的就是第 2 条（`spi_mode = 9`）。

---

## 四、`hw_pin.c` §188-293 SPI Flash 器件模型（W25Q）

### 4.1 §193-204 模型状态（7 个全局）

```c
static uint8_t  g_flash[8192];   /* 2 个 4KB 扇区，static 防真机爆栈 */
static int      g_flash_ready;   /* 是否已格式化为 0xFF */
static int      g_wren;          /* Write Enable Latch */
static int      g_busy_reads;    /* 之后 N 次 RDSR 见 busy */
static uint8_t  g_cmd;           /* 当前命令 */
static uint32_t g_addr, g_seq;   /* 命令内地址 / 已收字节数 */
```

**⚠️ `g_flash[8192]` 声明为 `static` 不是 `static` 函数内局部**——注释明写「真机禁用大局部数组」，对应家族坑 #7（栈帧炸弹）。

### 4.2 §206-217 惰性格式化

`sim_flash_fmt()` 只在**第一次访问时** `memset 0xFF`（`g_flash_ready` 守卫）。
**这是确定性设计的关键**：不这样做的话，每次 `init` 都要 memset，而 `init` 会在 selftest 中被调用多次。

### 4.3 §227-293 `sim_flash_byte()` — 核心状态机

**这是整个模块最值得精读的一段**。返回器件在此字节上驱动的 MISO 位。

#### 相位约定（`g_seq` 驱动）

| `g_seq` | 阶段 | 说明 |
|---|---|---|
| `0` | 操作码 | 记下 `g_cmd`，`g_seq→1` |
| `1,2,3` | 地址相位（0x03/0x02/0x20）或 ID 相位（0x9F） | |
| `≥4` | 数据相位 | READ 出数据 / PP 收数据 |

#### ⚠️ 最关键的一个坑（`:225-226` 源码原注）

> **RDID 的厂商码 EF 在「操作码字节那一拍」就随 MISO 出来（`rx[0]`），不是 `rx[1]`**——SPI 是全双工，发 0x9F 的同时就在读 ID。

代码印证：`:241-242` 在 `g_seq==0` 分支里 `return (uint8_t)(HW_PIN_FLASH_JEDEC & 0xFFu)` = `0xEF`。
`hw_pin_flash_rdid()`（`:829-830`）也印证：`v = rx[0] | rx[1]<<8 | rx[2]<<16`。

**这就是知识页记录的「RDID 厂商码在 `rx[0]` 非 `rx[1]`」坑的代码出处。**

#### 各命令行为

| 命令 | 行为 |
|---|---|
| `WREN 0x06` | `g_wren = 1`，回 0xFF |
| `RDSR 0x05` | 回 `(g_busy_reads > 0) ? 0x01 : 0x00`，**并递减 `g_busy_reads`** |
| `RDID 0x9F` | 操作码拍回 `0xEF`；`seq 1,2,3` 回 `40 18`（`>>8*k`）；`≥4` 回 0x00 |
| `READ 0x03` | 地址 3 字节；数据相位 `rx = g_flash[g_addr++]`（越界回 0xFF） |
| `PP 0x02` | 地址 3 字节；数据相位 **`if (g_wren && ...) g_flash[g_addr] = tx`** |
| `SE 0x20` | 第 3 个地址字节到齐即擦 4KB 扇区；**`g_wren = 0`（WEL 被消耗）**；`g_busy_reads = 1` |

**⚠️ WEL 消耗语义**（`:282`）：只有 `SE` 显式清 `g_wren`。`PP` **不清**——这符合真实器件行为（页编程后 WEL 自动清），但模型里省略了，即**模型的 WEL 在 PP 后仍然有效**。这是一个**模型与真实器件的已知偏差**，值得记一笔（不影响自检，因为自检每次都重新 WREN）。

**⚠️ 越界保护**：`g_addr < sizeof(g_flash)` 三处都有检查，越界回 0xFF（读）或丢弃（写）。**没有越界内存访问**。

---

## 五、`hw_pin.c` §295-545 STM32 AN3155 器件模型（UART）

### 5.1 §295-308 为什么需要两个不同的模型

源码明确解释了差异（`:297-303`）：

> SPI 器件模型是「逐位移位」；UART 不是，而是「**收 1 字节 → 可能吐若干字节**」的**非流水线**协议。

所以结构完全不同：
- SPI 模型 = **一个函数** `sim_flash_byte(tx) → rx`（一问一答）
- ISP 模型 = **两个函数**：`sim_isp_byte(in)`（主机喂入）+ `g_isp_out[]` FIFO（目标吐出），主机用 `getc` 取

### 5.2 §309-343 状态定义

```c
#define ISP_OUTQ 320   /* >= HW_PIN_ISP_MAXPKT(256)，否则大数据读会溢出 */
```
**⚠️ 320 vs 256 的余量只有 64 字节**。`isp_push()` 有 `if (g_isp_outn < ISP_OUTQ)` 守卫，溢出会**静默丢数据**。`hw_pin_isp_read` 的 `n > HW_PIN_ISP_MAXPKT` 检查（`:1070`）是唯一防线。

**11 条命令表**（`:312-317`）：前 9 条是宏，后 2 条 `0x82`（Readout Protect）/ `0x92`（Unprotect）字面量。

**12 个相位状态**（`:320-331`）：
```
SYNC → CMD → CMDC → ADDR → ADDC → LEN → LENC → DATA → DATC
                                              ↘ PAGES → PGLST → PGCHK
```
**注意 ER/WP 走的是 `PAGES` 分支（擦除/写保护都是按页号列表）**，不经过 ADDR 相位——这是 AN3155 的真实协议形状。

### 5.3 §361-366 `isp_mem_ok()` — 基址映射

```c
if (addr < HW_PIN_ISP_FLASH_BASE) return 0;     /* 0x08000000 */
*idx = addr - HW_PIN_ISP_FLASH_BASE;
return (*idx < HW_PIN_ISP_MEM_CAP);
```

**⚠️ 注释说明了为什么必须有这层**（`:359-360`）：
> 器件模型必须做基址映射，否则主机用真实地址（0x08000000）读写会**全部落到模型内存之外**。

**这是「模拟器必须懂真实器件的地址语义」的典型例子**——模型内部用 0 起始的数组，对外必须按 STM32 真实地址空间解释。

### 5.4 §387-432 `sim_isp_dispatch()` — 命令分发

| 命令 | 应答 | 下一相位 |
|---|---|---|
| `GET 0x00` | `[ACK][N=10][11 条命令]` | CMD |
| `GVR 0x01` | `[ACK][版本 0x31][N=10][11 条]` | CMD |
| `GID 0x02` | `[ACK][N-1=1][PID 低][PID 高]` | CMD |
| `RM/WM/GO` | `[ACK]` | ADDR |
| `ER/WP` | `[ACK]` | PAGES |
| `WRU/0x82/0x92` | `[ACK]` | CMD |
| 未知 | `[NACK 0x1F]` | CMD |

**⚠️ `N = 命令数 - 1`**：AN3155 协议的长度字段是「实际个数减 1」。代码两处都写 `(int)HW_PIN_ISP_NCMDS - 1` = 10。

### 5.5 §446-545 `sim_isp_byte()` — 主机字节消费状态机

**最关键的两条真机同款语义**（`:305-307` 源码原注）：

> ① **未握手时非 0x7F 的字节一律忽略** → 主机读不到任何字节（NOTGT）
> ② **命令码补码错 / 未知命令 → NACK；数据校验错 → NACK 且不落盘**

#### 逐相位逻辑

```c
/* 0x7F 特殊处理 (`:449`) */
if (in == 0x7F && (st == SYNC || st == CMD)) { 清 FIFO; st=CMD; push(ACK); return; }
```
**⚠️ 关键守卫 `(st == SYNC || st == CMD)`**——数据相位里的 `0x7F` 是**数据不是握手**。这个条件写错就会在写数据时被误触发重新握手。

| 相位 | 动作 | 失败处理 |
|---|---|---|
| `SYNC` | 直接 `return`（忽略） | — |
| `CMD` | 记 `g_isp_cmd`，算补码 `g_isp_xor = in ^ 0xFF` | — |
| `CMDC` | 校验补码 + 查命令表 | 任一不过 → **NACK**，回 CMD |
| `ADDR` | 收 4 字节移位入 `g_isp_addr`，`xor` 累计；`++cnt >= 4` → ADDC | — |
| `ADDC` | 校验地址 XOR | 错 → NACK；`GO` 直接回 CMD，RM/WM 进 LEN |
| `LEN` | `g_isp_ln = in`（N-1），算补码 | — |
| `LENC` | 校验补码 | 错 → NACK |
| `DATA` | 写 `g_isp_mem[idx]`，`addr++`，`xor ^= in`；`--cnt == 0` → DATC | 越界静默丢弃 |
| `DATC` | 校验数据 XOR | 错 → NACK |
| `PAGES` | `g_isp_ln = in`；**`0xFF` = 全局擦除哨兵** → 全清 + 进 PGCHK | — |
| `PGLST` | 按页号算地址 + 擦该页；`++pg >= ln+1` → PGCHK | 越界跳过 |
| `PGCHK` | 校验页号 XOR | 错 → NACK |
| `default` | **重置回 SYNC** | — |

**⚠️ `ISP_ST_PAGES` 的 `0xFF` 哨兵**（`:519`）：擦除命令的页数字段，AN3155 规定 `0xFF` = 全部擦除。代码 `memset` 全内存后**仍进 `PGCHK`**，因为主机侧仍会发一个 XOR 校验字节（`0xFF`，见 `hw_pin_isp_erase_all` 的 `:1126-1127`）。**主机和模型对「哨兵后还要校验」的认知必须一致**。

---

## 六、`hw_pin.c` §547-676 L1 驱动层（双模）

### 6.1 §550-555 全局状态

```c
static hw_pin_bsp_t  g_bsp;          /* 全空 = 模拟器 */
static const hw_pin_profile_t* g_active;
static uint8_t       g_active_idx;
static hw_pin_stat_t g_stat;
static char          g_err[96];      /* ⚠️ 声明了但全文未见写入 */
```

**⚠️ 顺手发现**：`g_err[96]` 在 `hw_pin_init` 里被清空、`hw_pin_result` 里被读取（`%s`），但**我在读的 1-1757 行里没找到任何写入点**。这意味着 `hw_pin_result()` 永远返回 `"-"`。**这可能是个残留字段，也可能是某处用 `snprintf(g_err,...)` 写的、我没扫到**——列为待查，不下结论。

### 6.2 §557-578 GPIO 原始层

```c
static int pin_gpio_write_raw(int gpio, int level) {
    if (gpio < 0) return HW_PIN_R_NODEV;                    /* 未映射直接拒 */
    if (g_bsp.gpio_write) return (g_bsp.gpio_write(...)==0) ? 0 : -1;  /* 真机 */
    if (gpio < PIN_GPIO_MAX) g_gpio_lvl[gpio] = (level!=0); /* 模拟 */
    g_stat.vex++;
    return 0;
}
```

**⚠️ 注意返回值的三层语义**：`0` = 成功，`-1` = 失败（**不是** `HW_PIN_R_*` 业务码），`HW_PIN_R_NODEV(0x06)` = 未映射。`hw_pin_gpio_write`（`:678`）再把 `-1` 翻译成 `NODEV`。

**⚠️ `vex++` 在真机和模拟器都涨**——因为它在 BSP 分支**之后**。真机上这个计数器不能证明「硬件被碰过」。

`pin_delay_us`：真机走 BSP，**模拟器完全不延时只计数**（`:577`）——保证跨平台确定性。

### 6.3 §591-631 `bb_spi_byte()` — bit-bang 一个字节

#### ⚠️ 全模块最重要的一行：`:605`

```c
use_dev = (g_bsp.gpio_read == NULL) ? 1 : 0;
if (use_dev) dev_rx = sim_flash_byte(tx);   /* 模拟器: 器件模型供 MISO */
```

**这一行是 SIM/REAL 的结构性分界线**（知识页原话）。它不看别的，只看「有没有装 `gpio_read` 回调」：
- 没装 → 没有真实输入源 → 从器件模型取「器件会驱动的 MISO 位」
- 装了 → 真读 MISO 引脚（可能读回目标真实驱动的数据，也可能读回 0）

#### 时序实现（`:608-628`）

```c
写 SCK = cpol                    /* 空闲电平 */
for (b = 7; b >= 0; b--) {       /* MSB 先行 */
    写 MOSI = (tx>>b)&1;
    写 SCK = cpol^1;  bb_toggles++;   /* 上升沿 */
    if (cpha == 0) 采样 MISO;        /* 模式 0/2: 第一沿采样 */
    写 SCK = cpol;    bb_toggles++;   /* 下降沿 */
    if (cpha == 1) 采样 MISO;        /* 模式 1/3: 第二沿采样 */
    pin_delay_us(1);
}
```

**⚠️ CPOL/CPHA 的提取**（`:602-603`）：`cpol = (spi_mode >> 1) & 1`，`cpha = spi_mode & 1`。即 mode 0=CPOL0/CPHA0，mode 1=CPOL0/CPHA1，mode 2=CPOL1/CPHA0，mode 3=CPOL1/CPHA1。**标准约定**。

**⚠️ 这个循环每个字节固定 2 次 `bb_toggles` 递增 × 8 位 = 16 次**，与 cpha 无关（两个沿都走，只是采样点不同）。`bb_toggles` 因此可以精确预测。

#### ⚠️ 一个未实现的能力（诚实记录）

代码在两条路径都采样 MISO（`use_dev` 和真机），但**真机路径下没有做任何 `gpio_dir` 调用**——方向得 BSP 自己配好。如果真机 BSP 忘了把 MISO 配成输入，`pin_gpio_read_raw` 会读到输出寄存器的值，`rx` 全是「自己发出去的东西」。**这是真实风险，不是代码 bug，是接口契约的隐含要求。**

### 6.4 §633-664 `hw_pin_spi_xfer()` — 三路分发

```
1. HW 模式 + 装了 spi_xfer  → 走硬件外设 (返回字节数，<0 → IOERR)
2. 否则 → 先查三根线是否映射 → 逐字节 bb_spi_byte
```

**⚠️ §647-654 那段注释记录了一个已修的 bug**：
> `bb_spi_byte` 的 `NODEV` 曾被我 `(void)` 丢弃，导致「档案没接 SPI 脚」被当成成功（**负向用例假通过**）。

**修法是在进循环之前就把三根线查一遍**。这正是 T12 用例（`:1593`）守护的东西。

---

## 七、`hw_pin.c` §666-772 片选 / UART / 电压层

### 7.1 §666-676 `hw_pin_spi_set_cs()` — ⚠️ 一条极其昂贵的设计决策

```c
int rc = pin_gpio_write_raw(hw_pin_gpio_of(HW_PIN_CS), level);
if (level != 0) {
    /* ⚠️ 不能在此清 g_wren: ... */
    sim_cmd_reset();
}
```

**注释 `:671-672` 记录了一个真实 bug**：
> 不能在此清 `g_wren`：RX-only 命令（如 RDSR 轮询 busy）也会拉高片选，一清 WEL 则**紧随其后的 PP/SE 必然写不进去**（曾致 T7 verify FAIL）。

**这是「协议状态机的清零时机」问题**：`set_cs(1)` = 命令会话结束，但**「会话结束」不等于「写使能失效」**。WEL 的生命周期由器件（真实 Flash / 模型）管，不由主机侧的会话边界管。

`sim_cmd_reset()` 清的是 `g_cmd/g_addr/g_seq`（命令内状态），**不是 `g_wren`**。

### 7.2 §689-732 UART 层

**`hw_pin_uart_xfer`**（`:689`）：逐字节一问一答——`putc` 后如果 `rx != NULL` 就 `getc`。
**⚠️ 注意 `rx` 非空才收**：如果调用方只发不收（`rx=NULL`），模型的输出 FIFO 会**堆积不消费**。

**`hw_pin_uart_putc`**（`:706`）：
```c
if (hw_pin_gpio_of(HW_PIN_TX) < 0) return HW_PIN_R_NODEV;  /* 档案没接 UART */
g_stat.isp_tx++;
if (g_bsp.uart_putc) return (bsp(...)==0) ? OK : IOERR;
sim_isp_byte(ch);       /* 模拟器: 喂给 AN3155 模型 */
```
**注意 `isp_tx++` 在 BSP 分支之前**——和 `vex` 一样，真机上它只证明「驱动接受了字节」，**不证明 pad 上有波形**。这正是 DMC Phase C 那个盲点（知识页记录：`tx=18` 仅证驱动吞字节）。

**`hw_pin_uart_getc`**（`:716`）：从 `g_isp_out[0]` 取，**用前移法出队**（`:728`）而非环形缓冲。
**⚠️ 性能**：`O(n)` 出队，`ISP_OUTQ=320`，最坏 320 次拷贝/字节。对 256 字节读 = 约 4 万次拷贝。**当前规模无所谓，量大时会成热点。**

### 7.3 §734-772 L0 电压层

**`g_vpp_mv[5] = {0, 5000, 12000, 12500, 21000}`**（`:737`）。

**`hw_pin_vpp_set`**（`:746`）的关键顺序：
```c
if (level 越界) return BADARG;
if (装了 bsp) bsp.vpp_set(level, g_vpp_mv[level]);   /* 下发档位+标称 mV */
g_stat.vpp_set_count++;
(void)hw_pin_vpp_read(&mv);    /* 立刻回读 */
g_stat.vpp_mv = mv;
```

**⚠️ 这里是「闭环雏形」但还不是真闭环**：set 之后 read 只是**把回读值记进统计**，**没有比较、没有重试、没有超差报警**。真正的闭环（分压比标定 + 误差放大 + 超差重调）在硬件分析文档里，属于尚未实现的部分。

**`hw_pin_vpp_read`**（`:761`）：真机走 BSP，模拟器**恒返回 `HW_PIN_VPP_MODEL_MV` = 5000**（`:767`）。
**⚠️ 即：模拟器下设 21V 也只会回读到 5V**——这是刻意的（模拟器没有 ADC），但意味着 **L0 层在模拟环境下的行为对档位不敏感**，自检 T9 只能验标称值不能验实际输出。

---

## 八、`hw_pin.c` §774-911 L3 插件 #1：25xx SPI Flash

### 8.1 §777-787 `flash_cmd1()` — 单命令公共外壳

```c
set_cs(1); set_cs(0);           /* 每条命令重新起一个会话 */
rc = spi_xfer(&tx, &rx, 1);
if (rc != OK) { set_cs(1); return rc; }   /* ⚠️ 失败路径也要拉高 */
if (out) *out = rx;
```
**⚠️ 每条命令都 `set_cs(1)→set_cs(0)` 重新开始**——模拟器里 `set_cs(1)` 会 `sim_cmd_reset()` 清 `g_cmd/g_addr/g_seq`，所以这条路径**依赖「set_cs(1) 恰好清状态」这个副作用**。真机上 `set_cs` 只是拉 GPIO，器件自己会处理片选。

### 8.2 §789-815 WREN / RDSR / wait

- `wren`：发 0x06，`set_cs(1)` 收尾
- `rdsr`：发 0x05，收 `sr` 字节
- `wait(timeout_ms)`：**轮询 RDSR 直到 `WIP(bit0) == 0`**

**⚠️ `wait` 的两处「凑合」**（`:809`）：
1. `timeout_ms ? timeout_ms : 1` —— **传 0 会被当成 1ms**，即「至少轮一次」，不是「立即超时」
2. **每次轮询是一次完整 SPI 命令**（`set_cs(1)→set_cs(0)→0x05→读→set_cs(1)`），**没有真实延时**。模拟器下 `g_busy_reads` 递减一次就结束，所以「忙」只有一次轮询。**真机上这是忙等，会实际占用 SPI 总线直到器件完成。**

### 8.3 §817-840 `hw_pin_flash_rdid()` — ⚠️ 两处都要看

```c
tx = { 0x9F, 0, 0, 0 };
set_cs(1); set_cs(0);
rc = spi_xfer(tx, rx, 4);       /* 4 字节 */
set_cs(1);
v = rx[0] | rx[1]<<8 | rx[2]<<16;    /* ⚠️ 小端组装，rx[0] 是厂商码 */
g_stat.flash_jedec = v;
if (rx[0] == 0x00 || 0xFF || 0x7F) { id_ok=0; return NOFLASH; }
```

**⚠️ JEDEC 合法性判据**（`:833`）：`0x00`（全 0 = 无器件/短路）、`0xFF`（悬空）、`0x7F`（上电默认）视为无器件。
**这三个魔数是经验值**，不是协议规定。`0x7F` 的含义是「未初始化的 JEDEC 读」。

### 8.4 §842-894 READ / PP / SE

三者的形状完全一致：**`[cmd][addr 高][addr 中][addr 低]` 四字节发完，再发/收数据**。
- `READ`：第二段 `spi_xfer(NULL, buf, n)` —— **`tx=NULL`，模块内部填 0xFF 发空时钟**（`:658` `tx ? tx[i] : 0xFFu`）
- `PP`：先 `wren()`，再 `spi_xfer(buf, NULL, n)`
- `SE`：先 `wren()`，只发命令

**⚠️ 地址是 3 字节（24 位）**：`cmd[1..3] = addr>>16, >>8, &0xFF`。**超过 16MB 的器件不支持**（W25Q128 及以下够用）。

**⚠️ `PP` 的长度检查是 `n > HW_PIN_FLASH_PAGE(256)` → BADARG**（`:864`）。**没有检查页内是否跨页**——真实器件页编程时跨页会自动回卷，这里模型也会（因为 `g_addr++` 连续），**模型行为与真器件一致**，是「模型保真」的一个好例子。

### 8.5 §896-911 `hw_pin_flash_verify()` — 分块回读

```c
static uint8_t rd[256];    /* ⚠️ static，防真机栈帧膨胀 */
while (got < n) {
    chunk = min(n - got, sizeof(rd));
    flash_read(addr + got, rd, chunk);
    if (memcmp(rd, buf + got, chunk) != 0) return VERIFY;
    got += chunk;
}
```
**逐块（256B）读回 + memcmp**。**⚠️ 第一次不匹配就返回，不报告地址**——T7/T8 只知道「verify 失败」，不知道「哪里失败」。诊断友好度可以再提。

---

## 九、`hw_pin.c` §913-1141 L3 插件 #2：STM32 UART ISP

### 9.1 §919-940 基础设施

```c
#define HW_PIN_ISP_MAXPKT 256u    /* ⚠️ 必须 <= ISP_OUTQ(320) */
static int isp_recv(void) { return hw_pin_uart_getc(); }
static int isp_send(int b) { return hw_pin_uart_putc(b); }
```

**`isp_cmd_begin(cmd)`**（`:928`）—— 所有命令的共同前置：
```c
发 cmd; 发 (cmd ^ 0xFF);        /* 命令码 + 补码 */
ack = recv();
if (ack < 0)           return NOTGT;
if (ack != 0x79)       return NAK;
```
**⚠️ `cmd ^ 0xFF` 的结果强制转 `(uint8_t)`**（`:933`）——因为 `cmd` 是 `uint8_t`，`^0xFF` 在 C 里会整型提升为 `int`，直接传给 `isp_send(int)` 虽然值对，但显式转换是**双形态（C11/C++17）一致性的必要写法**。

### 9.2 §943-963 `hw_pin_isp_sync()` — ⚠️ 一条编译陷阱注释

```c
/* ⚠️ 变量名勿用 try/catch/new/class 等 C++ 关键字:
 *    本文件同时以 C11 与 C++17 编译, C 里合法在 C++ 里是语法错。 */
for (attempt = 0; attempt < 3; attempt++) { ... }
```

**逻辑**：已 `isp_synced` 直接返回；否则发 `0x7F` 最多 3 次，收 `0x79`（ACK）即成功。
**⚠️ `isp_send(HW_PIN_ISP_SYNC)` 传的是 `0x7Fu`**（`u` 后缀，`:954`），而 `isp_send` 收 `int`——**这里依赖整型提升后的值恰好相同**。双形态下都对，但风格上不统一（对比 `isp_cmd_begin` 显式转了 `(uint8_t)`）。

**⚠️ 变量名陷阱值得单列**：这是「全跨式 C/C++ 双编译」家族特性的**具体代价**——同一个 `.c` 文件要同时过 `gcc -std=c11` 和 `g++ -std=c++17`，任何 C 合法的标识符在 C++ 里可能撞关键字。这是**家族纪律**（见 `dual-form-c-cpp-project` 技能）的现实案例。

### 9.3 §966-977 握手态管理

```c
int hw_pin_isp_reset_sync(void) {
    if (!g_bsp.uart_putc) sim_isp_reset();   /* ⚠️ 只重置模拟器模型 */
    g_stat.isp_synced = 0;
    return hw_pin_isp_sync();
}
```
**⚠️ 条件是 `!g_bsp.uart_putc`**——真机上不重置「模型」（因为模型不在真机路径上），只清标志重新握手。**这是「环境绑定决定走哪条路」的又一例**。

### 9.4 §980-1024 地址发送与命令表读取

**`isp_send_addr(addr)`**（`:980`）：4 字节 MSB 先行 + 1 字节 XOR + 收 ACK。
**⚠️ 主机算 XOR，模型也算 XOR**（`:479` `g_isp_xor ^= in`），**两侧算法一致才能对上**——这是一处「协议口径必须与模型同源」的隐含耦合。

**`isp_read_table(cmd, with_version, ...)`**（`:998`）—— GET 与 GVR 共用：
```
GET: [ACK][N][N+1 个命令字节]
GVR: [ACK][版本][N][N+1 个命令字节]     ← 多一个版本前缀
```
**⚠️ 源码注释（`:996`）明确警告**：
> **必须读完 N+1 个命令字节，否则残留会污染下一条命令。**

代码 `:1017` 严格 `for (i = 0; i <= n; i++)`（`n` 是 N-1，故 `i <= n` 收 N+1 个）。**同时对收到的命令字节现场算 FNV**（`:1020`）——所以 `hw_pin_isp_get()` 返回的 `cmdsum_out` 是**从线上真收来的字节算的**，不是读常量表。

**这是一个很漂亮的自证设计**：`HW_PIN_ISP_GOLDEN` 是编译期常量（本地表的哈希），`hw_pin_isp_get()` 返回的是「目标应答的哈希」。**两者相等 = 目标报告的命令表与本地模型一致**。不是「自己和自己比」。

### 9.5 §1027-1064 Get / GetVersion / GetID

- `hw_pin_isp_get`：调 `isp_read_table(GET, 0, NULL, &cs)`，返回 `cs` 应等于 `HW_PIN_ISP_GOLDEN`
- `hw_pin_isp_get_version`：调 `isp_read_table(GVR, 1, &v, &cs)`，`rdp` 恒置 0（**⚠️ 未实现读保护状态查询**）
- `hw_pin_isp_get_id`：收 `[N-1=1][PID 低][PID 高]`，小端组装

**⚠️ `hw_pin_isp_get_id` 的检查顺序**（`:1057-1060`）：
```c
n = isp_recv();  lo = isp_recv();  hi = isp_recv();
if (n < 0 || lo < 0 || hi < 0) return NOTGT;
```
**先收三个字节，再一起判**——不是收一个判一个。好处是**不会因第一个 NACK 而留下未消费字节**（与 DMC 那个「首个负返回码」坑同类）。

### 9.6 §1067-1116 Read / Write Memory

**READ**（`:1067`）：
```c
if (!buf || n == 0 || n > 256) return RANGE;    /* ⚠️ 上限硬检查 */
ensure_sync → cmd_begin(RM) → send_addr(addr)
发 (n-1) + 补码 → 收 ACK
for i in 0..n: buf[i] = recv()
```

**⚠️ `n > HW_PIN_ISP_MAXPKT` 直接 RANGE 拒绝**——这个检查**同时保护了模型的 `ISP_OUTQ`**（因为 RM 会一次 `isp_push_mem` n 个字节）。两层防护。

**WRITE**（`:1093`）：
```c
if (!buf || n == 0) return RANGE;
ensure_sync → cmd_begin(WM) → send_addr(addr)
/* ⚠️ 这里 n > 256 时静默截断，不是报错 */
ln = (n > 256) ? 255 : n-1;
if (n > 256) n = 256;     /* ⚠️ 静默截断 */
发 ln + 补码 → 收 ACK
for i: 发 buf[i], x ^= buf[i]
发 x → 收 ACK
```

**🔴 这里有一个真实的语义问题**：`hw_pin_isp_read` 超限**返回 RANGE 拒绝**（明确失败），而 `hw_pin_isp_write` 超限**静默截断只写前 256 字节**（看起来成功）。
**两者对同一类错误的行为不一致**：读会告诉你「太长」，写会悄悄只写一部分。调用方如果不检查长度，可能以为整个 buffer 写进去了。

⚠️ 记为**待确认项**：是刻意设计（协议本身分片，调用方应自行分片）还是遗漏。**不影响现有自检**（T15 只写 256B 恰在界内），所以不会在矩阵里暴露。

### 9.7 §1119-1141 EraseAll / Go

**`hw_pin_isp_erase_all`**（`:1119`）：
```c
发 0xFF;  发 0xFF;   /* 页数哨兵 + XOR(0xFF)=0xFF */
收 ACK
```
**⚠️ 两个字节都是 0xFF**——第一个是「页数 = 0xFF = 全局擦除」，第二个是校验和（`0xFF ^ 0xFF = 0xFF`）。源码注释 `:1127` 明确写了这一点。

**`hw_pin_isp_go(addr)`**（`:1134`）：`ensure_sync → cmd_begin(GO) → send_addr(addr)`。**⚠️ 不等 ACK 就返回**？不——`isp_send_addr` 内部会收 ACK。但 **GO 命令模型侧（`sim_isp_dispatch` 的 `ISP_ST_ADDC`）GO 是「校验通过就回 CMD，不等长度」**（`:487`），符合 AN3155。

---

## 十、`hw_pin.c` §1143-1226 生命周期与档案装载

### 10.1 §1146-1163 `hw_pin_profile_load()`

```c
if (!p) { g_active = default(); g_active_idx = 0; return OK; }   /* NULL = 复位默认 */
rc = valid(p);  if (rc != OK) return rc;                          /* 先校验 */
g_active = p;
/* 反查下标 */
for (i = 0; i < COUNT; i++) if (&g_profiles[i] == p) { g_active_idx = i; break; }
```
**⚠️ 反查用指针相等 `&g_profiles[i] == p`**——如果传入的是**栈上或动态分配的副本**，反查失败，`g_active_idx` 保持旧值（不报错）。这是「静默不完整」，但 `g_active` 指针本身是对的，功能不受影响。

**⚠️ 这个设计直接导致了 §11.2 `drv` 命令的悬垂陷阱**（见下）。

### 10.2 §1180-1184 `hw_pin_bsp_install()`

```c
if (bsp) g_bsp = *bsp;      /* 结构体整体拷贝 */
else     memset(&g_bsp, 0, sizeof(g_bsp));
```
**⚠️ `*bsp` 是浅拷贝**——如果 `hw_pin_bsp_t` 里加了函数指针以外的成员（比如用户上下文指针），要确认语义。

### 10.3 🔴 §1186-1210 `hw_pin_init()` — 本模块最重要的一段

```c
void hw_pin_init(void* arg) {
    /* ⚠️ 这里**故意不**清 g_bsp —— 环境绑定不是模块状态。 */
    memset(&g_stat, 0, ...);
    memset(g_gpio_dir, 0, ...);
    memset(g_gpio_lvl, 0, ...);
    g_err[0] = '\0';
    g_active = hw_pin_profile_default();
    g_active_idx = 0;
    g_flash_ready = 0;  sim_flash_fmt();
    g_wren = 0; g_busy_reads = 0;
    sim_cmd_reset();
    sim_isp_reset();
}
```

**⚠️ 注释 `:1189-1197` 记录了这个陷阱的完整因果**：

> `kvm_run` 上电会自动调本函数。若顺手把 `g_bsp` memset 掉，真机固件事先装好的 GPIO/UART 绑定会被悄悄换成模拟器：上层拿到的还是「看起来合理」的返回值，**但硬件压根没被碰过**——典型的**假成功**。
> （2026-10-01 ESP32-S3 真机 [B8] 实测复现，与 `hw_dc` 同一类陷阱。）

**契约写得很清楚**：
- `init` = 只复位**模块自己的状态**（统计/档案/器件模型/错误串）
- 环境绑定**只由** `hw_pin_bsp_install()` 改
- 卸载 = `hw_pin_bsp_install(NULL)`

**⚠️ 这与 `hw_dc` 的修复是同一件事**（知识页：hw_dc 的 `init` 会静默抹掉已装 BSP，`hw_dc_init()` 改成「只复位信号表不动 BSP」）。**两个模块犯同一个错，说明这是家族级教训**——已记入 `hw_family_guide.md` 坑 #3 和 #9。

**注意 `hw_pin_init` 里没有重置 `g_flash` 的内容**（只置 `g_flash_ready=0` 然后 `sim_flash_fmt()` 重置为 0xFF）——效果相同，但**顺序是「先标记未格式化，再格式化」**，等价于清空为 0xFF。

---

## 十一、`hw_pin.c` §1228-1445 命令分发

### 11.1 §1231-1244 `pin_dump_active()`

打印档案摘要。**⚠️ 只打印 `gpio[i] >= 0` 的信号**，末尾补 `GND=--`（因为 GND 不参与映射）。

### 11.2 §1246-1445 `hw_pin_cmd()` — 命令表

按 `strcmp` 链式匹配：

| 命令 | 行 | 动作 |
|---|---|---|
| `card` / `info` | 1251 | 打印模式 + 档案数 + 两个黄金值 + 活动档案 |
| `mode` | 1261 | 只打模式 |
| `profiles` | 1265 | 列全部 4 条档案 |
| `load NAME` | 1275 | 前缀匹配，`cmd+5` 取名 |
| `valid` | 1281 | 校验活动档案 |
| `drv hw\|bb` | 1287 | 🔴 **切驱动模式** |
| `id` / `rdid` | 1297 | 读 JEDEC |
| `read ADDR` | 1305 | 读 16 字节 |
| `erase ADDR` | 1320 | 扇区擦除 |
| `wtest` | 1326 | 擦→写 256B→读回校验 |
| `vppread` | 1341 | 读 VPP |
| `vpp N` | 1347 | 设 VPP 档位 |
| `isp [子命令]` | 1354 | ISP 分支（info/erase/read/go/wtest/chk） |
| `stat` | 1421 | 打印 `hw_pin_stat_t` |
| `selftest` | 1433 | 跑自检 |
| `help` | 1438 | 打印帮助，返回 `HELP(-2)` |
| 兜底 | 1444 | 返回 `NOCMD(-1)` |

#### 🔴 §1287-1296 `drv` 命令：静态副本防悬垂

```c
/* ⚠️ g_active 是「档案指针」, 不能指向栈上临时体 (退出即悬垂)。
 *    用静态副本承载运行期覆写, 静态表本体保持只读。 */
static hw_pin_profile_t s_ovr;
if (g_active) s_ovr = *g_active;
s_ovr.drv = (strcmp(cmd+4,"hw")==0) ? HW_PIN_DRV_HW : HW_PIN_DRV_BB;
g_active = &s_ovr;
```

**这是「指针生命周期」的正确处理**：`g_active` 是 `const hw_pin_profile_t*`，若直接改 `g_active->drv` 会**污染只读静态表**（影响 `HW_PIN_GOLDEN` 的前提），若指向局部变量会**悬垂**。用 `static` 副本两者都避开。

**⚠️ 副作用**：`g_active` 指向 `s_ovr` 后，`hw_pin_profile_load(NULL)` 能恢复默认，但 `hw_pin_stat` 的 `active_idx` 反查会失败（`&g_profiles[i] != &s_ovr`）→ 保持旧值。**功能不受影响，是显示层的细节**。

#### §1354-1358 ISP 分支的档案自动切换

```c
if (hw_pin_gpio_of(HW_PIN_TX) < 0) (void)hw_pin_profile_load_name("mcu-isp");
```
**⚠️ 这个「自动切换」是双刃的**：
- 好处：用户不用手动 `load mcu-isp`
- 风险：**它会静默改变当前档案**，用户以为在 w25q 上操作，结果被切到 mcu-isp

（`hw_pin_cmd` 里的 ISP 分支不像 CLI 层有「操作完恢复」的动作——只有 selftest 里的 T16/T17 显式 `profile_load(NULL)` 恢复。）

---

## 十二、`hw_pin.c` §1447-1705 自检（18 项）

### 12.1 §1455-1465 最重要的三行

```c
int hw_pin_selftest(int (*putf)(const char*)) {
    int fails = 0;
    hw_pin_bsp_t saved_bsp = g_bsp;      /* ① 存 */
    if (!putf) putf = pin_puts_null;
    memset(&g_bsp, 0, sizeof(g_bsp));    /* ② 清 */
    hw_pin_init(NULL);
    ...
    hw_pin_init(NULL);
    g_bsp = saved_bsp;                   /* ③ 还原 */
    return fails;
}
```

**⚠️ 注释 `:1458-1461` 解释为什么**：
> 自检必须在**确定性模拟器**上跑：它断言的是器件模型行为（W25Q 的 EF4018 / AN3155 的 PID/命令表），真机 BSP 在场时这些期望值本就不成立。故先存走环境绑定、跑完再还原——这样 `hw_pin("selftest")` 在**任何环境下都得到同一份结论**。

**这是「自检必须与环境解耦」的范例**。和 `hw_fault` 的「selftest 开头保存 + 卸载 BSP 结尾恢复」是同一手法（知识页记录，13-fails 事故的修复）。

### 12.2 18 项用例逐条

| # | 行 | 断言 | 类型 |
|---|---|---|---|
| **T1** | 1468 | 模式名不是 `"?"` | 冒烟 |
| **T2** | 1475 | `profile_checksum() == 0x9E0F10FA` | 🔴 黄金值 |
| **T3** | 1487 | 4 条档案全部 `valid` | 正向 |
| **T4** | 1497 | `w25q` 档案 MOSI=23/MISO=19/CK=18/CS=5 | 映射 |
| **T5** | 1506 | RDID 读出 `0x001840`（EF 40 18） | 🔴 全链 |
| **T6** | 1519 | 扇区擦除后读回 0xFF | 正向 |
| **T7** | 1530 | 页编程 256B + verify 通过 | 🔴 写全链 |
| **T8** | 1543 | **不 WREN 直接 PP → verify 必须失败** | 🔴 负向 |
| **T9** | 1569 | VPP 标称 5V/12V 正确 + 回读 5000mV | 正向 |
| **T10** | 1581 | `spi_mode=9` 的坏档案被拒 | 负向 |
| **T11** | 1590 | NULL-safe（占位） | 冒烟 |
| **T12** | 1594 | **`mcu-isp` 档案（无 SPI 脚）做 RDID → 必须 NODEV** | 🔴 负向 |
| **T13** | 1609 | `isp_checksum() == 0xD2A9A924` | 🔴 黄金值 |
| **T14** | 1621 | ISP 握手 + GetVersion(0x31) + GetID(0x0410) + Get(黄金) | 🔴 全链 |
| **T15** | 1641 | ISP 擦→写 256B→读回逐字节一致 | 🔴 写全链 |
| **T16** | 1658 | **`w25q` 档案（无 UART）做 ISP sync → 必须 NODEV** | 🔴 负向 |
| **T17** | 1667 | **命令码补码写错 → 目标回 NACK(0x1F)** | 🔴 协议负向 |
| **T18** | 1685 | 🔴 **`hw_pin_init()` 不得清掉 `g_bsp`** | 🔴 假成功护栏 |

#### 🔴 T18 的设计（`:1685-1700`）—— 最值得学的一条

```c
static int pin_selftest_sentinel_read(int pin) { (void)pin; return 0x5A5A; }
/* ... */
memset(&probe, 0, sizeof(probe));
probe.gpio_read = pin_selftest_sentinel_read;   /* 装一个不可能自然出现的值 */
g_bsp = probe;
hw_pin_init(NULL);                               /* ← 模拟 VM 上电 */
if (g_bsp.gpio_read != pin_selftest_sentinel_read) {
    fails++;  putf("  T18 init must keep BSP FAIL\n");
} else putf("  T18 init keeps BSP OK\n");
```

**为什么这个测试有效**：
1. **哨兵值 `0x5A5A`** 不可能来自任何真实逻辑——它是「这是我放的」的标记
2. 直接**比对函数指针**（不是调用它），所以不依赖返回值语义
3. 它测的是「`init` 有没有做那件不该做的事」——**一个「不变量」测试**，而非「功能」测试

**这正是知识页锚点 A（独立口径）+ D（阴性对照）的实践**。

**⚠️ 位置很讲究**：T18 放在最后，且在 `hw_pin_init(NULL); g_bsp = saved_bsp;` 之前——它**故意污染 `g_bsp`**，然后由收尾的还原语句恢复。这是「测试有副作用但有配对还原」的正确写法。

### 12.3 负向用例的分布

18 项里 **5 项是负向**（T8/T10/T12/T16/T17），且**都针对同一类病**：
> 「**本该失败的操作返回了成功**」

这与知识页记录的家族共性完全一致——**假成功是本项目头号敌人**。

**⚠️ T8 的判定写得特别小心**（`:1561-1565`）：
```c
if (rc != HW_PIN_R_VERIFY && rc != HW_PIN_R_OK) { fails++; ... }   /* 奇怪的错误码 */
else if (rc == HW_PIN_R_OK) { fails++; ... (unexpectedly wrote) }   /* 假成功 */
else { putf("  T8 ... OK\n"); }
```
**第一分支接受 `VERIFY` 也接受 `OK`？** 读一下：`rc != VERIFY && rc != OK` → 失败。意思是「返回既不是 VERIFY 也不是 OK 就算失败」。然后 `else if (rc == OK)` → 也失败。**净效果：只有 `rc == VERIFY` 才算通过。** 写法绕但结论正确——**第一分支其实可以简化为 `rc != VERIFY`**。属于可读性问题，不影响正确性。

---

## 十三、`hw_pin.c` §1707-1757 CLI

### 13.1 🔴 §1724-1739 参数拼接（家族坑 #9 的正解）

```c
if (argc >= 4) {
    /* ⚠️ 必须把 argv[2] 之后的参数拼回一条命令串, 否则
     *    `./xiaomo pin load w25q` 会被截成 "load" → 全分支不匹配 → 静默 exit 1
     *    (与家族坑 #9「argv 下标」同源: 零输出先怀疑命令拼接)。 */
    for (i = 2; i < argc; i++) { ... cmdline[off++] = ' '; ... }
    sub = cmdline;
} else if (argc >= 3 && argv[2]) {
    sub = argv[2];               /* 兼容 ./xiaomo pin "load w25q" */
}
```

**这是家族坑 #9（CLI 只取 `argv[2]` 致命令截断）的标准修法**：
- 家族约定 `argv[1]="pin"`，`argv[2..]` 是子命令
- **多参数必须拼回一条带空格的串**，因为 `hw_pin_cmd` 用的是 `strncmp(cmd, "load ", 5)` 这类**前缀匹配**
- 同时保留 `argc==3` 的单串写法（带引号）兼容

**⚠️ 越界保护**（`:1730`）：`if (off + l + 2u >= sizeof(cmdline)) break;`——留 2 字节给分隔符和 `\0`。

**⚠️ 缓冲区是 `static char cmdline[256]`**——不是栈上，防真机爆栈（家族一致性）。

### 13.2 退出码映射

```c
rc = hw_pin_cmd(sub, NULL);
return (rc >= 0) ? rc : 1;      /* 负码（-1 NOCMD / -2 HELP）统一 → 1 */
```
**⚠️ 已知局限**：`rc` 是 `uint8_t` 语义的正码时直接返回（如 `NOFLASH=4`），shell 看到的退出码是 4 而不是 1。**约定是「0 = 成功，非 0 = 失败，具体码看输出」**。

---

## 覆盖情况

| 文件 | 总行 | 已解析 | 覆盖率 |
|---|---|---|---|
| `hw_pin.h` | 271 | §1-258（API 清单全） | ~100% |
| `hw_pin.c` | 1757 | §1-1757 | **100%** |

**已解析的重点段落**（每段都读了原码）：
- ✅ 编译期探测（1-63）· 名称表（65-100）
- ✅ 档案表 + 选脚约束 + checksum（102-186）
- ✅ SPI 器件模型（188-293）
- ✅ AN3155 器件模型（295-545）
- ✅ BSP + GPIO 原始层（547-578）
- ✅ bit-bang SPI + `hw_pin_spi_xfer`（580-664）
- ✅ 片选 / UART / 电压层（666-772）
- ✅ 25xx Flash 插件（774-911）
- ✅ UART ISP 插件（913-1141）
- ✅ 生命周期 + 档案装载（1143-1226）
- ✅ 命令分发（1228-1445）
- ✅ 18 项自检（1447-1705）
- ✅ CLI（1707-1757）

**顺手记录的待查项**（不下结论）：
1. ✅ **已核实**：`g_err[96]` 全文只有 3 处出现（`hw_pin.c:555` 声明 / `1202` 清空 / `1225` 读取）——**无任何写入点** ⇒ `hw_pin_result()` 恒返回 `"-"`。属残留字段。
2. ✅ **已核实**：`hw_pin_isp_write` 超长**静默截断**（`:1103-1105`）vs `hw_pin_isp_read` 超长**明确拒绝**（`:1070`），行为不一致。
3. `use_dev` 分界只看 `gpio_read` 一个回调，若真机只注入了 `spi_xfer` 则 `bb_spi_byte` 不会被调用（此路径下 hw 模式优先，实际不成问题，但接口契约值得写明）。

---

## 实跑复核（2026-10-02，写完立刻跑）

```
$ ./xiaomo pin card
pin: mode=HOST profiles=4 golden=0x9E0F10FA
pin: L3 plugins: flash(25xx SPI RDID=EF4018) + isp(AN3155 UART cmdsum=0xD2A9A924)
pin: profile=esp32-9p drv=BB spi_mode=0 spi_hz=1000000 baud=115200 vpp=0
pin: MOSI=IO23 MISO=IO19 CK=IO18 CS=IO5 TX=IO17 RX=IO16 GND=--

$ ./xiaomo pin selftest
T1..T18 全 OK（0 fails）
```

**文档里引用的两个黄金值、4 条档案、18 项用例、默认档案的六个引脚，全部与实跑逐字一致。**

---

*本文件只覆盖 `hw_pin`。其余 16 个 `.c`（hw_pin 之外共 11,451 行）待续。*
*解析纪律：每段必须读过原码才写；行号可跳转核对；不确定的写「待查」不写「大概」。*
