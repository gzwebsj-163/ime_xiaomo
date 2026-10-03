# 逐行代码注解（二）按键层

> 对应源码：`main/btn_decode.h`(126行) + `main/btn.h`(112行) + `main/btn.c`(187行)
> 这一层是全工程**最值得学习的部分**——它记录了一次从"看似合理的实现"到
> "真机打脸 → 定位根因 → 结构性修复"的完整过程。

---

# 一、`btn_decode.h` — 纯函数解码状态机（126 行）

## 1.1 为什么要拆成独立文件（第 1-36 行注释）

```c
/**
 * btn_decode.h — 「电平 → 按键事件」解码状态机（纯逻辑，零 RTOS/驱动依赖）
 *
 * 为什么单独拆出来：
 *   长按不触发这类 bug 出在「物理触点 → 事件队列」这一段。
 *   往队列里注入合成事件的自检【结构上抓不到它】（注入点已在这段之后）。
 *   拆成纯函数后，可以用宿主单元测试喂合成波形（含瞬断/弹跳）来验证。
 *   测试见 tools/btn_decode_test.c
 */
```

> **这段注释解释了本工程最重要的一条方法论。**
>
> 我们已经有了"注入合成按键事件"的自检机制（`btn_inject()`），看起来能覆盖按键全链。
> **但它抓不到长按不触发** —— 因为注入点在解码器**之后**，直接跳过了出问题的代码。
>
> **教训**：当一种测试方法有结构性盲点时，不是"再加一个测试"就能解决的，
> 而是要**改造被测代码的形态**（抽成纯函数）让盲点消失。
>
> `static inline` + 头文件 = 不需要单独编译单元、零函数调用开销，
> 同时逻辑完全独立可测。这是 C 里"可测性 vs 性能"的最佳折中。

## 1.2 时间参数（第 41-45 行）

```c
#define BTN_POLL_MS        10    /* 采样周期 */
#define BTN_PRESS_N         3    /* 3×10ms  =  30ms 按下确认 */
#define BTN_RELEASE_N      30    /* 30×10ms = 300ms 抬起确认（瞬断静默期） */
#define BTN_TRUE_RELEASE_N 50    /* 50×10ms = 500ms 判定「真松开」（解除静默） */
```

> **这三个数字是非对称的，这是本文件的精髓。**
>
> | 参数 | 值 | 作用 | 为什么是这个值 |
> |---|---|---|---|
> | `PRESS_N` | 30ms | 抗毫秒级弹跳 | 跟手，不拖泥带水 |
> | `RELEASE_N` | 300ms | 抗慢速瞬断 | **关键**：按住时触点会慢速瞬断（实测 180ms），300ms 能吞掉 |
> | `TRUE_RELEASE_N` | 500ms | 判定真松开 | 比 300ms 更长，确保"真的松手了"才解除静默 |

## 1.3 状态结构体（第 47-55 行）

```c
typedef struct {
    uint8_t  last_raw;     /* 上次原始电平（1=按下） */
    uint8_t  stable;       /* 去抖后的稳定电平 */
    uint16_t cnt;          /* 连续一致采样计数 */
    uint8_t  holding;      /* 是否处于「按住中」（含瞬断静默期内） */
    uint8_t  long_sent;    /* 本次按住的长按事件是否已上报 */
    uint8_t  mute;         /* 长按已发出 → 静默，等真松开 */
    int64_t  press_us;     /* 本次按住的起始时刻 */
} btn_dec_t;
```

> **7 个字段，每个都有明确职责**：
>
> | 字段 | 作用 | 删掉会怎样 |
> |---|---|---|
> | `last_raw` | 边沿检测起点 | 每次都当有新变化，`cnt` 永远归零 |
> | `stable` | 去抖后的状态机状态 | 分不清"按下沿"和"抬起沿" |
> | `cnt` | 连续一致计数 | 无法做非对称去抖 |
> | `holding` | 按住中标志 | 瞬断时按压时刻被重置 → **长按永远凑不满**（原 bug） |
> | `long_sent` | 本次长按是否已发 | 长按会重复触发 |
> | `mute` | 长按后静默 | 长按尾部的抖动变成乱翻页 |
> | `press_us` | 按压起始时刻 | 无法算长按时长 |

> **注意 `holding` 和 `stable` 是两个独立状态**：
> - `stable` = 电平去抖后的结果（可能被瞬断影响）
> - `holding` = "用户意图上还在按住"（瞬断**不会**把它清掉）
>
> 这个分离就是"瞬断不结束按住"的实现方式。**这是修复原 bug 的核心。**

## 1.4 返回值定义（第 57 行）

```c
enum { BTN_EV_NONE = 0, BTN_EV_SHORT = 1, BTN_EV_LONG = 2 };
```

> 不用 `typedef enum` 而是匿名 `enum`，因为只需要常量不需要类型名。
> 匿名 enum 的常量在文件作用域可见。

## 1.5 `btn_decode_step()` 主体（第 60-126 行）

### 签名（第 60-62 行）

```c
static inline int btn_decode_step(btn_dec_t *s, uint8_t raw, int64_t now_us,
                                  int long_ms)
```

> **四个参数的设计意图**：
> - `s`：状态（调用方持有，每个键一个）
> - `raw`：本次采样的原始电平（1=按下，已做极性转换）
> - `now_us`：当前微秒时间戳（用 `esp_timer_get_time()`）
> - `long_ms`：长按阈值（传进来而不是宏写死 = **可测试、可调**）
>
> `static inline` 让编译器在调用点直接展开，无函数调用开销。

### ① 连续一致采样计数（第 63-69 行）

```c
if (raw != s->last_raw) {
    s->last_raw = raw;
    s->cnt = 0;
} else if (s->cnt < 60000) {
    s->cnt++;
}
```

> **边沿检测**：电平变了 → 重置计数；没变 → 累加。
>
> **`cnt < 60000` 是饱和保护**：`uint16_t` 最大 65535，长按超过 655 秒（约 11 分钟）
> 会溢出回绕。饱和在 60000 避免这个，虽然极端情况下没人会按 11 分钟。
>
> **`cnt` 归零而不是置 1**：变的是"这一拍"，不算作"已连续 1 拍"，
> 需要再采一次才算 1。这样 `cnt >= N` 才真正代表连续 N 拍。

### ② 按下沿确认（第 71-89 行）

```c
if (raw && !s->stable) {
    /* ---------- 按下沿确认 ---------- */
    if (s->cnt < BTN_PRESS_N) return BTN_EV_NONE;   /* 还没稳定，继续等 */

    s->stable  = 1;
    s->holding = 1;                                 /* 先立"按住"旗，瞬断也拔不掉 */

    if (s->mute) return BTN_EV_NONE;                /* ② 长按余波：吞掉 */

    s->long_sent = 0;                               /* 全新的按住手势 */
    s->press_us  = now_us;

    /* 🚨 关键修正（2026-09-29 第二轮）：
     * 按下瞬间 **不再** 上报短按 */
    return BTN_EV_NONE;
}
```

> **逐行看这 5 个动作**：
>
> 1. **`cnt < PRESS_N` 就返回** = 还在消抖期，不认这次按下。
> 2. **`stable = 1`** = 状态机进入"按下"态。
> 3. **`holding = 1`** = **关键**。注释写"瞬断也拔不掉"——
>    即使接下来 180ms 电平反弹，`holding` 也保持 1，长按计时不中断。
> 4. **`if (s->mute) return`** = 如果长按静默中（长按后的抖动尾巴），这次按下被吞掉。
> 5. **`press_us = now_us`** = 记录长按计时的起点。
>
> **最后 `return BTN_EV_NONE` 是第二轮修正的核心**：
>
> 🕳️ **原 bug**：按下瞬间就上报短按。
> 于是"长按 UP" = 短按（光标上移一格）+ 1.2s 后长按（进入下一级）。
> 用户看到的就是"长按时光标先跳走一格"。
>
> **修正为一点判定**：
> ```
> 按下：只记时间，不出事件
> 抬起（时长 < 阈值）：出 1 个短按 —— 锁定、一次性
> 到达阈值：出 1 个长按，之后静默
> ```
> ⇒ **长按手势永远只出 1 个事件，绝不会先动光标。**

### ③ 抬起沿确认（第 91-109 行）

```c
if (!raw && s->stable) {
    /* ---------- 抬起沿确认 ---------- */
    if (s->cnt < BTN_RELEASE_N) return BTN_EV_NONE; /* 慢速瞬断：吞掉，继续按住 */

    const uint8_t was_holding = s->holding;         /* 先取：本手势是否有效 */
    s->stable  = 0;
    s->holding = 0;

    /* 刚跑完一次长按手势 → 不补短按（否则长按后再蹦一格） */
    if (s->mute || s->long_sent) return BTN_EV_NONE;

    /* 上电时就一直被按住（没经过按下沿）→ 不算有效手势 */
    if (!was_holding) return BTN_EV_NONE;

    /* 完整的「按下 → 抬起」手势，且时长没够到长按阈值
     * ⇒ 此刻才上报短按：**锁定、一次性** */
    return BTN_EV_SHORT;
}
```

> **第 93 行是三重保险的 ①**：
> `cnt < 300ms` 就返回。触点慢速瞬断（实测 180ms）时，`raw` 变 0 但 `cnt` 达不到 300，
> **直接返回，`stable` 保持 1，`holding` 保持 1** → 用户视角"还在按住"，
> 长按计时不中断。
>
> **`was_holding` 为什么要先取**：清零 `holding` 之前先保存它。
> 之后要判断"这次抬起对应的是不是一个有效手势"。`btn_init()` 里会把
> `stable` 初始化成当前电平（如果上电时键就是按着的），这时抬起**没有对应的按下沿**，
> `was_holding` = 0 → 不算有效手势 → 吞掉。**防止上电瞬间的假事件。**
>
> **`s->mute || s->long_sent` 双重检查**：
> - `long_sent`：刚发过长按 → 抬手时不要再补一个短按（否则长按后再蹦一格）
> - `mute`：长按后处于静默期 → 同样吞掉
>
> **代价（明写在注释里）**：单击在抬手后约 300ms 才生效。那 300ms 是抗瞬断的抗噪预算。

### ④ 真松开判定（第 111-115 行）

```c
if (!raw && !s->stable) {
    /* 已确认抬起：连续够久 = 真松开 → 解除长按静默 */
    if (s->mute && s->cnt >= BTN_TRUE_RELEASE_N) s->mute = 0;
    return BTN_EV_NONE;
}
```

> **注意这里用的是 `cnt` 而不是新变量**。因为 `raw` 持续为 0 且 `stable` 已经为 0，
> 上面第 64-69 行的计数逻辑一直在跑，`cnt` 就是"松开后已经过了多少毫秒"。
>
> `cnt >= 500ms` = 真的松手了 → 解除 `mute`，下次按下可以正常产生事件。
>
> **三重保险的 ②** 就是这一段。

### ⑤ 长按判定（第 117-125 行）

```c
/* ---------- 按住中：长按判定 ---------- */
if (s->holding && !s->long_sent &&
    (now_us - s->press_us) > (int64_t)long_ms * 1000) {
    s->long_sent = 1;
    s->mute      = 1;                               /* ② 进入静默 */
    return BTN_EV_LONG;
}

return BTN_EV_NONE;
```

> **到达流水的最后一段**（前面的 `if` 都不成立意味着"电平没变化"）：
>
> 1. `s->holding` = 用户还按着
> 2. `!s->long_sent` = 本次按住还没发过长按（保证**只发一次**）
> 3. `now_us - press_us > 1200000` = 按住超过 1.2 秒
>
> 满足全部三条 → 发长按 + 置 `long_sent`（不再发）+ 置 `mute`（静默）。
>
> **三重保险的 ② 和 ③ 在这里生效**：
> - 长按发出后 `mute=1`，后续抖动被 `if (s->mute) return` 吞掉
> - 只有真松开（500ms）才解除 `mute`

## 1.6 完整状态机图

```
                    ┌──────────────────────────────────────┐
                    ↓                                      │
  [空闲 stable=0] ──按下 30ms──→ [按住 holding=1] ──到 1200ms──→ 发 LONG
   holding=0                     press_us=now                    │
   ↑                             │                             ↓
   │                             │                        [静默 mute=1]
   │                    抬起 300ms                    （吞掉所有事件）
   │                             │                             │
   └──真松开 500ms────────────────┴─────────────────────────────┘
        (解除 mute)

  [按住 holding=1] ──抬起 300ms 且时长<1200ms──→ 发 SHORT（一次性）
        │
        └─→ 瞬断 180ms：stable 保持 1，holding 保持 1，不产生任何事件
```

---

# 二、`btn.h` — 接口与接线（112 行）

## 2.1 接线表注释（第 4-27 行）

```c
/**
 * 接线方案（推荐，抗干扰最好）：
 *   ┌──────────┬──────────┬──────────────┬─────────────┐
 *   │ 功能     │ ESP32-S3 │ 按键另一端   │ 上拉电阻    │
 *   ├──────────┼──────────┼──────────────┼─────────────┤
 *   │ 上 UP    │ GPIO38   │ GND          │ 10kΩ → 3V3  │
 *   │ 下 DOWN  │ GPIO39   │ GND          │ 10kΩ → 3V3  │
 *   │ 返回 BACK│ GPIO17   │ GND          │ 10kΩ → 3V3  │
 *   └──────────┴──────────┴──────────────┴─────────────┘
 *
 * 🚨 2026-09-29 实测结论：GPIO35/GPIO36 **不可用**
 */
```

> **这段注释是踩坑换来的。** 曾经按用户要求把按键改到 GPIO35/36，
> 结果板子反复 TG1WDT 复位。`pin_probe.c` 做了受控 A/B 实验才定位到根因。
>
> **A/B 实验设计**（唯一变量=引脚）：
> ```
> 组 A（35/36）：gpio_config 成功 → PSRAM 密集访问立即卡死 → 5 次 TG1WDT 复位
> 组 B（IO18）：同码 0 复位
> ```
>
> **为什么 `gpio_config` 不报错**：`gpio_config()` **不检查保留引脚表**，
> 对 PSRAM 占用的脚照样返回 `ESP_OK` —— **静默通过**。
> 这是"API 返回成功 ≠ 硬件可用"的教科书案例。

## 2.2 引脚选脚理由（第 37-48 行）

```c
/* ===== 引脚定义（换脚只改这三行）-----
 * 选脚理由：避开
 *   GPIO0/3/45/46   strapping 脚（上电电平决定启动模式）
 *   GPIO19/20       USB-Serial-JTAG D+/D-
 *   GPIO26~32       SPI0/1 Flash
 *   GPIO33~37       八线 PSRAM（本板 8MB）
 *   GPIO43/44       UART0 控制台
 *   GPIO4/5/6/7/10  已给 LCD
 */
#define BTN_PIN_UP      38
#define BTN_PIN_DOWN    39
#define BTN_PIN_BACK    17
```

> **这张"避让表"就是本板所有可用引脚的地图。** 以后要加新外设，看这张表就知道
> 哪些脚能碰。
>
> **strapping 脚**（0/3/45/46）最危险：上电瞬间的电平决定芯片进正常启动还是下载模式。
> 按键把它们拉低可能导致板子永远进不了正常启动。

## 2.3 电平与阈值（第 50-55 行）

```c
/* 按下时的电平：0 = 低电平有效（按键接 GND） */
#define BTN_ACTIVE_LEVEL    0

/* 长按判定阈值（按住多久算长按）。实机手感：1000~1500ms 合适 */
#define BTN_LONG_PRESS_MS   1200
```

> `BTN_ACTIVE_LEVEL = 0` 因为按键一端接 GND。按下 = 接地 = 低电平。
> 固件开了内部上拉（~45kΩ）作为"没接外部上拉也能工作"的兜底，
> 但长引线建议加外部 10kΩ（内部上拉阻值大、抗噪差）。

## 2.4 按键语义（第 63-74 行）

```c
 *   短按 UP / DOWN   ：同层前后切换（菜单里移光标；功能页里换页）
 *   长按 UP          ：进入下一级（菜单 → 选中项的功能页）
 *   长按 DOWN        ：返回上一级（功能页 → 菜单）
 *   单击 BACK        ：返回主菜单（任何层级）
 *   长按 BACK        ：恢复 3s 自动轮播（AUTO）
 *
 * 🚨 事件时机（2026-09-29 第二轮定版，别再改回去）：
 *   短按 = 「按下 → 抬起」完整手势闭合后才上报（抬手后约 300ms）
 *   长按 = 按住到 1200ms 那一刻上报，且**只上报一次**
 *          之后静默到真松开。⇒ 长按是「锁定触发」，不会先附带一次短按。
```

> **这份语义表是 UI 层（`ui.c`）的契约。** 改这里的语义必须同步改 `ui.c` 的 `handle_buttons()`。
>
> ⚠️ 注意"3s 自动轮播"——但 `ui.c` 里 `s_auto_ms` 初值是 4000（4s）。注释略滞后于代码。

## 2.5 枚举与 API（第 76-108 行）

```c
typedef enum {
    BTN_UP = 0,
    BTN_DOWN,
    BTN_BACK,
    BTN_MAX          /* = 3，用作数组长度 */
} btn_id_t;
```

> `BTN_MAX` 不是真实按键，是数组长度用。`btn.c` 里 `s_pin[BTN_MAX]` 就靠它。

```c
esp_err_t btn_init(void);                    /* 配上拉输入 + 起 20ms 轮询任务 */
bool btn_get_event(btn_id_t *id, bool *is_long);  /* 非阻塞取事件 */
void btn_inject(btn_id_t id, bool is_long);   /* ⚠️ 测试钩子 */
```

> **`btn_get_event` 的线程约束（注释里强调）**：
> ```
> ⚠️ 必须从 LVGL 所在任务（如 lv_timer 回调）调用，才能安全地更新界面。
> ```
> 因为调用它的 `handle_buttons()` 会直接操作 LVGL 控件。
>
> **`btn_inject` 是测试钩子**：
> ```
 * 把一个**合成事件**直接塞进真实的事件队列 —— 与物理按键走的是**同一条路径**
 * （环形队列 → handle_buttons → ...），因此可以脚本化地自证「按键 → 后端」这条写路径。
 * 线程安全：内部走 portMUX 临界区，可从任意任务调用。
 ```
> **注意"与物理按键同路径"是关键**。如果它直接调 `handle_buttons()` 而不是 `push()`，
> 就绕过了队列，自证力度会大打折扣。

---

# 三、`btn.c` — 任务与队列实现（187 行）

## 3.1 文件头与状态机引入（第 1-51 行）

```c
/**
 * btn.c — 三键去抖 + 事件队列
 *
 * 设计要点：
 *   1. 20ms 轮询 + 「连续 3 次采样一致（60ms）」才算有效跳变 → 软件去抖
 *   2. 按键任务只写环形队列，不调用任何 lv_* 接口（LVGL 不是线程安全的）
 *   3. 队列用 portMUX 临界区保护，无动态分配、无 ISR，逻辑最短路径
 */
```

> ⚠️ **注释里第 1 点是过时的**（还写 20ms/60ms），实际是 `btn_decode.h` 的
> 10ms/30ms+300ms。**这正说明为什么去抖逻辑要抽出去**——改参数时
> 至少有两个地方要同步，抽出去后只有一个。

```c
static const int   s_pin[BTN_MAX]  = { BTN_PIN_UP, BTN_PIN_DOWN, BTN_PIN_BACK };
static const char *s_name[BTN_MAX] = { "UP", "DOWN", "BACK" };

/* ============================================================================
 * 去抖 = 交给 btn_decode.h 的纯逻辑状态机（单一实现，宿主可测）
 * 🕳️ 上一版的坑：按下沿和抬起沿都用同一个 60ms 窗口 → 只挡得住毫秒级弹跳。
 *    实机实测「按住 UP 不放」会连续吐出 12 个短按事件（间隔 300~900ms）
 *    详细复盘与修法见 btn_decode.h 顶部注释。
 * ========================================================================== */
#include "btn_decode.h"

#define POLL_MS  BTN_POLL_MS     /* ⚠️ 必须等于 BTN_POLL_MS，解码器按此周期计时 */
#define EVQ_LEN  16
```

> **`POLL_MS` 必须等于 `BTN_POLL_MS`** —— 因为解码器里 `cnt` 是"采样次数"，
> 要换算成毫秒必须乘以实际采样周期。如果这里改成 20 而解码器按 10 算，
> 所有去抖时间都会变成 2 倍。这条警告很重要。

```c
typedef struct {
    uint8_t id;
    uint8_t is_long;
} btn_ev_t;

static btn_ev_t          s_q[EVQ_LEN];
static volatile int      s_head = 0;      /* 写指针 */
static volatile int      s_tail = 0;      /* 读指针 */
static portMUX_TYPE      s_lock = portMUX_INITIALIZER_UNLOCKED;

static btn_dec_t s_dec[BTN_MAX];
```

> **环形队列**：固定 16 槽，满了丢最旧。不动态分配 = 不可能因内存不足失败。
>
> **`portMUX_INITIALIZER_UNLOCKED`**：ESP32 双核，`portMUX` 是自旋锁。
> 比 `SemaphoreHandle_t` 轻得多（临界区只有几条指令），适合这种极短的入队操作。
>
> **每个键一个独立的 `btn_dec_t`** = 三路独立解码，可以同时按住三个键。

## 3.2 `push()`（第 53-63 行）

```c
static void push(btn_id_t id, bool is_long)
{
    portENTER_CRITICAL(&s_lock);
    int next = (s_head + 1) % EVQ_LEN;
    if (next != s_tail) {                 /* 队列满则丢弃最旧事件 */
        s_q[s_head].id = (uint8_t)id;
        s_q[s_head].is_long = is_long ? 1 : 0;
        s_head = next;
    }
    portEXIT_CRITICAL(&s_lock);
}
```

> **满队列判定**：`next == s_tail` 表示"写到 tail 位置了 = 追上了读指针 = 满了"。
> **这是浪费一格的标准做法**（不用额外计数变量判断满/空）。
>
> **满了丢最旧而不是丢最新**：按键事件有时间顺序，丢最新的会让用户"最近按的那下没反应"，
> 比丢最旧的体感好。16 槽在 4 个间隔 400ms 的连打下也用不满。
>
> **`portENTER_CRITICAL` / `portEXIT_CRITICAL` 必须配对**，中间不能有 `return`。
> 这里 `if` 里没有 return，安全。

## 3.3 `btn_inject()` 与 `btn_get_event()`（第 65-83 行）

```c
void btn_inject(btn_id_t id, bool is_long)
{
    push(id, is_long);
}

bool btn_get_event(btn_id_t *id, bool *is_long)
{
    bool ok = false;
    portENTER_CRITICAL(&s_lock);
    if (s_tail != s_head) {
        *id = (btn_id_t)s_q[s_tail].id;
        *is_long = s_q[s_tail].is_long != 0;
        s_tail = (s_tail + 1) % EVQ_LEN;
        ok = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return ok;
}
```

> **`btn_inject` 就是 `push` 的别名** —— 一行代码。
> **这正是"同路径"的意义**：注入的事件和物理事件在队列里**完全无法区分**。
>
> **`btn_get_event` 非阻塞**：队列空立即返回 false，`handle_buttons` 里的
> `while (btn_get_event(...))` 就能一次性排空所有待处理事件。
>
> **出参用指针而不是返回值**：`btn_ev_t` 是结构体（2 字节），
> 塞进返回值在某些 ABI 下麻烦，指针更直接。

## 3.4 `poll_one()`（第 85-99 行）

```c
static void poll_one(int i)
{
    const uint8_t raw = (gpio_get_level(s_pin[i]) == BTN_ACTIVE_LEVEL) ? 1 : 0;
    const int64_t now = esp_timer_get_time();

    const int ev = btn_decode_step(&s_dec[i], raw, now, BTN_LONG_PRESS_MS);
    if (ev == BTN_EV_SHORT) {
        push((btn_id_t)i, false);
        ESP_LOGI(TAG, "%s 短按（抬起确认）", s_name[i]);
    } else if (ev == BTN_EV_LONG) {
        push((btn_id_t)i, true);
        ESP_LOGI(TAG, "%s 长按（按住 %dms 到阈值，锁定触发，之后静默直到真松开）",
                 s_name[i], BTN_LONG_PRESS_MS);
    }
}
```

> **`gpio_get_level(...) == BTN_ACTIVE_LEVEL ? 1 : 0`** = 极性归一化。
> 之后所有逻辑都只关心 `1=按下`，不用再关心是低有效还是高有效。
> **换极性只改 `BTN_ACTIVE_LEVEL` 一处。**
>
> **`now` 每 10ms 调一次 `esp_timer_get_time()`**：这是 64 位读，
> 在 Xtensa 上不是单指令（`RSR`+移位）。但 30Hz × 3 键 = 90 次/秒，
> 开销可忽略。
>
> **日志措辞"（抬起确认）"/"（按住 1200ms 到阈值，锁定触发）"**：
> 这不是随便写的——它把事件语义直接写进串口日志，
> 排查长按问题时会直接看到"到底发的是短按还是长按"。

## 3.5 `btn_task()`（第 101-108 行）

```c
static void btn_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < BTN_MAX; i++) poll_one(i);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
```

> **一轮采三个键，然后睡 10ms**。三个键在"同一时刻"采样，
> 避免了"先采 UP 再采 DOWN"造成的 1~2μs 时序差。
>
> `vTaskDelay` 让出 CPU（10ms 内这个任务不占核）。

## 3.6 `BTN_SELFTEST` 自检（第 110-150 行）

```c
typedef struct { btn_id_t id; bool lg; int delay_ms; const char *desc; } st_step_t;

static const st_step_t s_steps[] = {
    { BTN_DOWN, false, 3000, "DOWN 单击 -> 菜单光标 0->1 (I2C)" },
    { BTN_DOWN, false, 1000, "DOWN 单击 -> 菜单光标 1->2 (SPI)" },
    { BTN_UP,   false, 1000, "UP   单击 -> 菜单光标 2->1 (I2C)" },
    { BTN_UP,   true,  1000, "UP   长按 -> 进入下一级（选中 I2C -> page 7）" },
    { BTN_DOWN, false, 1000, "DOWN 单击 -> 同级前移 page 6 (SPI)" },
    { BTN_UP,   false, 1000, "UP   单击 -> 同级后移 page 7 (I2C)" },
    { BTN_DOWN, true,  1000, "DOWN 长按 -> 返回上一级（回菜单，光标留在 I2C）" },
    { BTN_UP,   true,  1000, "UP   长按 -> 再次进入（I2C）" },
    { BTN_BACK, false, 1000, "BACK 单击 -> 返回主菜单" },
    { BTN_BACK, true,  1000, "BACK 长按 -> 恢复 AUTO 轮播" },
};

static void selftest_task(void *arg)
{
    const int n = (int)(sizeof(s_steps) / sizeof(s_steps[0]));
    ESP_LOGW(TAG, "=== 自检开始：注入 %d 个合成按键事件（不碰物理引脚）===", n);
    vTaskDelay(pdMS_TO_TICKS(1500));

    for (int i = 0; i < n; i++) {
        vTaskDelay(pdMS_TO_TICKS(s_steps[i].delay_ms));
        ESP_LOGW(TAG, "[注入 %d/%d] %s", i + 1, n, s_steps[i].desc);
        push(s_steps[i].id, s_steps[i].lg);
    }

    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGW(TAG, "=== 自检结束：全程无异常则按键链路完好 ===");
    vTaskDelete(NULL);
}
```

> **`delay_ms` 的设计**：第一个 3000ms 是**等开机页的 2.4s 自动跳转**完成。
> 后面每个 1000ms 是给"上一步的 UI 刷新 + 用户肉眼看屏幕"留的时间。
>
> ⚠️ **注释里的 "I2C/SPI" 是旧版 UI 的菜单项名**，v3 已改成"探针/烧录/引脚..."。
> 但**行序仍然对应**（0=探针 1=烧录 2=引脚），所以自检逻辑依然有效，只是描述过时。
>
> **`vTaskDelete(NULL)`**：删掉自己（`NULL` = 当前任务）。不删的话任务会一直空转占 CPU。

## 3.7 `btn_init()`（第 152-187 行）

```c
esp_err_t btn_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BTN_MAX; i++) mask |= (1ULL << s_pin[i]);

    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,     /* 内部上拉兜底 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,      /* 轮询，不用中断 */
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) { ...; return err; }
```

> **三个键一次配好**：`mask` 是位掩码（`1 << GPIO号`），`gpio_config` 接受。
> 比调三次 `gpio_config` 简洁。
>
> **`intr_type = GPIO_INTR_DISABLE`** = 不用中断，用轮询。
> 为什么？**去抖本身就需要采样计数**，中断只会增加复杂度。
> 10ms 轮询三个键的 CPU 开销约 0.1%，完全可接受。

```c
    /* 初值对齐，避免上电瞬间误报按键 */
    for (int i = 0; i < BTN_MAX; i++) {
        uint8_t lv = (gpio_get_level(s_pin[i]) == BTN_ACTIVE_LEVEL) ? 1 : 0;
        s_dec[i].last_raw = lv;
        s_dec[i].stable   = lv;         /* cnt/其余字段 = 0：等真实的连续采样 */
        ESP_LOGI(TAG, "%s -> GPIO%d  初始电平=%d", s_name[i], s_pin[i], lv);
    }

    xTaskCreate(btn_task, "btn", 3072, NULL, 5, NULL);
```

> **初值对齐是防误触的关键**。假设上电时 BACK 键正被按住（`raw=1`）：
> - 如果 `stable` 初始化成 0，解码器会认为"刚发生了一次按下沿"→ 30ms 后发事件
> - 初始化成 1 就认为"一直按着"，等真正抬起才算一次完整手势
>
> **`cnt` 和其余字段保持 0**（`s_dec` 是静态全局，自动零初始化）。
> 让"真实的连续采样"来推动状态机。
>
> **日志打"初始电平"**：如果某个键的初始电平是 1（按下），说明它被卡住/接地了，
> 一眼能看出来。**这个诊断信息在真机排障时非常有用。**
>
> **栈 3072（3KB）优先级 5**：去抖逻辑极简，3KB 绰绰有余。优先级 5 = 高于 LVGL(4)，
> 保证按键响应不被 UI 刷新拖慢。

```c
#if BTN_SELFTEST
    xTaskCreate(selftest_task, "btntest", 3072, NULL, 4, NULL);
#endif
    ESP_LOGI(TAG, "3 键就绪：GPIO%d/%d/%d | 去抖 按下%dms/抬起%dms | 长按 %dms | 真松开 %dms",
             BTN_PIN_UP, BTN_PIN_DOWN, BTN_PIN_BACK,
             BTN_PRESS_N * BTN_POLL_MS, BTN_RELEASE_N * BTN_POLL_MS,
             BTN_LONG_PRESS_MS, BTN_TRUE_RELEASE_N * BTN_POLL_MS);
    return ESP_OK;
}
```

> **最后的日志把四个时间参数全部算成毫秒打出来** = 开机自检。
> 如果看到的不是"按下30ms/抬起300ms/长按1200ms/真松开500ms"，说明参数被改过或
> 宏不一致（比如 `POLL_MS ≠ BTN_POLL_MS`）。
>
> `BTN_PRESS_N * BTN_POLL_MS` = 3 × 10 = 30，这样写**自动跟随参数变化**，
> 不会因为改了 `PRESS_N` 而日志还是 30。
