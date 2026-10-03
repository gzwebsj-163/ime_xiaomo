# 逐行代码注解（四）UI 功能页与调度

> 对应源码：`main/ui.c` 第 479-1357 行
> 本篇讲：页 3-8（六个功能页）+ 页标 + 页面切换 + 按键导航 + 定时刷新 + 整树重建 + 入口

---

# 一、页 3：PROBE 探针检测（第 479-619 行）

## 1.1 状态与常量（第 480-485 行）

```c
static const char *PROBE_TGT[3] = { "s3-spi", "w25q", "s3-isp" };  /* 三个档案 */
static const char *PROBE_DRV[2] = { "SIM", "REAL" };                 /* 两种驱动 */
static int  s_pr_tgt = 0, s_pr_drv = 0, s_pr_sel = 0;
static lv_obj_t *s_pr_val[2];     /* 档案值 / 驱动值 */
static lv_obj_t *s_pr_cursor;     /* 光标条 */
static lv_obj_t *s_pr_res[4];     /* 四行结果 */
```

> **本页的"数据源"是 `prog_api`（真读芯片）** —— 与旧版最大的区别。
> 旧版的 SPI 页 TX/RX 是 `(i*7+3)&0xFF` 编出来的；本页的 JEDEC ID 是
> **真的从 MISO 引脚读回来的 3 个字节**。
>
> `s_pr_res[4]` 四行 = 厂商 / 器件+ID / 容量 / 序列号（ISP 模式下是 版本/PID/状态/类型）。

## 1.2 `jedec_vendor()` — 厂商码译码（第 487-500 行）

```c
static const char *jedec_vendor(uint8_t mfr)
{
    switch (mfr) {
    case 0xEF: return "Winbond";
    case 0xC8: return "GigaDevice";
    case 0x1F: return "Atmel/Adesto";
    case 0x20: return "Micron/ST";
    case 0xBF: return "SST";
    case 0x01: return "Spansion";
    case 0xC2: return "Macronix";
    case 0x62: return "SANYO";
    default:   return "Unknown";
    }
}
```

> **JEDEC 厂商码表**（JEDEC 标准 JESD126）。第一个字节是厂商 ID。
> 实际最常见的是 `0xEF`（Winbond，W25Q 系列）和 `0xC8`（GigaDevice，GD25Q）。
>
> **`default: return "Unknown"`** 而不是崩溃或空串 —— 未收录的厂商也能正常显示原始码值
> （下一行 `s_pr_res[1]` 会显示 `0xXX 0xXX`），**信息不丢失**。

## 1.3 `build_probe()`（第 502-526 行）

```c
static void build_probe(void)
{
    lv_obj_t *p = s_page[PG_PROBE];

    /* 卡片 A（高 58）：档案 + 驱动 */
    lv_obj_t *ca = mk_card(p, 8, BODY_Y, SCR_W - 16, 58, C_CYAN, LV_OPA_40);
    static const uint16_t key[2] = { TID_P_PROBE_PROFILE, TID_P_PROBE_DRIVER };
    for (int k = 0; k < 2; k++) {
        const int y = 6 + k * 22;
        mk_tr(ca, key[k], F14, C_GRAY, 14, y + 2);       /* 标签（灰） */
        s_pr_val[k] = mk_txt(ca, "-", F16, C_CYAN, 130, y); /* 值（青/黄/绿） */
    }
    s_pr_cursor = mk_bar(ca, 8, 4, SCR_W - 32, 21, C_CYAN);
    lv_obj_move_background(s_pr_cursor);

    /* 卡片 B（高 104）：启动 + 4 行结果 */
    lv_obj_t *cb = mk_card(p, 8, BODY_Y + 62, SCR_W - 16, 104, C_CYAN, LV_OPA_30);
    mk_section(cb, TR(TID_P_PROBE_START), C_CYAN, 2);
    mk_txt(cb, "0x9F  RDID        ISP 0x7F", F14, C_DIM, 12, 20);
    static const uint16_t rtid[4] = { TID_P_PROBE_VENDOR, TID_P_PROBE_DEV,
                                      TID_P_PROBE_CAPACITY, TID_P_PROBE_SERIAL };
    for (int i = 0; i < 4; i++) {
        const int y = 40 + i * 16;
        mk_tr(cb, rtid[i], F14, C_GRAY, 14, y);
        s_pr_res[i] = mk_txt(cb, "--", F14, C_TEXT, 104, y);
    }
    mk_hint(p, TR(TID_S_HINT_ACT));
}
```

> **两卡片布局**：
> ```
> 卡片 A  y=34,  h=58   → y=34..92
>   ├ y=6   "档案"  s3-spi
>   ├ y=28  "驱动"  SIM     (间隔 22px，F16 行高 20，间隙 2px)
>   └ 光标条 y=4/26, h=21, x=8, w=288
> 卡片 B  y=96,  h=104  → y=96..200
>   ├ 小节标题 "启动检测"（y=2）
>   ├ y=20  "0x9F RDID    ISP 0x7F"（提示发的命令码）
>   ├ y=40  厂商  ──────────
>   ├ y=56  器件  ────────── (间隔 16px，F14 行高 17 → 间隙 -1px ⚠️)
>   ├ y=72  容量
>   └ y=88  序列号
> 底部提示 y=203
> ```
>
> ⚠️ **注意 `y = 40 + i * 16` 而 F14 行高是 17** ⇒ **行间距 16 < 行高 17**，
> **这四行是重叠的！**
>
> **这是本篇发现的一个潜在问题。** 对比 BOOT 页的教训（那里因为粘行被 OCR 抓到），
> 这里 4 行都是 F14 且 y 间隔 16px，行高 17px ⇒ 每行底部压住下一行顶部 1px。
> **小字号下 1px 重叠视觉上可能勉强可接受**（不像 BOOT 页那样把两行粘成一个字符串），
> 但**这是欠 1px 的排版**。如果哪天 OCR 在这一页读出串行，要先怀疑这里。
>
> **修复方案**（如果确认有问题）：卡片 B 高度够（104px，4 行 × 20px = 80px），
> 改成 `y = 40 + i * 19` 或 `i * 20` 即可。**但要重新抓图确认不越界。**
>
> **"0x9F RDID   ISP 0x7F" 这一行**：告诉用户"我们会发这两个命令去读 ID"，
> 是**可解释性**设计 —— 用户看到失败时知道系统试过什么。

## 1.4 `probe_apply()` 与 `probe_refresh()`（第 528-598 行）

```c
static void probe_apply(void)
{
    if (s_pr_val[0]) lv_label_set_text(s_pr_val[0], PROBE_TGT[s_pr_tgt]);
    if (s_pr_val[1]) {
        lv_label_set_text(s_pr_val[1], PROBE_DRV[s_pr_drv]);
        lv_obj_set_style_text_color(s_pr_val[1], s_pr_drv ? C_YELLOW : C_MINT, LV_PART_MAIN);
    }
    (void)prog_api_cfg(s_pr_tgt, s_pr_drv, 0);      /* ★ 把设置同步到后端 */
}
```

> **`probe_apply` vs `probe_refresh` 的分工**：
> - `apply` = **写**（改设置 + 通知后端）
> - `refresh` = **读**（刷新显示）
>
> `prog_api_cfg()` 会真的调用 `prog_hw_install()` 装载档案/切驱动模式
> ——所以这是"有副作用"的操作，只在设置变化时调。
>
> **颜色语义**：SIM = `C_MINT`（薄荷绿，安全）；REAL = `C_YELLOW`（黄，警示）。
> **选 REAL 会真的去驱动 GPIO，没接芯片就是失败** —— 用颜色提前告知。

```c
static void probe_refresh(void)
{
    if (s_pr_cursor) {
        if (s_pr_sel < 2) {                            /* 光标在档案/驱动行 */
            lv_obj_set_pos(s_pr_cursor, 8, 4 + s_pr_sel * 22);
            lv_obj_set_style_bg_opa(s_pr_cursor, LV_OPA_30, LV_PART_MAIN);
        } else {                                       /* 光标在"执行"按钮 */
            lv_obj_set_style_bg_opa(s_pr_cursor, LV_OPA_TRANSP, LV_PART_MAIN);
        }
    }
    for (int k = 0; k < 2; k++) {
        if (!s_pr_val[k]) continue;
        lv_obj_set_style_text_color(s_pr_val[k],
            (k == s_pr_sel) ? C_TEXT : (k == 1 && s_pr_drv) ? C_YELLOW : C_CYAN, LV_PART_MAIN);
    }
```

> **光标有 3 个位置，但前 2 个有视觉指示**：
> - `s_pr_sel` 0（档案）/ 1（驱动）→ 光标条跟着移动，半透明青色背景
> - `s_pr_sel` 2（执行）→ 光标条隐藏（`TRANSP`），改成下面的"启动检测"小节
>   作为执行按钮的视觉暗示
>
> **`k == s_pr_sel ? C_TEXT : ...`** 三元嵌套：选中时统一亮白（与光标背景对比），
> 未选中时档案=青、驱动=SIM绿/REAL黄。

```c
    prog_status_t st;
    prog_api_status(&st);                    /* ★ 加锁拷贝快照 */
    char b[32];
    if (st.state == PROG_ST_RUNNING) {
        for (int i = 0; i < 4; i++) lv_label_set_text(s_pr_res[i], "...");
        return;
    }
```

> **★ `prog_api_status()` 是本工程的线程安全关键**：
> 它内部 `LOCK(); *out = s_st; UNLOCK();` —— **纯拷贝，无阻塞**，
> 可以安全从 LVGL 任务调用。而 `prog_api_start()` 会往队列投 job（快速返回）。
>
> **绝对不能**在 `tick_cb` 里直接调 `hw_pin_flash_rdid()`（阻塞几毫秒~几百毫秒）。
>
> **`RUNNING` 时显示 `...`**：后端在干活，结果还没出来。
> **不显示旧值** —— 显示上一次的结果会误导用户以为这次也成功了。

```c
    if (s_pr_tgt == 2) {                        /* ISP：版本 + PID */
        if (st.state == PROG_ST_DONE && (st.pid || st.ver)) {
            snprintf(b, sizeof(b), "0x%02X", (unsigned)st.ver);
            lv_label_set_text(s_pr_res[0], b);
            snprintf(b, sizeof(b), "0x%04X", (unsigned)st.pid);
            lv_label_set_text(s_pr_res[1], b);
            lv_label_set_text(s_pr_res[2], TR(TID_P_PROBE_FOUND));
            lv_label_set_text(s_pr_res[3], "STM32");
        } else if (st.state == PROG_ST_FAIL) {
            lv_label_set_text(s_pr_res[0], TR(TID_S_NONE_DETECTED));
            lv_label_set_text(s_pr_res[1], TR(TID_P_PROBE_TIP_CHECK));
            lv_label_set_text(s_pr_res[2], "--");
            lv_label_set_text(s_pr_res[3], "--");
        }
    } else {                                    /* SPI：JEDEC 译码 */
        const uint32_t j = st.jedec;
        const bool ok = (st.state == PROG_ST_DONE) && (j != 0) && ((j & 0xFFu) != 0xFFu);
```

> **★ `ok` 的三重判定**（这是"不假装成功"的核心）：
> ```c
> const bool ok = (st.state == PROG_ST_DONE)      // ① 后端说成功
>              && (j != 0)                          // ② JEDEC 非全 0
>              && ((j & 0xFFu) != 0xFFu);          // ③ 厂商码不是 0xFF
> ```
> **②③ 是防御性检查**：某些情况下 MISO 悬空读回全 1（0xFF），
> 后端可能误报成功。加上这两条，**全 1 或全 0 都判为失败**。
>
> **REAL 模式 + 没接芯片 → MISO 被上拉 → 读回 0xFF → 判失败 → 诚实显示"未检测到"**。
> 这就是"不假装成功"落到 UI 层的具体形态。

```c
        if (ok) {
            const uint8_t mfr = (uint8_t)(j & 0xFFu);           /* 厂商 */
            const uint8_t dev = (uint8_t)((j >> 8) & 0xFFu);    /* 器件 */
            const uint8_t cap = (uint8_t)((j >> 16) & 0xFFu);   /* 容量幂码 */
            const uint32_t bytes = (cap < 32u) ? (1u << cap) : 0u;
            lv_label_set_text(s_pr_res[0], jedec_vendor(mfr));
            snprintf(b, sizeof(b), "0x%02X 0x%02X", (unsigned)mfr, (unsigned)dev);
            lv_label_set_text(s_pr_res[1], b);
            if (bytes >= (1u << 20)) snprintf(b, sizeof(b), "%u MB", (unsigned)(bytes >> 20));
            else                     snprintf(b, sizeof(b), "%u KB", (unsigned)(bytes >> 10));
            lv_label_set_text(s_pr_res[2], b);
            snprintf(b, sizeof(b), "%06X", (unsigned)(j & 0xFFFFFFu));
            lv_label_set_text(s_pr_res[3], b);
        } else if (st.state == PROG_ST_FAIL) {
            lv_label_set_text(s_pr_res[0], TR(TID_S_NONE_DETECTED));
            lv_label_set_text(s_pr_res[1], TR(TID_P_PROBE_NO_CHIP));
            lv_label_set_text(s_pr_res[2], TR(TID_P_PROBE_TIP_CHECK));
            lv_label_set_text(s_pr_res[3], "--");
        }
    }
}
```

> **JEDEC ID 的字节序**（这是个容易搞反的点）：
> ```
> 0x9F 响应 3 字节：MFR(0xEF)  DEV(0x40)  CAP(0x18)
>                字节0      字节1      字节2
> ```
> `hw_pin_flash_rdid()` 把它打包成 `uint32_t`（**小端**）：
> ```c
> jedec = MFR | (DEV << 8) | (CAP << 16)
>        0xEF    0x40       0x18
> ```
> 所以：
> - `mfr = j & 0xFF` = `0xEF` ✓
> - `dev = (j >> 8) & 0xFF` = `0x40` ✓
> - `cap = (j >> 16) & 0xFF` = `0x18` ✓
>
> **★ 这正是 `hw_pin` 的一个已修 bug**：注释里记着"RDID 厂商码在 `rx[0]` 而非
> `rx[1]`" —— 因为 SPI 是全双工，发 `0x9F` 那一拍就在读回字节 0。
>
> **容量计算**：CAP 字节是**幂码**（log2），`0x18` = 24 → `1 << 24` = 16MB。
> `cap < 32` 防左移溢出。≥1MB 显示 MB，否则显示 KB。
>
> **"未检测到" + "检查接线"**：失败时**给出可执行的建议**，
> 而不只是 `FAIL`。这是"诚实失败"的完整形态：不只说失败，还说大概率原因。

## 1.5 `probe_page_key()`（第 600-619 行）

```c
static int probe_page_key(btn_id_t id, bool is_long)
{
    if (id == BTN_BACK) return 1;                    /* BACK 一律交回全局 */
    if (!is_long) {
        s_pr_sel = (s_pr_sel + 3 + ((id == BTN_UP) ? -1 : 1)) % 3;   /* 3 行循环 */
        probe_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;                    /* 长按下 = 返回 */

    if (s_pr_sel == 0)      { s_pr_tgt = (s_pr_tgt + 1) % 3; probe_apply(); }
    else if (s_pr_sel == 1) { s_pr_drv = (s_pr_drv + 1) % 2; probe_apply(); }
    else {
        prog_status_t st;
        prog_api_status(&st);
        if (st.state != PROG_ST_RUNNING)             /* ★ 忙则不受理 */
            (void)prog_api_start(s_pr_tgt == 2 ? PROG_OP_ISPSYNC : PROG_OP_RDID);
    }
    probe_refresh();
    return 0;
}
```

> **标准三行循环**：`+ 3` 避免负数，`% 3` 环绕。
>
> **`s_pr_tgt == 2`（s3-isp 档案）→ 发 `ISPSYNC`，否则 → 发 `RDID`**：
> 档案决定用什么协议。这是"档案 = 换一条路"设计在 UI 层的体现。
>
> **`if (st.state != RUNNING)` 是 UI 侧的额外保险**：
> `prog_api_start()` 内部也会检查并返回 -1，但 UI 先查一次可以**避免无谓的调用**，
> 而且逻辑更清楚。

---

# 二、页 4：FLASH 烧录器（第 621-761 行）

## 2.1 常量与状态（第 622-647 行）

```c
#define FLASH_CFG_N 3      /* 设置项：TARGET / DRIVER / IMAGE */
#define FLASH_OP_N  5      /* 操作项：READ ID / ERASE / PROGRAM / VERIFY / ISP SYNC */
#define FLASH_ROWS  (FLASH_CFG_N + FLASH_OP_N)   /* = 8 */
#define FLASH_LOG_N 4      /* 日志行数 */

static const char *FLASH_TGT[3] = { "s3-spi", "w25q", "s3-isp" };
static const char *FLASH_DRV[2] = { "SIM", "REAL" };
static const char *FLASH_IMG[3] = { "banner-256", "pattern-1k", "rand-2k" };

static const uint16_t FLASH_CFG_TID[FLASH_CFG_N] = {
    TID_P_PROG_TARGET, TID_P_PROG_DRIVER, TID_P_PROG_IMAGE
};
static const uint16_t FLASH_OP_TID[FLASH_OP_N] = {
    TID_P_PROG_READ_ID, TID_P_PROG_ERASE, TID_P_PROG_PROGRAM,
    TID_P_PROG_VERIFY, TID_P_PROG_ISP
};
static const char *FLASH_OP_GLYPH[FLASH_OP_N] = { "○", "■", "↓", "√", "↻" };
static const uint32_t FLASH_OP_HEX[FLASH_OP_N] = { 0x00E5FF, 0xFF3B57, 0x00FF9C, 0x39FF14, 0xFFC400 };
```

> **⚠️ 布局：左栏 8 行，右栏进度/日志** —— 这是全工程最复杂的一页布局。
>
> ```
> 8 行的语义（★ 顺序被 prog_ui_probe.c 硬依赖）：
>   0 = TARGET   （设置）
>   1 = DRIVER   （设置）
>   2 = IMAGE    （设置）
>   3 = READ ID  （操作）
>   4 = ERASE    （操作，红色 = 危险）
>   5 = PROGRAM  （操作，绿色）
>   6 = VERIFY   （操作，霓虹绿）
>   7 = ISP SYNC （操作，黄）
> ```
>
> **操作色编码**（`FLASH_OP_HEX`）：
> - READ ID 青 = 只读
> - ERASE **红** = 破坏性
> - PROGRAM / VERIFY 绿 = 写+验
> - ISP SYNC 黄 = 握手
>
> **「ERASE 是红色」是重要设计** —— 在一堆操作里用颜色标出危险项，
> 用户扫一眼就知道哪行要小心。

## 2.2 `flash_cfg_apply()` / `flash_refresh()`（第 649-694 行）

```c
static void flash_cfg_apply(void)
{
    if (s_fl_val[0]) lv_label_set_text(s_fl_val[0], FLASH_TGT[s_fl_tgt]);
    if (s_fl_val[1]) {
        lv_label_set_text(s_fl_val[1], FLASH_DRV[s_fl_drv]);
        lv_obj_set_style_text_color(s_fl_val[1], s_fl_drv ? C_YELLOW : C_MINT, LV_PART_MAIN);
    }
    if (s_fl_val[2]) lv_label_set_text(s_fl_val[2], FLASH_IMG[s_fl_img]);
    (void)prog_api_cfg(s_fl_tgt, s_fl_drv, s_fl_img);
}
```

> 同 PROBE 页的 `apply`。区别是这次也传了 `s_fl_img`（镜像选择）给后端。

```c
static void flash_refresh(void)
{
    prog_status_t st;
    prog_api_status(&st);

    for (int k = 0; k < FLASH_ROWS; k++) {
        const bool sel = (k == s_fl_sel);
        lv_obj_set_style_bg_opa(s_fl_cursor[k], sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_fl_name[k], sel ? C_BLACK : C_TEXT, LV_PART_MAIN);
    }
    for (int k = 0; k < FLASH_CFG_N; k++) {
        if (!s_fl_val[k]) continue;
        lv_obj_set_style_text_color(s_fl_val[k],
            (k == s_fl_sel) ? C_BLACK : (k == 1 && s_fl_drv) ? C_YELLOW : C_MINT, LV_PART_MAIN);
    }
```

> **8 个光标条循环**：每个 `s_fl_cursor[k]` 独立控制背景不透明度。
> 选中时用该行自己的颜色（`mk_bar` 时传入的）填满，文字变黑。
>
> **★ 每行光标条颜色在建时就定了**（见 `build_flash`）：
> 设置行用 `C_MINT`，操作行用各自的 `FLASH_OP_HEX`。
> **所以选中 ERASE 时整行变红底黑字** —— 危险操作在"选中瞬间"就有强视觉警示。

```c
    if (s_fl_bar) lv_obj_set_width(s_fl_bar, (st.pct * 118) / 100);
    if (s_fl_pct) {
        char b[8];
        snprintf(b, sizeof(b), "%3d%%", st.pct);
        lv_label_set_text(s_fl_pct, b);
    }
    if (s_fl_msg) {
        lv_label_set_text(s_fl_msg, st.msg);
        lv_obj_set_style_text_color(s_fl_msg,
            (st.state == PROG_ST_RUNNING) ? C_YELLOW :
            (st.state == PROG_ST_DONE)    ? C_GREEN  :
            (st.state == PROG_ST_FAIL)    ? C_RED    : C_GREEN, LV_PART_MAIN);
    }
    for (int i = 0; i < FLASH_LOG_N; i++) {
        if (!s_fl_log[i]) continue;
        lv_label_set_text(s_fl_log[i], st.log[i]);
        lv_obj_set_style_text_color(s_fl_log[i], (i == 0) ? C_TEXT : C_DIM, LV_PART_MAIN);
    }
}
```

> **★ 进度条是"真实进度"**：`st.pct` 来自后端 `set_pct()`，
> 由 `op_program` 里的 `(off + n) * 80 / sz` 等真实计算得出。
> **不是定时器匀速推的假进度。** 这是"进度条不是演出来的"的结构性保证。
>
> **进度条宽度**：`(st.pct * 118) / 100` —— 满格 = 118px（右栏宽度）。
>
> **状态色**：`RUNNING` 黄 / `DONE` 绿 / `FAIL` 红。
>
> **★ 日志 4 行，最新在最上面且最亮**：
> ```c
> st.log[0]  → s_fl_log[0] → C_TEXT（最亮）  ← 最新一行
> st.log[1]  → s_fl_log[1] → C_DIM
> st.log[2]  → s_fl_log[2] → C_DIM
> st.log[3]  → s_fl_log[3] → C_DIM            ← 最旧最暗
> ```
> 颜色递减 = 时间递减，视觉上形成"越往上越新"。
>
> **注意**：`prog_api.h` 里 `PROG_LOG_LINES = 3`，但 UI 显示 4 行。
> `st.log[3][40]` 数组的第 4 行（index 3）永远是空串。**UI 分配了 4 个 label
> 但只有 3 个有内容** —— 多出来的那行显示空。
> **这是个小瑕疵**（`FLASH_LOG_N=4` vs `PROG_LOG_LINES=3`），
> 不影响功能，但如果你要改，让两者一致更干净。

## 2.3 `build_flash()` — 双栏布局（第 696-733 行）

```c
static void build_flash(void)
{
    lv_obj_t *p = s_page[PG_FLASH];

    /* 左栏 8 行（3 设置 + 5 操作）：x 8..184 */
    int y = BODY_Y + 2;
    for (int k = 0; k < FLASH_ROWS; k++) {
        s_fl_cursor[k] = mk_bar(p, 8, y, 176, 17, (k < FLASH_CFG_N) ? C_MINT
                                 : lv_color_hex(FLASH_OP_HEX[k - FLASH_CFG_N]));
        if (k < FLASH_CFG_N) {
            s_fl_name[k] = mk_tr(p, FLASH_CFG_TID[k], F14, C_TEXT, 14, y + 2);
            s_fl_val[k]  = mk_txt(p, "-", F14, C_MINT, 108, y + 2);
        } else {
            const int i = k - FLASH_CFG_N;
            lv_color_t col = lv_color_hex(FLASH_OP_HEX[i]);
            mk_txt(p, FLASH_OP_GLYPH[i], F14, col, 14, y + 2);          /* 图标 */
            char b[24];
            snprintf(b, sizeof(b), "%u  %s", (unsigned)(i + 1), TR(FLASH_OP_TID[i]));
            s_fl_name[k] = mk_txt(p, b, F14, C_TEXT, 32, y + 2);       /* "3  读 ID" */
        }
        y += 18;
    }
```

> **左栏布局计算**：
> ```
> y 起 36，行高 17，步进 18px（间隙仅 1px ⚠️）
> 行 0: 36-53   行 1: 54-71   行 2: 72-89   行 3: 90-107
> 行 4: 108-125 行 5: 126-143 行 6: 144-161 行 7: 162-179
> ```
> **⚠️ 行高 17 / 步进 18 ⇒ 间隙 1px**。和 PROBE 页卡片 B 一样的"欠 1px"问题。
> **但这里影响小**：8 行都是短标签（"3 READ ID"），F14 行高 17，
> 1px 间隙在密集列表里视觉可接受（密集列表本来就靠位置区分）。
> **PROBE 页的 4 行结果同理**。
>
> **这个"行高 17 / 步进 18"是本工程的"密集列表"惯例**：
> 内容多的地方（菜单 8 行、日志 4 行）用 1px 间隙；内容少的地方
> （BOOT 页 4 行、设置页 9 行）用 7-27px 间隙。
> **改排版时先判断这一屏是"密集列表"还是"疏排"。**

```c
    /* 分隔竖线 */
    mk_bar(p, 188, BODY_Y, 1, 162, C_LINE);

    /* 右栏：进度 / 结论 / 日志  x 196..312 */
    s_fl_pct = mk_txt(p, "  0%", F16, C_MINT, 196, BODY_Y + 2);
    mk_bar(p, 196, BODY_Y + 26, 118, 8, C_LINE);        /* 进度条底槽 */
    s_fl_bar = mk_bar(p, 196, BODY_Y + 26, 0, 8, C_MINT); /* 进度条填充（初始宽 0） */
    s_fl_msg = mk_txt(p, "READY", F16, C_GREEN, 196, BODY_Y + 42);
    mk_section(p, "LOG", C_GRAY, BODY_Y + 66);
    for (int i = 0; i < FLASH_LOG_N; i++)
        s_fl_log[i] = mk_txt(p, "", F14, C_DIM, 196, BODY_Y + 84 + i * 16);

    flash_cfg_apply();
    flash_refresh();
}
```

> **双栏完整布局**：
> ```
> 左栏（x 8..184）          竖线        右栏（x 196..312）
> y=36..179  8 行操作       x=188       y=36   "  0%"（F16 百分比）
>                        y=34..196     y=60   进度条底槽 118×8
>                                      y=60   进度条填充（宽度随 pct 变）
>                                      y=76   "READY"/结论（F16，按状态染色）
>                                      y=100  "▌ LOG"（小节标题）
>                                      y=118  日志行 0（最新，最亮）
>                                      y=134  日志行 1
>                                      y=150  日志行 2
>                                      y=166  日志行 3（永远空，见 2.2 说明）
> ```
>
> **进度条画两次**（底槽 `C_LINE` + 填充 `C_MINT`），填充初始宽 0。
> `flash_refresh` 里只改填充的 `lv_obj_set_width`。
>
> **★ 末尾调 `flash_cfg_apply()` + `flash_refresh()`**：
> 建完立刻同步一次设置和状态，否则首次进入这一页会看到 `-` 和空进度条
> （虽然 `ui_goto` 也会调 `flash_refresh`，但 `cfg_apply` 只有建的时候调）。

## 2.4 `flash_page_key()`（第 735-761 行）

```c
static int flash_page_key(btn_id_t id, bool is_long)
{
    if (id == BTN_BACK) return 1;
    if (!is_long) {
        s_fl_sel = (s_fl_sel + FLASH_ROWS + ((id == BTN_UP) ? -1 : 1)) % FLASH_ROWS;
        flash_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;

    if (s_fl_sel < FLASH_CFG_N) {              /* 设置项：循环切值 */
        switch (s_fl_sel) {
        case 0: s_fl_tgt = (s_fl_tgt + 1) % 3; break;
        case 1: s_fl_drv = (s_fl_drv + 1) % 2; break;
        case 2: s_fl_img = (s_fl_img + 1) % 3; break;
        default: break;
        }
        flash_cfg_apply();                    /* ★ 立即生效到后端 */
    } else {                                  /* 操作项：起一次任务 */
        prog_status_t st;
        prog_api_status(&st);
        if (st.state != PROG_ST_RUNNING)
            (void)prog_api_start(s_fl_sel - FLASH_CFG_N);   /* 3→0, 4→1, ... 7→4 */
    }
    flash_refresh();
    return 0;
}
```

> **★ 索引偏移 `s_fl_sel - FLASH_CFG_N`**：UI 的行 3~7 对应 `PROG_OP_RDID`(0)~`ISPSYNC`(4)。
> 前 3 行是设置（`cfg`），后 5 行是操作（`op`），这里做减法对齐。
>
> **设置项与操作项在同一个长按里处理，但逻辑完全不同**：
> - 设置项：循环切值 + 立即 `cfg_apply`（同步后端）
> - 操作项：起 worker 任务（`prog_api_start` 投队列）

---

# 三、页 5-6：PINS 与 STATUS（第 763-881 行）

## 3.1 PINS 引脚档案（第 763-801 行）

```c
static lv_obj_t *s_pin_val[HW_PIN_SIG_MAX];   /* = 9 */
static lv_obj_t *s_pin_profile = NULL;

static void build_pins(void)
{
    lv_obj_t *p = s_page[PG_PINS];
    lv_obj_t *card = mk_card(p, 8, BODY_Y, SCR_W - 16, 164, C_YELLOW, LV_OPA_40);

    mk_tr(card, TID_P_PINS_PROFILE, F14, C_GRAY, 10, 4);
    s_pin_profile = mk_txt(card, "--", F16, C_YELLOW, 110, 1);

    mk_tr(card, TID_P_PINS_SIG, F14, C_GRAY, 16, 26);
    mk_tr(card, TID_P_PINS_GPIO, F14, C_GRAY, 250, 26);
    mk_bar(card, 12, 43, SCR_W - 44, 1, C_LINE);

    for (int i = 0; i < HW_PIN_SIG_MAX; i++) {
        const int y = 48 + i * 12;
        mk_txt(card, hw_pin_sig_name(i), F14, C_TEXT, 20, y);
        s_pin_val[i] = mk_txt(card, "--", F14, C_MINT, 250, y);
    }
    mk_hint(p, TR(TID_S_BACK));
}
```

> **本页展示 9 个逻辑信号 → 实际 GPIO 映射**。
> **这是 `hw_pin` 档案层的可视化** —— 让用户直接看到
> "这个 SPI 用的是哪些物理脚"，不用去翻代码。
>
> ⚠️ **`y = 48 + i * 12` 而 F14 行高 17** ⇒ **步进 12 < 行高 17，重叠 5px！**
> 9 行 = 48..144（每行 12px 步进），最后一行底 = 48 + 8*12 + 17 = 161。
> 卡片高 164 ⇒ **放得下，但行间距 12px 远小于行高 17px ⇒ 9 行会视觉粘成一片**。
>
> **这是一个明确的排版问题**（比 PROBE/FLASH 的 1px 间隙严重得多）。
> **但它和 PROBE 页结果行 / FLASH 密集列表的"1px 间隙"是不同的**：
> 1px 间隙还能靠位置区分行，**5px 重叠会让 9 行彻底糊成一片**。
>
> **如果 PINS 页抓图发现 9 行数字读不出来或串行，就是这里。**
> 修复：`i * 12` → `i * 14` 或 `i * 15`（9 行 × 15 = 135px，从 48 到 183，
> 超出 164 卡片高度，需同时加高卡片或缩小行距/字号）。
> **这是一处需要抓图验证 + 可能要调整的排版点。**

```c
static void pins_refresh(void)
{
    const hw_pin_profile_t *pr = hw_pin_active();
    if (s_pin_profile)
        lv_label_set_text(s_pin_profile, pr ? (pr->name ? pr->name : "--") : "--");
    for (int i = 0; i < HW_PIN_SIG_MAX; i++) {
        if (!s_pin_val[i]) continue;
        int gpio = hw_pin_gpio_of(i);
        char b[16];
        if (gpio < 0) snprintf(b, sizeof(b), "%s", TR(TID_P_PINS_UNMAPPED));
        else          snprintf(b, sizeof(b), "IO%d", gpio);
        lv_label_set_text(s_pin_val[i], b);
        lv_obj_set_style_text_color(s_pin_val[i], (gpio < 0) ? C_DIM : C_MINT, LV_PART_MAIN);
    }
}
```

> **数据源全是真的**：`hw_pin_active()` 返回当前档案，`hw_pin_gpio_of(i)`
> 查该信号映射到的 GPIO。
>
> **`gpio < 0` = 未映射**（档案里填的 -1）。显示"未映射"（暗灰）而不是 `IO-1`。
> **不映射的信号（如本版 VPP/VCC）诚实地显示"未映射"，不假装有。**
>
> **`hw_pin_active()` / `hw_pin_gpio_of()` 都是纯查询，不阻塞** ——
> 符合线程约束。

## 3.2 STATUS 状态信息（第 803-881 行）

```c
static lv_obj_t *s_st_val[7];
static const uint16_t ST_KEY[7] = {
    TID_P_STA_WIFI, TID_P_STA_IP, TID_P_STA_RSSI, TID_P_STA_UPTIME,
    TID_P_STA_HEAP, TID_P_STA_FLASH, TID_P_STA_TIME
};

static void build_status(void)
{
    lv_obj_t *p = s_page[PG_STATUS];
    lv_obj_t *card = mk_card(p, 8, BODY_Y, SCR_W - 16, 164, C_CYAN, LV_OPA_40);
    for (int i = 0; i < 7; i++) {
        const int y = 6 + i * 22;
        if (i > 0) mk_bar(card, 12, y - 5, SCR_W - 44, 1, C_LINE);   /* 行间分隔线 */
        mk_tr(card, ST_KEY[i], F14, C_GRAY, 16, y + 3);
        s_st_val[i] = mk_txt(card, "--", F16, C_TEXT, 118, y);
    }
    mk_hint(p, TR(TID_S_BACK));
}
```

> **7 行 KV 布局**：
> ```
> y = 6, 28, 50, 72, 94, 116, 138   (步进 22px)
> 每行: 分隔线(y-5) + 标签(y+3, F14灰) + 值(y, F16)
> 卡片高 164，最后一行底 = 138 + 20 = 158 < 164 ✓
> ```
> **步进 22px，F16 行高 20 ⇒ 间隙 2px**。比 PINS 页（12px 步进）合理。
>
> **7 行的数据源（全部真）**：
> | 行 | 数据源 | 性质 |
> |---|---|---|
> | WiFi | `wifi_st_get()` | 真实连接状态 |
> | IP | `w.ip` | DHCP 分配的真 IP |
> | RSSI | `w.rssi` | 真实信号强度 |
> | 运行时长 | `s_tick_ms` | LVGL tick 累计 |
> | 剩余堆 | `esp_get_free_heap_size()` | 系统真实 |
> | Flash | `esp_flash_get_size()` | 硬件真实 |
> | 时间 | `ui_cfg_now()` | 用户设置 + esp_timer 推进 |

```c
static void status_refresh(void)
{
    wifi_st_status_t w;
    wifi_st_get(&w);                    /* ★ 加锁拷贝快照 */
    char b[40];

    const char *ws;
    lv_color_t wc = C_GRAY;
    if (w.state == WIFI_ST_GOT_IP)      { ws = TR(TID_P_STA_LINKED);  wc = C_GREEN; }
    else if (!w.scan_done)              { ws = TR(TID_S_LOADING);     wc = C_CYAN; }
    else if (!w.ap_seen)                { ws = TR(TID_P_STA_NONET);   wc = C_RED; }
    else if (w.state == WIFI_ST_CONNECTING || w.state == WIFI_ST_STARTING)
                                        { ws = TR(TID_P_STA_LINKING); wc = C_YELLOW; }
    else                                { ws = TR(TID_P_STA_NONET);   wc = C_YELLOW; }
    lv_label_set_text(s_st_val[0], ws);
    lv_obj_set_style_text_color(s_st_val[0], wc, LV_PART_MAIN);
```

> **★ WiFi 状态五分支判据**（顺序有讲究）：
> ```
> 1. GOT_IP                → 已连接（绿）
> 2. !scan_done            → 还在扫描（青，"加载中"）
> 3. !ap_seen              → 扫完了但目标 AP 不可见（红，"无网络"）
> 4. CONNECTING/STARTING   → 正在连（黄）
> 5. else                  → 其它（黄）
> ```
> **顺序不能乱**：先判 `GOT_IP`（最高优先级），再判 `!scan_done`
> （扫描没完就报"找不到 AP"是**误导** —— 可能只是扫到一半）。
> `scan_done` 这个字段就是为了避免"扫描未完成就下结论"。
>
> **`ap_seen` = 定向扫描是否命中**：S3 只有 2.4GHz，
> 如果目标 SSID 是 5GHz 就永远扫不到 → 显示红色"无网络"，
> **配合串口日志里的 "NO_AP_FOUND：可能是 5GHz" 就完全闭环了。**

```c
    lv_label_set_text(s_st_val[1], (w.state == WIFI_ST_GOT_IP) ? w.ip : TR(TID_S_NONE));

    if (w.state == WIFI_ST_GOT_IP && w.rssi != 0) {
        snprintf(b, sizeof(b), "%d dBm", w.rssi);
        lv_label_set_text(s_st_val[2], b);
        lv_obj_set_style_text_color(s_st_val[2],
            (w.rssi > -60) ? C_GREEN : (w.rssi > -75) ? C_YELLOW : C_RED, LV_PART_MAIN);
    } else {
        lv_label_set_text(s_st_val[2], TR(TID_S_NONE));
        lv_obj_set_style_text_color(s_st_val[2], C_GRAY, LV_PART_MAIN);
    }
```

> **RSSI 阈值配色**：
> - `> -60 dBm` = 强（绿）
> - `-75 < x ≤ -60` = 中（黄）
> - `≤ -75 dBm` = 弱（红）
>
> **`w.rssi != 0` 才显示**：RSSI 是负数，0 = 未知。没连上就不显示具体值，
> 显示"无"。**不显示假的 -50。**

```c
    uint32_t s = s_tick_ms / 1000;
    snprintf(b, sizeof(b), "%02u:%02u:%02u", (unsigned)(s / 3600) % 100,
             (unsigned)(s / 60) % 60, (unsigned)(s % 60));
    lv_label_set_text(s_st_val[3], b);

    uint32_t heap = (uint32_t)esp_get_free_heap_size();
    snprintf(b, sizeof(b), "%u KB", (unsigned)(heap >> 10));
    lv_label_set_text(s_st_val[4], b);
    lv_obj_set_style_text_color(s_st_val[4], (heap < 64u * 1024u) ? C_YELLOW : C_TEXT, LV_PART_MAIN);
```

> **运行时长用 `% 100` 截断到 100 小时**（`s/3600 % 100`）——
> 避免显示 `119:59:59` 这种超长串。
>
> **堆内存 < 64KB 变黄预警**：LVGL 分配失败的早期信号。
> （真根因是 `LV_USE_CLIB_MALLOC`，但这个黄字是"快 OOM 了"的提示。）

```c
    uint32_t fsz = 0;
    if (esp_flash_get_size(NULL, &fsz) == ESP_OK) {
        snprintf(b, sizeof(b), "%u MB", (unsigned)(fsz >> 20));
        lv_label_set_text(s_st_val[5], b);
    } else {
        lv_label_set_text(s_st_val[5], TR(TID_S_UNKNOWN));
    }

    int64_t e = ui_cfg_now();
    if (e > UI_CFG_MIN_EPOCH) {
        time_t tt = (time_t)e;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
    } else {
        snprintf(b, sizeof(b), "%s", TR(TID_S_UNKNOWN));
    }
    lv_label_set_text(s_st_val[6], b);
}
```

> **Flash 容量**：`esp_flash_get_size(NULL, ...)` 读硬件真实值（16MB）。
>
> **时间**：`ui_cfg_now()` = 用户设的 epoch + esp_timer 推进。
> 未设定时显示"未知"。**不假装有时间**（无 RTC，未设就是不知道）。
>
> **`gmtime_r` + `+1900` / `+1`**：同向导页，见 ANNOT_03。

---

# 四、页 7-8：SETUP 与 ABOUT（第 883-1078 行）

## 4.1 SETUP 系统设置（第 883-1017 行）

```c
#define SET_ROWS 9
static const uint16_t SET_KEY[SET_ROWS] = {
    TID_P_SET_LANG, TID_W_TIME_YEAR, TID_W_TIME_MONTH, TID_W_TIME_DAY,
    TID_W_TIME_HOUR, TID_W_TIME_MIN, TID_P_SET_SNTP, TID_P_SET_SAVE, TID_P_SET_FACTORY
};
static lv_obj_t *s_set_cursor[SET_ROWS];
static lv_obj_t *s_set_name[SET_ROWS];
static lv_obj_t *s_set_val[SET_ROWS];
static lv_obj_t *s_set_foot = NULL;
static int       s_set_sel = 0;
static int       s_set_t[5];      /* 年月日时分 */
static int       s_set_arm = 0;    /* 恢复出厂的两段确认标志 */
```

> **9 行**：
> ```
> 0 = 语言
> 1-5 = 年 月 日 时 分
> 6 = 网络校时（SNTP）
> 7 = 保存
> 8 = 恢复出厂
> ```

```c
static void setup_refresh(void)
{
    char b[24];
    lv_label_set_text(s_set_val[0], UI_LANG_NAME[g_ui_lang]);
    for (int i = 0; i < 5; i++) {
        snprintf(b, sizeof(b), (i == 0) ? "%04d" : "%02d", s_set_t[i]);
        lv_label_set_text(s_set_val[1 + i], b);
    }
    lv_label_set_text(s_set_val[6], ui_cfg_sntp_get() ? TR(TID_S_ON) : TR(TID_S_OFF));
    lv_label_set_text(s_set_val[7], "--");
    lv_label_set_text(s_set_val[8], s_set_arm ? TR(TID_S_CONFIRM) : "--");

    for (int i = 0; i < SET_ROWS; i++) {
        const bool sel = (i == s_set_sel);
        lv_obj_set_style_bg_opa(s_set_cursor[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_set_name[i], sel ? C_BLACK : C_TEXT, LV_PART_MAIN);
        if (i != 8)
            lv_obj_set_style_text_color(s_set_val[i], sel ? C_BLACK : C_MINT, LV_PART_MAIN);
        else
            lv_obj_set_style_text_color(s_set_val[i], s_set_arm ? C_RED : C_GRAY, LV_PART_MAIN);
    }
}
```

> **恢复出厂行（8）的特殊配色**：
> - 未武装：`C_GRAY`（暗）+ "--"
> - 已武装（`s_set_arm=1`）：`C_RED`（红）+ "确认"
>
> **两段确认机制**：第一次长按"恢复出厂"只置 `arm=1` 显示红色"确认"，
> 第二次长按才真的擦 NVS + 重启。**防误触**。

```c
static int setup_page_key(btn_id_t id, bool is_long)
{
    if (id == BTN_BACK) return 1;
    if (!is_long) {
        s_set_sel = (s_set_sel + SET_ROWS + ((id == BTN_UP) ? -1 : 1)) % SET_ROWS;
        s_set_arm = 0;                      /* 移动光标 = 取消武装 */
        setup_refresh();
        return 0;
    }
    if (id == BTN_DOWN) return 1;

    if (s_set_sel == 0) {                        /* 语言：切换即存 + 重建 */
        uint8_t nl = (uint8_t)((g_ui_lang + 1) % UI_LANG_COUNT);
        g_ui_lang = nl;
        (void)ui_cfg_set_lang(nl);
        ui_rebuild();                            /* ★ 文案换 → 整树重建 */
        return 0;
    }
    if (s_set_sel >= 1 && s_set_sel <= 5) {      /* 时间字段：+1 循环 */
        static const int lo[5] = { 2024, 1, 1, 0, 0 };
        static const int hi[5] = { 2099, 12, 31, 23, 59 };
        const int f = s_set_sel - 1;
        int v = s_set_t[f] + 1;
        if (v > hi[f]) v = lo[f];
        s_set_t[f] = v;
        setup_refresh();
        return 0;
    }
    if (s_set_sel == 6) {                        /* 网络校时：开关 */
        uint8_t on = (uint8_t)(!ui_cfg_sntp_get());
        (void)ui_cfg_set_sntp(on);
        if (on) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            if (esp_netif_sntp_init(&cfg) != ESP_OK)
                ESP_LOGW(TAG_UI, "SNTP 启动失败（无网络时正常；开关已保存）");
        } else {
            esp_netif_sntp_deinit();
        }
        setup_refresh();
        return 0;
    }
```

> **语言切换也走 `ui_rebuild()`**（同向导）。切语言 = 换所有文案指针 = 必须重建。
>
> **SNTP 开关**：`ui_cfg_set_sntp()` 存 NVS，同时 `esp_netif_sntp_init/deinit`
> 启停真正的 SNTP 客户端。**无网络时 init 失败是正常的**（日志警告，不影响）。
>
> **⚠️ 边界表和向导页一样不校验"2 月 31 日"**（`lo`/`hi` 是通用范围）。

```c
    if (s_set_sel == 7) {                        /* 保存：写 NVS + 重设时间基准 */
        ui_cfg_t c;
        c.ver = UI_CFG_VER; c.lang = (uint8_t)g_ui_lang; c.sntp = ui_cfg_sntp_get();
        c._pad = 0;
        struct tm tmv;
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_year = s_set_t[0] - 1900; tmv.tm_mon = s_set_t[1] - 1; tmv.tm_mday = s_set_t[2];
        tmv.tm_hour = s_set_t[3];        tmv.tm_min = s_set_t[4];     tmv.tm_isdst = 0;
        time_t t = mktime(&tmv);
        c.epoch = (t > 0) ? (int64_t)t : ui_cfg_now();
        if (ui_cfg_save(&c) == ESP_OK) {
            if (t > 0) ui_cfg_set_now((int64_t)t);
            lv_label_set_text(s_set_foot, TR(TID_P_SET_SAVED));
        } else {
            lv_label_set_text(s_set_foot, TR(TID_S_ERROR));
        }
        setup_refresh();
        return 0;
    }
```

> **"保存"行做的事**（对比"语言"和"时间字段"行）：
> - 语言行：切了**立刻**存（`ui_cfg_set_lang`）
> - 时间字段行：改了**不存**，只改内存 `s_set_t[]`
> - **保存行：把时间字段正式写 NVS** + 重设运行期基准
>
> **这体现了两种交互模式**：
> - 语言/SNTP = 开关型，改了即生效即存
> - 时间 = 编辑型，改完要按"保存"才落盘
>
> **`ui_cfg_save()` 内部有"写入回读对拍"** —— 失败会返回错误，
> 这里显示"错误"，成功显示"已保存"。**给用户明确反馈。**
>
> **`if (t > 0) ui_cfg_set_now(t)`**：保存成功后重设运行期时间基准，
> 让时钟从新设的时刻继续走。

```c
    /* 恢复出厂：两段确认（第一下 arm，第二下执行） */
    if (!s_set_arm) {
        s_set_arm = 1;
        lv_label_set_text(s_set_foot, TR(TID_P_SET_FACTORY_WARN));
        setup_refresh();
        return 0;
    }
    ESP_LOGW(TAG_UI, "恢复出厂：擦 ui_cfg + 重启");
    ui_cfg_factory_reset();                      /* 内部 esp_restart()，不返回 */
    return 0;
}
```

> **两段确认**：
> ```
> 第一次长按：s_set_arm = 1，底部显示警告，"恢复出厂"行变红"确认"
> 第二次长按：ui_cfg_factory_reset() → 擦 NVS + esp_restart()
> ```
> `s_set_arm` 会被"移动光标"清零（见 `if (!is_long)` 分支）——
> **移开光标就取消武装**，防止用户武装完忘了又误触。
>
> **`ui_cfg_factory_reset()` 内部 `esp_restart()` 不返回**，所以后面的
> `return 0;` 实际不可达（保留是为了让编译器不警告函数缺返回）。

## 4.2 ABOUT 关于（第 1019-1078 行）

```c
static lv_obj_t *s_ab_val[7];
static const uint16_t AB_KEY[7] = {
    TID_P_ABOUT_MODEL, TID_P_ABOUT_NAME, TID_P_ABOUT_BUILD, TID_P_ABOUT_FLASH,
    TID_P_ABOUT_SCREEN, TID_P_ABOUT_BUTTONS, TID_P_ABOUT_HW
};
```

> **7 行 = 型号 / 产品名 / 构建 / 固件 / 屏幕 / 按键 / 硬件**。

```c
static void about_refresh(void)
{
    char b[48];
    esp_chip_info_t ci;
    esp_chip_info(&ci);                                  /* 芯片信息（核数/型号/特征） */
    const esp_app_desc_t *d = esp_app_get_description(); /* 固件描述（版本/时间） */
    uint32_t fsz = 0;
    (void)esp_flash_get_size(NULL, &fsz);

    lv_label_set_text(s_ab_val[0], chip_model_str(ci.model));
    lv_label_set_text(s_ab_val[1], TR(TID_W_BRAND));
    if (d) {
        /* ⚠️ 行序必须与 AB_KEY 严格对齐 …（详见下方） */
        snprintf(b, sizeof(b), "%s %s", d->date, d->time);
        lv_label_set_text(s_ab_val[2], b);                         /* 构建 = 编译时间 */
        snprintf(b, sizeof(b), "v%s  IDF %s", d->version, IDF_VER);
        lv_label_set_text(s_ab_val[3], b);                         /* 固件 = 版本 + IDF */
    }
    lv_label_set_text(s_ab_val[4], "2.0 ST7789 320x240");
    lv_label_set_text(s_ab_val[5], "UP / DOWN / BACK");
    snprintf(b, sizeof(b), "%d core @ %u MHz, %u MB", (int)ci.cores,
             (unsigned)(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ), (unsigned)(fsz >> 20));
    lv_label_set_text(s_ab_val[6], b);
}
```

> **★ 这个注释极重要，是"多行 KV 页最容易被骗"的教训**：
> ```c
> /* ⚠️ 行序必须与 AB_KEY 严格对齐：0 型号 1 产品名 2 构建 3 固件 4 屏幕 5 按键 6 硬件。
>  *    2026-10-02 抓图 OCR 抓到过「标签与数值错开一行」的错版（构建显示成 1、
>  *    固件显示成构建时间）—— 多行 KV 页最容易被这种错位骗过，改完务必再抓图核对。 */
> ```
>
> **机制**：`s_ab_val[i]` 的下标 i 必须和 `AB_KEY[i]` 的语义对应。
> 一旦错位（标签是"构建"但值填到了 `s_ab_val[3]`），
> **代码看起来完全正常，只有看屏幕才发现**。
>
> **⇒ 教训：多行 KV 页的数据填充，改完必须抓图 + OCR 核对每一行的标签和值对不对得上。**
>
> **`d->date` / `d->time`**：编译时嵌入的构建时间戳（不是运行时间）——
> **这是"这固件什么时候编的"的可追溯信息**。
>
> **固件版本行**：`v1  IDF 5.x` —— `d->version` 默认 "1"（工程无 version.txt），
> 补上 IDF 版本让这一行真能表达"跑的哪套固件"。

---

# 五、页标与页面切换（第 1080-1129 行）

## 5.1 `build_dots()` — 底部页标（第 1081-1092 行）

```c
static void build_dots(void)
{
    const int w = 10, gap = 6;
    const int total = PG_MAX * w + (PG_MAX - 1) * gap;   /* 9*10 + 8*6 = 138 */
    int x = (SCR_W - total) / 2;                          /* 居中： (320-138)/2 = 91 */
    for (int i = 0; i < PG_MAX; i++) {
        lv_obj_t *d = mk_bar(s_scr, x, 226, w, 3, C_LINE);   /* y=226, 10×3 小条 */
        lv_obj_set_style_radius(d, 2, LV_PART_MAIN);
        s_dot[i] = d;
        x += w + gap;
    }
}
```

> **9 个小圆角条横向排列在 y=226**，表示"9 页里的第几页"。
> 当前页在 `ui_goto()` 里点亮（用该页强调色 + 辉光）。
>
> **挂在 `s_scr` 而不是某个 page** —— 所以切换页面时页标始终可见
> （但开机页会隐藏，见 `ui_goto`）。

## 5.2 `ui_goto()`（第 1095-1129 行）

```c
void ui_goto(int n)
{
    if (n < 0 || n >= PG_MAX) return;             /* 越界保护 */
    for (int i = 0; i < PG_MAX; i++) {
        lv_obj_set_hidden(s_page[i], i != n);      /* 只显示第 n 页 */
        if (s_dot[i]) {
            const bool on = (i == n);
            lv_obj_set_style_bg_color(s_dot[i], on ? pac(i) : C_LINE, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_dot[i], on ? LV_OPA_COVER : LV_OPA_60, LV_PART_MAIN);
            lv_obj_set_style_shadow_width(s_dot[i], on ? 10 : 0, LV_PART_MAIN);
            lv_obj_set_style_shadow_color(s_dot[i], pac(i), LV_PART_MAIN);
            lv_obj_set_style_shadow_opa(s_dot[i], LV_OPA_60, LV_PART_MAIN);
        }
    }
    s_cur = n;
    s_auto_ms = 4000;                              /* 重置 AUTO 计时 */

    if (s_pill_mode) lv_obj_set_hidden(s_pill_mode, (n == PG_BOOT));
    for (int i = 0; i < PG_MAX; i++)
        if (s_dot[i]) lv_obj_set_hidden(s_dot[i], (n == PG_BOOT));

    /* 进哪页刷哪页（只刷当前页，省 CPU） */
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

> **9 个页全部 `lv_obj_set_hidden` 显隐切换** —— 不是创建/销毁，是显示/隐藏。
> 代价：内存里同时有 9 页的对象树（~225 个对象）。
> 收益：切换极快，状态（光标位置等）天然保留。
>
> **开机页特殊处理**（不显示胶囊和页标）：
> ```c
> if (s_pill_mode) lv_obj_set_hidden(s_pill_mode, (n == PG_BOOT));
> for (...) if (s_dot[i]) lv_obj_set_hidden(s_dot[i], (n == PG_BOOT));
> ```
> **2026-10-02 踩过**：开机页右上角挂着"手动"胶囊，像张没做完的截图。
> **开机页是"品牌闪现"，不该挂功能页的控件。**
>
> **进哪页刷哪页**：不是所有页都需要 `*_refresh`（比如 HOME 是纯导航不需要），
> 而且 PROBE 页额外调 `probe_apply()`（同步设置到后端）。

---

# 六、按键导航与定时刷新（第 1131-1268 行）

## 6.1 `update_mode_label()` / `set_auto()`（第 1132-1149 行）

```c
static void update_mode_label(void)
{
    if (!s_lbl_mode) return;
    lv_label_set_text(s_lbl_mode, s_auto_on ? TR(TID_S_AUTO) : TR(TID_S_MANUAL));
    lv_obj_set_style_text_color(s_lbl_mode, s_auto_on ? C_GRAY : C_YELLOW, LV_PART_MAIN);
    if (s_pill_mode) {
        lv_obj_set_style_border_color(s_pill_mode, s_auto_on ? C_LINE : C_YELLOW, LV_PART_MAIN);
        lv_obj_set_style_border_opa(s_pill_mode, s_auto_on ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
    }
}

static void set_auto(bool on)
{
    if (s_auto_on == on) return;      /* 状态没变就不做事 */
    s_auto_on = on;
    s_auto_ms = 4000;
    update_mode_label();
}
```

> **AUTO/MANUAL 胶囊**：右上角显示当前模式。
> AUTO（自动轮播）= 灰（中性），MANUAL（手动）= 黄（提示"你接管了"）。
>
> **`set_auto` 幂等 + 改状态就重置计时 + 刷标签**。

## 6.2 `auto_step()` / `menu_move()` / `menu_enter()` / `to_menu()`（第 1151-1173 行）

```c
static void auto_step(void)
{
    int n = s_cur + 1;
    if (n >= PG_MAX || n <= PG_HOME) n = PG_PROBE;   /* 只在 PG_PROBE..PG_ABOUT 转 */
    s_in_menu = false;
    for (int k = 0; k < MENU_N; k++) if (MENU_PG[k] == n) s_sel = k;  /* 同步主菜单光标 */
    ui_goto(n);
}

static void menu_move(int d) { s_sel = (s_sel + MENU_N + d) % MENU_N; home_apply_sel(); }
static void menu_enter(void) { s_in_menu = false; ui_goto(MENU_PG[s_sel]); }
static void to_menu(void) { s_in_menu = true; ui_goto(PG_HOME); home_apply_sel(); }
```

> **`auto_step` 的循环范围**：`n <= PG_HOME`（跳过 BOOT/WIZ/HOME）⇒ 只在
> `PG_PROBE(3)..PG_ABOUT(8)` 六个功能页之间轮播。**不自动翻开机页/向导/菜单。**
>
> **同步主菜单光标**：`MENU_PG[k] == n` 找到对应菜单项，光标跟着动 ——
> 这样 AUTO 轮播时如果用户按 BACK 回菜单，光标停在刚才看的那一项（不丢上下文）。
>
> **`to_menu()` 和 `menu_enter()` 是一对**：
> - `menu_enter` = 主菜单 → 功能页（`s_in_menu=false`）
> - `to_menu` = 任何地方 → 主菜单（`s_in_menu=true` + 刷光标）

## 6.3 `handle_buttons()` — 按键核心分发（第 1175-1229 行）

```c
static void handle_buttons(void)
{
    btn_id_t id;
    bool is_long;

    while (btn_get_event(&id, &is_long)) {           /* 排空队列 */
        /* 开机页：任意返回键 = 跳过 */
        if (s_cur == PG_BOOT && id == BTN_BACK) {
            s_in_menu = true;
            ui_goto(PG_HOME);
            home_apply_sel();
            continue;
        }

        const bool turning_on = (id == BTN_BACK && is_long);
        if (!turning_on) set_auto(false);           /* 任何操作都关 AUTO */

        /* 页内钩子：返回 0 = 本页已消费 */
        if (!s_in_menu) {
            int handled = 1;
            switch (s_cur) {
            case PG_WIZ:   handled = wiz_page_key(id, is_long);   break;
            case PG_PROBE: handled = probe_page_key(id, is_long); break;
            case PG_FLASH: handled = flash_page_key(id, is_long); break;
            case PG_SETUP: handled = setup_page_key(id, is_long); break;
            default:       handled = 1;                           break;
            }
            if (!handled) continue;              /* 本页消费了，不往下走 */
        }

        switch (id) {
        case BTN_UP:
        case BTN_DOWN: {
            const int d = (id == BTN_UP) ? -1 : +1;
            if (!is_long) {
                if (s_in_menu) menu_move(d);     /* 短按上下：只在菜单移光标 */
            } else if (id == BTN_UP) {
                if (s_in_menu) menu_enter();     /* 长按上：进选中项 */
            } else {
                if (!s_in_menu) to_menu();        /* 长按下：回主菜单 */
            }
            break;
        }
        case BTN_BACK:
            if (is_long) set_auto(true);          /* 长按返回：开 AUTO */
            else         to_menu();               /* 短按返回：回主菜单 */
            break;
        default: break;
        }

        ESP_LOGI(TAG_UI, "key=%s%s page=%d sel=%d mode=%s", ...);
    }
}
```

> **★ 这是整个 UI 的按键分发中枢。** 处理顺序：
> ```
> 1. 开机页 BACK → 直接进主菜单（特殊前置）
> 2. 除"长按 BACK"外，任何操作都 set_auto(false) 关轮播
> 3. 如果不在主菜单层 → 交给该页的 xxx_page_key()
>       返回 0 = 本页消费了 → continue（不走全局）
>       返回 1 = 本页不处理 → 落到全局
> 4. 全局语义：
>       短按 UP/DOWN → s_in_menu ? menu_move : (无操作)
>       长按 UP      → s_in_menu ? menu_enter : (无操作)
>       长按 DOWN    → !s_in_menu ? to_menu : (无操作)
>       长按 BACK    → set_auto(true)
>       短按 BACK    → to_menu()
> ```
>
> **两级模型的关键**：
> - 在**主菜单**（`s_in_menu=true`）：短按上下移光标，长按上进详情
> - 在**功能页**（`s_in_menu=false`）：按键先给页钩子，页钩子不收才走全局
>   （长按下 = 回主菜单）
>
> **`while (btn_get_event(...))` 排空队列**：一次 tick 里处理所有待处理事件。
> **注意**：`continue` 会跳到 `while` 条件重新取事件，不会漏掉后续事件。
>
> **每个按键都打日志**（`key=UP-LONG page=4 sel=3 mode=MANUAL`）——
> 真机调试时非常有用，能看到"用户按了什么、系统怎么响应"。

## 6.4 `tick_cb()` — 300ms 定时器（第 1232-1268 行）

```c
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    s_tick_ms += 300;

    handle_buttons();                     /* ★ 先处理按键 */

    if (s_cur == PG_BOOT) {
        s_boot_ms += 300;
        for (int k = 0; k < 7; k++)
            lv_obj_set_style_bg_color(s_boot_seg[k],
                (s_boot_ms > (k + 1) * 300) ? C_CYAN : C_LINE, LV_PART_MAIN);
        if (s_boot_ms >= 2400) {          /* 2.4s 自动进菜单 */
            s_in_menu = true;
            ui_goto(PG_HOME);
            home_apply_sel();
        }
        return;                           /* 开机页不跑下面的定时刷新 */
    }

    if (s_cur == PG_STATUS && (s_tick_ms % 600 == 0))  status_refresh();
    if (s_cur == PG_PINS   && (s_tick_ms % 3000 == 0)) pins_refresh();
    if (s_cur == PG_ABOUT  && (s_tick_ms % 3000 == 0)) about_refresh();
    if (s_cur == PG_PROBE  && (s_tick_ms % 300  == 0))  probe_refresh();
    if (s_cur == PG_FLASH  && (s_tick_ms % 300  == 0))  flash_refresh();

    if (ui_cfg_sntp_get() && (s_tick_ms % 3000 == 0)) {
        time_t now = time(NULL);
        if (now > (time_t)UI_CFG_MIN_EPOCH) ui_cfg_set_now((int64_t)now);
    }

    if (s_auto_on) {
        s_auto_ms -= 300;
        if (s_auto_ms <= 0) auto_step();
    }
}
```

> **`s_tick_ms += 300` 然后用 `% 600` / `% 3000` 取模**：
> 这是"分频"的廉价做法。300ms 一次 tick，600ms 整除得 2（每 2 tick 刷一次），
> 3000ms 整除得 10（每 10 tick 刷一次）。
>
> **★ 刷新频率按数据变化速度分层**（不是所有页都 300ms）：
> | 页 | 频率 | 理由 |
> |---|---|---|
> | FLASH/PROBE | 300ms | 等后端结果，要快 |
> | STATUS | 600ms | WiFi/heap 变化中等 |
> | PINS/ABOUT | 3000ms | 静态信息，很少变 |
>
> **省 CPU = 省电 = 少掉帧**。这是嵌入式 UI 的基本功。
>
> **开机页进度条**：`s_boot_ms > (k+1)*300` ⇒ 每 300ms 亮一段，7 段 = 2.1s，
> 2.4s 全部完成进菜单。**进度条是"定时推进"而非"真实进度"** ——
> 但开机页本来就没真实进度可报，这样做是合理的（它表达"启动中"而非"完成了 X%"）。
>
> **SNTP 同步**：开了 SNTP 且联网时，每 3s 把系统时间并回 `ui_cfg` 基准。

---

# 七、整树重建与入口（第 1270-1357 行）

## 7.1 `build_all()`（第 1271-1311 行）

```c
static void build_all(void)
{
    s_scr = lv_screen_active();
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, C_BLACK, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_scrollable(s_scr, false);

    memset(s_page, 0, sizeof(s_page));
    memset(s_title, 0, sizeof(s_title));
    memset(s_dot, 0, sizeof(dot));

    for (int i = 0; i < PG_MAX; i++) s_page[i] = mk_page(i);   /* 先建 9 个空页 */

    build_boot();
    build_wiz();
    build_home();
    build_probe();
    build_flash();
    build_pins();
    build_status();
    build_setup();
    build_about();
    build_dots();

    /* AUTO/MANUAL 胶囊（右上角） */
    s_pill_mode = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_pill_mode);
    lv_obj_set_size(s_pill_mode, 76, 20);
    lv_obj_align(s_pill_mode, LV_ALIGN_TOP_RIGHT, -6, 7);
    lv_obj_set_style_radius(s_pill_mode, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pill_mode, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pill_mode, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pill_mode, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollable(s_pill_mode, false);

    s_lbl_mode = lv_label_create(s_pill_mode);
    lv_obj_set_style_text_font(s_lbl_mode, F14, LV_PART_MAIN);
    lv_obj_center(s_lbl_mode);

    update_mode_label();
}
```

> **顺序：先 `mk_page` 建 9 个空容器，再逐个 `build_xxx` 填内容**。
> 这样 `mk_page` 能统一处理标题栏/强调色，`build_xxx` 只管内容。
>
> **胶囊建在 `s_scr` 上**（不是某个 page）⇒ 所有页共享，但开机页会隐藏。
>
> **`lv_obj_center(s_lbl_mode)`**：把文字在胶囊里居中。
>
> **每次 `build_all` 都重建胶囊** ⇒ `ui_rebuild` 时胶囊也是新的。
> `update_mode_label()` 在最后调，确保标签有正确文本。

## 7.2 `ui_rebuild()`（第 1313-1322 行）

```c
void ui_rebuild(void)
{
    const int  keep     = s_cur;        /* 记当前页 */
    const bool was_menu = s_in_menu;    /* 记是否在菜单 */
    lv_obj_clean(s_scr);                /* 删掉所有子对象 */
    build_all();                        /* 重建 */
    s_in_menu = was_menu;
    ui_goto(keep);                      /* 切回原来的页 */
    if (s_in_menu) home_apply_sel();    /* 在菜单就刷光标 */
}
```

> **★ 为什么切语言必须整树重建**：
> `TR(tid)` 返回 `UI_STR[g_ui_lang][idx]` 的**指针**，
> 所有控件建立时就把旧语言的字符串指针**烤进了 label**。
> 改 `g_ui_lang` 不会让已存在的 label 自动更新。
> **只有 `lv_obj_clean` + 重建才能让所有文案换过来。**
>
> **`lv_obj_clean(s_scr)`**：删除 `s_scr` 的所有子对象（不删 `s_scr` 本身）。
>
> **保留当前页和菜单状态**：切语言不应该把用户"踢"回开机页。
>
> **`home_apply_sel()` 条件调用**：只在菜单层才需要（功能页的光标由页自己管）。

## 7.3 `ui_start_wizard()`（第 1324-1332 行）

```c
void ui_start_wizard(void)
{
    s_boot_ms = 0;
    s_wiz_step = 0;
    s_wiz_sel = 0;
    wiz_time_from_now();      /* 用当前时间预填 */
    s_in_menu = false;
    ui_goto(PG_WIZ);
}
```

> **首启向导的初始化 + 切页**。`app_main` 在 `!cfg_loaded` 时调。

## 7.4 `ui_init()` — UI 入口（第 1335-1357 行）

```c
void ui_init(void)
{
    setenv("TZ", "UTC0", 1);     /* 与 ui_cfg 的 epoch 同口径，避免时区歧义 */
    tzset();

    /* 烧录后端必须先起来：build_probe/build_flash 末尾会调 prog_api_cfg()，
       那条路要用它的互斥锁。此刻刷新任务还没启动 → 无并发风险。 */
    prog_api_init();

    build_all();

    s_boot_ms = 0;
    s_wiz_step = 0;
    s_wiz_sel = 0;
    wiz_time_from_now();
    ui_goto(PG_BOOT);

    if (btn_init() != ESP_OK)
        ESP_LOGW(TAG_UI, "按键初始化失败 —— 退化为纯自动轮播");

    lv_timer_create(tick_cb, 300, NULL);
    ESP_LOGI(TAG_UI, "UI v3 就绪：9 页（开机/向导/菜单/探针/烧录/引脚/状态/设置/关于）");
}
```

> **★ `prog_api_init()` 必须在 `build_all()` 之前**：
> `build_probe()` / `build_flash()` 末尾调 `prog_api_cfg()`，
> 那条路要访问 `prog_api.c` 的互斥锁 `s_lock`。**锁还没建就访问 = 崩。**
>
> **★ `setenv("TZ", "UTC0", 1)` + `tzset()`**：
> 时区固定为 UTC+0，与 `ui_cfg` 存的 epoch（UTC 时间戳）同口径。
> 不设的话，`gmtime_r` 可能按本地时区解析，导致显示的时间和实际差几小时。
> `tzset()` 让时区设置生效。
>
> **★ `lv_timer_create(tick_cb, 300, NULL)` 放最后**：
> 注册 300ms 周期定时器。**但此刻 `lcd_hw_start()` 还没调** ——
> 定时器要等 `lvgl_task` 起来后才被 `lv_timer_handler` 驱动。
>
> **`btn_init()` 失败降级**：按键坏了还能用（虽然只能自动轮播，
> 而自动轮播默认是关的……这个降级路径实际体验不好，但至少不崩）。
>
> **最后的日志确认九页**：和 `app_main` 末尾的日志呼应，双重确认。

---

# 八、本篇发现的三个排版隐患

写这份注解时对照行高（F14=17 / F16=20）核算，发现三处**间距小于行高**：

| 位置 | 步进 | 行高 | 重叠 | 严重度 | 说明 |
|---|---|---|---|---|---|
| PROBE 卡片 B 结果行 | 16px | 17 (F14) | 1px | 轻 | 密集列表，可接受 |
| FLASH 左栏 8 行 | 18px | 17 (F14) | -1（间隙1px） | 无 | 步进>行高，正常 |
| **PINS 9 行映射** | **12px** | **17 (F14)** | **5px** | **重** | **9 行会糊成一片** |

> **PINS 页是最可疑的**：`y = 48 + i * 12` 排 9 行，步进 12px 而 F14 行高 17px。
> 换算下来 9 行挤在 108px 里，每行实际只有 12px 空间放 17px 高的字。
>
> **建议**（**改前必须抓图确认**）：
> ```c
> // 方案 A：缩字号到 12px 级别（需要另一个字体）
> // 方案 B：加大卡片高度 + 步进
> // 方案 C：只显示已映射的信号（本板 7/9 已映射，2 个 VPP/VCC 未映射）
> ```
> 方案 C 最优雅 —— 跳过 `gpio < 0` 的行，7 行 × 14px = 98px，
> 加上标题行完全放得下，且"未映射"本来就不占视觉空间。
>
> **但这需要真机抓图 + OCR 验证，不能凭算术定案。** 这正是本工程反复强调的：
> **编译通过 ≠ 排版正确，只有看渲染结果才能定案。**
