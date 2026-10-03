# esp32-s3-lvgl-ui 代码注解文档（v3 中文产线版）

> 生成时间：2026-10-02
> 代码基线：`build_v3/esp32-s3-lvgl-ui.bin` = 1,650,656 B，md5 `fe0b41df2e45f33013cc3e85f575b3e4`
> 适用对象：立创「AI 远程调试器」复刻工程，ESP32-S3 + 2.0" ST7789 + LVGL v9
> 阅读方式：先看第 1~3 章建立心智模型，再按需跳到对应模块章。

---

## 目录

| 章 | 内容 | 面向 |
|---|---|---|
| [1](#1-工程全景) | 工程全景：芯片/屏/按键/构建/产物 | 所有人 |
| [2](#2-分层架构与线程模型) | 7 层分层 + 5 个任务 + 硬约束 | 所有人 |
| [3](#3-启动时序app_mainc) | 启动时序逐行注解（最关键） | 所有人 |
| [4](#4-显示层lcd_hw) | SPI + ST7789 + LVGL 对接 | 改屏/改 UI |
| [5](#5-按键层btn_decodeh--btnc) | 去抖状态机（纯函数可单测） | 改交互 |
| [6](#6-ui-层uic1357-行) | 9 页逐页注解 + 绘制原语 | 改界面 |
| [7](#7-配置层ui_cfg) | NVS 持久化 | 改设置 |
| [8](#8-多语言i18n--字体) | 文案表 + 中文字体 | 改文案 |
| [9](#9-wifi-层wifi_sta) | 真实 WiFi STA | 改联网 |
| [10](#10-烧录器三层hw_pin--prog_hw--prog_api) | GPIO 级烧录器后端 | 改烧录 |
| [11](#11-探针体系7-个编译开关) | 7 个仅排查用开关 | 排障 |
| [12](#12-已知坑位档案) | 踩过的坑（防止重犯） | 所有人 |

---

<a name="1-工程全景"></a>
## 1. 工程全景

### 1.1 硬件定版

| 项 | 值 | 备注 |
|---|---|---|
| MCU | ESP32-S3（QFN56 rev0.2） | 双核 240 MHz，8MB PSRAM（八线 OCT） |
| Flash | 16 MB | `esptool flash_id` 实测 |
| 屏 | 2.0" ST7789P3，240×320 → **横屏 320×240** | MADCTL 0x60 |
| 背光 | LED+/LED− 接电源，**固件不控** | `bl=-1`，无背光 PWM |
| 供电 | 3.3 V | |

**屏接线（改屏只改 `lcd_hw.h`）**

```
MOSI/SDA = GPIO7    SCLK/SCL = GPIO6    DC/RS = GPIO4
RST = GPIO5         CS = GPIO10
背光 LED+ → 3V3(串限流电阻)    LED− → GND
```

⚠️ 10 脚裸屏的 LED+/LED− 是**背光电源脚**，不是控制脚。与固件无关。

**按键接线（`btn.h`）**

| 键 | GPIO | 有效电平 | 说明 |
|---|---|---|---|
| UP | 38 | 低 | 对 GND，内部上拉兜底 |
| DOWN | 39 | 低 | |
| BACK | 17 | 低 | |

🚨 **GPIO33~37 绝对不能用**：本板 `CONFIG_SPIRAM_MODE_OCT=y`，八线 PSRAM 硬占 33~37（35=SPIIO6 / 36=SPIIO7）。`gpio_config()` **不拦**、返回 `ESP_OK` 静默通过，但配置后 PSRAM 密集访问立即卡死 → 5 次 TG1WDT 复位。真机 A/B 实锤（`pin_probe.c`：唯一变量=引脚，35/36 组卡死 ×5，IO18 对照组 0 复位）。

**烧录器接线（`prog_hw.c`）**

```
MOSI=11  MISO=13  CK=12  CS=14   (SPI 四线，bit-bang)
TX=8     RX=9               (UART1，GPIO Matrix 任意路由)
RST=18                        (目标复位/进编程模式)
VPP=未映射  VCC=未映射       (本版无升压硬件)
```

### 1.2 分区表（`partitions.csv`）

```
nvs      data/nvs     0x9000    0x6000    ← WiFi 凭据 + UI 配置（不刷）
phy_init data/phy     0xF000    0x1000
factory  app/factory  0x10000   0x400000  ← 固件刷这里（4MB，当前用 39%）
storage  data/spiffs  0x410000  0xBF0000
```

🔒 **刷机硬规则**：只刷 `0x0`(bootloader) / `0x8000`(分区表) / `0x10000`(app) 三段，**`0x9000` NVS 绝不擦** —— WiFi 凭据和语言设置在里面。

### 1.3 构建

```bash
# 产品构建（显式传 0，防 CMake CACHE BOOL 粘住上次 -D 值）
idf.py -B build_v3 -DBTN_SELFTEST=0 -DPROG_SELFTEST=0 -DPROG_UI_TEST=0 \
       -DSHOT_PROBE=0 -DPIN_PROBE=0 -DCJK_SMOKETEST=0 -DUI_CFG_SELFTEST=0 \
       -DUI_CFG_PERSIST_TEST=0 -DWIFI_PASS_PROBE=0 -DRACE_AMPLIFY=0 build
```

**烧录**（本板 `esptool -b 921600` 会报 `Invalid head of packet(0x00)`，**必须用 115200**）：

```bash
esptool.py -p <PORT> -b 115200 write_flash 0x0 build_v3/bootloader/bootloader.bin \
    0x8000 build_v3/partition_table/partition-table.bin \
    0x10000 build_v3/esp32-s3-lvgl-ui.bin
```

**验收到位的六层判据**（缺一层都可能骗人）：

1. 回读 app 分区，`cmp` 与本地产物**逐位一致**（长度必须精确等于镜像长度，读多读少都会造成假差异）
2. 串口抓完整 boot（`tools/boot_cap.py`），`rst:0x1` **只出现 1 次**（多次=崩溃重启循环）
3. 崩溃关键字 7 类全 0（Guru Meditation / StoreProhibited / LoadProhibited / abort / Task watchdog ...）
4. `LCD 就绪 320x240` + `UI v3 就绪：9 页` + `LVGL 刷新任务已启动`
5. 产物内 7 项探针标记全 0（用 Python 按字节查，`strings` 只认 ASCII 会漏）
6. 6 项 build 目录配置全对（`grep XXX:BOOL build_v3/CMakeCache.txt`）

---

<a name="2-分层架构与线程模型"></a>
## 2. 分层架构与线程模型

### 2.1 七层结构

```
┌──────────────────────────────────────────────────────┐
│ L6  app_main.c     启动编排（顺序是硬约束）              │
├──────────────────────────────────────────────────────┤
│ L5  ui.c           9 页 UI · 唯一调 lv_* 的地方         │
│     ├ i18n.c       文案表（生成物）                     │
│     └ ui_cjk_14/16 字体（生成物）                       │
├──────────────────────────────────────────────────────┤
│ L4  lcd_hw.c       SPI+ST7789+LVGL 绑定 + 刷新任务      │
│     btn.c          去抖 + 环形队列（绝不碰 lv_*）        │
│     btn_decode.h   纯函数解码器（宿主可单测）            │
├──────────────────────────────────────────────────────┤
│ L3  prog_api.c      烧录任务化外壳（进度/日志/结论）      │
│     ui_cfg.c       NVS 配置层（写入+回读对拍）           │
│     wifi_sta.c     真实 WiFi STA（快照读）               │
├──────────────────────────────────────────────────────┤
│ L2  prog_hw.c       ESP32-S3 真机 BSP（引脚档案 + GPIO）   │
├──────────────────────────────────────────────────────┤
│ L1  hw_pin.c        跨平台引脚档案/双模驱动/25xx+ISP 协议 │
│                    （只认 BSP 回调，默认走确定性模拟器）  │
└──────────────────────────────────────────────────────┘
        ↑ 探针层（仅排查构建）：pin_probe / shot_probe /
          prog_ui_probe / cjk_probe
```

**依赖方向铁律**：`L5 只能向下调 L4/L3 拿快照，绝不直接调 L2/L1`。
任何阻塞调用（`hw_pin_flash_*` / `esp_wifi_*`）**都不许从 LVGL 任务发起**。

### 2.2 五个 FreeRTOS 任务

| 任务 | 栈 | 优先级 | 职责 | 碰 LVGL？ |
|---|---|---|---|---|
| `lvgl` | 8192 | 4 | `lv_timer_handler()` 循环 | ✅ **独占** |
| `btn` | 3072 | 5 | 10ms 轮询 + 去抖 + 入队 | ❌ 绝不 |
| `prog` | 4096 | 5 | 烧录 worker（收队列 → 跑 op） | ❌ 绝不 |
| `wifi_mgr` | 4096 | 5 | 扫描 + 连接 + 重连 | ❌ 绝不 |
| `sys_evt`(IDF) | — | — | WiFi 事件回调 | ❌ 绝不 |

🔒 **LVGL 线程安全铁律**：任何时刻只有一个任务调 `lv_*`。所有 UI 更新都发生在
`lvgl` 任务里的 `tick_cb`（LVGL 定时器回调）→ `handle_buttons()`。

### 2.3 跨任务数据流（全部走"加锁拷贝快照"，绝不共享指针）

```
btn 任务 ──push()──▶ 环形队列(portMUX 临界区) ──get()──▶ lvgl 任务 handle_buttons()
                                                                 │
lvgl ──prog_api_start()──▶ jobq ──▶ prog 任务 ──▶ 改 s_st(mutex) ──▶ lvgl prog_api_status() 拷贝
lvgl ──wifi_st_get()──▶ s_st(mutex) 拷贝 ◀── WiFi 事件/mgr 任务 改 s_st
```

---

<a name="3-启动时序app_mainc"></a>
## 3. 启动时序（app_main.c）

### 3.1 顺序是硬约束，不能调换

```c
cjk_probe_report();            // ① 字形覆盖自检（产品=空实现），刷机前就能跑
ui_cfg_init(&cfg);             // ② NVS 载入 → 灌 g_ui_lang（必须早于 UI 建对象）
g_ui_lang = cfg.lang;

esp_err_t err = lcd_hw_init(); // ③ SPI→panel_io→panel→LVGL display（不含刷新任务）
if (err != ESP_OK) return;

ui_init();                     // ④ 建全部 9 页对象（此刻 LVGL 独占，无并发）
if (!cfg_loaded) ui_start_wizard();   // 首启走向导，否则停在开机页
wifi_st_start();               // ⑤ WiFi 后台起（放在 UI 之后，避免它抢内存）
shot_probe_start();            // ⑥ 仅排查构建：注册一次性定时器

err = lcd_hw_start();          // ⑦ 最后才启动 LVGL 刷新任务
```

**为什么 ④ 必须在 ⑦ 之前**：
LVGL 非线程安全，刷新任务一旦跑起来就持续 `lv_timer_handler` 遍历对象树，
此时若 `ui_init` 还在建对象 → 对象图被撕裂 → 现象是
`lv_display_refr_timer` → `lv_obj_get_style_prop_internal` NULL 解引用
（LoadProhibited，EXCVADDR 很小）。

🕳️ **但这不是当年的真根因**（2026-09-29 受控实验定论，注释里写死了）：
曾假设"崩溃=刷新任务 vs ui_init 竞态"，用 `RACE_AMPLIFY` 放大到必然并发后
**14/14 不崩 → 假设被证伪**。真根因是 **LVGL 内置 TLSF 堆只有 64KB**，
UI 构建期分配失败 → `lv_malloc` 返回 NULL 而 LVGL 默认不检查 → 拿 NULL 当合法
指针写 → StoreProhibited 崩溃循环。修复 = `CONFIG_LV_USE_CLIB_MALLOC=y` + `ASSERT_MALLOC`。

拆时序（⑦ 最后）依然保留：并发本身就是 UB，属正确的防御性写法，但非根因。

**为什么 ⑤ 在 ④ 之后**：先让 UI 把内部 RAM 占稳，再让 WiFi 驱动申请它那几十 KB
缓冲，避免反过来把 UI 挤到 OOM（UI 分配失败曾以 StoreProhibited 崩溃循环形式出现）。

### 3.2 首启判定

`ui_cfg_init()` 返回 `false` = NVS 里没有合法配置（首启 / 版本不符 / 记录残缺）
→ `ui_start_wizard()` 直接进向导页，不显示开机页。

---

<a name="4-显示层lcd_hw"></a>
## 4. 显示层（lcd_hw.c/h）

### 4.1 初始化五步

```c
1. spi_bus_initialize(SPI2_HOST, ...)        // SCLK=6 MOSI=7, 20MHz
2. esp_lcd_new_panel_io_spi(...)            // CS=10 DC=4, trans_queue_depth=10
3. esp_lcd_new_panel_st7789(...)            // RST=5
4. send_vendor_init()  →  esp_lcd_panel_init()
5. swap_xy(true) / mirror(true,false) / set_gap(0,0) / invert(false) / disp_on(true)
```

⚠️ **厂商序列必须在 `panel_init` 之前** —— 其中 `0x21 INVON` 要生效在任何像素写入
之前，否则首帧颜色是反的。序列 golden `0xA36B04D1`（真机锁定，14 条）。

### 4.2 flush 回调（最容易漏的一步）

```c
static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* ① 字节序：LVGL 出 RGB565 小端，LCD 走 SPIMODEE 大端 → 必须 swap */
    uint32_t w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(px_map, w * h);
    /* ② 异步 DMA 送面板，完成回调里 flush_ready */
    esp_lcd_panel_draw_bitmap(s_panel, ...);
}
```

**🕳️ 三个必踩的坑**：

1. **`lv_display_flush_ready` 必须在 DMA 完成回调里调**，不能在 `flush_cb` 末尾调。
   否则 LVGL 会认为"没画完"，缓冲永远不释放 → 内存耗尽或画面卡住。
2. **`io_cfg.user_ctx` 创建后不可改** → display 只能在 panel_io 之后创建，
   时序上无解。解法：文件内静态 `s_disp` 兜底（回调只在首帧后才触发）。
3. **双缓冲必须在内部 DMA 内存**：`heap_caps_malloc(..., MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`。
   各 1/10 屏（320×24×2 = 15,360 B），`LV_DISPLAY_RENDER_MODE_PARTIAL`。

### 4.3 刷新任务

```c
static void lvgl_task(void *arg) {
    for (;;) {
        s_refresh_iters++;
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 16) delay_ms = 16;   // 上限 ~60fps，别饿死其他任务
        if (delay_ms < 2)  delay_ms = 2;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
```

`lcd_hw_start()` 幂等（`s_lvgl_started` 标志），重复调直接返回。

`lvgl_tick_cb` 用 `esp_timer_get_time()/1000` 提供 LVGL 心跳，不需要额外定时器。

---

<a name="5-按键层btn_decodeh--btnc"></a>
## 5. 按键层（btn_decode.h / btn.c）

### 5.1 为什么把解码器拆成纯函数

长按不触发这类 bug 出在"物理触点 → 事件队列"这一段。往队列注入合成事件的自检
**结构上抓不到它**（注入点已在这段之后 —— 这是本项目最重要的一条方法论）。

拆成 `static inline` 纯函数后，可用宿主单元测试（`tools/btn_decode_test.c`）
喂合成波形（含瞬断/弹跳）验证，而不是靠真机手按碰运气。

### 5.2 非对称去抖（三重保险）

| 参数 | 值 | 作用 |
|---|---|---|
| `BTN_POLL_MS` | 10 | 采样周期 |
| `BTN_PRESS_N` | 3 → **30ms** | 按下确认（跟手） |
| `BTN_RELEASE_N` | 30 → **300ms** | 抬起确认（瞬断静默期） |
| `BTN_TRUE_RELEASE_N` | 50 → **500ms** | 判定"真松开"（解除长按静默） |
| `BTN_LONG_PRESS_MS` | 1200 | 长按阈值 |

```
① 非对称去抖：按下 30ms 认，抬起要连续 300ms 才认
   → 触点慢速瞬断（实测 180ms 高电平）被当噪声吞掉，不结束"按住"状态
② 长按后静默（mute）：长按发出后该键不再产生任何事件，直到"真松开"（500ms）
③ 副作用（明说）：同一键连按间隔需 ≥300ms 才会计数
```

### 5.3 一点判定（教科书语义，第二轮修正）

```
· 按下：只记时间，**不出事件**
· 抬起（且时长 < 阈值）：出 1 个短按 —— 锁定、一次性
· 到达 1200ms：出 1 个长按，之后静默到真松开
```

🕳️ 原实现在"按下瞬间"就上报短按 → 长按手势必然先附带一次短按：
`长按 UP` = 光标先上移一格（`menu_move`）+ 1.2s 后进入下一级（`menu_enter`）。
用户看到的正是"光标被跳走一格"。真机日志实锤：按住 UP 5 秒 = 12 个短按事件，
间隔 300~900ms。

**代价**：单击改为抬手后约 300ms 生效（那 300ms 是抗瞬断的抗噪预算）。

### 5.4 状态机字段

```c
typedef struct {
    uint8_t  last_raw;   // 上次原始电平（1=按下）
    uint8_t  stable;     // 去抖后的稳定电平
    uint16_t cnt;        // 连续一致采样计数
    uint8_t  holding;    // 是否处于"按住中"（含瞬断静默期内）
    uint8_t  long_sent;  // 本次按住的长按是否已上报
    uint8_t  mute;       // 长按已发出 → 静默，等真松开
    int64_t  press_us;   // 本次按住的起始时刻
} btn_dec_t;
```

### 5.5 事件队列与测试钩子

```c
static btn_ev_t     s_q[16];                    // 环形队列，满了丢最旧
static volatile int s_head, s_tail;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
```

**`btn_inject(id, is_long)`** —— 测试钩子，把合成事件塞进**真实队列**，
与物理按键同路径（`环形队列 → handle_buttons → ...`）。这是
`prog_ui_probe.c` 能自证"按键→后端"写路径的关键。

**按键任务绝不碰 LVGL**：只做"去抖 + 入队"，界面更新全部在 `lvgl` 任务的
`tick_cb → handle_buttons()` 里。

---

<a name="6-ui-层uic1357-行"></a>
## 6. UI 层（ui.c，1357 行）

### 6.1 第一原则：每个数字都必须有真源头

上一版（复刻自立创 160×80 工程）的根本问题不是"不好看"，是"假"：
10 页里 8 页的数据是编出来的（SPI TX/RX = `(i*7+3)&0xFF`、PWM 波形 = 定时器推、
RX 终端 = demo 字符串循环、CONFIG/电池 = 递增假数）。

⇒ **没有后端支撑的页面一律删除，不保留"看起来在干活"的装饰。**

### 6.2 九页与数据源（逐页可查）

| # | 页面 | 数据源 | 交互 |
|---|---|---|---|
| 0 | BOOT 开机 | 静态 + 7 段进度条 | 2.4s 自动进菜单（BACK 跳过） |
| 1 | WIZ 首启向导 | `ui_cfg`（NVS） | 语言 → 时间 → 进菜单 |
| 2 | HOME 主菜单 | 纯导航（7 项） | 短按移光标，长按 UP 进入 |
| 3 | PROBE 探针检测 | `prog_api → hw_pin`（真读 0x9F JEDEC / ISP 0x7F） | 3 行：档案/驱动/执行 |
| 4 | FLASH 烧录器 | `prog_api`（5 操作，进度+日志全来自后端） | 8 行：3 设置 + 5 操作 |
| 5 | PINS 引脚档案 | `hw_pin` 档案表（9 信号 → 真 GPIO） | 只读 |
| 6 | STATUS 状态 | `wifi_sta` + heap/flash/esp_timer | 7 行 KV |
| 7 | SETUP 系统设置 | `ui_cfg`（语言/时间/SNTP/保存/出厂） | 9 行 |
| 8 | ABOUT 关于 | `esp_app_desc` / `esp_chip_info` / `esp_flash` | 只读 |

### 6.3 视觉语言

纯黑底 + 霓虹强调色 + **一套字体**。这是被字体现实逼出来的：
本工程只编进 `ui_cjk_14` / `ui_cjk_16`，**不含 FontAwesome**，
所以 `LV_SYMBOL_*` 会渲染成缺字方块（旧版靠 Montserrat 的 59 个符号撑图标，
改中文后那条路就断了）。

⇒ 图标语言改用"字体里确实存在的字符"：`●○■□▲→←↑↓★☆√✕↻ ◆ ◎` 与汉字。

⚠️ **换符号前先查 `tools/font_charsets.json`**，别凭想象填（会出方块）。

### 6.4 色板与几何

```c
C_BG/C_BLACK  0x000000    C_LINE  0x1B2530    C_DIM  0x3A4A57
C_GRAY        0x6E8496    C_TEXT  0xE6F7FF    C_CYAN 0x00E5FF
C_GREEN       0x39FF14    C_YELLOW 0xFFC400   C_RED  0xFF3B57
C_MAGENTA     0xFF2D95    C_MINT   0x00FF9C

SCR_W 320  SCR_H 240  TITLE_H 32  BODY_Y 34  HINT_Y 203
```

**每页强调色**（`PINFO[9]` 表）：

| 页 | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|---|---|---|---|---|---|---|---|---|---|
| 色 | 青 | 青 | 青 | 青 | **薄荷绿** | **黄** | 青 | **品红** | 青 |

主菜单每行的边框色 = 它指向的功能页的强调色（视觉上提前告知"进去是什么颜色"）。

### 6.5 绘制原语（改 UI 必用这几个，别直接 `lv_obj_create`）

```c
lv_obj_t *mk_bar (parent, x, y, w, h, color);              // 纯色块（去全部样式）
lv_obj_t *mk_txt (parent, txt, font, color, x, y);         // 文本
lv_obj_t *mk_tr  (parent, tid, font, color, x, y);         // 走 i18n 的文本
lv_obj_t *mk_card(parent, x, y, w, h, accent, border_opa); // 圆角卡片
void      mk_section(parent, txt, accent, y);              // 小节标题（色条+文字）
void      mk_hint  (parent, txt);                          // 底部操作提示
lv_obj_t *mk_page (i);                                     // 页面容器 + 标题栏 + 渐变线
```

`mk_page` 统一生成：左侧 3px 强调色竖条 + 16px 标题 + 3px 水平渐变辉光 + 1px 分隔线。
**PG_BOOT 例外**（只返回裸容器，因为开机页不需要标题栏）。

### 6.6 字体（只有两款）

```c
#define F16 (&ui_cjk_16)   // 标题、数值、大字
#define F14 (&ui_cjk_14)   // 标签、日志、次要信息
```

**行高铁律**：F16 行高 20，F14 行高 17。同一区域连续放两行时，
下一行 y 至少是 `上一行 y + 上一行行高 + 7`。

🕳️ 2026-10-02 踩过：BOOT 页原来 92/114/132 的间距只有 2px/1px → 两行 F14
在屏上**视觉粘成一行**（离线 OCR 把 "AI WIRELESS DEBUGGER" 和 "ESP32-S3 LVGL"
识别成一个字符串）。改完统一按 82/116/140 排。

### 6.7 页面切换（ui_goto）

```c
void ui_goto(int n) {
    if (n < 0 || n >= PG_MAX) return;
    for (int i = 0; i < PG_MAX; i++) {
        lv_obj_set_hidden(s_page[i], i != n);
        // 页标：当前页用该页强调色 + 10px 辉光
    }
    s_cur = n;
    s_auto_ms = 4000;                       // 重置 AUTO 计时

    // 开机页不挂 AUTO/MANUAL 胶囊和页标 —— 那两样是功能页的控件
    if (s_pill_mode) lv_obj_set_hidden(s_pill_mode, (n == PG_BOOT));
    for (int i = 0; i < PG_MAX; i++)
        if (s_dot[i]) lv_obj_set_hidden(s_dot[i], (n == PG_BOOT));

    // 进哪页刷哪页（只刷当前页，省 CPU）
    switch (n) {
    case PG_PINS:   pins_refresh();   break;
    case PG_STATUS: status_refresh(); break;
    case PG_ABOUT:  about_refresh();  break;
    case PG_SETUP:  setup_refresh();  break;
    case PG_PROBE:  probe_apply(); probe_refresh(); break;
    case PG_FLASH:  flash_refresh();  break;
    case PG_WIZ:    wiz_refresh();    break;
    default: break;
    }
}
```

🕳️ 2026-10-02 踩过：开机页右上角挂着"手动"胶囊，像张没做完的截图 → 已加隐藏逻辑。

### 6.8 按键导航（两级模型 + 页内钩子）

```
s_in_menu == true  → 当前在主菜单，光标 s_sel 在 7 项间移动
s_in_menu == false → 在功能页，s_sel 由该页自己管

事件处理顺序：
  1. 开机页 + BACK → 直接进主菜单
  2. 除非是"长按 BACK"（切 AUTO），否则 set_auto(false) → 关轮播
  3. 【页内钩子】if (!s_in_menu)：交给 xxx_page_key()，返回 0 = 本页已消费
  4. 未消费 → 走全局语义
```

**全局语义**：

| 键 | 短按 | 长按 |
|---|---|---|
| UP | 菜单内移光标（`d=-1`） | 菜单内进入选中项 |
| DOWN | 菜单内移光标（`d=+1`） | 功能页返回上一级（回菜单） |
| BACK | 直接回主菜单（任何层级） | 开关 AUTO 轮播 |

**四个页内钩子**（各有 `xxx_page_key(id, is_long)`，返回 0=已消费 / 1=交回全局）：

- `wiz_page_key`：3 步（语言 → 时间 5 字段 → 完成），选中"下一步"进下一步
- `probe_page_key`：3 行（档案 / 驱动 / 执行），执行行起 `RDID` 或 `ISPSYNC`
- `flash_page_key`：8 行（前 3 是设置项，后 5 是操作项）
- `setup_page_key`：9 行（语言 / 5 个时间字段 / SNTP / 保存 / 出厂）

**页内钩子的统一套路**（照抄这个模式即可）：

```c
static int xxx_page_key(btn_id_t id, bool is_long) {
    if (id == BTN_BACK) return 1;                       // BACK 一律交回全局
    if (!is_long) {                                     // 短按 = 移光标
        s_xxx_sel = (s_xxx_sel + N + ((id==BTN_UP)?-1:+1)) % N;
        xxx_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;                       // 长按 DOWN = 返回
    /* 长按 UP = 执行 */
    ...
    return 0;
}
```

### 6.9 主菜单（7 项）

```
行序: 0=探针 1=烧录 2=引脚 3=状态 4=设置 5=关于 6=向导
字形: ○ ↓ ■ ◎ ☆ ◆ ↻     (MENU_GLYPH 表)
```

每行 = 边框（该页强调色）+ 字形（强调色）+ 文案（TR）+ 右箭头 `→`。
选中行：背景填充强调色 + 10px 辉光 + 文字变**黑**（`C_BLACK`）保证对比度。

🕳️ **2026-10-02 踩过的隐形 bug**：`build_home` 末尾**必须调一次**
`home_apply_sel()`。因为菜单项的文字是在 `home_apply_sel()` 里才被赋色的，
而那一层又是用 `lv_obj_remove_style_all()` 建的（无默认样式）。
如果只等"用户按键时"才调用，首次进入主菜单会看到**一整页隐形的菜单**
（黑字画在黑底上）—— 抓图探针实测踩到，非黑像素只有 3219。

### 6.10 定时刷新（tick_cb，300ms 周期）

```c
s_tick_ms += 300;
handle_buttons();                       // ← 唯一消费按键事件的地方

if (s_cur == PG_BOOT) { 进度条推进; >=2400ms 进菜单; return; }

STATUS: 每 600ms 刷新（WiFi/heap/时间变化快）
PINS  : 每 3000ms
ABOUT : 每 3000ms
PROBE : 每  300ms（等后端结果）
FLASH : 每  300ms（等进度/日志）
SNTP  : 每 3000ms 检查，已同步则把系统时间并回 ui_cfg 基准
AUTO  : s_auto_ms -= 300; <=0 则 auto_step()
```

**刷新频率按数据变化速度分层**（不是所有页都 300ms —— 省 CPU，也是 LVGL 掉帧主因）。

### 6.11 AUTO 轮播

```c
static void auto_step(void) {
    int n = s_cur + 1;
    if (n >= PG_MAX || n <= PG_HOME) n = PG_PROBE;   // 只在 PG_PROBE..PG_ABOUT 之间转
    s_in_menu = false;
    for (int k = 0; k < MENU_N; k++) if (MENU_PG[k] == n) s_sel = k;  // 同步主菜单光标
    ui_goto(n);
}
```

⚠️ 2026-09-30 决定：**关掉开机自动轮播**（`s_auto_on` 初始为 `false`），
避免用户操作被自动切页打断。需要时长按 BACK 打开。

### 6.12 整树重建（切语言）

```c
void ui_rebuild(void) {
    const int  keep     = s_cur;        // 记当前页
    const bool was_menu = s_in_menu;    // 记是否在菜单
    lv_obj_clean(s_scr);
    build_all();                        // 重建全部 9 页
    s_in_menu = was_menu;
    ui_goto(keep);
    if (s_in_menu) home_apply_sel();
}
```

**为什么切语言必须整树重建**：`TR(tid)` 返回的是 `UI_STR[lang][idx]` 的**指针**，
所有控件建立时就把旧语言的字符串指针烤进了 label。改 `g_ui_lang` 不会让已存在的
label 自动更新。

🕳️ `TR()` 的返回值**不要长期缓存** —— 切语言后旧指针仍指向旧语言，
会显示成"半中半英"。要缓存就存 `idx`，切语言时统一重刷。

### 6.13 各页数据结构速查

| 页 | 状态变量 | 控件数组 | 刷新函数 |
|---|---|---|---|
| BOOT | `s_boot_ms` | `s_boot_seg[7]` | `tick_cb` 内联 |
| WIZ | `s_wiz_step/s_wiz_sel/s_wiz_t[5]` | `s_wiz_row[8]` | `wiz_refresh()` |
| HOME | `s_sel` | `s_menu_row[7]` | `home_apply_sel()` |
| PROBE | `s_pr_tgt/s_pr_drv/s_pr_sel` | `s_pr_val[2] s_pr_res[4]` | `probe_apply()+probe_refresh()` |
| FLASH | `s_fl_sel/s_fl_tgt/s_fl_drv/s_fl_img` | `s_fl_cursor[8] s_fl_name[8] s_fl_val[3] s_fl_log[4]` | `flash_cfg_apply()+flash_refresh()` |
| PINS | — | `s_pin_val[9]` | `pins_refresh()` |
| STATUS | — | `s_st_val[7]` | `status_refresh()` |
| SETUP | `s_set_sel/s_set_t[5]/s_set_arm` | `s_set_cursor[9] s_set_name[9] s_set_val[9]` | `setup_refresh()` |
| ABOUT | — | `s_ab_val[7]` | `about_refresh()` |

---

<a name="7-配置层ui_cfg"></a>
## 7. 配置层（ui_cfg.c/h）

### 7.1 它实现的产品判据

**P0 判据之二：断电重启设置不丢。**

NVS 布局（namespace `"ui_cfg"`）：

| 键 | 类型 | 含义 |
|---|---|---|
| `cfg_ver` | u8 | 版本号，不符 ⇒ 当首启（升级即重走向导） |
| `lang` | u8 | 0=中文 1=English |
| `epoch` | i64 | 设定那一刻的绝对时刻（Unix 秒） |
| `sntp` | u8 | 0=关（纯手动） 1=开 |

恢复出厂 = `nvs_erase_all("ui_cfg")` + `esp_restart()`。

### 7.2 无 RTC 的诚实说明

存的是"设定那一刻的绝对时刻"，运行期用 `esp_timer` 推进：

```c
int64_t ui_cfg_now(void) {
    if (s_base_epoch <= 0) return -1;   // 时间未定
    return s_base_epoch + (esp_timer_get_time() - s_base_us) / 1000000;
}
```

**掉电期间不走时 —— 无 RTC 做不到，不是 bug 是物理事实。** SNTP 开启且联网时可自动纠正。

### 7.3 三层结构，刻意不合并

```c
nvs_read_raw()   纯读，无副作用（不碰时间基准、不碰运行期缓存）
ui_cfg_init()    读 + 校验 + 立时间基准 + 更新缓存   ← 有副作用
ui_cfg_save()    写 + 用 nvs_read_raw 回读对拍
```

🕳️ 踩过的坑：回读若走 `ui_cfg_init()`，会把 `s_base_us` 重置成"此刻"，
**时钟每被读一次就冻结一次**。回读必须走无副作用那条路。

### 7.4 写入回读逐位对拍

`ui_cfg_save()` 写完后立刻 `nvs_read_raw()` 回读，逐字段比对，不一致返回错误：

```c
if (!cfg_eq(&back, &c)) {
    ESP_LOGE(TAG, "对拍失败：ver %u/%u lang %u/%u sntp %u/%u epoch %lld/%lld", ...);
    return ESP_ERR_INVALID_STATE;
}
```

🕳️ **绝不用 `memcmp` 比结构体**：`ui_cfg_t` 里 `int64_t` 要求 8 字节对齐，
于是 `_pad`(偏移3) 与 `epoch`(偏移8) 之间存在 4 字节**对齐空洞**。结构体初始化
不会写它，两边的空洞各是各自的栈垃圾，`memcmp` 必然失败 —— 而逐个字段打印
出来却"全都一样"。这个坑真踩过：selftest 报"对拍失败：ver 1/1 lang 1/1 ..."。

**所以只比数据字段，填充字节不是数据**（`cfg_eq`）。

### 7.5 越界防护

- `lang > 1` ⇒ 读时回落中文（不崩），写时返回 `ESP_ERR_INVALID_ARG`
- `epoch < UI_CFG_MIN_EPOCH`（2020-01-01）⇒ 当"未设定"
- `epoch < 0` ⇒ 落盘成 0

### 7.6 自检（仅排查构建）

`ui_cfg_selftest()` 4 项，跑完自动还原原配置：

1. 写入 → 回读 → 逐位对拍（正向）
2. 二次写入不同值：抓"改了但读到旧值"（陈旧读）
3. 擦除后必须真的读不到：抓"首启判定恒假"
4. 还原原状并复核

`ui_cfg_persist_probe()` 必须**跨两次开机**才有意义：
第 1 次写 + 打标记，第 2 次校验。中间隔一次硬复位（flash 重新上电读取）
—— 这才真的过了 flash 介质这一层。

⚠️ 硬复位只证"重启不丢"，**"掉电瞬间不丢"需要人工拔电一次** —— NVS 写入中途
掉电的磨损均衡行为，程序无法自证。

---

<a name="8-多语言i18n--字体"></a>
## 8. 多语言（i18n.c）与字体（fonts/）

### 8.1 三者关系（⚠️ 生成链，改错会被无声覆盖）

```
main/i18n/strings.txt          ← 真源（人改这个）
      │ tools/gen_i18n.py
      ▼
main/i18n.c  +  main/i18n.h    ← 生成物（191 条 × 2 语言）
      │ tools/gen_font.py
      ▼
main/fonts/ui_cjk_14.c/16.c    ← 生成物（字形子集）
```

**加文案的正确做法**：
```bash
vim main/i18n/strings.txt          # 加一行 "TID_XXX|中文|English"
python3 tools/gen_i18n.py
python3 tools/gen_font.py          # ⚠️ 漏了这步会出方块字
```

🔴 **绝不手改 `i18n.c` / `i18n.h` / `fonts/*.c`** —— 会在下次生成时被无声覆盖。

### 8.2 TID 机制

```c
#define TID_S_OK 0u        // 编译期常量，与 UI_STR 数组下标一一对应
...
#define UI_STR_COUNT 191

#define TR(idx) ui_tr(idx)              // → UI_STR[g_ui_lang][idx]
const char *ui_tr(uint16_t idx);
uint16_t    ui_tr_id(const char *key); // 按 key 查索引，查不到返回 0xFFFF
```

⚠️ `TR()` 结果不要长期缓存（见 6.12）。

### 8.3 字体配置（sdkconfig.defaults，最关键的三行）

```ini
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_USE_CLIB_MALLOC=y       # 见下
CONFIG_LV_USE_ASSERT_MALLOC=y     # 第二道保险
CONFIG_LV_USE_FONT_COMPRESSED=y   # 🔴 见下
```

#### 🔴 `LV_USE_FONT_COMPRESSED` —— 2026-10-01 的 P0 事故

`main/fonts/*.c` 是 `lv_font_conv` 生成的**压缩**位图
（`font_dsc.bitmap_format = 1` = `LV_FONT_FMT_TXT_COMPRESSED`）。

`lv_font_fmt_txt.c` 的取位图分支：

```c
if(bitmap_format == PLAIN) { ... }
else {
    #if LV_USE_FONT_COMPRESSED
    ...
    #else
    return NULL;         // ← 不开这个，每个字形都返回 NULL
    #endif
}
```

**不开 ⇒ 全屏一字符都画不出**（只有一条 LV_LOG_WARN）。

🕳️ **危险的地方**：`lv_font_get_glyph_dsc()` 读的是 `glyph_dsc` 表，**不需要解压**，
所以"字形覆盖自检"照样 100% 全绿 —— **两条路径一条通一条不通**。
⇒ 判据必须覆盖解压这步（`cjk_probe` 的渲染冒烟页 + OCR 就是干这个的）。

#### 🔴 `LV_USE_CLIB_MALLOC` —— 崩溃循环的真根因

默认 `LV_USE_BUILTIN_MALLOC` 只给 64KB（`CONFIG_LV_MEM_SIZE`），
本 UI 约 225 个对象 + 每帧 draw task 直接撑爆 → `lv_malloc` 返回 NULL
且 LVGL 默认**不检查** → 写 0x0 → StoreProhibited/LoadProhibited 崩溃循环。

`CONFIG_LV_USE_ASSERT_MALLOC=y` 是第二道保险：分配失败立刻明确报错，不再静默踩内存。

### 8.4 🕳️ 找配置真相的唯一权威

**ESP-IDF 没有 `build/config/sdkconfig` 纯文本**，只有 `.h` / `.cmake` / `.json`。
grep 错路径必返空，而**空返回极易被误读成"没定义"**。

同一个开关在四个地方会给出四个不同答案：

| 位置 | 可靠性 |
|---|---|
| 源码里 grep | ⚠️ 常被 `#ifndef` 兜底 |
| 根目录 `sdkconfig` | ⚠️ 是上次 menuconfig 的产物 |
| `sdkconfig.defaults` | ⚠️ 只是默认值，被 menuconfig 覆盖 |
| **`build_xxx/config/sdkconfig.h`** | ✅ **唯一权威（烘焙产物）** |

🚨 **教训**：2026-10-01 我写过的总结把因果读反了（称"probe 开着压缩所以危险、
生产要用 build/"），照做会刷出全黑屏。**自己写过的总结同样会错，动手前必须重新查证。**

### 8.5 字符集来源

`ui_cjk_14/16` = `strings.txt` 的字 ∪ GB2312 一级常用字(3755) ∪ ASCII ∪ 符号。
所以**改文案不必重跑 `gen_font.py`**（全量字库已覆盖）；
只有换字体/改 bpp 才需要重跑。

⚠️ 换图标符号前先查 `tools/font_charsets.json`，别凭想象填（会出方块）。

---

<a name="9-wifi-层wifi_sta"></a>
## 9. WiFi 层（wifi_sta.c/h）

### 9.1 生命周期（全在 `wifi_mgr_task` 里串行执行）

```
app_main ──wifi_st_start()──▶ (立即返回)
                                 │ 建 netif / event loop / 驱动
                                 ▼
                        ┌─ 定向扫描（只扫目标 SSID）
                        │   → 命中 = 该 AP 确实在 2.4GHz 上
                        │   → 空   = 它是 5GHz / 太远 / 隐藏（关键判据！）
                        ├─ 全量扫描 → 打日志列出附近 AP（方便换 2.4G 的 SSID）
                        └─ esp_wifi_connect() → 等 GOT_IP
                                 │
              GOT_IP（成功）    ┴    DISCONNECTED（失败/掉线）
                 │                          │
          存 IP/RSSI/信道           记 reason + retries++，3s 后重连
```

### 9.2 两条"必须这么写"的理由

1. **扫描用阻塞式但绝不放在事件回调里** —— 事件回调跑在 `sys_evt` 任务上，
   阻塞 2~3s 会顶住整个 WiFi 事件循环。所以扫描/连接统一在自己的 `wifi_mgr_task` 里。
2. **状态用互斥量 + 拷贝暴露给 UI** —— LVGL 任务与 WiFi 事件任务是两个线程，
   共享字符串必须上锁，**绝不能把结构体指针直接递给 UI**。

```c
void wifi_st_get(wifi_st_status_t *out) {
    if (!out) return;
    st_lock(); *out = s_st; st_unlock();     // 拷贝，不共享指针
}
```

### 9.3 reason 码翻译（可读性设计）

```
201 = NO_AP_FOUND        → "2.4GHz 上扫不到该 SSID"
                             （若 AP 只在 5GHz，S3 永远连不上 —— 无 5GHz 射频）
15 / 202                  → "认证/四次握手失败：大概率密码不对"
```

这条设计极有价值：一眼能看出是"找不到 AP"还是"密码错"。

### 9.4 凭据：默认打底 + NVS 优先

```c
// 首次开机用 wifi_cfg.h 的默认值，连上后写进 NVS，之后以 NVS 为准
cred_load():  先 snprintf 默认值 → 再尝试 NVS 覆盖
cred_save():  GOT_IP 时自动持久化
```

🔒 **`wifi_cfg.h` 里有明文密码**。该目录**不在任何 git 仓库内**（已用
`git rev-parse --show-toplevel` 核实），暂无泄露风险。

⚠️ **将来若要纳入 git，必须先做**：① 加 `.gitignore` 改构建期注入，
或 ② 改 NVS 首次配网。**切勿 `git add .`** —— 明文口令进历史后删除无效。

### 9.5 已知遗留

`4WAY_HANDSHAKE_TIMEOUT`（reason=15）是既有未解问题，改前要先确认路由器密码。

---

<a name="10-烧录器三层hw_pin--prog_hw--prog_api"></a>
## 10. 烧录器三层

### 10.1 职责切分

| 层 | 文件 | 认识 ESP32 吗？ | 职责 |
|---|---|---|---|
| L1 | `hw_pin.c` (1716行) | ❌ 不认识 | 引脚档案 + 双模驱动 + 25xx/ISP 协议 |
| L2 | `prog_hw.c` (288行) | ✅ | 把引脚动作接到真 GPIO/UART |
| L3 | `prog_api.c` (526行) | ✅ 间接 | 任务化外壳：进度/日志/结论/线程边界 |

### 10.2 L1 hw_pin：三层合一

```
L2 引脚档案  hw_pin_profile_t : 逻辑信号 → 物理 GPIO 的映射表
L1 引脚驱动  hw_pin_spi_xfer() : 双模
                ├ HW 模式 : 外设映射 (ESP32 GPIO Matrix / IOMUX)
                └ BB 模式 : 纯 GPIO bit-bang, 任意时序/时钟拉伸
L0 电压层    hw_pin_vpp_set()  : 可调编程电压 (5/12/12.5/21V 档)
```

**9 个逻辑信号**（GND 是公共地，不参与映射）：
`MOSI MISO CK CS TX RX RST VPP VCC`

**"换目标 = 换一条档案"** —— 与 `panel_lcd` 的"换屏 = 换一条档案"同一设计。
本工程三条档案：

| 档案 | 映射 | 用途 |
|---|---|---|
| `s3-spi` | MOSI=11 MISO=13 CK=12 CS=14 TX=8 RX=9 RST=18 | SPI 四线 + UART 两线（全功能） |
| `w25q` | MOSI=11 MISO=13 CK=12 CS=14 | 纯 SPI 四线 |
| `s3-isp` | TX=8 RX=9 RST=18 | 纯 UART 三线 |

### 10.3 🔴 SIM / REAL 的分水岭就是那个 NULL

```c
// prog_hw.c
if (!real) {
    /* hw_pin 的 bit-bang 里 use_dev = (g_bsp.gpio_read == NULL)，
       全 NULL → 器件模型供 MISO → RDID 稳定 EF 40 18 */
    hw_pin_bsp_install(NULL);
    return HW_PIN_R_OK;
}
if (setup_pins(p) != 0) return HW_PIN_R_IOERR;
hw_pin_bsp_install(&s_bsp_real);   // 装了 gpio_read → 真读 MISO
```

- **SIM** → BSP 全 NULL → 器件模型供 MISO → 操作会成功（验证链路与 UI）
- **REAL** → 装了 `gpio_read` → 真读 MISO → 没接芯片就报 NOFLASH

**这是"不假装成功"的结构性保证，不靠上层自觉。** 这也是为什么 DRIVER 那一行
选了 REAL 时值是警示色（黄）。

**MISO 配内部上拉**（`cfg_in(p->gpio[HW_PIN_MISO], 1)`）：悬空时读 1
（与真闪存未选中时的高电平一致）→ JEDEC 读到 0xFF → 诚实报 NOFLASH。

### 10.4 三个方向要 BSP 自己配

1. **不动 hw_pin 的内置黄金表** —— 本板档案用 `hw_pin_profile_load()` 注入，
   这样 `HW_PIN_GOLDEN`（内置 4 条档案的 FNV）仍成立，回归不破。
2. **方向要自己配** —— hw_pin **从不调用** `bsp->gpio_dir`（grep 实测零命中），
   所以 MOSI/CK/CS/RST 必须先配成输出、MISO 配成输入，否则 bit-bang 直接失真。
3. **UART 走 GPIO Matrix** —— `uart_set_pin()` 允许 TX/RX 路由到任意脚。

🕳️ **S3 矩阵的硬限制（2026-10-02 受控 A/B 实测）**：
`U2TXD_GPIO_NUM = (-1)` 说明 S3 只有 UART0/1 有 IOMUX 专属脚。
实测 IO17（IOMUX 脚）→ 1974/7000，IO9（matrix 脚）→ **0/7000**。
⇒ **UART TX 走 matrix 脚无效。SPI 是否同样失效未测**（底层同一个 `gpio_matrix_out`，
列为高风险待验，不得当结论）。

### 10.5 L3 prog_api：任务化外壳

**为什么不能直接在 LVGL 回调里干**：
1. 擦除/编程是毫秒~百毫秒级，卡在 LVGL 任务里会掉帧甚至喂不上看门狗；
2. RDSR 轮询 busy、UART 等待应答都是**阻塞**操作；
3. 后台跑 + 前端轮询进度，是"进度条不是演出来的"的结构性保证。

**五个操作**：

| 枚举 | 名称 | 内部流程 | 进度 |
|---|---|---|---|
| `PROG_OP_RDID` | READ ID | 0x9F → JEDEC | 0→100 |
| `PROG_OP_ERASE` | ERASE | 按扇区循环 + RDSR 等 busy | 0→100 |
| `PROG_OP_PROGRAM` | PROGRAM | 擦→页编程→**立刻自检** | 0→80→85→100 |
| `PROG_OP_VERIFY` | VERIFY | 回读逐字节 + 定位首个不符字节 | 20→100 |
| `PROG_OP_ISPSYNC` | ISP SYNC | 0x7F 握手 + GetVersion + GetID | →100 |

**镜像（确定性，同一 IMAGE 选择永远是同一份字节）**：

| 名称 | 大小 | 内容 | 用途 |
|---|---|---|---|
| `banner-256` | 256B | 可读文本 `"XIAOMO FLASHER * ..."` | 串口/逻辑分析仪直接认出来 |
| `pattern-1k` | 1024B | `(i ^ (i>>3)) & 0xFF` | 便于脚本比对错位 |
| `rand-2k` | 2048B | 确定性 LCG（不用 `rand()`） | 跨次运行一致，可复现 |

**三个镜像纪律**：

1. `s_image[2048]` **必须 static** —— "≥512B 局部数组一律 static" 是
   hw_flash 那个 25KB 真机栈炸弹换来的纪律。
2. `op_verify` 的 `static uint8_t rd[256]` 同样 static。
3. 每页编程后 `vTaskDelay(1)` —— 别饿死 IDLE（TG1WDT）。

**`diag_real()` —— REAL 模式失败后的自动诊断**：

```c
/* 把「引脚/时序对不对」从「芯片有没有接」里切出来 */
if (rc == HW_PIN_R_OK || s_drv != 1) return;    // 只有 REAL 失败才做
lb = prog_hw_loopback(tx, rx, &n);              // MOSI↔MISO 短接回环
  lb == 0  → "loopback MOSI-MISO OK" + "pins ok, target silent"
  lb > 0   → "loopback err@N XX!=YY" + "check wiring/jumper"
```

**否则用户永远在猜是哪一头坏了。** SIM 模式没意义（MISO 由器件模型供，怎么测都对）。

**线程安全**：
- `s_lock` 互斥量保护 `s_st` 全部字段（含 `log[3][40]`）
- `s_jobq` 深度 1 的队列 —— **不排队，运行中再来直接拒**（返回 -1）
- `prog_api_cfg()` 在 `RUNNING` 时返回 -1，避免中途换档案把操作打断成半截

🕳️ 局部数组 >512B 的两个来源：真机栈只有 ~4KB（`prog` 任务），
而 `s_image` 2KB + `rx[8]` 等若放栈上会链式溢出 → TG1WDT 复位。
**`hw_flash` 那边实测过整条 selftest 链 25KB 炸栈。**

---

<a name="11-探针体系7-个编译开关"></a>
## 11. 探针体系（7 个编译开关）

产品构建**全部为 0**，探针翻译单元变空，零开销。

| 开关 | 文件 | 证什么 | 关键判据 |
|---|---|---|---|
| `BTN_SELFTEST` | `btn.c` | 队列→tick→导航全链 10 步 | 无异常即通过 |
| `PIN_PROBE` | `pin_probe.c` | 按键引脚可用性 + PSRAM 完整性 | A/B 对照（唯一变量=引脚） |
| `SHOT_PROBE` | `shot_probe.c` | 9 页真实渲染 dump → 本地 PNG | **非黑像素数 + 页间差异非 0** |
| `CJK_SMOKETEST` | `cjk_probe.c` | 中文字体端到端可上屏 | **OCR 真读得出中文** |
| `UI_CFG_SELFTEST` | `ui_cfg.c` | NVS 写-回读-对拍 4 项 | `fails=0` |
| `UI_CFG_PERSIST_TEST` | `ui_cfg.c` | 跨重启持久化 | **必须跑两轮**（写/验分离） |
| `PROG_SELFTEST` | `prog_api.c` | 后端 8 项（S1~S8） | S7 **必须失败**（诚实性铁证） |
| `PROG_UI_TEST` | `prog_ui_probe.c` | 「按键→后端」写路径 23 步 | 成对 `op … start/end` |
| `WIFI_PASS_PROBE` | `wifi_sta.c` | 密码候选探测 | 拿到 IP 即锁定 |
| `RACE_AMPLIFY` | `app_main.c` | 放大 LVGL 竞态窗口 | 已被证伪，保留为反面教材 |

### 11.1 🔴 CMake `CACHE BOOL` 会粘住上次 -D 值

```cmake
set(PROG_SELFTEST 0 CACHE BOOL "...")
```

**实测踩过**：改了 `-DPROG_SELFTEST=1` 跑完自检，下次不传参数"重编产品版"
**仍得到自检版** —— CMake 记住上次的缓存值了。

**纪律**：产品构建**显式传 `-DXXX=0`**，并用
`grep XXX:BOOL build/CMakeCache.txt` 复核，最后**回读 md5 对拍**（信产物不信日志）。

⚠️ 例外：`BTN_SELFTEST` 是**硬编码 0**（不是 `CACHE`），所以不出现在 CMakeCache.txt 里。

### 11.2 探针的三个正确用法（`shot_probe` 为例）

```bash
# ① 只抓一页，压小与其它探针文本交错的窗口
idf.py -B build_shot -DSHOT_PROBE=1 -DSHOT_PAGE_FIRST=4 -DSHOT_PAGE_LAST=4 build flash
# ② 抓完整 boot（不能用 tail，会把崩溃-重启循环伪装成"一次干净启动"）
python3 tools/boot_cap.py out.log 30
# ③ 本地还原 PNG + 离线 OCR（macOS Vision 框架，零 API 零费用）
python3 tools/shot_fetch.py
swift tools/ocr_vision.swift out/shot4.png
```

🕳️ **shot 探针 `FIRST>0` 抓错页的老坑**：曾设 `s_page=9` 却**从未调
`ui_goto(9)`** → 首次 tick 抓的还是 `ui_init` 末尾留的第 0 页却打上 `##SHOT 9`
标签 → 越界。修法：`if (FIRST > 0) ui_goto(FIRST);`

🕳️ **超时给足**：shot 每页 ~17s，10 页要给 300~360s。给 120s 会卡在 page6
超时并**误判为失败**。

### 11.3 探针的取证铁律

- **别信 `strings`** 查探针标记（只认 ASCII，会漏）。用 Python 按字节查。
- **二进制载荷不要先 decode 成 str 再正则** —— 会把字节抹成替换字符 ⇒ 假阴性。
  字节级检索一律走 `.py`。
- **日志必须与指纹同时归档**，否则退化成不可复核的单源传说。
- **阴性对照**：每个探针都要有"应该不出现"的判据（LINK=0 专属串零命中、
  空白区非黑像素必须为 0）。

---

<a name="12-已知坑位档案"></a>
## 12. 已知坑位档案

### 12.1 验证工具的错比产品的错更危险

**产品代码的错会有人报错；验证工具的错会让所有人去怀疑产品代码。**

| 坑 | 现象 | 真因 |
|---|---|---|
| 回读长度写错 | `cmp` 报"有差异" | 读短 96 B（读多也会假差异） |
| `memcmp` 比结构体 | 对拍失败但字段全对 | 8 字节对齐空洞是栈垃圾 |
| `strings` 查探针标记 | 漏报残留 | 只认 ASCII |
| decode 后正则 | `帧=0` 假阴性 | 字节被抹成替换字符 |
| `tail -20` 看 boot | 崩溃循环伪装成干净启动 | 真相在日志开头 |
| `cmake -D` 省略 | 产品版仍带探针 | `CACHE BOOL` 粘值 |
| 声称为根因实为假设 | 照做刷出全黑屏 | 上一轮总结把因果读反了 |
| 用旧 md5 当当前 | 回读对不上 | 产物不可复现（时间戳+ELF SHA 变） |

### 12.2 编译/配置类

1. **LVGL 内置 TLSF 堆 64KB 撑爆** → `LV_USE_CLIB_MALLOC=y` + `ASSERT_MALLOC`
2. **`LV_USE_FONT_COMPRESSED=0`** → 每个字形返回 NULL → 全屏无字（只一条 WARN）
3. **配置真相只在 `build_xxx/config/sdkconfig.h`** —— 其它三处 grep 给四种答案
4. **搜错文件路径**（如 `io_mux_periph_signal.h` 在 esp32s3 下压根不存在）
   → 空返回极易被误读成"没定义"
5. **不死代码开关**：`ui_cfg.c` 曾新写 `cfg_eq()` 忘换调用点，编译零错误、
   日志照打、`fails=4` ⇒ 开了 `-Wunused-function` 让编译器指着它报

### 12.3 硬件类

6. **GPIO33~37 是八线 PSRAM**，`gpio_config` 静默通过，配置后 PSRAM 密集访问卡死
7. **背光 LED+/LED− 是电源脚**，不是控制脚 —— 这么接还黑 = 电源回路问题
8. **S3 只有 2.4GHz 射频** —— 5GHz AP 物理上扫不到（reason=201）
9. **S3 UART TX 走 GPIO Matrix 脚无效**（受控 A/B 实测 0/7000）
10. **`esptool -b 921600` 在本板报 `Invalid head of packet(0x00)`** → 用 115200
11. **NVS 在 `0x9000`** —— 刷机三段（`0x0`/`0x8000`/`0x10000`）不擦它

### 12.4 并发/时序类

12. **LVGL 非线程安全** —— 刷新任务必须最后启动（但这不是当年崩溃的真根因）
13. **栈帧炸弹**：函数内 `char buf[1<<20]` → GCC 给整函数分配 1MB 栈帧
    → `sp` 掉出 RAM → 硬栈保护 panic。宿主 8MB 栈完全掩盖
14. **忙等饿死 IDLE** → `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y` 下 TG1WDT 复位
    （症状与"硬件坏了"几乎一样）
15. **长循环必须 `vTaskDelay(1)` 让出 CPU**
16. **`flush_ready` 必须在 DMA 完成回调里** —— 放错位置会卡住缓冲
17. **`uart_set_loop_back` 使能瞬间冒 0xff** → 需灌注 2 字节 0x00 清空

### 12.5 方法论类（本工程最值钱的部分）

18. **注入点在解码之后 → 结构上抓不到该类 bug** ⇒ 解码器必须抽成纯函数 +
    宿主单元测试喂合成波形
19. **"编译通过"完全掩盖"修复没被使用"** ⇒ `-Wunused-function` + 回读 md5
20. **`lv_font_get_glyph_dsc` 不需解压** ⇒ 覆盖自检全绿但渲染 100% 失败 ⇒
    判据必须覆盖解压
21. **"没崩"不等于"对了"** ⇒ 抓图 + 非黑像素统计 + OCR 读回
22. **单次现象不能定案** ⇒ 受控 A/B（唯一变量）+ 阴性对照
23. **`-Werror=format-truncation`** 会拦 `%s`（SSID 最长 32B，NVS 缓冲 65B）
24. **产物不可复现** —— 同尺寸不同 md5（时间戳+内嵌 ELF SHA+尾部 SHA）
    ⇒ 回读对拍只认**本次本地产物**，别用记忆里的旧 md5
25. **遮挡式 bug**：A/B 唯一变量之外，**若两者共享同一盲点，实验无区分力**
    （DMC 的 CX1/CX2 都依赖"pad 输入缓冲器能否读回内部驱动信号"）
