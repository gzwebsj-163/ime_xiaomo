# `hw_flash` 逐段代码解析（ESP32 ROM 下载协议烧录层）

> **源文件**：`include/hw_flash.h`(226 行) + `src/hw/hw_flash.c`(1117 行) = 1343 行
> **定版日期**：2026-09-30 ｜ **解析日期**：2026-10-02
> **一句话职责**：把「给 ESP32 烧固件」这件事，从外部 Python 工具（esptool）**收进 xiaomo 架构本身**——纯 C、全跨式六模式，烧录可由 `.mo` 字节码驱动（`hw_flash("run 4096")`）。
> **本文档 100% 覆盖两个文件全部非空行，行号可跳转核对，零编造。**

---

## 目录

| 段 | 行号范围 | 内容 |
|---|---|---|
| 一 | `.h:1-49` | 文件头：设计契约与「四条铁律」 |
| 二 | `.h:51-122` | 常量区：模式枚举、ROM 命令码、SLIP、返回码 |
| 三 | `.h:124-158` | 三个数据结构 + BSP 回调表 |
| 四 | `.c:33-84` | 编译期模式探测（六模式优先级） |
| 五 | `.c:89-156` | ROM 命令表 + 黄金 FNV + XOR 校验 + 测试镜像 |
| 六 | `.c:161-271` | MD5（RFC1321）纯整数实现 |
| 七 | `.c:276-368` | SLIP 编解码 + 请求/应答帧 |
| 八 | `.c:375-562` | 🔴 ROM 模拟器（设备侧确定性实现） |
| 九 | `.c:567-696` | 会话状态 + `fl_req` + 协议原语 |
| 十 | `.c:703-796` | 🔴 烧录主流程 `hw_flash_run` |
| 十一 | `.c:802-966` | 自检 19 项（含 4 项负向） |
| 十二 | `.c:971-1117` | 命令分发 + CLI |
| 附录 A | — | 精读中发现的 6 个问题（**A-0 已修复**，A-6 为全族普查） |
| 附录 B | — | 自检项与代码行对照表 |

---

# 一、文件头：设计契约与「四条铁律」

`hw_flash.h:1-49`。这个头注释是全模块最有价值的部分——它把**四条踩过坑的协议铁律**写在了最前面。

## 1.1 协议四层结构（`.h:16-30`）

```
1. SLIP (RFC1055) 帧封装 : 0xC0 定界 + 0xDB 转义
     字面 C0 -> DB DC ; 字面 DB -> DB DD   (方向不可反!)
2. 请求帧  "<BBHI" : dir(1)+cmd(1)+len(2)+checksum(4) + body
     checksum = body 逐字节异或, 初值 ESP_CHECKSUM_MAGIC(0xEF)
3. 应答帧  "01 op len(u16) val(u32)" + data[len] + status(1) + error(1)
     ⚠️ 应答头第 3 字段 = data 长度(不是请求回显), 第 4 字段 = val;
     末两字节 = [status, error], 缺了即 "Invalid response"
4. ROM 命令集 (S3 ROM 无 stub 全放行): 9 条
```

## 1.2 四条铁律逐条拆解

### 铁律 1：SLIP 转义方向不可反（`.h:18`）

注释里罕见地直接写了「**方向不可反!**」三个字。为什么值得单独强调？

SLIP RFC1055 的转义表只有两个条目，而 **ESP32 实现把 `0xDC` 和 `0xDD` 的含义对调了**（相对于某些 RFC1055 实现的标准写法）：

| 字面字节 | 编码后 |
|---|---|
| `0xC0` | `DB DC` |
| `0xDB` | `DB DD` |

这两条一旦写反（即 `C0 → DB DD`、`DB → DB DC`），**对称的编解码自测会全部通过**（因为 encode 和 decode 用了同一张反向表，roundtrip 天然成立），**但一上真机就全错**。

> 📌 这个坑我在知识页 `esp32-sim-flasher-rom-protocol.md` 里有完整记录：2MB 镜像碰巧不含 `0xDC`/`0xDD` 字节，**假通过**；换 4MB 随机镜像后 MD5 当场抓获差异。
> **判别法**：负向向量必须**包含两个方向**，且要与独立实现（esptool）逐字节对拍。

### 铁律 2：请求帧 `body` 从 `pd+8` 起（`.h:19-20`）

```
"<BBHI" = dir(1) + cmd(1) + len(2, u16 LE) + checksum(4, u32 LE)
         └─ 合计 8 字节 ─┘
```

`HW_FLASH_HDR_LEN = 8`（`.h:92`）。这个「8」是全模块最常出现的魔数。

### 铁律 3：应答末两字节 `[status, error]` 不可省（`.h:23`）

这是**最容易被漏掉**的一环。如果只读到 `data[len]` 就认为「一帧收完了」，真实 ROM 会一直等剩下 2 字节 ⇒ **永久阻塞**。

### 铁律 4：ROM 无 stub 模式不收 `FLASH_END`（`.h:29-30`）

```
无 stub 时块尺寸 = 1024B ; ROM 模式不收 FLASH_END,
最后一条协议命令是 SPI_FLASH_MD5(0x13), 由它触发烧完校验。
```

> 📌 **这个知识点与 `hw_dmc` 那个「上界回绕」bug 是同一类**——都是「协议有多个收尾方式，押错一种就整条流错位」。esptool 源码里对应 `cmds.py` 的 `IS_STUB` 守卫。

## 1.3 全跨式设计三条（`.h:32-41`）

1. 编译期探测六模式，`-DHW_FLASH_MODE_OVERRIDE=n` 可强指
2. 核心（SLIP/帧/XOR/MD5/烧录流程）只依赖 `stdint` + `string` ⇒ **freestanding 可编译**
3. 底层字节收发走 BSP 回调：默认 = 确定性 ROM 模拟器；真机注入真实 UART，**上层零改动**
4. 黄金参考 `HW_FLASH_GOLDEN` = 命令表 FNV-1a-32，**独立 Python 对拍锁定**

## 1.4 六层接入清单（`.h:42-48`）

| 层 | 接入点 |
|---|---|
| Makefile | `HW_SRCS` 收编（`Makefile.linux` 同补） |
| VM 内核 | `OP_HW_FLASH_CALL`（`vm_core.c`，`kvm_run` 上电自动 `hw_flash_init`） |
| 编译器 | `mo2kbc` 内置 `hw_flash("...")` → `OP_HW_FLASH_CALL` |
| CLI | `./xiaomo flash [card\|mode\|slip\|md5\|sync\|chip\|run N\|verify\|selftest]` |
| 示例 | `examples/flash_test.mo`（`.kbc` 端到端） |
| 测试 | `tests/run_tests.sh` flash 块 |

---

# 二、常量区：模式枚举、ROM 命令码、SLIP、返回码

## 2.1 六模式枚举（`.h:52-60`）

```c
typedef enum {
    HW_FLASH_MODE_HOST    = 0,   /* 宿主 (macOS/Windows) */
    HW_FLASH_MODE_LINUX   = 1,   /* Linux (kickpi/server/Termux) */
    HW_FLASH_MODE_KELL    = 2,   /* 内核嵌入 (TinyEMU riscv64 真内核) */
    HW_FLASH_MODE_ESP32   = 3,   /* ESP32-S3/C3/C6 (IDF) */
    HW_FLASH_MODE_ESP8266 = 4,   /* ESP8266 (RTOS/NonOS) */
    HW_FLASH_MODE_TEST    = 5,   /* 未知平台兜底 */
    HW_FLASH_MODE_MAX     = 6
} hw_flash_mode_t;
```

> 📌 **`KELL` 排第 3**（在 LINUX 之后、ESP32 之前）不是随便排的——探测优先级必须与枚举顺序无关，但数值分配要保证 `mode_str()` 数组下标对齐（见 §4.2）。

## 2.2 九条 ROM 命令码（`.h:63-71`）

```c
#define HW_FLASH_CMD_FLASH_BEGIN   0x02u
#define HW_FLASH_CMD_FLASH_DATA    0x03u
#define HW_FLASH_CMD_FLASH_END     0x04u
#define HW_FLASH_CMD_SYNC          0x08u
#define HW_FLASH_CMD_WRITE_REG     0x09u
#define HW_FLASH_CMD_READ_REG      0x0Au
#define HW_FLASH_CMD_CHANGE_BAUD   0x0Fu
#define HW_FLASH_CMD_FLASH_MD5     0x13u   /* SPI_FLASH_MD5 */
#define HW_FLASH_CMD_SEC_INFO      0x14u   /* GET_SECURITY_INFO */
```

> 📌 **`0x04 FLASH_END` 出现在表里但 ROM 模式从不发**（铁律 4）。它保留在表里有两个作用：
> ① 命令表完整性/文档价值（stub 模式需要）
> ② **黄金 FNV 把它算进哈希** ⇒ 任何人删掉这一行，`selftest` 立刻 FAIL（`cmd table size == 9` + FNV 同时炸）

## 2.3 SLIP 常量（`.h:82-85`）

```c
#define HW_FLASH_SLIP_END     0xC0u   /* 帧定界 */
#define HW_FLASH_SLIP_ESC     0xDBu   /* 转义引导 */
#define HW_FLASH_SLIP_ESC_END 0xDCu   /* 字面 C0 的转义形态: DB DC */
#define HW_FLASH_SLIP_ESC_ESC 0xDDu   /* 字面 DB 的转义形态: DB DD */
```

**命名即文档**：`ESC_END` = 「用来转义 END(C0) 的那个字节」，不是「ESC 序列的结尾」。

## 2.4 容量宏与最坏膨胀（`.h:91-96`）

```c
#define HW_FLASH_BLOCK_MAX   1024u    /* S3 ROM 无 stub 块尺寸 */
#define HW_FLASH_HDR_LEN     8u       /* 请求/应答帧头 */
#define HW_FLASH_DATA_HDR    16u      /* FLASH_DATA 请求体前缀: size+seq+2×0 */
#define HW_FLASH_RSP_TAIL    2u       /* status + error */
/* SLIP 编码最坏膨胀: 单帧 (8+16+1024+4) -> 2x + 2 定界 */
#define HW_FLASH_SLIP_CAP    ((8u + 16u + 1024u + 4u) * 2u + 2u)
```

`HW_FLASH_SLIP_CAP` = `(1052 × 2) + 2` = **2106 字节**。

> 📌 **这个「最坏膨胀 2×」的推导是全模块容量计算的标准范式**：每个字节最坏变 2 字节，再加前后两个定界符。**任何新增长度都必须重算此宏**，否则 SLIP 编码在满帧时返回 -1（写一半的帧发出去 = 协议错）。

> 📌 **`HW_FLASH_DATA_HDR = 16` 不是 8**：`FLASH_DATA` 的 body 是 `size(4) + seq(4) + 0(4) + 0(4) + data[bsize]`，即**两个 u32 长度/序号 + 两个 u32 保留**。只有 SEQ 真正被使用（见 §8.4）。

## 2.5 黄金值区（`.h:104-111`）

```c
#define HW_FLASH_GOLDEN        0xAF05978Au  /* ROM 命令表 FNV-1a-32 */
#define HW_FLASH_IMG4096_MD5   "4e328028738d17bb7ff82667d5803369"
#define HW_FLASH_CHIP_ID       0x00000009u  /* ESP32S3 IMAGE_CHIP_ID */
#define HW_FLASH_RDID_WINBOND  0x001640EFu  /* 4MB Winbond RDID(0x9F) 回读 */
#define HW_FLASH_SPI_MAGIC_ADDR  0x60001F10u
#define HW_FLASH_SPI_CMD_ADDR    0x60002000u
#define HW_FLASH_SPI_RDID_ADDR   0x60002058u
```

**三个 MMIO 地址是硬件契约，不是随便写的**：
- `0x60001F10` = SPI 魔数寄存器 → 读出 `9` 即 ESP32-S3
- `0x60002000` = `SPI_CMD_REG` → 空闲时读 0
- `0x60002058` = `SPI_USER2_REG`（W0=RDID 结果）→ 只有先 `WRITE_REG` 写过 `0x9F` 才有非 0 值

## 2.6 返回码（`.h:114-122`）

```c
#define HW_FLASH_R_OK        0x00
#define HW_FLASH_R_NOARGS    0x01
#define HW_FLASH_R_BADARG    0x02
#define HW_FLASH_R_IOERR     0x03
#define HW_FLASH_R_PROTO     0x04   /* 协议错 (SLIP/帧/状态尾不符) */
#define HW_FLASH_R_CHECKSUM  0x05   /* XOR 校验和不符 */
#define HW_FLASH_R_BADSIZE   0x06   /* 镜像尺寸非法 (0 或超模型容量) */
#define HW_FLASH_R_NOCMD     (-1)   /* 未识别命令 */
#define HW_FLASH_R_HELP      (-2)   /* help 已吐出 */
```

> 📌 **正 0~6 与负 -1/-2 的分界是刻意的**：`hw_flash_cli:1116` 用 `rc >= 0 ? rc : 1` 归一化。**结果码 0 = 成功 = exit 0**，与 shell 约定一致；若把所有码都做成正数，`exit 4` 会被 shell 当成「命令找不到」类错误。

---

# 三、三个数据结构 + BSP 回调表

## 3.1 `hw_flash_session_t`（`.h:125-136`）

```c
typedef struct {
    uint32_t offset;          /* flash 内起始偏移 */
    uint32_t image_size;      /* 镜像总字节 */
    uint32_t block_size;      /* 本次会话块尺寸 (<= HW_FLASH_BLOCK_MAX) */
    uint32_t blocks_written;  /* 已写块数 */
    uint32_t bytes_written;   /* 已写字节 */
    uint32_t progress;        /* 0..100 */
    uint8_t  done;            /* 1 = 流程走完 */
    uint8_t  md5_match;       /* 1 = 设备回读 MD5 == 本地 MD5 */
    uint8_t  md5_local[16];
    uint8_t  md5_remote[16];
} hw_flash_session_t;
```

> 📌 **`md5_local` 与 `md5_remote` 并存**是本模块的核心设计：不仅告诉你「校验通过/失败」，还让你**事后能对比到底哪 16 字节不一样**。真机排障时这比一个 bool 有用得多。

## 3.2 `hw_flash_stat_t`（`.h:139-147`）

```c
uint32_t sessions;        /* 烧录会话数 */
uint32_t cmd_tx;          /* 发出的 ROM 命令数 */
uint32_t cmd_rx;          /* 收到的应答数 */
uint32_t slip_escaped;    /* 编码时转义字节数 */
uint32_t bytes_flashed;   /* 累计写入 flash 字节 */
uint32_t last_md5_ok;     /* 最近一次 MD5 是否一致 */
uint8_t  chip_id;         /* 最近一次探测的芯片 ID */
```

> 🕳️ **`slip_escaped` 曾是死字段**——已修，见附录 A-1。

## 3.3 BSP 回调表（`.h:153-158`）

```c
typedef struct {
    int (*uart_open)(uint32_t baud);
    int (*uart_close)(void);
    int (*uart_write)(const uint8_t* d, uint32_t n);
    int (*uart_read)(uint8_t* d, uint32_t cap);
} hw_flash_bsp_t;
```

**返回约定（`.h:150-152`）**：
- `uart_open/close`：`0` = 成功，`open` 失败回 `<0`
- `uart_write`：**实际写入字节数**，`<0` 失败
- `uart_read`：**读到字节数**，`0` = 暂无，`<0` 失败

> 📌 **「`uart_read` 返回 0 = 暂无而不是 EOF」是全模块最重要的一条约定**。`fl_req:643` 靠它实现「没数据就 continue 重试」，而 `0` 若被误当成 EOF，接收循环会立刻退出 ⇒ 表现为 `proto:no-frame`。

> 📌 **4 个回调里只有 2 个真被调用**（见 §9.1）——`uart_open`/`uart_close` 全模块零引用点。**设计上预留**（真机固件需要），但值得知道。

---

# 四、编译期模式探测（六模式优先级）

`.c:33-84`。

## 4.1 探测优先级链（`.c:35-51`）

```c
#if defined(HW_FLASH_MODE_OVERRIDE)     return OVERRIDE;      /* ① 强指，最高 */
#elif defined(HW_FLASH_KELL)             return KELL;         /* ② 内核嵌入 */
#elif defined(CONFIG_IDF_TARGET_ESP8266) || defined(__ESP8266__)
                                          return ESP8266;     /* ③ */
#elif defined(CONFIG_IDF_TARGET_ESP32) || ... || ESP32C6 || ESP32H2
                                          return ESP32;       /* ④ */
#elif defined(__linux__)                 return LINUX;        /* ⑤ */
#elif defined(__APPLE__)||defined(_WIN32)||defined(__unix__)
                                          return HOST;         /* ⑥ */
#else                                    return TEST;          /* ⑦ 兜底 */
```

**顺序的三个讲究**：

1. **`OVERRIDE` 永远排第 1** ⇒ 任何环境下都能用 `-DHW_FLASH_MODE_OVERRIDE=n` 做**交叉验证**（六模式矩阵的驱动手段）
2. **`KELL` 必须在 `__linux__` 之前** ⇒ TinyEMU 里编译出的代码同时满足两者，若按 `__linux__` 排就会误判成 LINUX 模式
3. **`ESP8266` 在 `ESP32` 之前** ⇒ 两者都定义 `__ESP32__` 家族宏时，先命中更具体的那个

> 📌 **注意第 ④ 条的 6 个目标**：`ESP32 / S2 / S3 / C3 / C6 / H2` 全部归为同一个 `ESP32` 模式。**模式是「平台族」不是「芯片型」**——想知道具体是哪颗芯片，读 `chip_id` 而不是看 mode。

## 4.2 一次性缓存（`.c:54-59`）

```c
uint8_t hw_flash_mode(void)
{
    static uint8_t cached = 0xFFu;
    if (cached == 0xFFu) cached = fl_mode_probe();
    return cached;
}
```

> 📌 **`0xFF` 是 `0xFFu` 字面量比较，不是有符号判断** ⇒ 在 `-Werror` 下也不会触发符号比较告警。**这是家族里统一的写法**，可追溯到 `hw_token`。

## 4.3 模式名与结果码映射（`.c:61-84`）

```c
static const char* const fl_mode_names[HW_FLASH_MODE_MAX] = {
    "HOST", "LINUX", "KELL", "ESP32", "ESP8266", "TEST"
};
const char* hw_flash_mode_str(uint8_t mode)
{
    return (mode < HW_FLASH_MODE_MAX) ? fl_mode_names[mode] : "?";
}
```

**数组下标 = 枚举值**，越界回 `"?"`（不崩）。

`hw_flash_result_code_str()`（`.c:70-84`）是纯 `switch`，9 个码一一对应字符串，`default` 回 `"?"`。

---

# 五、ROM 命令表 + 黄金 FNV + XOR 校验 + 测试镜像

`.c:89-156`。

## 5.1 命令表与黄金值算法（`.c:89-121`）

```c
static const hw_flash_cmd_info_t g_cmd_table[] = {
    { 0x02, "FLASH_BEGIN" }, { 0x03, "FLASH_DATA" }, { 0x04, "FLASH_END" },
    { 0x08, "SYNC" }, { 0x09, "WRITE_REG" }, { 0x0A, "READ_REG" },
    { 0x0F, "CHANGE_BAUD" }, { 0x13, "FLASH_MD5" }, { 0x14, "SEC_INFO" }
};

uint32_t hw_flash_cmd_checksum(void)
{
    uint32_t h = 2166136261u;                       /* FNV offset basis */
    for (i = 0; i < hw_flash_cmd_count(); i++) {
        h ^= (uint32_t)g_cmd_table[i].code;  h *= 16777619u;   /* 先 code */
        for (k = 0; name[k] != '\0'; k++) {
            h ^= (uint32_t)(uint8_t)name[k]; h *= 16777619u;   /* 再逐字符 */
        }
    }
    return h;
}
```

**哈希了什么**（很关键，防误改）：
- ✅ **每条的 `code`（1 字节）**
- ✅ **每条的 `name` 每个字符**（含 `'\0'` 终止符前的所有字符，**但不含 `'\0'` 本身**）
- ❌ 不含顺序分隔符、不含表长度

> 📌 **「先 code 再 name」这个顺序是黄金值的一部分**。若有人改成「先 name 再 code」，`selftest` 立刻 FAIL。这与知识页锚点 **E（指纹归属）** 同源：**黄金值锁的不只是数据，还有「数据怎么进哈希」这条路径**。
>
> 📌 `code` 用 `(uint32_t)` 显式提升、`name[k]` 用 `(uint32_t)(uint8_t)` 双重截断——**在交叉编译器上 `char` 默认无符号性可变**（`ARM` 编译器 `-funsigned-char` 差异），显式 `(uint8_t)` 锁死跨编译器一致性。

## 5.2 XOR 校验和（`.c:134-141`）

```c
uint8_t hw_flash_checksum(const uint8_t* d, uint32_t n)
{
    uint8_t c = HW_FLASH_CHECKSUM_MAGIC;    /* 0xEF */
    if (!d) return c;                        /* NULL → 返回初值（空 body 的校验和）*/
    for (i = 0; i < n; i++) c ^= d[i];
    return c;
}
```

> 📌 **`0xEF` 初值（`ESP_CHECKSUM_MAGIC`）不是随便的**——它让「空 body」的校验和 = `0xEF` 而非 `0x00`，避免「空 body」和「body 全零且无初值」在 wire 上不可区分。

## 5.3 确定性测试镜像（`.c:143-156`）

```c
/* b[i] = (i*73+11)&0xFF, 每 97 字节置 C0, 每 131 置 DB */
for (i = 0; i < n; i++) {
    uint8_t v = (uint8_t)((i * 73u + 11u) & 0xFFu);
    if      (i % 97u  == 0u) v = 0xC0;   /* SLIP_END */
    else if (i % 131u == 0u) v = 0xDB;   /* SLIP_ESC */
    buf[i] = v;
}
```

**三个设计意图**：

| 意图 | 实现 | 对应自检项 |
|---|---|---|
| 伪随机内容 | `i*73+11`（73 与 256 互质 ⇒ 256 周期全覆盖） | MD5 黄金值 |
| **强制含 C0** | 每 97 字节 | SLIP 转义方向 |
| **强制含 DB** | 每 131 字节 | SLIP 转义方向 |

> 📌 **这就是知识页记录的「2MB 假通过」的正解**——测试镜像**主动注入**两个特殊字节，4096B 里就有 59 个 C0 + 47 个 DB（见自检项文案）。**镜像多大不重要，重要的是它一定含陷阱字节。**

---

# 六、MD5（RFC1321）纯整数实现

`.c:161-271`。**零动态内存、零查表**（`fl_md5_k` / `fl_md5_s` 是静态常量数组）。

## 6.1 上下文（`.c:161-166`）

```c
typedef struct {
    uint32_t h[4];      /* A B C D */
    uint64_t len;       /* 已处理位长 */
    uint8_t  buf[64];   /* 不足一块的残余 */
    uint32_t buflen;
} fl_md5_ctx;
```

## 6.2 轮函数四段（`.c:200-208`）

```c
if      (i < 16) { f = (b & cc) | (~b & d);       g = i; }              /* F */
else if (i < 32) { f = (d & b) | (~d & cc);       g = (5*i+1) & 15; }  /* G */
else if (i < 48) { f = b ^ cc ^ d;                g = (3*i+5) & 15; }  /* H */
else             { f = cc ^ (b | ~d);             g = (7*i)   & 15; }  /* I */
tmp = d; d = cc; cc = b;
b = b + rotl32(a + f + k[i] + m[g], s[i]);
a = tmp;
```

> 📌 **MD5 的输入取字节序是「小端读 u32」**（`.c:195-198`：`p[i*4] | p[i*4+1]<<8 | ...`），**输出也是小端**（`.c:243-248`）。**标准 MD5 就是小端**（大端的是 SHA 系列），RFC1321 明确规定。

## 6.3 填充（`.c:233-242`）

```c
pad[0] = 0x80u;
padlen = (buflen < 56) ? (56 - buflen) : (120 - buflen);
for (i = 1; i < padlen; i++) pad[i] = 0x00;
for (i = 0; i < 8; i++) pad[padlen+i] = (bits >> (8*i)) & 0xFF;   /* 长度小端 8B */
```

> 📌 **`pad[72]` 这个尺寸的推导**：最坏 `buflen = 63` ⇒ `padlen = 120-63 = 57` ⇒ `padlen + 8 = 65`。**但 `pad[0]` 已用掉 1 字节，所以最大写入下标是 64** ⇒ **`pad[65]` 就够，写 72 是留了余量**。同时 `buflen < 56` 时 `padlen ≤ 55`，`55+8 = 63 < 72`。**两个分支都在 72 内。**

## 6.4 便捷包装（`.c:251-271`）

```c
void hw_flash_md5(const uint8_t* d, uint32_t n, uint8_t out16[16]);
void hw_flash_md5_hex(const uint8_t* d, uint32_t n, char out33[33]);
```

`md5_hex` 用静态查表 `"0123456789abcdef"` 逐字节转 32 字符 + `'\0'`。

> 📌 **`hw_flash_md5_hex(NULL, 0, buf)` 是合法调用**（`:880` 自检就这么用）⇒ 打出的是**空串的 MD5** `d41d8cd9...`，即 RFC1321 标准向量。

---

# 七、SLIP 编解码 + 请求/应答帧

`.c:276-368`。**这是全模块的核心，也是铁律 1/2/3 的落点。**

## 7.1 编码（`.c:276-300`）

```c
out[o++] = 0xC0;                              /* 前导定界 */
for (i = 0; i < n; i++) {
    uint8_t b = in[i];
    if      (b == 0xC0) { 检查 o+2<=cap; out[o++]=0xDB; out[o++]=0xDC; }  /* C0→DB DC */
    else if (b == 0xDB) { 检查 o+2<=cap; out[o++]=0xDB; out[o++]=0xDD; }  /* DB→DB DD */
    else                { 检查 o+1<=cap; out[o++] = b;                 }
}
out[o++] = 0xC0;                              /* 收尾定界 */
```

**三个「每步都查容量」的分支**——`o+2 > cap` / `o+1 > cap`，**任何一步越界都 `return -1` 且不写出半帧**（前面的字节已经写了，但调用方看到 -1 就不会发）。

> 📌 **容量检查写在写出之前**（`if (o+2u > cap) return -1;` 在 `out[o++]` 之前）⇒ 越界时**至少当前这个转义对不会被写坏**。虽然函数已经返回 -1，但这是良好的局部不变量。

## 7.2 解码（`.c:302-326`）

```c
while (i < n && in[i] == 0xC0) i++;      /* 跳过前导定界符（可多个！）*/
for (; i < n; i++) {
    uint8_t b = in[i];
    if (b == 0xC0) break;                /* 收尾定界 → 停 */
    if (esc) {
        if      (b == 0xDC) b = 0xC0;   /* DB DC → C0 */
        else if (b == 0xDD) b = 0xDB;   /* DB DD → DB */
        else return -1;                 /* 非法转义 */
        esc = 0;
    } else if (b == 0xDB) { esc = 1; continue; }
    if (o + 1u > cap) return -1;
    out[o++] = b;
}
if (esc) return -1;                     /* 帧尾还挂着未闭合的 ESC */
return (int)o;
```

**三处精妙设计**：

1. **`while` 跳多个前导定界符** ⇒ 容忍「上一帧残留的尾 C0 + 本帧的头 C0」连在一起（真实串口上极常见）
2. **`if (esc) return -1` 收尾检查** ⇒ 帧尾的孤立 `0xDB` 会被识别为错误而非静默丢弃
3. **`else return -1` 非法转义** ⇒ `DB xx`（xx 既非 DC 也非 DD）直接判错

> 📌 **「解码器不消费收尾 C0 之后的数据」是分帧的关键**：`break` 之后 `i` 停在收尾定界上，调用方（`fl_req:645`）每次重新从 `rsp[0]` 整体解码，**靠 `g_rsp_pos` 消费指针**避免重复。**注意这里没有「解了一半保留余量」的机制**——真实串口分片到达时靠 §9.2 的累积重试解决。

## 7.3 请求帧构造（`.c:331-351`）

```c
int hw_flash_frame_build(uint8_t cmd, const uint8_t* body, uint32_t blen, uint8_t* out, uint32_t cap)
{
    if (!out) return -1;
    if (blen > 0 && !body) return -1;
    if (blen > 0xFFFF) return -1;                        /* len 是 u16 */
    if (cap < HW_FLASH_HDR_LEN + blen) return -1;        /* 容量预检 */
    ck = hw_flash_checksum(body, blen);                  /* 只校验 body，不含头 */
    out[0] = 0x00;                /* dir = REQ */
    out[1] = cmd;
    out[2] = (uint8_t)(blen & 0xFF);   out[3] = (uint8_t)(blen >> 8);   /* u16 LE */
    out[4] = ck;   out[5] = 0x00;   out[6] = 0x00;   out[7] = 0x00;    /* checksum 占 4B，XOR 只 1B */
    memcpy 展开: out[8 + i] = body[i];
    return HW_FLASH_HDR_LEN + blen;
}
```

> 📌 **`checksum` 字段占 4 字节但只有第 1 字节是 XOR 值，后 3 字节补 0**（`:345-348`）。这与帧格式 `"<BBHI"` 严格对应（H = u32），**且是合法的**——真实 ROM 只取低 8 位。
>
> 📌 **`blen > 0xFFFF` 显式拒绝**——这是 `hw_dmc` 那个「252→258→`(uint8_t)258` 静默回绕」的**同类防护**。**但这里用的是正确的做法**：`if (cap < ...)` 在**任何写出之前**完成，不会发出半截帧。

## 7.4 应答帧解析（`.c:353-368`）

```c
int hw_flash_frame_parse(const uint8_t* in, uint32_t n, uint8_t* cmd, uint32_t* val,
                         const uint8_t** body, uint32_t* blen, uint8_t* st)
{
    if (!in || n < HW_FLASH_HDR_LEN + HW_FLASH_RSP_TAIL) return -1;   /* 至少 10B */
    if (in[0] != HW_FLASH_DIR_RSP) return -1;                          /* 必须 0x01 */
    if (cmd) *cmd = in[1];
    len = in[2] | (in[3] << 8);                 /* ⚠️ data 长度，不是请求回显 */
    if (val) *val = in[4] | (in[5]<<8) | (in[6]<<16) | (in[7]<<24);
    if (HW_FLASH_HDR_LEN + len + HW_FLASH_RSP_TAIL > n) return -1;   /* 声明长度不许超实际 */
    if (body) *body = in + HW_FLASH_HDR_LEN;        /* 指针，不拷贝 */
    if (blen) *blen = len;
    if (st)   *st   = in[HW_FLASH_HDR_LEN + len];    /* ⚠️ status 在 data 之后 */
    return HW_FLASH_HDR_LEN + len + HW_FLASH_RSP_TAIL;
}
```

**三个要点**：

1. **返回值 = 整帧长度**（含 2 字节状态尾），调用方可用它推进
2. **`body` 是指向 `in` 内部的指针，不拷贝** ⇒ 调用方必须在 `in` 失效前用完（`fl_req:664` 传出给 `hw_flash_run:776` 立即用，安全）
3. **`st` 取 `in[8+len]` 而非 `in[8]`** ⇒ 这就是铁律 3 的落点

> 📌 **`n < 10` 提前拒绝**（`8 + 2`）——一条零 data 的应答最短 10 字节。**这个下界检查放在解析最前面**，避免 `in[2]`/`in[3]` 读到越界。

---

# 八、🔴 ROM 模拟器（设备侧确定性实现）

`.c:375-562`。**这是本模块最有价值的设计之一**：不接真机也能把整条烧录链路跑通并逐位回归。

> 📌 头注释（`.c:371-373`）：**与已验证的 IDE 模拟器 `esp32-sim/src/pty_sim.c` 的 `rom_reply`/`handle_cmd` 语义逐条对齐，但零文件 IO / 零 SSL**。flash 模型驻留静态数组 ⇒ 全平台逐位一致。

## 8.1 静态状态（`.c:375-382`）

```c
static hw_flash_bsp_t g_bsp;              /* 函数指针全空 = 用模拟器 */
static uint8_t  g_rsp[HW_FLASH_SLIP_CAP];  /* 模拟器 → 上层 的 SLIP 应答 */
static uint32_t g_rsp_len, g_rsp_pos;
static uint8_t  g_model[HW_FLASH_MODEL_CAP];   /* 32768B flash 模型 */
static uint32_t g_model_len;              /* 本次会话镜像长度 */
static uint32_t g_model_off;              /* flash 起始偏移 */
static uint32_t g_model_blk;              /* FLASH_BEGIN 声明的块尺寸 */
static uint8_t  g_spi_cmd;                /* WRITE_REG 记下的 SPI_USR2 子命令 */
```

> 📌 **`g_model` = 32KB 静态数组**。这是**模块最大的 RAM 开销**（`HW_FLASH_MODEL_CAP` 可用 `-D` 调，见 `.h:99-101`）。真机 BSP 装上后这个数组**仍然占着**（C 不回收静态内存）——真机固件应当用小值编译。

## 8.2 应答编码 `sim_reply`（`.c:385-405`）

```c
static uint32_t sim_reply(uint8_t op, uint32_t val, const uint8_t* data, uint32_t len)
{
    uint8_t raw[8 + 64 + 2];          /* ⚠️ 栈上 74 字节 */
    if (len > 64u) return 0;          /* 硬上限 64B */
    raw[0]=0x01; raw[1]=op;
    raw[2]=len&0xFF; raw[3]=len>>8;
    raw[4..7]=val 小端;
    memcpy data;
    raw[o++] = 0x00;  /* status */
    raw[o++] = 0x00;  /* error  */
    e = slip_encode(raw, o, g_rsp, sizeof(g_rsp));
    if (e > 0) { g_rsp_len = e; g_rsp_pos = 0; return e; }
    return 0;
}
```

> 📌 **`raw[74]` 仍放在栈上**——这是模拟器内部函数，不在 `fl_req` 的 6.3KB 静态缓冲链上，74 字节无害。但**它与 §9 的 static 加固是同一个问题的两种处理**，值得知道它**没加固**。

## 8.3 `sim_read_reg`：READ_REG 精确语义（`.c:407-416`）

```c
if (addr == 0x60001F10) return 9;                          /* chip_id */
if (addr == 0x60002000) return 0;                          /* SPI_CMD 空闲 */
if (addr == 0x60002058) return (g_spi_cmd == 0x9F) ? 0x001640EF : 0;   /* W0 RDID */
if (addr >= 0x60007000 && addr < 0x60007700) return 0x5e0508b7;        /* efuse */
return 0;
```

> 📌 **注释直说「错一个地址 esptool 就死在探测/轮询」**——这不是夸张。真实 esptool 探测芯片型号就靠读 `0x60001F10`，读错地址拿不到魔数就完全不知道在跟谁说话。
>
> 📌 **`0x001640EF` 是 Winbond 4MB RDID**（厂商 `EF` + 容量码 `16` + 类型码 `40`）。**这与 `hw_pin` 的 `0x9F` RDID 是同一个物理事实的两条路径**：`hw_pin` 走 SPI 外设位翻，这里走 ROM `WRITE_REG`+`READ_REG`。**互相印证了「SPI 是否受 matrix 限制」这个问题的重要性**（见附录 A-5）。

## 8.4 `sim_handle`：九条命令分发（`.c:425-526`）

| 命令 | 处理 | 行号 |
|---|---|---|
| `SYNC` | 回 **8 帧**，`val=0x20121220` | `:436-445` |
| `READ_REG` | `sim_read_reg(rd_u32le(body))` | `:446-449` |
| `WRITE_REG` | 记 `g_spi_cmd`（若 `addr==0x60002020`） | `:451-458` |
| `FLASH_BEGIN` | 读 `len/off/blk` + `memset 0xFF`（擦除） | `:460-469` |
| `FLASH_DATA` | `off = seq * g_model_blk` 落盘 | `:471-482` |
| `FLASH_END` | 空回 | `:484-486` |
| `CHANGE_BAUD` | 空回 | `:488-490` |
| `FLASH_MD5` | 从 `g_model` 算 MD5 → **32 字节 ASCII hex** | `:492-510` |
| `SEC_INFO` | 20 字节 `si[12]=chip_id, si[16]=0x03` | `:512-520` |

### 8.4.1 `SYNC` 的 8 帧（`.c:436-445`）

```c
for (i = 0; i < 8u; i++) {
    uint32_t e = sim_reply(SYNC, 0x20121220u, NULL, 0);
    if (e) tot = e;          /* 只保留最后一帧给上层读 */
}
(void)tot;
```

> 📌 **这是全模块最反直觉的一段**。真实 ROM 对一次 SYNC 会回 **8 帧**（bootloader 的同步信号特征），而模拟器**连发 8 次、每次覆盖 `g_rsp`** ⇒ 上层只读到最后一帧。**语义上等价**（上层只读一帧），但**它没有模拟「上层要吃掉 8 帧」这个事实**。
>
> 📌 如果将来要支持「真机 + 模拟器对照」，**这里会是一个行为差异点**：真机 `uart_read` 会连续吐出 8 帧的内容。当前 `fl_req:640` 的循环**只解出第一帧就 break**，剩余 7 帧会**滞留在 rsp 缓冲**里，被下一条命令误当数据。**真机实测 PASS 恰好掩盖了这一点**（详见附录 A-4 ②，该项仍未处理）。

### 8.4.2 `FLASH_DATA` 的 SEQ 定位（`.c:471-482`）

```c
uint32_t sz  = rd_u32le(body);        /* size */
uint32_t seq = rd_u32le(body + 4);    /* seq  */
uint32_t off = seq * g_model_blk;     /* ⚠️ 用块尺寸算偏移，不累加 */
if (blen - HW_FLASH_DATA_HDR < sz) sz = blen - HW_FLASH_DATA_HDR;   /* 截断保护 */
for (i = 0; i < sz; i++)
    if (off + i < HW_FLASH_MODEL_CAP) g_model[off + i] = body[16 + i];  /* 越界保护 */
```

> 📌 **`off = seq * blk` 而不是维护一个写指针**——这**忠实还原了真实 ROM 的寻址方式**（ROM 只认 SEQ，靠 `FLASH_BEGIN` 声明的块尺寸换算）。**这个"纯函数式"的设计让模拟器天然无状态、可重入、可乱序容忍**。
>
> 📌 **两重截断保护**：`sz` 按实际 body 长度截，`off+i` 按模型容量截。**这正是 `hw_dmc` 那个上界 bug 的正确解法**（对比 `.c:479` 有护栏 vs `hw_dmc` 缺护栏）。

### 8.4.3 `FLASH_MD5` 的地址换算（`.c:492-510`）

```c
uint32_t addr = rd_u32le(body);
uint32_t size = rd_u32le(body + 4);
const uint8_t* base = g_model + (addr >= g_model_off ? (addr - g_model_off) : 0u);
hw_flash_md5(base, size, d2);
/* → 32 字节小写 hex ASCII → sim_reply(cmd, 0, hex, 32) */
```

> 📌 **`addr >= g_model_off` 这个条件**：请求地址落在烧录起始偏移**之前**（正常不会）⇒ base 退化为 `g_model`（即从 0 开始）。**保守降级而非报错**。
>
> 📌 **回的是 ASCII hex 32 字节，不是裸 16 字节**——这是 ESP32 ROM 的真实行为，客户端必须做 hex 解码（§10.5）。

### 8.4.4 `SEC_INFO` 20 字节布局（`.c:512-520`）

```c
uint8_t si[20]; memset(si, 0, 20);
si[12] = HW_FLASH_CHIP_ID;   /* chip_id = 9 → ESP32-S3 */
si[16] = 0x03u;              /* api_version */
sim_reply(cmd, 0, si, 20);
```

> 📌 **`si[12]` 这个偏移是硬契约**——真实 `esp_image_header_t.security_info` 结构里 `chip_id` 就在第 12 字节。**自检项 6 直接断言 `rbody[12] == 9`**，把它锁死。

## 8.5 `sim_write` / `sim_read`：默认传输（`.c:531-549`）

```c
static int sim_write(const uint8_t* d, uint32_t n)
{
    static uint8_t dec[8 + 16 + 1024 + 2 + 8];      /* static! */
    int dl = hw_flash_slip_decode(d, n, dec, sizeof(dec));
    if (dl < 0) return -1;
    g_rsp_len = 0; g_rsp_pos = 0;                   /* 清消费指针 */
    sim_handle(dec, dl);
    return (int)n;
}

static int sim_read(uint8_t* d, uint32_t cap)
{
    uint32_t avail = g_rsp_len - g_rsp_pos;
    if (avail == 0) return 0;                        /* 0 = 暂无（§3.3 约定）*/
    if (avail > cap) avail = cap;
    memcpy(d, g_rsp + g_rsp_pos, avail);
    g_rsp_pos += avail;
    return (int)avail;
}
```

> 📌 **`static uint8_t dec[...]` 在函数内**——这正是我在知识页记录的「**栈帧炸弹（宿主永远测不出）**」的修法。注释（`.c:528-530`）写清了代价：
> > 大缓冲一律 static (真机加固): 本模块是单实例全局状态机 (g_sess/g_stat/g_model), 把 ~1KB 帧缓冲放栈上, 在 ESP32 小栈任务里会直接踩爆 (**实测 IDLE1 栈溢出复位**)。
>
> **代价：函数不可重入**。但本模块本就是单实例全局状态机，**重入本来就没意义**——**用「反正不能重入」换「不炸栈」是正确的取舍**。

## 8.6 BSP 优先分派（`.c:552-562`）

```c
static int fl_tx(const uint8_t* d, uint32_t n)
{
    if (g_bsp.uart_write) return g_bsp.uart_write(d, n);
    return sim_write(d, n);
}
static int fl_rx(uint8_t* d, uint32_t cap)
{
    if (g_bsp.uart_read) return g_bsp.uart_read(d, cap);
    return sim_read(d, cap);
}
```

> 📌 **SIM/REAL 的分界是「函数指针非空」**——与 `hw_pin` 用 `g_bsp.gpio_read == NULL` 判 `use_dev` 是**完全同款的结构性分界**。
>
> 📌 **注意这里只判 `uart_write` / `uart_read` 两个**。自检项 8（`.c:930`）故意只装 `uart_write` 而**让 `uart_read` 留 NULL** ⇒ **写走坏桩、回读走模拟器**。**这个「半安装」是负向测试的关键设计**（详见 §11.3）。

---

# 九、会话状态 + `fl_req` + 协议原语

`.c:567-696`。

## 9.1 全局状态与结果串（`.c:567-610`）

```c
static hw_flash_session_t g_sess;
static hw_flash_stat_t    g_stat;
static char               g_last[128];
```

`fl_set_result`（`:571-577`）是**手写 strncpy**（防 `-Wall` 建议，且显式截断到 `sizeof-1`）。

`hw_flash_bsp_install`（`:593-597`）：
```c
if (bsp) g_bsp = *bsp;              /* 整体覆盖 */
else     memset(&g_bsp, 0, sizeof(g_bsp));   /* NULL = 还原模拟器 */
```

> 📌 **注意 `hw_flash_init`（`:579-589`）也 memset 了 `g_bsp`**——这和 `hw_dc` / `hw_pin` 的「init 静默清 BSP」是**同一个陷阱的第三个实例**。**但这里恰好不是问题**，因为 `init` 的调用时机是 `kvm_run` 上电（`:44` 注释）⇒ 真机固件在 `app_main` 里 `install` 之后不会再有 `kvm_run` 触发 `init`。**同一个模式，三种运气**——详见附录 A-5。

## 9.2 🔴 `fl_req`：发一收一的核心（`.c:615-667`）

```c
/* static (真机加固): 合计 ~6.3KB */
static uint8_t frame[8 + 16 + 1024];          /* 1048 B */
static uint8_t slip[HW_FLASH_SLIP_CAP];        /* 2106 B */
static uint8_t raw[8 + 16 + 1024 + 4];        /* 1052 B */
static uint8_t rsp[HW_FLASH_SLIP_CAP];        /* 2106 B */
```

**四个 static 缓冲合计 6.3KB**。注释直说「真机加固」。

### 9.2.1 发送侧（`:631-636`）

```c
fl = hw_flash_frame_build(cmd, body, blen, frame, sizeof(frame));
if (fl < 0) return HW_FLASH_R_BADARG;
fl = hw_flash_slip_encode(frame, fl, slip, sizeof(slip));
if (fl < 0) return HW_FLASH_R_BADARG;
g_stat.cmd_tx++;
if (fl_tx(slip, fl) < 0) { fl_set_result("ioerr:tx"); return HW_FLASH_R_IOERR; }
```

### 9.2.2 接收侧：累积到整帧（`:638-655`）

```c
got = 0;
for (tries = 0; tries < 4096u; tries++) {
    int r = fl_rx(rsp + acc, sizeof(rsp) - acc);
    if (r < 0) { fl_set_result("ioerr:rx"); return HW_FLASH_R_IOERR; }
    if (r == 0) continue;                        /* 暂无数据 */
    acc += r;
    dl = hw_flash_slip_decode(rsp, acc, raw, sizeof(raw));
    if (dl > 0 && (uint32_t)dl >= 8 + 2) {
        uint32_t need = 8 + raw[2] + (raw[3] << 8) + 2;   /* 从解出的头读声明长度 */
        if ((uint32_t)dl >= need) { raw_len = dl; got = 1; break; }
    }
    if (acc >= sizeof(rsp)) { acc = 0; }        /* 缓冲满 → 重新同步 */
}
if (!got) { fl_set_result("proto:no-frame"); return HW_FLASH_R_PROTO; }
```

**四层保护**：

| 机制 | 位置 | 作用 |
|---|---|---|
| `r == 0` continue | `:643` | 「暂无数据」不是错误（§3.3 约定）|
| `acc` 累积 | `:644` | 真实串口分片到达 |
| `need` 按**声明长度**判定 | `:647-649` | **不等缓冲满**，够一帧就解 |
| `acc >= sizeof(rsp)` → `acc = 0` | `:653` | 缓冲满后**丢弃重来**（重同步） |

> 📌 **`need` 从 `raw[2]/raw[3]` 读声明长度，这是「不等满就解」的关键**。若改成「等 `acc == sizeof(rsp)`」，每条命令都要凑满 2106 字节 ⇒ 真机永远超时。**这一行是整个接收逻辑能不能工作的分水岭。**

> 📌 **`tries < 4096` 不是超时而是「轮次上限」**。`r == 0` 时 `continue` 不消耗时间（模拟器立即返回 0），所以**模拟器路径下这个循环会在真机数据到位前自旋 4096 次然后失败**。真机 BSP 的 `uart_read` **必须自己带超时**（`.h:152` 注释：「0=暂无, <0 失败」——"暂无"意味着内部已阻塞等待）。

### 9.2.3 解析与状态检查（`:657-666`）

```c
if (hw_flash_frame_parse(raw, raw_len, &rcmd, &rval, &rbody, &rblen, &rst) < 0) {
    fl_set_result("proto:bad-frame"); return HW_FLASH_R_PROTO;
}
g_stat.cmd_rx++;
if (out_body) *out_body = rbody;      /* ⚠️ 必须判空: sync 等调用传 NULL */
if (out_val)  *out_val  = rval;
if (out_blen) *out_blen = rblen;
if (rst != HW_FLASH_ST_OK) { fl_set_result("proto:status-fail"); return HW_FLASH_R_PROTO; }
return HW_FLASH_R_OK;
```

> 📌 **「status != OK」被归到 `R_PROTO` 而不是 `R_IOERR`**——语义上正确（设备明确回了失败），但**返回码不区分「设备拒绝」和「帧格式错」**，排障时只能靠 `g_last` 里的 `proto:status-fail` 区分。**可接受但不够细。**

## 9.3 协议原语（`.c:669-696`）

### `hw_flash_sync`（`:669-678`）

```c
uint8_t body[36];
for (i = 0; i < 36; i++) body[i] = 0x07u;    /* esptool sync 载荷：全 0x07 */
rc = fl_req(SYNC, body, 36, NULL, NULL, NULL);
```

> 📌 **36 字节全 `0x07` 是 esptool 的硬编码**。这个载荷内容**设备基本不看**（SYNC 的意义在于「打破当前状态」），但要与 esptool 一致。
>
> 📌 **这 36 字节是栈上的**——但因为 `fl_req` 把大缓冲全改成 `static` 了，所以这条栈帧只有 36 + 少量，是安全的。

### `hw_flash_read_reg`（`:680-696`）—— 🔴 有个 API 语义修复

```c
rc = fl_req(READ_REG, body, 4u, &v, NULL, NULL);
/* 探测 magic 地址即落存芯片型号 → 令 chip_id() API 自洽 */
if (rc == HW_FLASH_R_OK && addr == HW_FLASH_SPI_MAGIC_ADDR)
    g_stat.chip_id = (uint8_t)v;
if (val) *val = v;
```

> 📌 **这段注释记录了一次真实 bug 修复**：
> > (真机 diag 曾裸调 read_reg 后查 chip_id 得 0, 是 **API 语义缺口而非真机差异**)
>
> **这是知识页锚点 I「值对归属错」的另一个变体**——`read_reg` 成功读到了 `9`，但 `chip_id()` 返回 `0`，**两个 API 各自都"对"，组合起来是错的**。修法是**在语义归属的那一层做落存**，而不是让每个调用者自己记得赋值。
>
> 📌 **注意 `hw_flash_run:728-729` 又重复了一遍同样的落存**：
> ```c
> if (hw_flash_read_reg(HW_FLASH_SPI_MAGIC_ADDR, &v) == OK) g_stat.chip_id = (uint8_t)v;
> ```
> 这是**冗余但无害**的重复（`read_reg` 内部已经做了）。**留着是因为它显式表达意图**。

---

# 十、🔴 烧录主流程 `hw_flash_run`

`.c:703-796`。**本模块的主战场。**

## 10.1 前置检查与 static 加固（`:703-723`）

```c
/* static (真机加固): body 1KB + begin/md5b/hex */
static uint8_t body[HW_FLASH_DATA_HDR + HW_FLASH_BLOCK_MAX];   /* 16 + 1024 = 1040 B */
uint8_t begin[20];   /* 栈上 */
uint8_t md5b[8];     /* 栈上 */
char lhex[33], rhex[33];   /* 栈上，共 66 B */
```

```c
if (!image || len == 0) return HW_FLASH_R_BADSIZE;
if (!g_bsp.uart_write && len > HW_FLASH_MODEL_CAP) return HW_FLASH_R_BADSIZE;
   /*              ↑ 只有「走模拟器」才受 32KB 限制 */
```

> 📌 **第 719 行是重要设计**：真机烧录**不受 32KB 模型容量限制**（模拟器限制），真机可以烧几 MB。**但 `g_model` 数组仍然占着 32KB 内存**。

## 10.2 流程全景（`.c:725-790`）

```
 1. SYNC                                    :725
 2. READ_REG 0x60001F10 → chip_id          :728
 3. FLASH_BEGIN(len, nblk, 1024, offset)   :741
 4. for each block: FLASH_DATA              :744-762
 5. FLASH_MD5(offset, len) → 32B ASCII      :772
 6. 本地 MD5 vs 设备 MD5 → md5_match        :783-784
 ❌ 无 FLASH_END                             （铁律 4）
```

## 10.3 `FLASH_BEGIN` 载荷 20 字节（`:732-741`）

```c
begin[0..3]   = len     (u32 LE)   /* 镜像总长 */
begin[4..7]   = nblk    (u32 LE)   /* 块数 = (len+bs-1)/bs */
begin[8..9]   = bs      (u16 LE)   /* 块尺寸 = 1024 */
begin[10..11] = 0                    /* 高位补零 */
begin[12..15] = offset (u32 LE)   /* flash 内偏移 */
begin[16..19] = 0                    /* encrypt/params 保留 */
```

> 📌 **注意 `bs` 只写 2 字节后补 2 个 0**（`:736-737`）⇒ 仍是 4 字节字段，与模拟器 `rd_u32le(body+8)`（`.c:463`）严格对应。**这 20 字节布局是 `sim_handle` 的 `.c:462-464` 消费方，两边必须同步改。**

## 10.4 `FLASH_DATA` 载荷（`:744-762`）

```c
for (i = 0; i < nblk; i++) {
    uint32_t chunk = len - i * bs;
    if (chunk > bs) chunk = bs;
    body[0..3] = bs     (u32 LE)      /* 每块声明的 size（固定 1024）*/
    body[4..7] = i      (u32 LE)      /* SEQ */
    body[8..15] = 0                    /* 8 字节保留 */
    for (j = 0; j < bs; j++)
        body[16 + j] = (j < chunk) ? image[i*bs + j] : 0xFF;   /* ⚠️ 补 0xFF */
    rc = fl_req(FLASH_DATA, body, 16 + bs, NULL, NULL, NULL);
    ...
}
```

> 📌 **`body[0..3] 写的始终是 `bs`（1024）而不是 `chunk`**——**这是正确的**。真实 ROM 认 `size` 字段决定读多少，而 SEQ 定位靠 `seq * bs`（§8.4.2）。如果写 `chunk`，最后一块的 `size` 变小，**真实 ROM 会按 `chunk` 读但模拟器按 `bs` 定位** ⇒ 两者行为分叉。
>
> 📌 **末块补 `0xFF` 填充到满块**（`j < chunk ? ... : 0xFF`）——这模拟了「flash 未写区是 0xFF」的真实物理特性。
>
> 📌 **本地 MD5 算的是原始 `image`（`len` 字节），不是补齐后的镜像**（`:765`）——**这是正确的**，因为设备侧 `FLASH_MD5(size=len)` 也只算 `len` 字节。

## 10.5 MD5 比对（`:764-789`）

```c
hw_flash_md5_hex(image, len, lhex);                    /* 本地 32 字符 hex */
for (i = 0; i < 16; i++) g_sess.md5_local[i] = 0;      /* 占位，下面回填 */
hw_flash_md5(image, len, g_sess.md5_local);             /* 本地 16 裸字节 */

md5b[0..3] = offset;  md5b[4..7] = len;
rc = fl_req(FLASH_MD5, md5b, 8u, &v, &rbody, &rblen);
if (rblen >= 32u && rbody) {
    for (k = 0; k < 32; k++) rhex[k] = (char)rbody[k];   rhex[32] = '\0';
    for (k = 0; k < 16; k++) {                            /* ASCII hex → 裸字节 */
        uint8_t hi = (rbody[k*2]   >= 'a') ? (rbody[k*2]   - 'a' + 10) : (rbody[k*2]   - '0');
        uint8_t lo = (rbody[k*2+1] >= 'a') ? (rbody[k*2+1] - 'a' + 10) : (rbody[k*2+1] - '0');
        g_sess.md5_remote[k] = (uint8_t)((hi << 4) | lo);
    }
    for (k = 0; k < 32u; k++) if (rhex[k] != lhex[k]) break;   /* 逐字符比 */
    g_sess.md5_match = (k == 32u) ? 1u : 0u;
}
g_stat.last_md5_ok = g_sess.md5_match;
g_sess.done = 1u;  g_sess.progress = 100u;
ret = g_sess.md5_match ? HW_FLASH_R_OK : HW_FLASH_R_CHECKSUM;
```

**三个值得注意的点**：

1. **两处冗余**：`md5_local` 先清零再回填（`:766-767`）——清零毫无意义（下一行全覆盖）。**无害但无意义**。
2. **「逐字符比 hex」和「比裸字节」并存**：前者用于 `md5_match` 判定，后者填 `md5_remote` 供事后诊断。**双份数据源，都正确。**
3. **ASCII hex 解码只处理小写**（`>= 'a' ? … : …`）——若设备回**大写** hex，解码会全错。**真实 ESP32 ROM 回小写，但代码没有任何防护**。见附录 A-2。

> 📌 **`rblen >= 32` 的守卫是必要的**（`:774`）——若设备回短帧，`rbody[k*2]` 会越界读。**这个守卫把「设备行为异常」从「内存破坏」降级为「md5_match 保持 0」**。

## 10.6 错误路径（`:791-795`）

```c
done:
    if (out) *out = g_sess;              /* 无论成败都回填会话 */
    fl_set_result(ret == OK ? "run ok: md5 verified" :
                  (ret == CHECKSUM ? "run fail: md5 mismatch" : "run fail"));
    return ret;
```

> 📌 **`if (out) *out = g_sess` 在 `done:` 标签下、所有 `goto done` 都会经过** ⇒ **失败时也能拿到已写了多少块**。这是很好的 API 设计：`rc != 0` 不等于「什么都没发生」。

---

# 十一、自检 19 项（含 4 项负向）

`.c:802-966`。

## 11.1 宏与加固（`.c:828-848`）

```c
#define FL_CHK(cond, msg) do { if (!(cond)) { fails++; if (putf) putf("  [FAIL] " msg "\n"); } \
                               else if (putf) putf("  [ok] " msg "\n"); } while (0)
```

```c
/* static (真机加固): 合计 ~16.4KB —— 真机 IDLE/小栈任务里必然栈溢出
 * (实测: 把整条 selftest→run→fl_req 链放栈上 ≈25KB → IDLE1 栈溢出复位) */
static uint8_t img[4096];
static uint8_t enc[8192 + 8];
static uint8_t dec[4096];
char buf[128];
```

> 📌 **16.4KB 这个数字是实测得来的，不是估算**。知识页记录的「栈帧炸弹」修复过程：`-fstack-usage` 实测 **25KB → ≈1.0KB**。**这条注释本身就是那份事故的墓碑。**

## 11.2 正向用例（§1~7、10）

| # | 位置 | 断言 | 关键点 |
|---|---|---|---|
| 1 | `:851-853` | mode < 6 / cmd==9 / FNV==`0xAF05978A` | **黄金三连** |
| 2 | `:856-868` | SLIP 向量 `C0 DB 00 DC DD 02` → 10 字节含**两个方向** | 铁律 1 |
| 3 | `:871-876` | 4096B roundtrip（59×C0 + 47×DB）| 铁律 1 压测 |
| 4 | `:879-885` | XOR(`img[0..1024]`)==`0xFD` / MD5("")/MD5("abc")/MD5(4096)==黄金 | RFC1321 标准向量 |
| 5 | `:888-896` | 帧构造 12 字节 + `body@8` | 铁律 2 |
| 6 | `:899-910` | SYNC / READ_REG magic→9 / SPI_CMD→0 / **SEC_INFO 20B `chip_id@12`** / W0→0 | 设备语义 |
| 7 | `:913-921` | 端到端 4096B → md5_match / 4 blocks / 100% / stat | 完整链路 |
| 8 | `:956-957` | `mode_str` / `result_code_str` | 字符串助手 |

**MD5 的三个标准向量**（`:880-885`）是全模块**最硬的外部锚点**：

```c
hw_flash_md5_hex(NULL, 0, buf);                              // d41d8cd98f00b204e9800998ecf8427e
hw_flash_md5_hex((const uint8_t*)"abc", 3, buf);             // 900150983cd24fb0d6963f7d28e17f72
hw_flash_md5_hex(img, 4096, buf);                            // == HW_FLASH_IMG4096_MD5
```

> 📌 **前两个是 RFC1321 官方测试向量**（空串 + "abc"），任何 MD5 实现错误都会被抓。**第三个锁住「测试镜像生成逻辑」**——改 `i*73+11` 就会炸。

## 11.3 🔴 负向用例：单比特线路损坏（`.c:926-938`）

```c
/* 故障注入传输桩: 转发给模拟器, 但把每条 FLASH_DATA 的首个数据字节翻 1 bit */
static int fl_stub_corrupt_write(const uint8_t* d, uint32_t n)
{
    static uint8_t dec[8 + 16 + 1024 + 8];      /* static! */
    static uint8_t enc[HW_FLASH_SLIP_CAP];
    int dl = hw_flash_slip_decode(d, n, dec, sizeof(dec));
    if (dl < 0) return -1;
    if (dl >= 8 + 16 && dec[1] == FLASH_DATA)
        dec[8 + 16] ^= 0x01u;                    /* 翻首数据字节 */
    e = hw_flash_slip_encode(dec, dl, enc, sizeof(enc));
    if (e < 0) return -1;
    if (sim_write(enc, e) < 0) return -1;
    return (int)n;
}
```

```c
memset(&cb, 0, sizeof(cb));
cb.uart_write = fl_stub_corrupt_write;   /* uart_read=NULL → 回读仍走模拟器 */
hw_flash_bsp_install(&cb);
rc = hw_flash_run(img, 4096, 0x10000, &sess, NULL, NULL);
hw_flash_bsp_install(NULL);
FL_CHK(rc == HW_FLASH_R_CHECKSUM && sess.md5_match == 0, "negative: 1-bit wire corruption -> CHECKSUM");
FL_CHK(hw_flash_run(img, 4096, 0x10000, &sess, NULL, NULL) == OK, "BSP uninstall restores clean flash");
```

**这是全模块设计最精妙的一个测试**，理由有三：

1. **注释解释了「为什么不能偷懒」**（`:923-925`）：
   > 不能靠"烧前改镜像"——模拟器忠实存下所收内容, 两端仍一致; **必须让"线上数据"被损坏, 才真正考验 MD5 校验路径**

2. **`uart_read` 故意留 NULL** ⇒ **写走坏桩、回读走模拟器**（§8.6）。这实现了「数据在传输中损坏，但设备如实反映了自己收到的内容」——**这正是真实串口噪声的场景**。

3. **第二个断言是「阴性对照」**：`bsp_install(NULL)` 后必须**恢复正常**。**它同时验证了两件事**：① 桩能卸干净 ② 卸载路径不破坏默认状态。

> 📌 **这个「拆开两个方向分别注入」的技巧值得推广到其他 hw 模块**——它把「设备错了」和「链路错了」两种故障源分离开，各造各的用例。**这比只测一个 `run` 成功有价值得多。**

## 11.4 负向用例：坏 BSP → IOERR（`.c:941-953`）

```c
static int fl_stub_fail_write(...) { return -1; }
static int fl_stub_fail_read(...)  { return -1; }
...
bad.uart_write = fl_stub_fail_write;
bad.uart_read  = fl_stub_fail_read;
hw_flash_bsp_install(&bad);
rc = hw_flash_sync();
hw_flash_bsp_install(NULL);
FL_CHK(rc == HW_FLASH_R_IOERR, "negative: bad BSP -> IOERR");
FL_CHK(hw_flash_sync() == HW_FLASH_R_OK, "BSP uninstall restores simulator");
```

> 📌 **注意 `:945-946` 有个无害的自我纠正**：
> ```c
> bad.uart_write = NULL;  /* 无 write -> 走模拟器; 改用返回 -1 的桩 */
> bad.uart_write = fl_stub_fail_write;
> ```
> **第一行被第二行立刻覆盖**。注释保留了「原本想干什么 + 为什么改」。**这种「留下推理痕迹」的写法很好**——它告诉后来者「空指针 ≠ 失败，走模拟器」这个反直觉的事实。

## 11.5 汇总输出（`:959-965`）

```c
snprintf(line, sizeof(line), "hw_flash selftest: %s (%d fails)\n",
         fails == 0 ? "ALL PASS" : "FAILED", fails);
return fails;      /* 0 = 全过 */
```

> 📌 **返回失败数而非 bool** ⇒ CLI 可以报「第 3 项失败」而不仅「失败」。**但 CLI（`:1106`）只报了总数**，没报项号——**信息在传递链上被压扁了一层**。

---

# 十二、命令分发 + CLI

`.c:971-1117`。

## 12.1 `hw_flash_cmd`：10 条命令（`.c:1019-1093`）

| 命令 | 返回 | 行为 |
|---|---|---|
| `help` / 空 | `-2` | 吐命令列表 |
| `card` | 0 | 档案卡（含命令表逐条打印）|
| `mode` | 0 | 模式名 + 数值 |
| `slip` | 0/-4 | 黄金向量演示 + roundtrip |
| `md5` | 0 | 4096B 镜像 MD5 vs 黄金 |
| `sync` | rc | SYNC 握手 |
| `chip` | rc | 读 magic → "ESP32-S3" / "unknown" |
| `run [N]` | rc | 端到端烧录（N 默认 4096）|
| `verify` | rc | **发 FLASH_MD5 空载荷** |
| `stat` | 0 | 会话统计 |
| `selftest` | 0/-4 | 跑自检 |
| 其他 | `-1` | `R_NOCMD` |

### 12.1.1 `run N` 的 N 解析（`.c:1069-1072`）

```c
if (strncmp(cmd, "run", 3) == 0) {
    uint32_t n = (cmd[3] != '\0') ? (uint32_t)strtol(cmd + 3, NULL, 0) : 0u;
    return fl_cmd_run(n);
}
```

> 📌 **`strtol(..., 0)` 基数 0** ⇒ 支持 `run 0x1000` 十六进制。这是**唯一用 `strncmp` 前缀匹配的分支**（其他都是 `strcmp` 全等）。
>
> ⚠️ **`run abc` 会被 `strtol` 静默返回 0** ⇒ 落进 `fl_cmd_run(0)` ⇒ `n == 0` ⇒ 默认 4096。**用户打错字却烧了个 4096B 镜像**——🔴 **此问题已修复，见附录 A-0**。

### 12.1.2 `verify` 的可疑实现（`.c:1073-1079`）

```c
if (strcmp(cmd, "verify") == 0) {
    uint32_t v = 0, rblen = 0;
    const uint8_t* rbody = NULL;
    int rc = fl_req(HW_FLASH_CMD_FLASH_MD5, NULL, 0, &v, &rbody, &rblen);
    printf("flash: verify -> rc=%d md5_match=%u\n", rc, (unsigned)g_sess.md5_match);
    return rc;
}
```

> 🔴 **这个 `verify` 基本是坏的**，理由见附录 A-3：
> ① 载荷 `NULL/0` ⇒ 设备侧 `addr=0, size=0` ⇒ 对**空数据**算 MD5
> ② 它读的是 **`g_sess.md5_match`（上一次 `run` 的旧值）**，不是本次的
> ③ **`rbody`/`rblen` 拿到后完全没用** ⇒ 回读的 MD5 被丢弃
>
> ✅ **本节已作废（2026-10-02 修复）**：以上是**修复前**的代码。`verify` 现为
> **自包含 `verify [N]`**——区间显式给定，本地镜像用与 `run` 同一确定性发生器重建，
> 两边 MD5 各算各的真比较，返回值由匹配结果决定。
> **签名对照**：`verify` → `rc=0 md5_match=0`（旧·假成功）／ `MD5 应答长度 0` + `rc=4`（新·诚实失败）。
> 完整根因分析、越界读连带发现与受控对照实验见 **附录 A-3**。

## 12.2 `hw_flash_cli`（`.c:1095-1117`）

```c
const char* sub = (argc >= 3 && argv[2]) ? argv[2] : "card";
hw_flash_init(NULL);                    /* CLI 一次性路径也须有干净现场可读 */

if (strcmp(sub, "selftest") == 0) { ... return (fails == 0) ? 0 : 1; }
if (strcmp(sub, "help") == 0) { ... return 0; }

rc = hw_flash_cmd(sub, NULL);
return (rc >= 0) ? rc : 1;
```

> 📌 **`argv[2]` 而非 `argv[1]`**——注释（`:1097-1098`）记录了原因：
> > 家族约定: argv[1]="flash", 命令串在 argv[2] (argc>=3); 缺省 = card。
> > ⚠️ **上一轮误用 argv[1] → 永远取到 "flash" → 全分支不匹配 → exit 255 零输出**。
>
> **这是家族坑 #9 的标准修法**（我在 `hw_pin` 解析里也见到同款）。**注释保留了失败现场**，让下一次不会再犯。

> 📌 **`:1102` 显式 `hw_flash_init(NULL)`**——因为 CLI 是「一次性进程」路径，**不会走 `kvm_run` 的自动 init**。**这条注释「CLI 一次性路径也须有干净现场可读」点出了集成层的隐含契约**。

---

# 附录 A：精读中发现的 7 个问题（A-0 / A-2 / A-3 已修复）

> 以下都是**实读代码核实 + 实跑坐实**的，不是推测。每条给出代码位置、影响判定与建议。

### 状态总表（2026-10-02 收尾）

> 📌 **A 系列 7 条已全部结案**（2026-10-02）。
> A-4②、A-5、A-1 是最后三条，**均已修复 + 判别性实验 + 三路交叉编译零警告**。
> **仍挂着的两条不是 A 系列**：A-3 与 A-4② 均「模拟器已修、真机未验证」。

| 条目 | 状态 | 备注 |
|---|---|---|
| **A-0** 🔴🔴 `run 2048` 静默烧成 4096B | ✅ **已修复** | 三处改动 + 受控对照实验 |
| **A-1** `g_stat.slip_escaped` 死字段 | ✅ **已修复（2026-10-02）** | 统计点选在 `fl_req` 而非 `slip_encode`（后者会被 selftest 自测调用污染） |
| **A-2** MD5 hex 只解小写 | ✅ **已修复** | 并入 A-3，落到 `fl_hexval()` |
| **A-3** 🔴 `verify` 假成功 | ✅ **已修复** | 四重缺陷 + 越界读 + 受控对照实验 |
| A-4 两处静默接受非法输入 | ✅ **已修复（2026-10-02）** | ① 并入 A-0 / ② SYNC 8 帧残留：夹具三层作弊 + `fl_req` opcode 校验 + 判别性实验 |
| **A-5** `hw_flash_init` 清空 BSP | ✅ **已修复（2026-10-02）** | 与 `hw_dc`/`hw_pin` 对齐：init 不碰 BSP，selftest 9b 断言护栏 |
| A-6 全族 CLI 契约不一致 | ✅ **已结案（结论：不动）** | 深挖后确认是**结构差异非风格差异**，见该条 |

> ⚠️ **A-3 的「已修复」不等于「verify 可用」**——模拟器下它恒诚实失败（`rc=4`），
> 且 selftest 里的 `md5 verified` 是**自证循环**，证明不了真机会 MATCH。
> 真机验证**本轮未做，不得记为已验证**。详见 A-3 的「局限声明」。

## A-0 🔴🔴 已修复：`./xiaomo flash run 2048` 静默烧成 4096B

**本模块最严重的问题，而且是实跑抓出来的，不是读代码看出来的。**

### 症状

```
$ ./xiaomo flash run 2048
flash: 镜像 4096 B (确定性模式镜像, 含 C0/DB 转义字节)   ← 用户要的是 2048
$ ./xiaomo flash run 1024
flash: 镜像 4096 B                                          ← 一模一样
$ ./xiaomo flash run 0x800
flash: 镜像 4096 B                                          ← 十六进制也被吞
$ ./xiaomo flash run abc
flash: 镜像 4096 B ... flash: OK                          ← 错别字也报成功
```

**用户以为烧了 2048B，实际烧了 4096B，而输出里没有任何警告。**

### 根因

原 `hw_flash_cli`（修复前）：

```c
const char* sub = (argc >= 3 && argv[2]) ? argv[2] : "card";
/* argv = ["xiaomo", "flash", "run", "2048"]
   argv[2] = "run"   argv[3] = "2048"  ← argv[3] 从未被读 */
```

子命令走 `strncmp(cmd, "run", 3)` **前缀匹配**，尺寸靠 `strtol(cmd + 3, ...)` 从**同一根字符串**里取。`argv[2]` 被截成 `"run"` ⇒ `cmd+3` 是 `\0` ⇒ `n = 0` ⇒ `fl_cmd_run(0)` 里 `if (n == 0) n = 4096`。

> 📌 **判别关键**：`hw_dc`/`hw_core` 用 `strcmp` + `argv[3]` **独立解析**，而 `hw_flash`/`hw_pin` 用 `strncmp` + **拼接**。**只有「前缀匹配」这一种形态才需要拼接**——这是本条能藏住的根本原因。

### 双重静默

| 环节 | 表现 |
|---|---|
| `strtol("abc")` | 解析失败**静默返回 0**，无 `errno`/`endptr` 检查 |
| `n == 0 → 4096` | 兜底默认值，把「没给参数」和「参数非法」混为一谈 |

**两个静默叠在一起 ⇒ 错别字变成一个成功的烧录操作。**

### 修复（三处，照抄 `hw_pin_cli` 的成熟修法）

**① `hw_flash_cli` 补 argv 拼接**（`:1112-1133`）——含溢出保护，且**保留带引号的单参数写法**：

```c
static char cmdline[256];
const char* sub = "card";
if (argc >= 4) {
    for (i = 2; i < argc; i++) {
        size_t l = argv[i] ? strlen(argv[i]) : 0u;
        if (off + l + 2u >= sizeof(cmdline)) break;      /* 溢出保护 */
        if (i > 2) cmdline[off++] = ' ';
        if (l) memcpy(cmdline + off, argv[i], l);
        off += l; cmdline[off] = '\0';
    }
    sub = cmdline;
} else if (argc >= 3 && argv[2]) {
    sub = argv[2];
}
```

**② `run` 分支校验 `endptr`**（`:1069-1083`）——错别字必须变错误：

```c
uint32_t n = 4096u;                        /* 默认值上移到「未给参数」分支 */
if (cmd[3] != '\0') {
    char* end = NULL;
    long v = strtol(cmd + 3, &end, 0);
    if (end == cmd + 3 || *end != '\0' || v < 0) {       /* 未消费/有残留/负数 */
        printf("flash: bad size in \"%s\" (期望整数, 例: run 4096 / run 0x1000)\n", cmd);
        return HW_FLASH_R_BADARG;
    }
    n = (uint32_t)v;
}
```

**③ 删掉 `fl_cmd_run` 里的 `if (n == 0) n = 4096`**——默认值已上移，留着会继续把「显式传 0」误当成「没传」。

### 判别性实验（受控对照，不是"跑一次通过"）

**本次修复最关键的一步——必须证明新测试有区分力，而不是恒真断言。**

| | `run 2048` 实际 | 新测试项 |
|---|---|---|
| **回退版**（bug 在） | `镜像 4096 B` | 🔴 `[FAIL] flash CLI 空格参数 (rc=0/0/0)` |
| **修复版** | `镜像 2048 B` | 🟢 `[PASS] flash CLI 空格参数` |

**做法**：`cp -R Makefile include src examples tests /tmp/flash_ab/` 建**干净副本**，**只在副本**上回退拼接段（**主源码全程不动**），跑**同一份** `tests/run_tests.sh`。判别力成立后**清理副本**。

> 📌 **判别力自证时踩到的坑**：第一次做回退时，我的 Python 切割脚本用 `s.index('int rc;')` 定位，**匹配到了 `hw_flash_cmd` 里的同名变量**，把函数边界切坏 → 20 个编译错误。而 `make 2>&1 | tail -2` **把错误码吞了**，只看输出才发现。
> 这是知识页锚点 **F（检索/工具静默失败）** 的又一次现场重演：**管道尾端的退出码不是被测命令的退出码**。`$?` 取管道**最后一环**（`tail`/`head`）的码。

### ⚠️ 这个 bug 为什么能活到今天

**测试恰好绕开了它。** `tests/run_tests.sh:405` 一直是这样写的：

```bash
fl1_out=$("$BIN" flash "run 4096" 2>&1)      # ← 带引号的单参数写法
```

而下面第 420 行（`.mo` 侧）：
```bash
#   hw_flash("run ", 2048) → "run 2048" → 0        ← 双参 imm 动态拼接, 测了
```

**`.mo` 侧的动态拼接测得很细（`run 2048` / `run 99999`），唯独 CLI 的空格写法从未测过。**

> 📌 **通用教训**：**一个功能的「所有调用形态」里，如果只测了其中一种，测试矩阵的覆盖感会严重高估实际覆盖率。**
> 本例中「双参 imm 动态拼接」测得那么细，反而**制造了"这个功能测得很全"的错觉**。

新增回归（`tests/run_tests.sh:418-441`），用**非 4096 的尺寸**做判别（若参数被吞则一律输出 4096）：

```bash
flsp_out=$("$BIN" flash run 2048 2>&1)       # 期望 "镜像 2048 B"
flbad_out=$("$BIN" flash run abc 2>&1)       # 期望 bad size + rc!=0
flbig_out=$("$BIN" flash run 99999 2>&1)     # 期望 model cap + rc!=0
```

**修复后全族回归：`66 通过 / 0 失败`**（基线 65 + 本条）。

> 🕳️ **本条同时暴露了另外 5 个模块的同类契约不一致**——见 A-6。

## A-1 ✅ 已修复（2026-10-02）：`g_stat.slip_escaped` 是死字段

**原证据**（三处，全文无第四处）：

```
.h:143   uint32_t slip_escaped;    /* 编码时转义字节数 */   ← 声明
.c:583   memset(&g_stat, 0, ...)                              ← init 清零
.c:599-602 hw_flash_stat() → 拷贝整个 g_stat                  ← 唯一读取路径
```

**全文零写入点。** `stat` 里这一栏永远显示 0 ⇒ 用户想用「转义字节占比」
评估链路健康度或判断镜像是否含大量特殊字节，**拿不到数据**。

**修法**：在**真实发帧点 `fl_req`** 逐字节统计帧内需要转义的字节（`C0` / `DB`）。

> ⚠️ **刻意没在 `hw_flash_slip_encode` 内部计数。** 它是公开 API，
> selftest 会**直接调 4 次**（黄金向量 / 往返 / 4096B 镜像）——
> 在函数内计数会被**自测调用污染**，读数失真就失去了诊断价值。
> 原笔记建议的「给 `slip_encode` 加计数出口」正是这条陷阱。

**判据**：

```c
FL_CHK(st.slip_escaped > 0u, "slip_escaped > 0 (转义统计非死字段)");
```

取「> 0」而非精确值：统计口径与镜像内容 / 分块策略耦合，精确值会随分块策略漂移。
但**恒为 0 一定是坏值**——确定性镜像（含 `C0`/`DB`，selftest 第 5 项已断言）
走完整链路后必有转义发生。


## A-2 ✅ 已修复（2026-10-02，并入 A-3）：MD5 hex 解码只处理小写

> **状态：已修复。** 修 A-3 时引入的 `fl_hexval()` 顺手覆盖了本条。
> `grep -nE ">= *'a'|- *'a' *\+" src/hw/hw_flash.c` 现只剩 **3 处命中，全部在注释与
> `fl_hexval` 函数体内部**（无残留的旧式解码）。两个解码点（`run` 的 `.c:808-809`、
> `verify` 的 `.c:1173-1174`）已全部改道 `fl_hexval`。

**原位置**：`.c:779-780`（修复前）

```c
uint8_t hi = (uint8_t)((rbody[k*2]   >= 'a') ? (rbody[k*2]   - 'a' + 10) : (rbody[k*2]   - '0'));
uint8_t lo = (uint8_t)((rbody[k*2+1] >= 'a') ? (rbody[k*2+1] - 'a' + 10) : (rbody[k*2+1] - '0'));
```

**问题**：分支条件只有 `>= 'a'`。如果设备（或未来某固件）回**大写** hex（`A`~`F`），`0x41 >= 0x61` 为假 ⇒ 走 `- '0'` 分支 ⇒ `0x41 - 0x30 = 0x11` ⇒ **完全错误的字节值**。

**影响**：真实 ESP32 ROM 回小写，所以**现在不会触发**。但这是**「依赖外部行为恰好正确」的典型脆弱点**——没有断言、没有告警、没有任何防护。

**建议**：解码前先校验字符集，或加大小写双向：

```c
static int hexval(uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;                       /* 非法字符 → 应报 CHECKSUM 而非乱算 */
}
```

> ✅ **上述代码已按 `fl_hexval` 落地**（`.c:635-641`），命名从 `hexval` 改为 `fl_hexval`
> 以避免与文件内其它符号撞名。**比原建议多做了一步**：非法字符不只是「乱算」，
> 而是**显式拒绝**——`verify` 路径收到非 hex 会直接报 `MD5 应答非 hex` 并返回 PROTO，
> **垃圾字节不许静默变成「看起来合法」的数值**。

**这与 `hw_dmc` 的「负向用例必须取首个负返回码并立即 break」是同一类问题**——解码器不校验输入域。

## A-3 ✅ 已修复（2026-10-02）：`verify` 的四重缺陷 + 一处越界读

> **状态：已修复，已通过受控对照实验。** 修复后签名 `verify N → MD5 应答长度 0 / rc=4`（诚实失败），
> 修复前签名 `verify → rc=0 md5_match=0`（假成功）。两版对照见下节。

**原位置**：`.c:1073-1079`（修复前）

| # | 缺陷 | 后果 |
|---|---|---|
| ① | 载荷传 `NULL, 0` ⇒ 设备侧 `addr=0, size=0` | 对**空数据**算 MD5，与上次烧的镜像无关 |
| ② | 打印 `g_sess.md5_match` | 那是**上一次 `run` 的旧值**，不是本次结果 |
| ③ | `rbody`/`rblen` 收到后未使用，直接 `return rc` | 设备真实回读的 MD5 被丢弃；且 `rc` 是**协议层**结果，与匹配与否无关 |
| ④ | hex 解码 `(c>='a') ? c-'a'+10 : c-'0'` | 遇**大写** `'A'` 走错分支（`0x41-'0'=0x11`），全错且不报错 |

**净效果**：`./xiaomo flash verify` 输出 `rc=0 md5_match=0`——**自相矛盾的「成功」**。
这是「假成功」的一种：不是返回错误，而是返回了一个无意义却看起来正常的结果。

### 🕳️ 比四个缺陷更深的一层根因（第一版修法就栽在这）

我第一版修法是「让 verify 复用 `g_sess` 记录的 `offset/image_size`」——**当场被自己的测试推翻**：
`run` 成功后再 `verify`，仍报「无烧录会话」。

根因：`g_sess` / `g_model` 都是**进程内 static 状态**，而 CLI 的 `run` 与 `verify` 是
**两个独立进程**。⇒ 任何依赖 `g_sess` 的 verify，**在 CLI 下结构上是死命令**。

> 📌 **可迁移教训**：把「库 API 的连续调用」（`run()` 后 `verify()` 读 `g_sess`）
> **直接暴露成 CLI** 本身就是错的设计。CLI 的契约是「一次调用一件事、自包含」，
> 跨调用的隐式状态在 CLI 里永远是全零。凡是「CLI 里有个命令依赖上一条命令的残留状态」，
> 都要按结构性缺陷处理，而不是「初始化一下就好」。

**正确语义**：`verify` 改为**自包含** `verify [N]`——区间由命令行显式给出，本地镜像用
与 `run` **相同的确定性发生器**（`hw_flash_test_image`）重建，两边 MD5 各算各的再比，
不依赖任何进程内状态。

### 🔴 顺带揪出的越界读（比 A-3 本身更严重）

修复后 `verify` 恒 `MISMATCH`，追查发现设备侧模拟器 `.c:497`：

```c
const uint8_t* base = g_model + (addr >= g_model_off ? (addr - g_model_off) : 0u);
```

**零边界检查**。`verify` 传 `addr=0x10000` 而 `g_model` 只有 32768 字节
⇒ **越界读 32KB 之外的相邻内存**，MD5 是个「随机但看起来合法」的值，
且读到什么取决于链接布局 ⇒ **同一条命令两次跑结果可能不一致**。

> 📌 附带发现的坐标系错误：`FLASH_DATA` 是按 `seq * g_model_blk` **线性**存的，
> 与这里的 `addr - g_model_off` **不是同一套坐标系**。即使不越界，MD5 也是错的。

已加显式区间检查，越界时**诚实失败**而不是给一个假 MD5。

### 修复清单

| 处 | 改动 |
|---|---|
| 客户端 `.c:1097+` | `verify` 改为自包含 `[N]`；真比较 `memcmp`；返回值由匹配结果决定 |
| 客户端 | 参数校验同 `run`（`endptr` + 完整消费 + `>0`），**错别字必须报错，不猜量级**（A-0 教训） |
| 客户端 | 引入 `fl_hexval()`，**大小写通吃 + 显式拒绝非 hex 字符**（垃圾字节不许静默变「合法」值） |
| 设备侧 `.c:497` | 显式区间检查（`g_model_len==0 \|\| rel>=g_model_len \|\| size>g_model_len-rel` ⇒ 回 CHECKSUM） |

### 判别性实验（受控对照，不是「跑一次通过」）

**做法**：`cp -R` 建干净副本 → 只在**副本**上 `git show HEAD:src/hw/hw_flash.c` 回退（主源码全程不动）
→ 强制重编 → 跑**同一份** `tests/run_tests.sh`。

| | A-0 测试 | A-3 测试 | 汇总 |
|---|---|---|---|
| **回退版**（602480B） | 🔴 `rc=0/0/0` | 🔴 `rc=0/0/0/0` | **65 / 2 失败** |
| **修复版**（602808B） | 🟢 | 🟢 | **67 / 0** |

回退版实跑签名（**与修复前的记录逐字吻合**）：

```
$ flash verify        →  flash: verify -> rc=0 md5_match=0      rc=0   ← 假成功
$ flash run 2048      →  flash: 镜像 4096 B (确定性模式镜像...)          ← A-0 指纹
```

**判别力成立**：两条测试都精确地在「有 bug 的那版」失败、在修复版通过。

### ⚠️ 局限声明（必须诚实，不可算作「verify 完全可用」）

**模拟器下 `verify` 永远诚实失败（`rc=4`）**，因为 `g_model` 是进程内 static，
跨进程的 verify 必然读到空设备。**这是模拟器的固有局限，不是本次改动引入的。**

因此新测试锁的是「**不许假成功**」，**不是「能 MATCH」**。

> 🕳️ 更需要警惕的是：selftest 里的 `md5 verified` 是**自证循环**——`hw_flash_run` 取设备 MD5
> 走的 `fl_req` 在模拟器下直接回读刚写入的**同一个 `g_model`**，两边必然一致。
> **它证明不了真机上也会 MATCH。** 真机验证需另行安排（本轮未做，不得记为已验证）。

## A-4 ✅ 已修复：两处「静默接受非法输入」

> ①（`run abc` 静默变 4096B）已在 A-0 中修复；②（SYNC 8 帧）已在 2026-10-02 单独修复。
> 本条已**全部结案**。

### ① `run abc` → 默认烧 4096B

`.c:1070`：`strtol("abc", NULL, 0)` 返回 **0**（不报错），`fl_cmd_run(0)` 里 `if (n == 0) n = 4096`。

**用户打错字，程序安静地烧了个 4096B 镜像并报 OK。**

**建议**：`strtol` 后检查 `errno` 或用 `endptr` 验证至少消费了一个字符。

### ② ✅ 已修复（2026-10-02）：SYNC 的 8 帧残留

#### 原判断被实测证伪

> ⚠️ **本节开头曾写着「为什么真机实测没炸：残留会被下一条命令的解码器当成噪声，
> 而 `slip_decode` 遇到前导 `C0` 会 `while` 跳过 ⇒ 恰好被容错掉了」。**
>
> **这个推断是错的。** 错在：残留的 SYNC 帧**每一帧都是完整合法帧**
> （`01 08 00 00 20 12 12 20 00 00 C0`），根本不会退化成「噪声」——
> 下一条命令 `fl_req` 从 `rsp[0]` 解出来的**就是它**，而且 `fl_req`
> **从不比较 `rcmd` 与 `cmd`**，于是 `0x20121220` 被当成应答值直接交出去。
>
> 📌 **「真机没炸」不等于「设计正确」**——它只是说明真机那次实测
> 恰好没走到会暴露问题的调用次序上。这条已被下面的夹具修复**当场证伪**：
> 一旦夹具忠实，`run` 立刻 `rc=5 (CHECKSUM)`。

#### 根因链（三层，缺一层都测不出来）

| 层 | 位置 | 问题 |
|---|---|---|
| 协议层 | `fl_req` | ① 累积缓冲 `acc` 每次调用归零，解出首帧即 `break`，残余字节**被丢弃**；② 解析出 `rcmd` 却**从不与 `cmd` 比较**；③ 没有「消费长度」概念——而 `slip_decode` 返回的是**解码后**长度，有转义时 ≠ **消费**长度，**想丢都丢不对** |
| 解码器 | `hw_flash_slip_decode` | 缺「消费长度」出口（已补 `hw_flash_slip_decode_ex`，旧行为逐位不变） |
| **夹具** | `sim_reply` / `sim_write` / `sim_read` | **三层作弊，见下** |

#### 🔴 最大发现：夹具有三层作弊（锚点 G 教科书案例）

`:441` 注释直接自曝：`if (e) tot = e; /* 只保留最后一帧给上层读 */`

| # | 位置 | 作弊内容 | 后果 |
|---|---|---|---|
| ① | `sim_reply` | `g_rsp_len = e` **覆盖** | SYNC 8 帧只留 1 帧 |
| ② | `sim_write` | `g_rsp_len = 0` 每请求清空 | 未读完的旧字节被扔掉 |
| ③ | `sim_read` | 一次性吐光整个缓冲 | 模拟「同步全量交付」，真 UART 不会 |

**每一层单独都足以保证「永远只有一个在途应答」** ⇒ 上层的多帧缺陷**结构性隐形**。
这就是**自证工具与被测物共享盲点**（锚点 G）：夹具替上层做了它该做的工作，
测的其实是「我们希望的样子」。

> 🕳️ **我分三次才找全三层。** 前两层改完实跑仍 `OK`，差点记成「改了但缺陷不复现」
> 的**假阴性**。一次「改了没复现」只说明**还有一层在挡**，不等于缺陷不存在。

#### 修改

**夹具（3 处）**：`sim_reply` 覆盖→**追加**（溢出诚实失败）；`sim_write` 去掉清零；
`sim_read` 改为**一次只交付一个完整 SLIP 帧**（扫到收尾定界 `C0` 为止；`cap` 装不下则交付半帧，
以考验上层累积重试循环）。

**解码器**：`hw_flash_slip_decode_ex(..., uint32_t* consumed)`——`consumed`
**仅在确实解到收尾定界时**写入，否则置 0（若把「已扫过的字节」当已消费会切掉半帧）。
`hw_flash_slip_decode()` 改为薄封装，**旧行为逐位不变**。

**`fl_req`**：改用 `decode_ex` 取 `consumed` → `memmove` 把解出的帧从累积缓冲里摘掉
→ `raw[1] != cmd` 判为**残留帧**，计入 `g_stat.stale_frames` 并 `continue` 继续读
→ 直到读到 opcode 对得上的帧。

**`stale_frames` 是让修复可观测的关键**：没有它，「残留被丢弃」和「残留被误当应答」
在日志上**长得一模一样**。

> 📌 **未采用「SYNC 后显式排空 7 帧」方案**（原建议）。理由：opcode 校验是
> **通用机制**，对任何命令都健壮；「sync 专门排空」只对 sync 有效。

#### 判别性实验（回退 `fl_req`，保留忠实夹具）

| 夹具 | `fl_req` | selftest |
|---|---|---|
| 作弊 | 有缺陷 | **ALL PASS（假绿）** |
| **忠实** | **回退到有缺陷** | **9 fails** |
| 忠实 | 已修复 | ALL PASS（真绿） |

回退后 9 个 FAIL 中，`READ_REG magic` 也挂——残留 SYNC 帧的 `val=0x20121220`
顶替了 magic 读数；`stale_frames >= 7` 判据明确 FAIL（读数 0）。

**同一份 selftest，夹具忠实后才真正有判别力。**

#### 修复前后的实跑对照

```
夹具修好、fl_req 未修：  flash run 2048 → rc=5 (CHECKSUM)   ← 缺陷暴露
夹具修好、fl_req 已修：  flash run 2048 → rc=0, MD5 VERIFIED
```

**纪律**：夹具修好后的第一件事是「**看见缺陷**」，不是「宣布修好」。

#### 新增判据

```c
FL_CHK(st.stale_frames >= 7u, "stale_frames >= 7 (SYNC 8 帧的残留被识别丢弃)");
```

用「≥7」而非精确值：调用次序会改变残留总数，但只要 SYNC 真吐 8 帧，
`stale_frames` **为 0 就不可能**——而「为 0」恰恰是夹具作弊时的读数，故此下界有判别力。

#### 验证

- 编译：宿主 C++17 / **C11 `-Wall -Wextra` 零警告**
- 交叉：`riscv32-esp-elf` + `xtensa-esp-elf` **freestanding 零警告**
- `flash selftest`：**ALL PASS (0 fails)**（含新判据）
- 全族回归：**67 / 0** 零退化
- **真机未验证**（本轮全程未碰真机，见下）

#### ⚠️ 局限声明

**本条全部证据来自模拟器。真机行为未验证，不得记为已实证。**
且模拟器是本轮**亲手证明不可信**的那个工具（夹具三层作弊）。

残留帧的**具体分布**依赖真实 ROM 的吐帧时序——`stale_frames` 正是为此设计的观测口：
**真机上若此值持续为 0 而 SYNC 后 `chip_id` 又不对，说明固件只回了 1 帧。**


## A-5 ✅ 已修复（2026-10-02）：`hw_flash_init` 清空 BSP

`.c:650`（修前）：`memset(&g_bsp, 0, sizeof(g_bsp));`

**与另外两个模块完全同一个错误模式**：
- `hw_dc`：`hw_dc_init()` 静默抹掉已装 BSP（知识页记为「头号假成功陷阱」）
- `hw_pin`：`hw_pin_init` 故意**不**清 BSP（`:1189-1197` 注释记了完整因果）
- `hw_flash`：原为**清了**，本轮对齐前两者

**为什么此前没出事**：`init` 由 `kvm_run` 上电自动调用（`vm_core.c:475`），
真机固件在 `app_main` 里 install BSP **之后**不会再触发 `kvm_run`。

> ⚠️ **但这只是调用时序的巧合，不是设计保证。**
> 后果是**静默的假成功**：真机装好 UART 绑定后，VM 一启动就把 BSP 抹掉，
> 命令照样返回 OK，烧的却是 ROM 模拟器而非那块板子。

**修法**（与 `hw_dc` / `hw_pin` 完全对齐）：**`init` 不碰 `g_bsp`**，
BSP 生命周期完全由 `hw_flash_bsp_install()` 管理，卸载只走 `bsp_install(NULL)`。
`init` 仍清会话 / 统计 / 镜像模型 / 应答缓冲。

**判别性断言（先失败后修）**：

```c
hw_flash_bsp_install(&cb);        /* cb = 读写都失败的桩 */
hw_flash_init(NULL);              /* 模拟 kvm_run 上电自动调用 */
FL_CHK(hw_flash_sync() == HW_FLASH_R_IOERR, "A-5: init 不得清掉已装 BSP");
```

修前该断言 **FAIL**（桩被清掉 ⇒ 命令静默走模拟器返回 OK ⇒ 假成功坐实）；
修后 **[ok]**。

**验证**：`flash selftest` ALL PASS（0 fails）· `flash run 2048` 仍 VERIFIED
（CLI 入口也调 `init`，未退化）· 全族回归 **67 / 0** ·
C11 / riscv32 / xtensa freestanding 三路**零警告**。

**真机未验证**（本轮全程未碰真机）。


## A-6 🔴 全族 CLI 契约不一致（普查发现）

**修完 A-0 顺手普查全部 12 个 hw 模块的 CLI 参数读法，发现两套契约并存**：

| 模块 | 带参子命令读法 | 空格写法 | 引号写法 |
|---|---|---|---|
| `hw_core` | `argv[3]` 独立解析 | ✅ | ❌ |
| `hw_dc` | `argv[3]` 独立解析 | ✅ | ❌ |
| `hw_fault` | `argv[3]` 独立解析 | ✅ | ❌ |
| `hw_main` | `argv[3]` 独立解析 | ✅ | ❌ |
| `hw_asr` | `argv[3]`+`argv[4]` 两个**独立位置参数** | ✅ | ❌ |
| `hw_flash` | 拼接（本次修复） | ✅ | ✅ |
| `hw_pin` | 拼接（早就有） | ✅ | ✅ |
| `hw_wdbg` | 拼接 | ✅ | ✅ |
| `hw_dev`/`hw_token`/`hw_oem`/`hw_hex` | 无带参子命令 | — | — |

**实跑证据**：

```
$ ./xiaomo dc sig 3          →  MININ = 0 (输入信号最小值)   ✅ 空格可用
$ ./xiaomo dc "sig 3"        →  === xiaomo hw_dc ... ===       ❌ 退化成 card
$ ./xiaomo core idx 1        →  DNA[1] = DFF 0x0DFF (3583)     ✅
$ ./xiaomo core "idx 1"      →  === xiaomo hw_core ... ===     ❌
```

**影响**：`./xiaomo flash "run 4096"`（带引号）能用，`./xiaomo dc "sig 3"`（带引号）却**静默退化成 `card`**。**同一个二进制，两种心智模型。**

> 📌 **判定要区分两种形态**：`hw_asr` 虽然有 `argv[3]/argv[4]`，但那是**两个独立位置参数**（`enroll <file> <label>`），不是「一个带空格的子命令」——**不是同款 bug**。

**建议**：**统一到 `hw_pin`/`hw_flash` 的拼接式**（已验证成熟、有溢出保护、**且向后兼容空格写法**）。这是**纯增量修改，不破坏任何现有调用**。


---

# 附录 B：自检项 ↔ 代码行对照表

| 自检项 | 断言内容 | 验证的代码 | 对应铁律/概念 |
|---|---|---|---|
| `:851` | `mode < 6` | `fl_mode_probe` (§4.1) | 六模式 |
| `:852` | `cmd == 9` | `g_cmd_table` (§5.1) | 命令表完整性 |
| `:853` | `FNV == 0xAF05978A` | `hw_flash_cmd_checksum` (§5.1) | **黄金值** |
| `:863` | SLIP 黄金 10 字节 | `slip_encode` (§7.1) | **铁律 1** |
| `:867` | SLIP 解码回原值 | `slip_decode` (§7.2) | 铁律 1 |
| `:876` | 4096B roundtrip | 两者 + 测试镜像 | 铁律 1 压测 |
| `:879` | XOR == `0xFD` | `hw_flash_checksum` (§5.2) | 铁律 2 |
| `:881` | MD5("") | `fl_md5_*` (§6) | **RFC1321 外部锚点** |
| `:883` | MD5("abc") | 同上 | **RFC1321 外部锚点** |
| `:885` | MD5(4096) == 黄金 | 同上 + 测试镜像 | 黄金值 |
| `:895` | 帧 12B + body@8 | `frame_build` (§7.3) | **铁律 2** |
| `:899` | SYNC | `fl_req` + `sim_handle` | 协议原语 |
| `:901` | magic → 9 | `sim_read_reg` (§8.3) | 设备语义 |
| `:903` | SPI_CMD → 0 | 同上 | 设备语义 |
| `:907` | SEC_INFO 20B `chip_id@12` | 同上 | 设备语义 |
| `:910` | W0 → 0（未发 RDID）| 同上 | 设备语义 |
| `:915` | 端到端 OK | `hw_flash_run` (§10) | 完整链路 |
| `:916-918` | md5_match / 4 blocks / 100% | 同上 | 会话状态 |
| `:920` | sessions / chip_id | `g_stat` | 统计 |
| `:934` | **1-bit 损坏 → CHECKSUM** | `fl_stub_corrupt_write` (§11.3) | 🔴 **负向** |
| `:937` | BSP 卸载后恢复正常 | `bsp_install(NULL)` | **阴性对照** |
| `:951` | 坏 BSP → IOERR | `fl_stub_fail_*` (§11.4) | 🔴 **负向** |
| `:952` | 卸载后模拟器恢复 | 同上 | **阴性对照** |
| `:956-957` | `mode_str` / `code_str` | (§4.3) | 字符串助手 |

**统计**：正向 **22** 项 / 负向 **2** 项 / 阴性对照 **2** 项（负向的恢复断言）。

> 📌 **「阴性对照」是本模块比 `hw_pin` 更强的地方**——每个负向用例后面都跟一个「卸载 BSP 后恢复正常」的断言。**这确保负向测试本身没有破坏后续测试的前提**（知识页锚点 C「干净副本」的精神）。

---

## 结语：本模块的三条可迁移经验

1. **SLIP 转义方向必须用「含双向陷阱字节」的镜像验证**（§5.3 + §11.2-2）——roundtrip 自洽不能证明方向对，2MB 镜像碰巧无特殊字节是**假通过**的经典。
2. **故障注入要拆开「写」和「读」两个方向**（§11.3）——只装 `uart_write` 让 `uart_read` 走模拟器，才能造出「链路坏但设备如实」的真实场景。
3. **负向用例后必须跟「恢复」断言**（§11.3、§11.4）——否则负向测试可能让后续测试在污染的环境里「假绿」。

---

*本文档基于 2026-10-02 实读 `include/hw_flash.h`（226 行）与 `src/hw/hw_flash.c`（1117 行）写成，100% 覆盖。文中所有行号、常量、黄金值均来自源码原文，可用 `sed -n 'Np'` 直接核对。*
