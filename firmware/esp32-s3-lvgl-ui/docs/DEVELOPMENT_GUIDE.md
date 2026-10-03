# ESP32-S3 AI 远程调试器 — 整体开发文档

> 工程路径：`~/cow/esp32-s3-lvgl-ui`
> 硬件：ESP32-S3 QFN56 rev0.2 / 16MB Flash / 8MB PSRAM / ST7789P3 240×320
> 当前产品固件：`build_v3/esp32-s3-lvgl-ui.bin` · 1,650,656 B · md5 `fe0b41df2e45f33013cc3e85f575b3e4`

---

## 0. 这份文档怎么用

这是**总入口**。它不重复讲每一行代码，而是回答四个问题：这是什么、怎么构建、代码怎么组织、改东西怎么做。

要逐行看代码，另外三份：

| 文档 | 规模 | 覆盖 |
|---|---|---|
| **本文** `DEVELOPMENT_GUIDE.md` | — | 全局地图 / 构建手册 / 改代码的操作手册 / 决策记录 |
| [`CODE_ANNOTATION_v3.md`](CODE_ANNOTATION_v3.md) | 1095 行 | 16 个模块的**架构级**注解，含 25 条坑位档案 |
| [`ANNOT_01_boot_display.md`](ANNOT_01_boot_display.md) | 634 行 | `app_main.c` + `lcd_hw.c/h` **逐行** |
| [`ANNOT_02_button.md`](ANNOT_02_button.md) | 680 行 | `btn_decode.h` + `btn.h` + `btn.c` **逐行** |
| [`ANNOT_03_ui_core.md`](ANNOT_03_ui_core.md) | 952 行 | `ui.c` 1–460 行（骨架 / 原语 / 页 0–2）**逐行** |
| [`ANNOT_04_ui_pages.md`](ANNOT_04_ui_pages.md) | 1470 行 | `ui.c` 479–1357 行（页 3–8 / 导航 / 调度）**逐行** |

**读法建议**

- 第一次接触本工程 → 读本文第 1、2、4 章 → 再读 `CODE_ANNOTATION_v3.md` 第 2、3 章
- 要改 UI → 本文第 6 章「开发手册」→ 跳到对应 `ANNOT_0*` 找那一段
- 排查 bug → 本文第 10 章「坑位档案」先查一遍，再看架构文档
- 源码与文档冲突 → **以源码为准**，回来改文档

---

## 1. 产品是什么

一块带屏的 ESP32-S3 调试器，配 3 个物理按键，屏幕 320×240。固件做两件事：

**① 自诊断面板（探针/引脚/状态/设置/关于）**
显示真实数据：WiFi 连接状态与 IP、Flash 容量、剩余堆、运行时长、当前时间、当前引脚档案。

**② GPIO 级芯片编程器（烧录页）**
用板上的 GPIO 自己 bit-bang 出 SPI/UART 波形，去读写目标芯片。引脚映射是**声明式档案**——换目标芯片只改一条档案，协议代码一行不动。

### 1.1 明确不做的

| 不做 | 原因 |
|---|---|
| 没有屏幕驱动的固件 | `esp32-s3-lvgl-ui` 是**唯一**带屏固件。刷测试固件后必须回烧它 |
| 5GHz WiFi | S3 无 5GHz 射频，物理事实 |
| 掉电期间走时 | 无 RTC。存的是「设定那一刻」，掉电期间不计时（`ui_cfg.h` 已写明，不美化） |
| VPP 高压编程 | 档位 API 在，**本版无升压硬件**，`prog_hw.c` 里映射为 `-1` |
| 在线刷 ESP32 固件 | 目标 ESP32 的 VPP/VCC 未接 |

### 1.2 一条设计红线

> **没有后端支撑的页面一律不做。**
>
> 上一版（v2）的问题是"假"——页面好看但数据是编的。这一版每个数字都必须能追到来源：要么是 `esp_wifi_get_ip_info()`，要么是 `esp_flash_get_size()`，要么是 `hw_pin` 的状态快照。
>
> 这条红线催生了 `PROG_OP_RDID` 在 REAL 模式下**必须失败**这件事：没接芯片就报 `NOFLASH`，绝不假装成功。

---

## 2. 硬件定版

### 2.1 屏幕接线（10 脚裸屏）

⚠️ 实物是 10 脚：`GND RS CS SCL SDA RESET VDD GND LED+ LED-`（同链接的 14 脚详情图是错的，见第 10 章坑 #1）

| 屏脚 | ESP32-S3 | 说明 |
|---|---|---|
| SDA | **IO7** | SPI MOSI |
| SCL | **IO6** | SPI 时钟 |
| DC | **IO4** | 数据/命令 |
| RST | **IO5** | 复位 |
| CS | **IO10** | 片选 |
| VDD / GND | 3V3 / GND | |
| LED+ / LED− | 背光**电源脚**，非控制脚 | 与固件无关。标准接法=纯电源常亮 |

`bl = -1`（固件不驱动背光）。上电前必须限流点测背光两脚。

### 2.2 按键接线（低有效 + 内部上拉）

| 键 | GPIO | 作用 |
|---|---|---|
| UP | **IO15** | 上 / 加 / 确认 |
| DOWN | **IO16** | 下 / 减 / 返回 |
| BACK | **IO17** | 取消 / 退出 |

禁用脚：S3 strapping `0/3/45/46`；USB-JTAG `19/20`；八线 PSRAM `33~37`；UART0 控制台 `43/44`；LCD `4/5/6/7/10`；按键 `15/16/17`。

### 2.3 烧录器引脚（本板档案）

| 信号 | GPIO | 说明 |
|---|---|---|
| MOSI | 11 | 输出 |
| MISO | 13 | 输入 + 内部上拉（悬空读 1 → 诚实报 NOFLASH） |
| CK | 12 | 输出，空闲低（SPI mode 0） |
| CS | 14 | 输出，空闲高（未选中） |
| TX | 8 | UART1 发 |
| RX | 9 | UART1 收 |
| RST | 18 | 输出，空闲高 |
| VPP / VCC | – | 本版未映射 |

**换线只改 `prog_hw.c` 顶部的 7 个 `#define`**，档案表和协议代码不动。

### 2.4 分区表

```
nvs,      data, nvs,     0x9000,   0x6000      # 24KB  UI 配置 + WiFi 凭据
phy_init, data, phy,     0xf000,   0x1000
factory,  app,  factory, 0x10000,  0x400000    # 4MB   app
storage,  data, spiffs,  0x410000, 0xbf0000
```

**烧录只刷三段**：`0x0`（bootloader）、`0x8000`（分区表）、`0x10000`（app）。
**`0x9000`（NVS）不动** —— 刷测试固件回来后 WiFi 凭据和语言设置还在。

### 2.5 关键 sdkconfig

| 配置 | 值 | 备注 |
|---|---|---|
| `CONFIG_IDF_TARGET` | `esp32s3` | |
| `CONFIG_ESPTOOLPY_FLASHSIZE` | `16MB` | |
| `CONFIG_LV_COLOR_DEPTH` | `16` | |
| `CONFIG_LV_USE_FONT_COMPRESSED` | `y` | ⚠️ 见第 10 章坑 #2，这是 P0 事故 |

⚠️ **配置真相只在 `build_xxx/config/sdkconfig.h`**。ESP-IDF 没有 `config/sdkconfig` 纯文本，只有 `.h`/`.cmake`/`.json`。grep 错路径返回空，空返回极易被误读成"没定义"。

---

## 3. 快速开始

### 3.1 环境

```
ESP-IDF v5.x（xtensa-esp32s3 工具链已在 PATH）
macOS：/dev/cu.usbmodem*  （本板 esptool 必须用 115200，921600 报 Invalid head of packet）
```

### 3.2 产品构建（唯一正确的命令）

```bash
cd ~/cow/esp32-s3-lvgl-ui

# ⚠️ 必须显式传全部开关为 0 —— CMake CACHE BOOL 会粘住上次的 -D 值
idf.py -B build_v3 -DBTN_SELFTEST=0 -DRACE_AMPLIFY=0 -DPIN_PROBE=0 \
       -DPROBE_TEST_FREE_PIN=0 -DSHOT_PROBE=0 -DCJK_SMOKETEST=0 \
       -DUI_CFG_SELFTEST=0 -DUI_CFG_PERSIST_TEST=0 \
       -DPROG_SELFTEST=0 -DWIFI_PASS_PROBE=0 -DPROG_UI_TEST=0 \
       -DSHOT_PAGE_FIRST=0 -DSHOT_PAGE_LAST=9 build
```

复核（**必做**）：

```bash
grep -E "^(BTN_SELFTEST|RACE_AMPLIFY|PIN_PROBE|SHOT_PROBE|CJK_SMOKETEST|UI_CFG_SELFTEST|UI_CFG_PERSIST_TEST|PROG_SELFTEST|WIFI_PASS_PROBE|PROG_UI_TEST):BOOL" \
     build_v3/CMakeCache.txt
# 期望：10 个全部 =0
```

`BTN_SELFTEST` 是硬编码在 `CMakeLists.txt:26` 的（不是 CACHE），所以**不会**出现在 `CMakeCache.txt` 里 —— 这是已知例外。

### 3.3 烧录

```bash
esptool.py -p /dev/cu.usbmodemXXXX -b 115200 write_flash 0x0    build_v3/bootloader/bootloader.bin
esptool.py -p /dev/cu.usbmodemXXXX -b 115200 write_flash 0x8000 build_v3/partition_table/partition-table.bin
esptool.py -p /dev/cu.usbmodemXXXX -b 115200 write_flash 0x10000 build_v3/esp32-s3-lvgl-ui.bin
```

### 3.4 验收（八层，全绿才算完）

```bash
# ① 烘焙配置对
grep LV_USE_FONT_COMPRESSED build_v3/config/sdkconfig.h        # =1

# ② 构建无警告
# ③ 产物内无探针标记（阳性对照 + 字节口径）
python3 tools/check_probe_marks.py build_v3/esp32-s3-lvgl-ui.bin   # 期望退出码 0

# ④ 烧录 verified ×2

# ⑤ 回读逐位一致（⚠️ 必须按字节截断比对，不能直接 cmp）
esptool.py ... read_flash 0x10000 0x19A100 tmp/readback.bin
python3 -c "
a=open('build_v3/esp32-s3-lvgl-ui.bin','rb').read()
b=open('tmp/readback.bin','rb').read()[:len(a)]
print('IDENTICAL' if a==b else f'DIFF at {[i for i,(x,y) in enumerate(zip(a,b)) if x!=y][:5]}')"

# ⑥ 运行时单次启动（数 rst:0x 次数）
python3 tools/boot_cap.py tmp/out.log 20
grep -c "rst:0x" tmp/out.log        # =1
grep -cE "Guru Meditation|abort\(\)|LoadProhibited|StoreProhibited" tmp/out.log   # =0

# ⑦ 屏上真出画面
grep -E "LCD就绪|UI就绪|LVGL刷新任务已启动" tmp/out.log

# ⑧ 逐页抓图 + 离线 OCR 自证（可选但强烈建议改 UI 后做）
```

判据的完整版在 [`CODE_ANNOTATION_v3.md`](CODE_ANNOTATION_v3.md) 第 1 章。

---

## 4. 架构总览

### 4.1 七层

```
┌──────────────────────────────────────────────────────────────┐
│  L6  UI 层        ui.c (1357)   9 页 / 绘制原语 / 导航 / 定时刷新 │
├──────────────────────────────────────────────────────────────┤
│  L5  配置层      ui_cfg.c(384)  NVS 持久化 / 时间基准 / 自检      │
│                   i18n.c(429)   191 条 × 2 语言（生成产物）      │
├──────────────────────────────────────────────────────────────┤
│  L4  网络层      wifi_sta.c(474) 真实 STA / 扫描诊断 / 重连      │
├──────────────────────────────────────────────────────────────┤
│  L3  烧录协议    prog_api.c(526)  worker 任务 / 进度 / 日志      │
│                   hw_pin.c(1716)  引脚档案 / 双模驱动 / 25xx+ISP │
│                   prog_hw.c(288)  ESP32-S3 BSP（9 个回调）        │
├──────────────────────────────────────────────────────────────┤
│  L2  输入/显示   btn.c(187)+btn_decode.h(126)   3 键 / 环形队列  │
│                   lcd_hw.c(223)                  ST7789 + LVGL    │
├──────────────────────────────────────────────────────────────┤
│  L1  平台        ESP-IDF v5.x / FreeRTOS / NVS / esp_wifi       │
└──────────────────────────────────────────────────────────────┘
```

依赖方向**严格向下**，无反向依赖。`hw_pin.c` 是唯一不认识 ESP32 的层（只依赖 `stdint` + `string`），这也是它能原样搬回 xiaomo 宿主的原因。

### 4.2 五个 FreeRTOS 任务

| 任务 | 栈 | 优先级 | 职责 | 能调 LVGL？ |
|---|---|---|---|---|
| `lvgl` | 8192 | 5 | `lv_timer_handler` + 按键分发 + 页面刷新 | ✅ 唯一 |
| `btn` | 4096 | 6 | 20ms 轮询引脚 → `btn_decode_step` → 环形队列 | ❌ |
| `wifi_mgr` | 4096 | 5 | 扫描 / 连接 / 重连（串行，无并发） | ❌ |
| `prog` | 4096 | 5 | 烧录 worker：RDSR 轮询、页编程、ISP 握手 | ❌ |
| `main` | 3584 | 1 | `app_main`，跑完自删 | — |

**线程边界铁律**（违反就是隐蔽 bug）：

1. **只有 lvgl 任务能调 LVGL API**。其它任务要改 UI，只能往队列塞数据或改加锁的快照。
2. **任何 hw_pin 调用都不得从 UI 任务发起**。擦除/编程是毫秒~百毫秒级 + 阻塞轮询，会掉帧甚至喂不上看门狗。
3. **共享字符串必须上锁 + 拷贝**。`prog_api_status()` / `wifi_st_get()` 都是加锁拷贝，绝不能把结构体指针递给另一个任务。

### 4.3 跨任务数据流

```
 GPIO 引脚
    │  20ms 轮询
    ▼
 [btn 任务] ──解码──▶ 环形队列(16) ──▶ [lvgl 任务] 消费
                                                    │
                          ┌─────────────────────────┼──────────────┐
                          ▼                         ▼              ▼
                    页面切换/选中             prog_api_start()   读快照
                    (纯内存)                       │              (加锁)
                                                    ▼
                                              [prog 任务]
                                              hw_pin API
                                                    │
                                        BSP 回调 ────┴──▶ GPIO
                                                    │
                                       写快照(加锁) ──▶ [lvgl 任务] 轮询刷新

 [wifi_mgr 任务] ──写快照(加锁)──▶ [lvgl 任务] 状态页轮询
 [NVS] ──▶ [ui_cfg 缓存] ──▶ getter（不碰 flash）
```

### 4.4 启动时序（顺序是硬约束）

```c
cjk_probe_report();                    // ① 字形覆盖自检（刷机前就能跑，不依赖屏幕）
ui_cfg_init(&cfg); g_ui_lang = cfg.lang;// ② 配置必须赶在 UI 第一次取文案之前
lcd_hw_init();                         // ③ 建 display（不开刷新任务）
ui_init();                             // ④ 建 9 页对象 —— 此刻独占 LVGL
  └─ 首启? → ui_start_wizard()
cjk_probe_init();                      // ⑤ 中文冒烟屏（产品构建=空实现）
wifi_st_start();                       // ⑥ 让 UI 先占稳内部 RAM，再让 WiFi 申请缓冲
shot_probe_start();                    // ⑦ 仅探针构建
lcd_hw_start();                        // ⑧ 对象建完，才放刷新任务出来
prog_ui_probe_start();                 // ⑨ 仅探针构建
```

**为什么 WiFi 排在 `ui_init()` 之后** —— 反过来会让 WiFi 驱动先申请几十 KB 缓冲，把 UI 挤到 OOM，表现为 `StoreProhibited` 崩溃循环。

**关于 ④⑧ 的顺序**：源码注释写"刷新任务必须最后启动"。这是**正确的防御性写法，但不是当年崩溃的根因**。当年的受控实验（`RACE_AMPLIFY`）已证伪时序假设，真根因是 LVGL 64KB TLSF 堆耗尽。拆时序保留，因为它把一类风险在结构上消掉了。

---

## 5. 模块手册

每条格式：**职责 → 关键决策 → 改它要注意什么**。

### 5.1 `app_main.c`（151 行）

**职责**：唯一入口，编排启动顺序。

**关键决策**
- 探针调用集中在自检块里，产品构建全部编译掉
- `RACE_AMPLIFY` 分支是**反面教材留档**：故意用旧顺序 + `ui_init` 里让出 CPU，用来对竞态假设做因果实证

**改它要注意**
- 想加东西，问"它必须在 LVGL 独占期之前还是之后"
- 新探针插进来时注意顺序：`UI_CFG_SELFTEST` 会擦 NVS 再还原，必须排在 `UI_CFG_PERSIST_TEST` 之前，否则 persist 写进去的配置会被 selftest 的还原覆盖

### 5.2 `lcd_hw.c/h`（223 / 57 行）

**职责**：ST7789P3 初始化 + LVGL display 绑定 + 刷新任务。

**关键决策**
- 20MHz 硬 SPI + 横屏 320×240
- `flush_ready` 必须在 **DMA 完成中断**里调，不能在 flush 回调末尾
- 缓冲必须 `MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL`
- `io_cfg.user_ctx` 创建后不可改，所以回调里走文件内静态 `s_disp`

**改它要注意**（三个必踩坑）
1. `flush_ready` 放错位置 → 花屏/撕裂
2. 缓冲不是 DMA 内部内存 → 偶发花屏，且**不报错**
3. `lv_display_set_buffers` 的 size 是**字节**还是**像素**未定案（疑越界读写致首帧崩）—— 改这块要配抓图验证

### 5.3 `btn_decode.h` + `btn.c`（126 + 187 行）

**职责**：3 键去抖解码 + 环形队列。

**关键决策 —— 非对称去抖**

| 事件 | 阈值 | 为什么 |
|---|---|---|
| 按下 | 30ms | |
| 抬起 | 300ms | |
| 真松开 | 500ms | 长按后静默 |
| 长按 | 1.2s | |

**这是整个工程最反直觉的一处设计，理由必须记住**：
真机按住一个键 5 秒，触点会慢速瞬断（实测 180ms 高电平）。如果按下和抬起共用 60ms 去抖，瞬断会被识别成"抬起 + 重新按下"，`press_us` 反复重置，长按永远凑不满 1.2s。真机铁证：**一次 5 秒长按 = 12 个短按事件，间隔 300~900ms**。

**改它要注意**
- `btn_decode.h` 是**纯函数**（无全局状态、无硬件访问），所以能编到宿主跑单元测试：`tools/btn_decode_test.c` 喂瞬断波形，8/8 PASS
- ⚠️ 单元测试失败时**先怀疑用例**：10ms 采样粒度表达不了 <10ms 的特征
- 事件注入点 `btn_inject()` 塞进**真实环形队列**，与物理按键同路径

### 5.4 `ui.c`（1357 行）

**职责**：9 页的构建、刷新、按键分发、页签导航。

**页面表**

| # | 枚举 | 名称 | 主题色 | 数据源 |
|---|---|---|---|---|
| 0 | `PG_BOOT` | 开机 | 青 | 品牌 + 2.4s 计时 |
| 1 | `PG_WIZ` | 首启向导 | 青 | `ui_cfg` 写 NVS |
| 2 | `PG_HOME` | 主菜单 | 青 | 本地静态表 |
| 3 | `PG_PROBE` | 探针检测 | 青 | `hw_pin` 状态快照 |
| 4 | `PG_FLASH` | 烧录器 | 绿 | `prog_api` 状态快照 |
| 5 | `PG_PINS` | 引脚档案 | 黄 | `hw_pin_active()` |
| 6 | `PG_STATUS` | 状态信息 | 青 | WiFi / esp_flash / esp_timer / 堆 |
| 7 | `PG_SETUP` | 系统设置 | 粉 | `ui_cfg` 写 NVS |
| 8 | `PG_ABOUT` | 关于 | 青 | `esp_app_get_description()` / `esp_flash_get_size()` |

**几何常量**（改布局前先看这三个）

```c
#define SCR_W   320     # 屏宽
#define SCR_H   240     # 屏高
#define TITLE_H 32      # 标题栏高
#define BODY_Y  34      # 内容区起点
#define HINT_Y  203     # 底部提示栏
```

**绘制原语**（全部是 `static`，加新页只用这几个）

```c
lv_obj_t *mk_page (int pg, const char *title, uint32_t accent);
lv_obj_t *mk_card (lv_obj_t *parent, int x, int y, int w, int h, uint32_t accent, lv_opa_t opa);
lv_obj_t *mk_txt  (lv_obj_t *parent, const char *s, const lv_font_t *f, uint32_t color, int x, int y);
lv_obj_t *mk_tr   (lv_obj_t *parent, uint16_t tid, const lv_font_t *f, uint32_t color, int x, int y);
lv_obj_t *mk_bar  (lv_obj_t *parent, int x, int y, int w, int h, uint32_t color);
lv_obj_t *mk_hint (lv_obj_t *page, const char *s);
```

**⚠️ 多卡布局的定版规矩**：LVGL 后建的对象在上层，所以多卡页面**凭感觉写 y 必致底卡盖上卡底部**（踩过：PROG 页卡 A 被盖 14px、IMAGE 行剩半截）。

**算累加高度**并把算式写进注释。已定版：卡 A `38-92`、卡 B `94-182`、卡 C `184-228`。
自证方法：逐行数非黑像素 —— 边框行应 ~300px 整条，卡间间隙行应 0。

**⚠️ 字体行高陷阱**（写文档时算出来的，未修）

`ui_cjk_14` 的 `line_height = 17`。但 PINS 页用 `48 + i*12` 步进 12px 放 9 行：

| 位置 | 步进 | 字高 | 结论 |
|---|---|---|---|
| PINS 9 行 | 12 | 17 | ⚠️ **重叠 5px，9 行会糊** |
| PROBE 4 行 | 16 | 17 | 轻（1px，密集列表可接受） |
| FLASH 左栏 8 行 | 18 | 17 | ✅ 正常 |

建议方案：跳过 `gpio < 0` 的未映射行（7 行 × 14px = 98px，放得进 164px 卡片），且"未映射"本就不该占视觉空间。
**但改之前必须抓图 + OCR 验证，不能凭算术定案**。

**其它两处轻微发现**
- FLASH 页：`FLASH_LOG_N = 4` 但 `PROG_LOG_LINES = 3` → 第 4 行 label 永远空
- `wiz_refresh()` 里的 `(void)rows;` 是重构残留，`rows` 算出来从未用

**改它要注意**
- **不要长期缓存 `TR(...)` 返回的指针** —— 切语言后旧指针仍指向旧语言，会显示成"半中半英"。要缓存就存 `idx`，切语言时统一重刷
- `lv_color_hex()` 在 LVGL v9 是**函数不是常量**，不能进静态初始化表。所以 `PINFO[]` 存 `uint32_t` 再用 `pac(i)` 转换
- 页面对象 `s_page[]` 全部在 `ui_init()` 里建好并常驻，切换只改 `lv_obj_set_hidden` —— 不要在按键回调里 `lv_obj_del` 再建

### 5.5 `ui_cfg.c/h`（384 / 91 行）

**职责**：NVS 持久化 + 无 RTC 时间基准。

**关键决策 —— 三层结构不可合并**

```
nvs_read_raw()  纯读，无副作用
ui_cfg_init()   读 + 校验 + 立时间基准 + 更新缓存   ← 有副作用
ui_cfg_save()   写 + 用 nvs_read_raw 回读对拍
```

**回读绝不能走 `ui_cfg_init()`** —— 它会把 `s_base_us` 重置成"此刻"，时钟每被读一次就冻结一次。这个坑写在文件头注释里。

**⚠️ 绝不用 `memcmp` 比结构体**
`ui_cfg_t` 里 `int64_t` 要求 8 字节对齐，`_pad`(偏移 3) 与 `epoch`(偏移 8) 之间有 4 字节**对齐空洞**。结构体初始化不写它，两边的空洞各是各自的栈垃圾 → `memcmp` 必然失败，而逐字段打印却"全都一样"。真踩过：`selftest` 报"ver 1/1 lang 1/1 sntp 1/1 epoch 相等"。只比数据字段。

**自检 4 项**（`ui_cfg_selftest`）
1. 写入 → 回读 → 逐位对拍（正向）
2. 二次写入不同值（抓"改了但读到旧值"）
3. 擦除后必须真的读不到（抓"首启判定恒假"）
4. 还原原状

**跨重启探针**（`ui_cfg_persist_probe`）—— 这是"断电重启设置不丢"判据的**真形态**。selftest 只证单次开机内一致，证不了掉电。必须跑两轮，中间硬复位。

**改它要注意**
- 改字段必须同步 `UI_CFG_VER`，否则旧记录会被判首启
- 越界值在两处都要挡：读时回落、写入时拒收

### 5.6 `i18n.c/h` + `fonts/`（429 / 248 行 + 生成物）

**职责**：191 条 × 2 语言文案表 + 两款中文子集字体。

**⚠️ 这三个是生成产物，绝不手改**

| 生成物 | 源真 | 生成脚本 |
|---|---|---|
| `i18n.c` / `i18n.h` | `main/i18n/strings.txt` | `tools/gen_i18n.py` |
| `fonts/ui_cjk_14.c` / `ui_cjk_16.c` | `strings.txt`（同源） | `tools/gen_font.py` |

手改会在下次生成时被**无声覆盖**。

**字体链的依赖关系**（漏一步就出方块字）

```
strings.txt ──gen_i18n.py──▶ i18n.c/h
                │
                └──gen_font.py──▶ ui_cjk_14.c / ui_cjk_16.c
```

字体子集依赖文案表 —— 改了文案**必须重跑两个脚本**。

**其它要点**
- `UI_STR_COUNT 191`，自检按这个数断言
- `UI_STR_MISSING` 故意是醒目非空串（`✗ [i18n MISSING]`），因为空串在屏上表现为"标签凭空消失"，排查成本极高
- 字体格式 `bitmap_format = 1`（压缩），与 `CONFIG_LV_USE_FONT_COMPRESSED=y` 必须严格一致

### 5.7 `wifi_sta.c/h`（474 / 59 行）

**职责**：真实 STA 连接 + 启动期扫描诊断 + 掉线重连。

**关键决策**

1. **扫描用阻塞式但绝不放在事件回调里**。事件回调跑在 `sys_evt` 任务，阻塞 2~3s 会顶住整个 WiFi 事件循环。所以扫描/连接统一放自己的 `wifi_mgr` 任务。
2. **先定向扫描再全量扫描**。定向扫描直接问"这个 SSID 在不在 2.4GHz 上" —— 空结果就是强证据（5GHz AP / 太远 / 隐藏 / 拼写不同），比全量后过滤硬得多。
3. **状态用互斥量 + 拷贝暴露**，绝不把结构体指针递给 UI。
4. `threshold.authmode = WIFI_AUTH_WPA2_PSK` 明确限到 WPA2（AP 实测 auth4=WPA/WPA2 混合，不含 WPA3），排除 WPA3-SAE 协商这条岔路。
5. reason 15 / 202 的日志明确写"**大概率密码不对**"。

**凭据存储**：`WIFI_CFG_SSID` / `WIFI_CFG_PASS`（`main/wifi_cfg.h`）作默认值打底，NVS `wifi_cfg` 命名空间覆盖，连上即落盘。

> 🔒 `main/wifi_cfg.h` 里有明文密码，但**该目录不在任何 git 仓库内**（`git rev-parse --show-toplevel` 核实过）。若将来并入 git，必须先 gitignore 或改成 NVS 配网，**切勿 `git add .`**。该纪律已写进文件注释。

**密码候选探测**（`WIFI_PASS_PROBE=1`，仅排查）
起因：用户给的密码末尾带 `..`，疑似省略号。策略 = 4 个候选逐个灌进驱动，谁先拿到 IP 就是它。**产品构建必须为 0**。

**改它要注意**
- `snprintf` 到 `wifi_config` 的字段必须限宽（`%.31s` / `%.63s`），直接 `%s` 会被 `-Werror=format-truncation` 拦下
- NVS 读出的 buf 是 65 字节但 SSID 最长 32 → 同样必须 `%.32s` / `%.64s`

### 5.8 烧录器三层

这是全工程最复杂的部分，**三层职责必须分清**。

```
hw_pin.c (1716)   协议 + 档案 + 驱动   ← 跨平台，不认识 ESP32
    ↑ 9 个 BSP 回调注入
prog_hw.c (288)   ESP32-S3 BSP          ← 把引脚动作接到真 GPIO/UART
    ↑
prog_api.c (526)  任务化外壳            ← 进度 / 日志 / 结论 / 线程边界
```

#### 5.8.1 `hw_pin.c` —— 三层合一

| 层 | 内容 |
|---|---|
| **L2 引脚档案** | `hw_pin_profile_t`：9 个逻辑信号 → GPIO 的映射表 + UART 波特 + SPI 参数 |
| **L1 双模驱动** | `hw_pin_spi_xfer()`：HW 模式（外设映射）/ BB 模式（纯 GPIO bit-bang） |
| **L0 电压层** | `hw_pin_vpp_set()`：5 档编程电压（0/5/12/12.5/21V），本版无硬件 |
| **L3 协议插件 ×2** | 25xx SPI Flash（0x9F/0x03/0x02/0x20/0x06/0x05/WEL）+ STM32 UART ISP（AN3155） |

**内置 4 条档案** + 黄金值 `HW_PIN_GOLDEN = 0x9E0F10FA`（档案表 FNV-1a-32，独立 Python 对拍锁定）。

⚠️ **别动内置黄金表**。本板档案用 `hw_pin_profile_load()` 注入，这样 `HW_PIN_GOLDEN` 校验（内置 4 条档案的 FNV）仍成立，回归不破。

**器件模型**（SIM 模式）：W25Q128 状态机，与时钟边沿无关 → 确定性。RDID 稳定返回 `EF 40 18`。
**ISP 模型**：STM32F103 medium-density bootloader v3.1，8KB 内存，11 条命令表，PID `0x0410`。

**⚠️ 本工程只跑 BB 模式。** `s_bsp_real.spi_xfer = NULL` → 强制 bit-bang。HW 模式（外设映射）留待后续版本。

> 🔴 已知限制：ESP32-S3 的 **UART TX 走 GPIO Matrix 无效**（受控 A/B 实测：IOMUX 脚 IO17 → 1974/7000 采样点，matrix 脚 IO9 → 0/7000）。S3 的 UART0/UART1 各只有一对 IOMUX 专属脚。本板 TX=IO8 / RX=IO9 **恰好是 UART1 的专属脚**，所以这条路能成立。**SPI 是否同样受限未测** —— 列为高风险待验。

#### 5.8.2 `prog_hw.c` —— BSP

9 个回调：`gpio_dir` / `gpio_write` / `gpio_read` / `delay_us` / `spi_xfer` / `uart_putc` / `uart_getc` / `vpp_set` / `vpp_read`。
任意一个可以是 NULL → 该动作回落模拟器（所以可以只注入 SPI，其余仍可回归）。

**★ SIM/REAL 的分水岭就是那个 NULL**

```c
// hw_pin.c 的 bit-bang 里：
use_dev = (g_bsp.gpio_read == NULL)
//   SIM  → BSP 全 NULL → 器件模型供 MISO → RDID 稳定 EF 40 18
//   REAL → 装了 gpio_read → 真读 MISO 引脚 → 没接芯片就诚实报 NOFLASH
```

这是"不假装成功"的**结构性保证**，不靠上层自觉。所以自检 S7 **必须失败**才算过。

**其它要点**
- hw_pin 从不调用 `bsp->gpio_dir`（grep 实测零命中）→ **方向必须 BSP 自己配**，否则 bit-bang 直接失真
- MISO 配内部上拉：悬空读 1（与真闪存未选中时一致）→ JEDEC 读到 `0xFF` → 诚实 `NOFLASH`
- `uart_putc` 后必须 `uart_wait_tx_done`：ISP 是半双工一问一答，不等会读到自己的回显
- **真机自证 `prog_hw_loopback()`**：MOSI↔MISO 跳线短接，走的就是生产路径，能同时证"档案对 + 方向对 + 时序跑得动 + 接线通"

#### 5.8.3 `prog_api.c` —— 任务化外壳

**为什么要独立任务**：① 擦除/编程是毫秒~百毫秒级，卡在 LVGL 任务会掉帧甚至喂不上看门狗；② RDSR 轮询 busy、UART 等应答都是**阻塞**操作；③ 后台跑 + 前端轮询 = "进度条不是演出来的"的结构性保证。

**5 个操作**

| op | 做什么 | 关键点 |
|---|---|---|
| `READ ID` | `0x9F` → JEDEC | 厂商码译码在 UI 侧 |
| `ERASE` | 扇区擦除 + RDSR 轮询 | 每次循环 `vTaskDelay(1)` 让出，别饿死 IDLE |
| `PROGRAM` | 擦 → 页编程 → **自动 verify** | 写前必擦，否则 verify 必挂 |
| `VERIFY` | 回读逐字节比对 + 定位首个不符 | 定位比一句 FAIL 有用得多 |
| `ISP SYNC` | UART `0x7F` 握手 + 读版本/PID | 每次都重置握手态，别把上轮值留在屏上 |

**3 份镜像**（确定性，同一选择永远是同一份字节）

| 名称 | 大小 | 用途 |
|---|---|---|
| `banner-256` | 256 B | 可读文本，串口/逻辑分析仪直接认出来 |
| `pattern-1k` | 1 KB | 递增+异或，便于脚本比对错位 |
| `rand-2k` | 2 KB | 确定性 LCG（**不用 `rand()`** → 跨次运行一致可复现） |

**诚实性诊断 `diag_real()`**
REAL 模式失败后跑一次 MOSI↔MISO 回环，把"引脚/时序对不对"从"芯片有没有接"里切出来：

| 回环结果 | 日志 | 含义 |
|---|---|---|
| `= 0` | `loopback MOSI-MISO OK` / `pins ok, target silent` | 引脚好，目标沉默 → 没接芯片 |
| `> 0` | `loopback err@N XX!=YY` / `check wiring/jumper` | 引脚/接线问题 |
| `< 0` | `loopback n/a` | 档案里没 SPI 脚，测不了 |

**改它要注意**
- `s_image[2048]` **必须 static** —— ">512B 局部数组一律 static"是 hw_flash 那个 25KB 真机栈炸弹换来的纪律
- `prog_api_cfg()` 在 worker 跑时返回 -1，不换档案（否则把操作打断成半截）
- `prog_api_start()` 不排队直接拒（避免误以为在排队）

### 5.9 探针模块（4 个文件）

| 文件 | 行数 | 触发宏 | 干什么 |
|---|---|---|---|
| `btn.c` | 187 | `BTN_SELFTEST` | 注入按键事件，验证队列→tick_cb→`ui_goto`→显隐全链 |
| `shot_probe.c` | 223 | `SHOT_PROBE` | `lv_snapshot` 逐页 dump 到串口，本地还原 PNG |
| `cjk_probe.c` | 239 | `CJK_SMOKETEST` | 191 条 × 2 语言字形覆盖自检 + 中文冒烟屏 |
| `prog_ui_probe.c` | 121 | `PROG_UI_TEST` | 30 步脚本化注入按键，走真实队列 → 烧录后端 |
| `pin_probe.c` | 145 | `PIN_PROBE` | GPIO33~37 保留状态 + PSRAM 完整性对拍 |

⚠️ **`CJK_SMOKETEST` 的由来**：字形数组是 `static`，会被 `--gc-sections` 回收。踩过一次：**编译零错误、固件一字节没变、nm 查不到符号，却看起来一切正常**。编译不能证上屏。

---

## 6. 开发手册（How-to）

### 6.1 加一个页面

```c
// ① main/ui.c 顶部：加枚举（在 PG_MAX 前）
enum { PG_BOOT=0, PG_WIZ, PG_HOME, PG_PROBE, PG_FLASH,
       PG_PINS, PG_STATUS, PG_SETUP, PG_ABOUT,
       PG_YOURPAGE, PG_MAX };

// ② 加 PINFO 条目（tid 用 i18n key，accent 用主题色）
{TID_P_YOUR_TITLE, 0x00E5FF},

// ③ 写 static build_yourpage(void)，照抄 build_pins 的骨架：
//    lv_obj_t *p = s_page[PG_YOURPAGE];
//    lv_obj_t *card = mk_card(p, 8, BODY_Y, SCR_W-16, 164, C_YELLOW, LV_OPA_40);
//    mk_tr(card, TID_P_YOUR_LABEL, F14, C_GRAY, 10, 4);
//    mk_txt(card, "--", F16, C_MINT, 110, 1);   // ← 存进 static 供 refresh 改
//    mk_bar(card, 12, 26, SCR_W-44, 1, C_LINE);
//    mk_hint(p, TR(TID_S_BACK));

// ④ 写 static yourpage_refresh(void)（如有动态数据）
// ⑤ ui_init() 里调用 build_yourpage()
// ⑥ ui_goto() 里补显隐分支
// ⑦ build_dots() 的页标数量会自动跟着 PG_MAX 走
// ⑧ handle_buttons() 里补按键处理
```

**必做检查**
- 多卡布局**算累加高度**并把算式写进注释
- 字体行高 vs 步进（`F14`=17 / `F16`=20）
- 改完抓图 + OCR 验证，见 `CODE_ANNOTATION_v3.md` 第 6 章

### 6.2 加一条文案

```bash
# ① 改源真（绝不改 i18n.c）
$EDITOR main/i18n/strings.txt
#    格式：key | 中文 | English

# ② 重跑生成（两个都要跑，字体子集依赖文案表！）
python3 tools/gen_i18n.py
python3 tools/gen_font.py

# ③ 检查 UI_STR_COUNT 是否变了 —— i18n.h 里的 #define UI_STR_COUNT 要跟着更新
grep UI_STR_COUNT main/i18n.h

# ④ 构建 + 抓图 + OCR 确认新字上屏了
```

漏跑 `gen_font.py` → 新字显示为方块。

### 6.3 加一种语言

比 6.2 麻烦，因为涉及枚举、存储、UI：

```c
// ① i18n.h：UI_LANG_COUNT 改 3，UI_LANG_LAST 改 2
// ② ui_cfg.c：越界检查 `c.lang > 1` 改 `> UI_LANG_LAST`（ui_cfg.c:120 和 :256 两处）
// ③ i18n/strings.txt：加第三列
// ④ 重跑两个生成脚本
// ⑤ 检查 gen_i18n.py 是否支持 3 列（大概率要改）
```

⚠️ `ui_cfg.c` 里有**两处** `> 1` 硬编码（读时回落 + 写入时拒收），容易只改一处。

### 6.4 改引脚

| 改什么 | 改哪 |
|---|---|
| 烧录器引脚 | `main/prog_hw.c` 顶部 7 个 `#define` |
| 烧录器档位/协议 | `main/prog_hw.c` 三条 `s_prof_*` 档案 |
| 按键引脚 | `main/btn.h`（`btn_init` 里的 `s_pin[]`） |
| 屏引脚 | `main/lcd_hw.c` 顶部的 `#define` |

改完**必做**：改 `PROBE_TEST_FREE_PIN` 跑换脚对照实验 —— "换引脚再跑一遍"才能区分"引脚的问题"和"探针自己的问题"。

### 6.5 加一个烧录操作

```c
// ① prog_api.h：enum 加一项（放在 PROG_OP_MAX 前）
PROG_OP_MYOP,

// ② prog_api.c：写 static void op_myop(void)
//    套路：plogf("op: 起始描述") → 调 hw_pin API → set_pct() 分段
//    → 失败时 diag_real(rc) → set_finish(rc, NULL)
// ③ prog_api.c：g_op_name[] 同步加名字（顺序必须和 enum 一致）
// ④ prog_api.c：run_op() 的 switch 加 case
// ⑤ i18n/strings.txt：加文案 → 重跑生成脚本
// ⑥ ui.c：FLASH 页加一条操作项
// ⑦ prog_api.c：自检加一项（SIM 要真跑，REAL 要诚实失败）
```

⚠️ `g_op_name[]` 是**按位置**索引的，加 enum 不加它会显示错名字，而且不报错。

### 6.6 加一个探针

```cmake
# ① main/CMakeLists.txt 加：
set(MY_PROBE 0 CACHE BOOL "我的探针（仅排查用）")
target_compile_definitions(${COMPONENT_LIB} PRIVATE MY_PROBE=${MY_PROBE})
# ② main/my_probe.c 里用 #if MY_PROBE 包起来
# ③ app_main.c 里 #if MY_PROBE 调用（放在 lcd_hw_start 之后还是之前，取决于要不要读屏）
# ④ 写 tools/my_check.py 解析串口输出成 PASS/FAIL
#    （my_probe.c / my_check.py 是**你要新建的文件名**，不是现成工具）
# ⑤ 收尾：把新旋钮加进本文 7.1 表 + 3.2 的产品构建命令 + 8.1 判据 3a 的 grep
```

⚠️ **产品构建必须显式传 `-DMY_PROBE=0` 并 `grep` 复核 `CMakeCache.txt`** —— CMake `CACHE BOOL` 会粘住上次的值。

---

## 7. 探针与自证体系

### 7.1 13 个构建旋钮

| 宏 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `BTN_SELFTEST` | **硬编码** | 0 | 按键链路合成事件自检（不在 CMakeCache，靠 `strings` 查产物） |
| `RACE_AMPLIFY` | CACHE BOOL | 0 | 竞态放大器（旧顺序 + `ui_init` 让出） |
| `PIN_PROBE` | CACHE BOOL | 0 | GPIO 保留状态 + PSRAM 对拍 |
| `PROBE_TEST_FREE_PIN` | CACHE BOOL | 0 | 探针改用空闲脚 IO18 做对照组 |
| `SHOT_PROBE` | CACHE BOOL | 0 | 逐页 `lv_snapshot` dump |
| `CJK_SMOKETEST` | CACHE BOOL | 0 | 191×2 字形覆盖 + 中文冒烟屏 |
| `UI_CFG_SELFTEST` | CACHE BOOL | 0 | NVS 写入回读对拍（4 项） |
| `UI_CFG_PERSIST_TEST` | CACHE BOOL | 0 | NVS 跨重启持久化（**两轮**） |
| `PROG_SELFTEST` | CACHE BOOL | 0 | 烧录后端自检（SIM 真跑 + REAL 诚实失败） |
| `WIFI_PASS_PROBE` | CACHE BOOL | 0 | WiFi 密码候选探测 |
| `PROG_UI_TEST` | CACHE BOOL | 0 | 烧录写路径 30 步注入 |
| `SHOT_PAGE_FIRST` | CACHE STRING | 0 | 抓图起始页 |
| `SHOT_PAGE_LAST` | CACHE STRING | 9 | 抓图结束页 |

> 早先的架构文档写"7 个编译开关"是**过期记录** —— 那时候还没加 `UI_CFG_*` / `PROG_UI_TEST` / `WIFI_PASS_PROBE`。以本表为准。

### 7.2 抓图 + OCR 流水线

```bash
# ① 抓（⚠️ 只能抓一页一次，压小与其它探针文本交错的窗口）
idf.py -B build_shot -DSHOT_PROBE=1 -DSHOT_PAGE_FIRST=3 -DSHOT_PAGE_LAST=3 build flash
python3 tools/boot_cap.py tmp/shot.log 60      # 抓完整 boot，不能用 tail
python3 tools/shot_fetch.py tmp/shot.log tmp/   # 串口 dump → 本地 PNG

# ② OCR（macOS Vision 框架，零 API 零费用）
swift tools/ocr_vision.swift tmp/page3.png
```

⚠️ **`tail -20` 会把崩溃-重启循环伪装成"一次干净启动"** —— 崩溃后 panic 重启那轮往往完全成功，真相在日志**开头**。必须全文落盘。

### 7.3 产物探针标记检查

用 `tools/check_probe_marks.py`。它比一条 `strings | grep` 强在四点：

1. **按字节统计**，绕开 `strings` 只认 ASCII 的静默失败
2. **先跑阳性对照**（`LVGL` / `ui_cfg` / `backend up` / `jumper` 必须命中）——
   对照没过就停止，不对本镜像下结论
3. **顺带打印 CMakeCache 该核对的旋钮名**
4. **退出码可用于脚本门禁**

```bash
$ python3 tools/check_probe_marks.py build_v3/esp32-s3-lvgl-ui.bin
镜像: build_v3/esp32-s3-lvgl-ui.bin  (1650656 字节)
── 阳性对照（必须命中，否则本工具不可信）──
  LVGL               4  OK
  ui_cfg             2  OK
  backend up         1  OK
  jumper             1  OK
── 探针标记（产品固件必须全部 0）──
  SELFTEST 0 / selftest 0 / SHOT_PROBE 0 / ##SHOT 0 / ##DONE 0
  [persist] 0 / UI_CFG_PERSIST_TEST 0 / RACE_AMPLIFY 0
  PROG_UI_TEST 0 / WIFI_PASS_PROBE 0 / CJK_SMOKETEST 0
  password_probe 0 / candidate 0
✅ 结论：产物内探针标记全部 0 命中（阳性对照已通过）。   退出码 0
```

**区分力实测**（同一工具跑两份镜像）：

| 镜像 | 命中数 | 退出码 | 判定 |
|---|---|---|---|
| `build_v3`（产品） | 0 | 0 | ✅ 干净 |
| `build_shot`（探针） | 3（`SHOT_PROBE` 1 + `##SHOT` 3 + `##DONE` 1） | 1 | ❌ 逮到 |

⚠️ **这个工具自己也踩过一次假阳性**（见第 10 章坑 #13）。教训已写进工具源码注释。

### 7.4 CMakeCache 侧核对

```bash
$ grep -E "^(RACE_AMPLIFY|PIN_PROBE|PROBE_TEST_FREE_PIN|SHOT_PROBE|CJK_SMOKETEST|UI_CFG_SELFTEST|UI_CFG_PERSIST_TEST|PROG_SELFTEST|WIFI_PASS_PROBE|PROG_UI_TEST):BOOL" \
       build_v3/CMakeCache.txt
CJK_SMOKETEST:BOOL=0          PIN_PROBE:BOOL=0
PROBE_TEST_FREE_PIN:BOOL=0    PROG_SELFTEST:BOOL=0
PROG_UI_TEST:BOOL=0           RACE_AMPLIFY:BOOL=0
SHOT_PROBE:BOOL=0             UI_CFG_PERSIST_TEST:BOOL=0
UI_CFG_SELFTEST:BOOL=0        WIFI_PASS_PROBE:BOOL=0
```

10 个全 0。`BTN_SELFTEST` 是硬编码在 `CMakeLists.txt:26` 的，不在 cache 里，靠 `check_probe_marks.py` 的产物字节查兜底。

### 7.5 自证的三个层次

| 层次 | 能证什么 | 不能证什么 |
|---|---|---|
| 编译通过 | 语法、类型、链接 | 上屏、上电、时序 |
| 运行时日志 | 任务跑起来了、数据合理 | 像素画对了 |
| 抓图 + OCR | 像素上确实有这些字 | 字是怎么来的 |

**OCR 只作辅助证据，最终以源码级来源核裁定案**。例：OCR 出 `cfg s3-spi/SIM` 后，还要 grep 这个精确串在 `ui.c` 里零命中、在 `prog_api.c:392` 命中 —— 才能坐实"后端数据上屏"。

---

## 8. 验证判据

### 8.1 产品固件六层判据

| # | 判据 | 怎么查 | 当前实测 |
|---|---|---|---|
| 1 | 烘焙配置对 | `grep LV_USE_FONT_COMPRESSED build_v3/config/sdkconfig.h` | `=1` ✅ |
| 2 | 构建零警告 | `idf.py build` 输出 | ✅ |
| 3a | CMakeCache 10 旋钮全 0 | 见 7.4 | 10/10 = 0 ✅ |
| 3b | 产物内探针标记全 0 | `python3 tools/check_probe_marks.py build_v3/esp32-s3-lvgl-ui.bin` | 13/13 = 0，退出码 0 ✅ |
| 4 | 烧录 `verified` ×2 | esptool 输出 | ✅ |
| 5 | 回读逐位一致 | 字节截断比对（见第 10 章坑 #10） | ✅ md5 `fe0b41df…` |
| 6 | 运行时健康 | 数 `rst:0x` 次数 + 崩溃关键字 | 单次 `rst:0x1`、崩溃 0 ✅ |

> 判据 2 只说"零警告"，不写死编译目标数（`1850/1850` 之类）—— 那个数字随依赖版本漂移，且日志不留档时无法复核。**判据要写成能重跑的形式，不能写成引用一个查不到来源的数字。**

### 8.2 改 UI 后的加严判据

7. 逐页抓图（9 页）
8. 离线 OCR 逐页（确认文案正确、中文没变方块）
9. **逐行数非黑像素**（布局/粘行）
10. **阴性对照**（空白区必须 0 墨点）

### 8.3 改后端后的加严判据

11. 对应探针跑通（如改烧录器 → `PROG_UI_TEST` 30/30）
12. **诚实失败仍在**（REAL 模式必须仍报 NOFLASH —— 改着改着让假成功溜进来是这类改动的头号风险）

---

## 9. 决策记录

记录**为什么是这样**，防止后来者"优化"回去。

| # | 决策 | 理由 |
|---|---|---|
| D1 | 探针一律编译期开关，产品构建全 0 | 运行时开关会被忘关。CMake CACHE 粘值 → 显式传 + grep 复核 |
| D2 | 拆 `nvs_read_raw` / `ui_cfg_init` | 回读走有副作用那条会冻结时钟 |
| D3 | 不用 `memcmp` 比结构体 | 对齐空洞 → 假失败 |
| D4 | `lv_obj_del` 不用，页面常驻 | 避免运行期分配/释放失败模式；切页只改 hidden |
| D5 | `static` 一切 >512B 缓冲 | hw_flash 25KB 栈炸弹（真机 IDLE 栈溢出重启，宿主 8MB 栈掩盖） |
| D6 | 烧录走 bit-bang 而非外设映射 | HW 模式在 S3 上是否受 IOMUX 限制**未测**，列为高风险；BB 模式不受影响 |
| D7 | VPP 映射为 -1 而非假值 | 没有升压硬件。映射假 GPIO 会让人以为能编程 EPROM |
| D8 | `lcd_hw_start` 最后启动 | 拆时序是正确防御性写法（但非当年崩溃根因，见 4.4） |
| D9 | PINS 主题色用黄色 | 唯一"配置类"页，与操作类（绿）区分 |
| D10 | 开机 2.4s 后进菜单，不自动轮播 | 上一版开机自动轮播导致"菜单选不动"排查了半天 |
| D11 | 生成文件绝不手改 | 手改会在下次生成时被无声覆盖 |
| D12 | 探针用真实路径注入 | `btn_inject` 塞真实队列而非直接调 `prog_page_key`，否则测的不是生产路径 |

---

## 10. 坑位档案

> 完整 25 条见 [`CODE_ANNOTATION_v3.md`](CODE_ANNOTATION_v3.md) 第 12 章。这里列最容易再踩的。

### 🔴 P0 级

**1. 屏驱动引脚是 10 脚不是 14 脚**
同一商品链接的详情图是 14 脚版（`NC GND LEDK LEDA GND RESET DC SDA SCL VCC IOVCC CS GND NC`），实物是 10 脚（`GND RS CS SCL SDA RESET VDD GND LED+ LED-`）。
教训升级为：**口述脚位 ≠ 实测脚位**。上电前必须限流点测现场确认。

**2. `LV_USE_FONT_COMPRESSED=0` → 全屏无字**
CJK 字体 `bitmap_format = 1`（压缩格式），而 `lv_font_fmt_txt.c:212-214` 在 `LV_USE_FONT_COMPRESSED=0` 时**直接 `return NULL`**（只打一条 `LV_LOG_WARN`）。⇒ 覆盖自检 100% 全绿但渲染 100% 失败。
配置真相只在 `build_xxx/config/sdkconfig.h`。**上一轮总结把因果读反过一次**（说"probe 开着压缩所以危险，生产要用 build/"）—— 照做会刷出全黑屏。自己写过的总结同样会错。

**3. 刷了零屏驱动的测试固件 → 屏幕全黑**
已发生两次（9/30 刷 `hw_flash`、10/02 刷 `hw_dmc`），两次根因相同：测试固件零 LCD 代码。
判别法：读前 216784B 若与已知测试固件逐位相同且工程 `grep lcd_hw|st7789|LVGL` 零命中 ⇒ 就是它。
**共享板硬规则**：`esp32-s3-lvgl-ui` 是唯一带屏固件，每轮测试固件收尾必须回烧 `build_v3/` 三段（`0x0`/`0x8000`/`0x10000`，NVS `0x9000` 不动）。

**4. NVS 残留导致开机是英文**
`lang=1` 留在 NVS 里。不是 bug。`wifi_sta.c` 的「NVS无凭据」与 `ui_cfg` 的「lang=1」**并存不是 bug**（两个独立命名空间）。

### 🟠 编译/配置级

**5. CMake `CACHE BOOL` 粘值**
`set(... CACHE BOOL ...)` 会粘住上次 `-D` 值 → 不显式传 0 则"重编产品版"仍得自检版。纪律：显式传 + `grep CMakeCache.txt` 复核 + readback md5 对拍（**信产物不信日志**）。

**6. 产物不可复现**
两次产品版同尺寸 1,656,576B 但 md5 不同，逐字节 diff 仅 3 处构建元数据（时间戳 + 内嵌 ELF SHA + 尾部 SHA）。
纪律：**回读对拍只认本次本地产物，别用记忆里的旧 md5**。"同尺寸"只证布局没变不证内容没变。

**7. `SHOT_PROBE` 的 `FIRST>0` 抓错页**
`shot_probe_start` 设 `s_page=9` 却**从未 `ui_goto(9)`** → 首次 tick 抓的还是 `ui_init` 末尾留的第 0 页 MENU 却打上 `##SHOT 9` 标签。`FIRST=0` 全量抓取碰巧对，所以藏了很久。

### 🟡 工具/方法论级

**8. `tail` 二次伤害**
崩溃后 panic 重启那轮往往完全成功，真相在日志**开头**。`tail -20` 会把崩溃-重启循环伪装成"一次干净启动"。

**9. `strings` 只认 ASCII**
查中文/长探针标记一律零命中，看起来像"清理干净了"。必须 Python 按字节查。

**10. `cmp` 比回读件必报假差异**
回读长度（`0x35000`）> 镜像长度（216576）时 `cmp` 报差异。必须**按字节截断后比对**。尾部 512B 全 `0xFF` = 未写区。

**11. esptool 波特率**
本板 `-b 921600` 报 `Invalid head of packet(0x00)`，必须用 **115200**。
另：「已回刷原版」是歧义句 —— 报告时必须写明恢复到哪份 md5。

**12. OCR 的语言纠正开关是双刃剑**
`usesLanguageCorrection=false` 时中文全糊成拉丁乱码（`探针检测` → `*TTN`）；读代码/十六进制时又必须关（否则模型"猜词"失真）。
读中文 UI 标签 → `true`；读十六进制/代码 → `false`。

**13. 🔴 检查工具自己踩的假阳性（写这份文档时当场抓到）**
`check_probe_marks.py` 初版用裸子串 `"persist"` 当 `UI_CFG_PERSIST_TEST` 探针的标记，对**产品固件**报出 2 处命中，退出码 1，结论"不是干净的产品固件"。

追下去发现来源与本工程无关：
```
偏移 0x011F0E: b'b->state != TIME-WAIT\n\x00...tcp_slowtimr: persist ticking with in-flight data'
偏移 0x011F42: b'ing with in-flight data\n\x00...tcp_slowtimr: persist ticking with empty send buffer'
```
—— lwIP 自带的 TCP 慢启动日志。

**这就是"验证工具的假阳性"**：工具自信地给出一个失败，而那个失败没有意义。如果当时选择"相信工具"去重新刷固件，就是白忙一轮；如果选择"相信产品"就放过，那么工具从此不可信。
**判别动作**：任何子串命中，先 dump 命中处的**上下文字节**看来源，别急着下结论。
**修法**：标记串改成带上下文的 `[persist]` / `UI_CFG_PERSIST_TEST`。改完重跑，阳性对照（`LVGL`/`ui_cfg`/`backend up`/`jumper`）仍 4/4 命中，区分力实测：产品 0 命中 / 探针 3 命中。

> 一般化教训：**子串匹配必须挑带前缀或上下文的串**。否则第三方库里任何一个沾同样字母的字符串，都会把你的判据变成随机数。

---

## 11. 附录

### 11.1 文件清单

```
main/
├── app_main.c        151   唯一入口
├── lcd_hw.c/.h       280   ST7789 + LVGL display + 刷新任务
├── btn.c/.h          299   3 键轮询 + 环形队列
├── btn_decode.h      126   纯函数解码状态机（非对称去抖）
├── ui.c              1357  9 页 UI
├── ui_cfg.c/.h       475   NVS 持久化 + 时间基准
├── i18n.c/.h         677   191×2 文案表（生成）
├── i18n/strings.txt  8979  文案源真
├── fonts/            生成  ui_cjk_14.c / ui_cjk_16.c + 头
├── wifi_sta.c/.h     533   真实 STA
├── wifi_cfg.h        34    SSID/密码默认值（⚠️ 明文，非 git 仓库）
├── hw_pin.c/.h       1987  引脚档案 + 双模驱动 + 电压层 + 25xx/ISP
├── prog_hw.c/.h      350   ESP32-S3 BSP
├── prog_api.c/.h     602   烧录 worker 外壳
├── shot_probe.c/.h   244   抓屏探针
├── cjk_probe.c/.h    274   字体覆盖探针
├── prog_ui_probe.c   121   烧录写路径探针
├── pin_probe.c/.h    160   引脚探针
└── CMakeLists.txt    111   源文件 + 13 个构建旋钮

tools/
├── gen_i18n.py       文案生成器
├── gen_font.py       字体子集生成器
├── font_charsets.json / font_decomp_check.py / glyph_dump.py   字体附属工具
├── check_probe_marks.py  产物探针标记检查（阳性对照 + 字节口径，退出码可门禁）
├── btn_decode_test.c 按键纯函数宿主单元测试（8/8）
├── btn_trace_demo.c  按键波形演示
├── boot_cap.py       抓完整 boot（DTR=False + RTS 1→0.2s→0 硬复位）
├── boot_loop.py      N 次复位统计崩溃次数
├── cap.py/cap_probe.py/cap_reset.py/live_log.py   串口抓取
├── loggrep.py        日志关键字提取
├── shot_fetch.py     串口 dump → 本地 PNG
├── ocr_vision.swift  macOS Vision 离线 OCR（零 API）
└── i18n_hosttest.c   文案表宿主自检
```

**行数核对**（`wc -l`，2026-10-02）：

| 文件 | 行 | 文件 | 行 |
|---|---|---|---|
| `hw_pin.c` | 1716 | `ui.c` | 1357 |
| `i18n.c` | 429 | `ui_cfg.c` | 384 |
| `prog_api.c` | 526 | `wifi_sta.c` | 474 |
| `prog_hw.c` | 288 | `cjk_probe.c` | 239 |
| `lcd_hw.c` | 223 | `shot_probe.c` | 223 |
| `btn.c` | 187 | `pin_probe.c` | 145 |
| `btn_decode.h` | 126 | `prog_ui_probe.c` | 121 |
| `app_main.c` | 151 | `btn.h` | 112 |
| `ui_cfg.h` | 91 | `i18n.h` | 248 |
| `prog_api.h` | 76 | `lcd_hw.h` | 57 |
| `wifi_sta.h` | 59 | `prog_hw.h` | 62 |
| `hw_pin.h` | 271 | `wifi_cfg.h` | 34 |

`hw_pin.c` 单文件占全工程 22% 行数，是烧录能力的核心。

### 11.2 符号速查

**显示** `lcd_hw_init()` / `lcd_hw_start()` / `lcd_hw_refresh_iters()`（仅探针）
**按键** `btn_init()` / `btn_get_event(&e)` / `btn_inject(id, is_long)`
**UI** `ui_init()` / `ui_goto(pg)` / `ui_start_wizard()`
**配置** `ui_cfg_init(&c)` / `ui_cfg_save(&c)` / `ui_cfg_now()` / `ui_cfg_factory_reset()`
**网络** `wifi_st_start()` / `wifi_st_get(&st)`
**烧录** `prog_api_init()` / `prog_api_cfg(t, d, i)` / `prog_api_start(op)` / `prog_api_status(&st)` / `prog_api_log(s)`
**hw_pin** `hw_pin_init(NULL)` / `hw_pin_active()` / `hw_pin_gpio_of(sig)` / `hw_pin_spi_xfer(tx, rx, n)` / `hw_pin_mode()`
**i18n** `TR(TID_*)` / `g_ui_lang`

### 11.3 关键常量

```c
/* 屏 */
SCR_W 320   SCR_H 240   TITLE_H 32   BODY_Y 34   HINT_Y 203

/* 字体（F14 line_height=17, F16 line_height=20） */
F14 = &ui_cjk_14      F16 = &ui_cjk_16

/* 按键去抖（btn_decode.h） */
BTN_DEBOUNCE_DOWN_MS 30     BTN_DEBOUNCE_UP_MS 300
BTN_RELEASE_MS       500    BTN_LONG_MS         1200

/* 队列 */
BTN_Q_SLOTS 16

/* 烧录 */
HW_PIN_SIG_MAX      9      /* MOSI MISO CK CS TX RX RST VPP VCC */
HW_PIN_FLASH_SECTOR 4096   HW_PIN_FLASH_PAGE 256
HW_PIN_GOLDEN       0x9E0F10FA
HW_PIN_ISP_GOLDEN   0xD2A9A924
PROG_LOG_LINES      3      PROG_LOG_WIDTH 40

/* i18n */
UI_STR_COUNT 191
```

### 11.4 命令速查

```bash
# 产品构建（唯一正确）
idf.py -B build_v3 -DBTN_SELFTEST=0 -DRACE_AMPLIFY=0 -DPIN_PROBE=0 \
       -DPROBE_TEST_FREE_PIN=0 -DSHOT_PROBE=0 -DCJK_SMOKETEST=0 \
       -DUI_CFG_SELFTEST=0 -DUI_CFG_PERSIST_TEST=0 \
       -DPROG_SELFTEST=0 -DWIFI_PASS_PROBE=0 -DPROG_UI_TEST=0 \
       -DSHOT_PAGE_FIRST=0 -DSHOT_PAGE_LAST=9 build

# 核 CMakeCache（10 个旋钮应全 0）
grep -E "^(RACE_AMPLIFY|PIN_PROBE|PROBE_TEST_FREE_PIN|SHOT_PROBE|CJK_SMOKETEST|UI_CFG_SELFTEST|UI_CFG_PERSIST_TEST|PROG_SELFTEST|WIFI_PASS_PROBE|PROG_UI_TEST):BOOL" \
     build_v3/CMakeCache.txt

# 核产物无探针（阳性对照 + 字节口径，期望退出码 0）
python3 tools/check_probe_marks.py build_v3/esp32-s3-lvgl-ui.bin

# 烧录三段（NVS 不动）
esptool.py -p PORT -b 115200 write_flash 0x0    build_v3/bootloader/bootloader.bin
esptool.py -p PORT -b 115200 write_flash 0x8000 build_v3/partition_table/partition-table.bin
esptool.py -p PORT -b 115200 write_flash 0x10000 build_v3/esp32-s3-lvgl-ui.bin

# 抓 boot
python3 tools/boot_cap.py tmp/out.log 20

# 硬复位
python3 tools/cap_reset.py

# OCR
swift tools/ocr_vision.swift tmp/page.png

# 按键纯函数单元测试
cc -o /tmp/btntest tools/btn_decode_test.c && /tmp/btntest
```

---

## 12. 维护约定

1. **源码改动 → 同步改本文 + 对应 `ANNOT_*`**，一次改完
2. **改了构建旋钮 → 更新第 7.1 表**（早先"7 个开关"的过期记录就是这么来的）
3. **踩新坑 → 补第 10 章 + `CODE_ANNOTATION_v3.md` 第 12 章**，两处都补
4. **产品固件变更 → 更新第 0 章的 md5 指纹**（且只写本次本地产物的）
5. **文档与源码冲突 → 以源码为准，立即修文档**
