/**
 * panel_lcd.c — 面板档案表 + 软件 SPI 驱动实现
 *
 * 档案：
 *   GM1020-05-10P  金逸晨 2.0" TFT · ST7789P3 · 240x320 · **10 脚**（用户实物定义）
 *   GM1020-05-14P  同一块屏的 14 脚插接版（淘宝详情图 OCR，保留对照）
 *   GM13-240x240   旧 1.3" 方屏 · ST7789V · 240x240 · 10 脚 · 偏移 0,80
 *
 * 分辨率差异决定 RAM 偏移：
 *   240x320 → 偏移 0,0（整片 RAM 都是可见区）
 *   240x240 → 偏移 0,80（IC RAM 是 240x320，方屏只占中间一段）
 *
 * ⚠️ 本文件对同一块屏保留两条档案：换屏只需把工程里的 ACTIVE_PANEL 换一行，
 *    不必改驱动代码——这正是档案化驱动的意义。
 *
 * 平台无关策略：GPIO 层用 ESP_PLATFORM 守卫，非 ESP 平台（宿主/KELL）仍可
 * 用档案表与校验和，但绘制返回 PANEL_E_NOPLAT（不静默假成功）。
 */
#include "panel_lcd.h"
#include <string.h>
#include <stdio.h>

#if defined(ESP_PLATFORM)
#include "driver/gpio.h"
#define PANEL_HAS_GPIO 1
#endif

/* ---------------- 编译器可移植性 ---------------- */
/* 部分静态函数只在 PANEL_HAS_GPIO(=ESP 平台) 下才被调用；
 * 宿主/KELL 形态下它们是「合法但未使用」，不该产生告警。 */
#if defined(__GNUC__) || defined(__clang__)
#define PANEL_UNUSED __attribute__((unused))
#else
#define PANEL_UNUSED
#endif

/* ---------------- 返回码 ---------------- */
#define PANEL_OK        0
#define PANEL_E_ARG    -1
#define PANEL_E_WIRE   -2
#define PANEL_E_ROT    -3
#define PANEL_E_NOPLAT -4
#define PANEL_E_IC     -5

/* ---------------- 信号名表（顺序必须与 panel_sig_t 严格一致） ---------------- */
static const char *const SIG_NAME[P_SIG__MAX] = {
    "NC", "GND", "LEDK", "LEDA", "RESET", "RS", "DC", "SDA",
    "SCL", "VCC", "IOVCC", "VDD", "CS", "LED+", "LED-"
};

/* ---------------- 座子针脚定义 ---------------- */

/* ★ 10 脚（用户实物定义，金逸晨模组）
 *   1 GND / 2 RS(DC) / 3 CS / 4 SCL / 5 SDA / 6 RESET / 7 VDD / 8 GND / 9 LED+ / 10 LED-
 * ⚠️ 10 脚只有一根电源脚 VDD（VCC 与 IOVCC 在模组内部已并），
 *    背光是 LED+/LED- 分立（LED+ 需串限流电阻），无 SDO/MISO（纯写屏）。 */
static const uint8_t PINMAP_10P[10] = {
    P_SIG_GND,   P_SIG_RS,    P_SIG_CS,    P_SIG_SCL,  P_SIG_SDA,
    P_SIG_RESET, P_SIG_VDD,   P_SIG_GND,   P_SIG_LEDP, P_SIG_LEDM
};

/* 14 脚插接版（淘宝商品详情图 OCR，同一块 2.0" 屏的另一种座子）
 *   1 NC / 2 GND / 3 LEDK / 4 LEDA / 5 GND / 6 RESET / 7 DC
 *   8 SDA / 9 SCL / 10 VCC / 11 IOVCC / 12 CS / 13 GND / 14 NC
 */
static const uint8_t PINMAP_14P[14] = {
    P_SIG_NC,  P_SIG_GND, P_SIG_LEDK, P_SIG_LEDA, P_SIG_GND,   P_SIG_RESET, P_SIG_DC,
    P_SIG_SDA, P_SIG_SCL, P_SIG_VCC,  P_SIG_IOVCC, P_SIG_CS,   P_SIG_GND,   P_SIG_NC
};

/* ---------------- ST7789 初始化序列 ---------------- */
/* 240x320（本次屏）：VCOMS 用 2.0" 常见值 0x28；gamma 一次发满 14 字节
 * ⚠️ 同一命令(0xE0/0xE1)不可拆成多次 wr_cmd 重发：ST7789 收到同命令会把
 *    参数指针复位到 0，后半段会**覆盖**前段而不是追加 → 必须一次发全。 */
static const panel_init_cmd_t INIT_240x320[] = {
    { 0x01, 0,  { 0 }, 120 },                                          /* SWRESET */
    { 0x11, 0,  { 0 }, 120 },                                          /* SLPOUT */
    { 0x3A, 1,  { 0x05 }, 0 },                                         /* COLMOD = RGB565 */
    { 0xB2, 5,  { 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 0 },                 /* PORCTRL */
    { 0xB7, 1,  { 0x35 }, 0 },                                         /* GCTRL */
    { 0xBB, 1,  { 0x28 }, 0 },                                         /* VCOMS */
    { 0xC0, 1,  { 0x2C }, 0 },                                         /* LCMCTRL */
    { 0xC2, 1,  { 0x01 }, 0 },                                         /* VDVVRHEN */
    { 0xC3, 1,  { 0x12 }, 0 },                                         /* VRHS */
    { 0xC4, 1,  { 0x20 }, 0 },                                         /* VDVS */
    { 0xC6, 1,  { 0x0F }, 0 },                                         /* FRCTRL2 */
    { 0xD0, 2,  { 0xA4, 0xA1 }, 0 },                                   /* PWCTRL1 */
    { 0xE0, 14, { 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B,
                  0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23 }, 0 }, /* PVGAMCTRL */
    { 0xE1, 14, { 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C,
                  0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23 }, 0 }, /* NVGAMCTRL */
    { 0x21, 0,  { 0 }, 0 },                                            /* INVON */
    { 0x13, 0,  { 0 }, 0 },                                            /* NORON */
    { 0x29, 0,  { 0 }, 10 },                                           /* DISPON */
};

/* 240x240（旧屏）：沿用台架 / STC32 已验证序列 */
static const panel_init_cmd_t INIT_240x240[] = {
    { 0x01, 0, { 0 }, 120 },
    { 0x11, 0, { 0 }, 120 },
    { 0x3A, 1, { 0x05 }, 0 },
    { 0xB2, 5, { 0x0C, 0x0C, 0x00, 0x33, 0x33 }, 0 },
    { 0xB7, 1, { 0x35 }, 0 },
    { 0xBB, 1, { 0x19 }, 0 },
    { 0xC0, 1, { 0x2C }, 0 },
    { 0xC2, 1, { 0x01 }, 0 },
    { 0xC3, 1, { 0x12 }, 0 },
    { 0xC4, 1, { 0x20 }, 0 },
    { 0xC6, 1, { 0x0F }, 0 },
    { 0xD0, 2, { 0xA4, 0xA1 }, 0 },
    { 0x21, 0, { 0 }, 0 },
    { 0x13, 0, { 0 }, 0 },
    { 0x29, 0, { 0 }, 10 },
};

/* ---------------- 档案表 ---------------- */

/* ★ 当前实物档案：2.0" 240x320 竖屏 · 10 脚
 * 偏移 0,0（240x320 占满 IC RAM）；反色 ON（TN 屏常见，若颜色反了改成 0 并去掉 INVON） */
const panel_profile_t PANEL_GM1020_05_10P = {
    "GM1020-05-10P", "GoldenMorning(金逸晨)", "ST7789P3", "4wireSPI",
    240, 320, 0, 0, 0x00, 1, 0x05,
    "10P", PINMAP_10P, 10, INIT_240x320,
    (uint16_t)(sizeof(INIT_240x320) / sizeof(INIT_240x320[0])),
    0xA36B04D1u   /* tools/panel_golden.py 独立对拍锁定 */
};

/* 同一块 2.0" 屏的 14 脚插接版（保留对照 → 换座子只换 ACTIVE_PANEL） */
const panel_profile_t PANEL_GM1020_05_14P = {
    "GM1020-05-14P", "GoldenMorning(金逸晨)", "ST7789P3", "4wireSPI",
    240, 320, 0, 0, 0x00, 1, 0x05,
    "14P", PINMAP_14P, 14, INIT_240x320,
    (uint16_t)(sizeof(INIT_240x320) / sizeof(INIT_240x320[0])),
    0x528468AAu
};

/* 旧 1.3" 240x240 方屏（RAM 偏移 0,80，序列 VCOMS=0x19、无 gamma 条目） */
const panel_profile_t PANEL_GM13_240x240 = {
    "GM13-240x240", "GoldenMorning(金逸晨)", "ST7789V", "4wireSPI",
    240, 240, 0, 80, 0x00, 1, 0x05,
    "10P", PINMAP_10P, 10, INIT_240x240,
    (uint16_t)(sizeof(INIT_240x240) / sizeof(INIT_240x240[0])),
    0x62CBB8FFu
};

static const panel_profile_t *const PANEL_TABLE[] = {
    &PANEL_GM1020_05_10P, &PANEL_GM1020_05_14P, &PANEL_GM13_240x240
};
#define PANEL_COUNT ((int)(sizeof(PANEL_TABLE) / sizeof(PANEL_TABLE[0])))

/* ---------------- 状态 ---------------- */
static panel_lcd_env_t g_env;
static panel_wire_t    g_wire;
static const panel_profile_t *g_prof;
static uint16_t        g_w, g_h;
static uint8_t         g_xoff, g_yoff;
static uint16_t        g_rot;
static int             g_up;

/* ---------------- SPI 字节流记录（协议层对拍） ----------------
 * spi_send8 是**所有** SDA 字节的唯一出口（命令、参数、像素都经它），
 * 因此在这里喂哈希即可覆盖整条协议流，无需改动各调用点。 */
static uint32_t g_st_csum = 0x811C9DC5u;   /* FNV-1a-32 初值 */
static uint32_t g_st_len  = 0;
static int      g_dryrun  = 0;             /* 1 = boot 不碰 GPIO */

void panel_lcd_dryrun(int on) { g_dryrun = on ? 1 : 0; }

void panel_lcd_stream_begin(void)
{
    g_st_csum = 0x811C9DC5u;
    g_st_len  = 0;
}

uint32_t panel_lcd_stream_csum(void) { return g_st_csum; }
uint32_t panel_lcd_stream_len(void)  { return g_st_len; }

void panel_lcd_size(uint16_t *w, uint16_t *h)
{
    if (w) { *w = g_up ? g_w : 0; }
    if (h) { *h = g_up ? g_h : 0; }
}

static void stream_feed(uint8_t b)
{
    g_st_csum ^= (uint32_t)b;
    g_st_csum *= 0x01000193u;
    g_st_len++;
}

void panel_lcd_env(const panel_lcd_env_t *e)
{
    if (e) { g_env = *e; } else { g_env.delay_ms = 0; g_env.log = 0; }
}

static void plog(const char *line)
{
    if (g_env.log) { g_env.log(line); }
}

static void PANEL_UNUSED pdel(uint32_t ms)
{
    /* dry-run = 不碰硬件、纯算流：等 IC 上电就绪毫无意义 → 直接跳过。
     * ⚠️ 延时**不产生任何字节**，跳掉不影响协议流哈希（黄金表逐位不变）。
     *    实测：init 表里 SWRESET/SLPOUT 各 120ms + DISPON 10ms = 250ms/次，
     *    12 组自检 × 250ms = 3.0s 纯空等（占自检总耗时 3.6s 的绝大部分）。 */
    if (g_dryrun) { return; }
    if (g_env.delay_ms) { g_env.delay_ms(ms); }
}

/* ---------------- 档案校验和（规范串，Python 逐位对拍） ---------------- */
uint32_t panel_lcd_csum(const panel_profile_t *p)
{
    char     buf[256];
    int      n;
    uint8_t  i;
    uint32_t h = 0x811C9DC5u;   /* FNV-1a-32 */

    if (!p) { return 0; }

    n = snprintf(buf, sizeof(buf), "%s|%s|%ux%u|%u,%u|inv%u|0x%02X|%s|%s:",
                 p->id, p->ic, (unsigned)p->w, (unsigned)p->h,
                 (unsigned)p->x_off, (unsigned)p->y_off,
                 (unsigned)p->invert, (unsigned)p->colmod,
                 p->iface, p->pin_id);
    if (n < 0) { return 0; }

    for (i = 0; i < p->pinmap_len; i++) {
        uint8_t s = p->pinmap[i];
        const char *nm = (s < P_SIG__MAX) ? SIG_NAME[s] : "?";
        int r = snprintf(buf + n, sizeof(buf) - (size_t)n, i ? " %s" : "%s", nm);
        if (r < 0) { return 0; }
        n += r;
        if ((size_t)n >= sizeof(buf)) { return 0; }
    }

    for (i = 0; buf[i]; i++) {
        h ^= (uint8_t)buf[i];
        h *= 0x01000193u;
    }
    return h;
}

const panel_profile_t *panel_lcd_by_id(const char *id)
{
    int i;
    if (!id) { return 0; }
    for (i = 0; i < PANEL_COUNT; i++) {
        if (PANEL_TABLE[i]->id && strcmp(PANEL_TABLE[i]->id, id) == 0) {
            return PANEL_TABLE[i];
        }
    }
    return 0;
}

static int panel_is_st7789(const panel_profile_t *p)
{
    return p && p->ic && (strncmp(p->ic, "ST7789", 6) == 0);
}

/* ---------------- 软件 SPI 底层（mode 0，MSB 先行，大端） ---------------- */
/* 所有 SDA 字节的唯一出口：先记流，再（视平台）驱动 GPIO。
 * 这样协议流哈希覆盖命令/参数/像素全部字节，且与硬件存在与否无关。 */
static void PANEL_UNUSED spi_send8(uint8_t dat)
{
    stream_feed(dat);
#ifdef PANEL_HAS_GPIO
    /* ⚠️ 这一层必须也被 dry-run 守卫住：否则"只算流不点屏"就是假的
     *    （12 组自检 × 15 万字节 = 2200 万次 GPIO 调用，实测耗时 17 秒，
     *      而且真的会去拨 SPI 线 → 自检会扰动屏上画面）。 */
    if (!g_dryrun) {
        int     i;
        uint8_t b = dat;
        for (i = 0; i < 8; i++) {
            gpio_set_level((gpio_num_t)g_wire.scl, 0);
            gpio_set_level((gpio_num_t)g_wire.sda, (b & 0x80u) ? 1 : 0);
            b = (uint8_t)(b << 1);
            gpio_set_level((gpio_num_t)g_wire.scl, 1);
        }
        gpio_set_level((gpio_num_t)g_wire.scl, 0);
    }
#endif
}

/* dry-run 或非 ESP 平台下，GPIO 动作空转；字节流照常记录 */
#ifdef PANEL_HAS_GPIO
#define PL_GPIO(pin, lvl) do { if (!g_dryrun) { gpio_set_level((gpio_num_t)(pin), (lvl)); } } while (0)
#else
#define PL_GPIO(pin, lvl) do { (void)(pin); (void)(lvl); } while (0)
#endif

static void wr_cmd(uint8_t cmd)
{
    PL_GPIO(g_wire.cs, 0);
    PL_GPIO(g_wire.dc, 0);
    spi_send8(cmd);
    PL_GPIO(g_wire.cs, 1);
}

static void wr_dat8(uint8_t dat)
{
    PL_GPIO(g_wire.cs, 0);
    PL_GPIO(g_wire.dc, 1);
    spi_send8(dat);
    PL_GPIO(g_wire.cs, 1);
}

static void wr_dat16(uint16_t v)
{
    wr_dat8((uint8_t)(v >> 8));
    wr_dat8((uint8_t)(v & 0xFFu));
}

/* ---------------- 显示窗口（含档案 RAM 偏移） ---------------- */
void panel_lcd_set_window(int x0, int y0, int x1, int y1)
{
    wr_cmd(0x2A);
    wr_dat16((uint16_t)(x0 + g_xoff));
    wr_dat16((uint16_t)(x1 + g_xoff));
    wr_cmd(0x2B);
    wr_dat16((uint16_t)(y0 + g_yoff));
    wr_dat16((uint16_t)(y1 + g_yoff));
    wr_cmd(0x2C);   /* RAMWR */
}

/* MADCTL 按旋转取值（ST7789: MY=0x80 MX=0x40 MV=0x20） */
static uint8_t PANEL_UNUSED madctl_of(const panel_profile_t *p, uint16_t rot)
{
    uint8_t base = p ? p->madctl : 0;
    switch (rot) {
        case 90:  return (uint8_t)(base | 0x60u);   /* MV|MX */
        case 180: return (uint8_t)(base | 0xC0u);   /* MX|MY */
        case 270: return (uint8_t)(base | 0xA0u);   /* MV|MY */
        default:  return base;                      /* 0 */
    }
}

int panel_lcd_boot(const panel_profile_t *p, const panel_wire_t *w, uint16_t rotation)
{
    uint16_t i;

    if (!p || !w) { return PANEL_E_ARG; }
    if (w->sda < 0 || w->scl < 0 || w->dc < 0 || w->rst < 0 || w->cs < 0) { return PANEL_E_WIRE; }
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) { return PANEL_E_ROT; }
    if (!panel_is_st7789(p)) { return PANEL_E_IC; }

    g_prof = p;
    g_wire = *w;
    g_rot  = rotation;

    /* ⚠️ 旋转 90/270 时 MADCTL 的 MV 位把 X/Y 轴对调，
     *    可见区在 IC RAM 中的偏移必须**跟着对调**，否则整幅画面平移错位。
     *    240x320 屏偏移为 0,0 时看不出来；240x240 屏偏移 0,80 会明显偏一条。 */
    if (rotation == 90 || rotation == 270) {
        g_xoff = p->y_off;
        g_yoff = p->x_off;
    } else {
        g_xoff = p->x_off;
        g_yoff = p->y_off;
    }
    g_w  = (rotation == 90 || rotation == 270) ? p->h : p->w;
    g_h  = (rotation == 90 || rotation == 270) ? p->w : p->h;
    g_up = 0;

    /* 非 ESP 平台：没有 GPIO 就没有真绘制 → 不假装成功（返回 NOPLAT）。
     * 但 dry-run（宿主纯协议建模 / 真机只算流哈希）要继续往下走。 */
#ifndef PANEL_HAS_GPIO
    if (!g_dryrun) { (void)i; return PANEL_E_NOPLAT; }
#else
    if (!g_dryrun) {
        int pins[5];
        int k;
        pins[0] = w->sda; pins[1] = w->scl; pins[2] = w->dc; pins[3] = w->rst; pins[4] = w->cs;
        for (k = 0; k < 5; k++) {
            gpio_set_direction((gpio_num_t)pins[k], GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)pins[k], 0);
        }
        if (w->bl >= 0) {
            gpio_set_direction((gpio_num_t)w->bl, GPIO_MODE_OUTPUT);
            gpio_set_level((gpio_num_t)w->bl, 1);
        }
        /* 硬件复位 */
        gpio_set_level((gpio_num_t)w->rst, 0);
        pdel(20);
        gpio_set_level((gpio_num_t)w->rst, 1);
        pdel(120);
    }
#endif

    for (i = 0; i < p->init_len; i++) {
        const panel_init_cmd_t *c = &p->init[i];
        uint8_t k;
        if (c->cmd == 0xFFu) {                 /* 纯延时条目 */
            if (c->delay_ms) { pdel(c->delay_ms); }
            continue;
        }
        wr_cmd(c->cmd);
        for (k = 0; k < c->len; k++) { wr_dat8(c->data[k]); }
        if (c->delay_ms) { pdel(c->delay_ms); }
    }

    /* 旋转（MADCTL）在序列之后补发，保证旋转指令不被表格覆盖 */
    wr_cmd(0x36);
    wr_dat8(madctl_of(p, rotation));

    g_up = 1;
    return PANEL_OK;
}

void panel_lcd_fill(uint16_t rgb565)
{
    int i;

    /* 没 boot 过（含非 ESP 平台且未 dry-run）→ 不画，也不假装成功 */
    if (!g_up) { return; }

    panel_lcd_set_window(0, 0, (int)g_w - 1, (int)g_h - 1);
    PL_GPIO(g_wire.cs, 0);
    PL_GPIO(g_wire.dc, 1);
    for (i = 0; i < (int)g_w * (int)g_h; i++) {
        spi_send8((uint8_t)(rgb565 >> 8));
        spi_send8((uint8_t)(rgb565 & 0xFFu));
    }
    PL_GPIO(g_wire.cs, 1);
}

/* ---------------- 打印 ---------------- */
static int has_sig(const panel_profile_t *p, panel_sig_t s);

/* snprintf 返回值是「想要的长度」，可能超出缓冲；每次拼接后夹一下，防越界 */
static int clamp_n(int n, size_t cap)
{
    return (n < 0) ? 0 : ((size_t)n > cap ? (int)cap : n);
}

void panel_lcd_dump(const panel_profile_t *p, const panel_wire_t *w)
{
    char    line[256];
    char    map[192];
    int     n = 0;
    uint8_t i;

    if (!p) { plog("panel: (null)"); return; }

    snprintf(line, sizeof(line),
             "panel: %s | %s | %s | %s | %ux%u | off %u,%u | MADCTL 0x%02X | INV %u",
             p->id, p->vendor, p->ic, p->iface, (unsigned)p->w, (unsigned)p->h,
             (unsigned)p->x_off, (unsigned)p->y_off, (unsigned)p->madctl, (unsigned)p->invert);
    plog(line);

    map[0] = '\0';
    for (i = 0; i < p->pinmap_len; i++) {
        uint8_t s = p->pinmap[i];
        const char *nm = (s < P_SIG__MAX) ? SIG_NAME[s] : "?";
        int r = snprintf(map + n, sizeof(map) - (size_t)n, i ? " %u=%s" : "%u=%s",
                         (unsigned)(i + 1), nm);
        if (r < 0) { break; }
        n += r;
        if ((size_t)n >= sizeof(map)) { break; }
    }
    snprintf(line, sizeof(line), "panel: %s 针脚 %s", p->pin_id, map);
    plog(line);

    snprintf(line, sizeof(line),
             "panel: golden 0x%08X | csum 0x%08X | rotation %u | init %u steps",
             (unsigned)p->golden, (unsigned)panel_lcd_csum(p),
             (unsigned)g_rot, (unsigned)p->init_len);
    plog(line);

    if (w) {
        char pw[160];
        int  m = 0;
        snprintf(line, sizeof(line), "panel: wire SDA=%d SCL=%d DC=%d RST=%d CS=%d BL=%d",
                 (int)w->sda, (int)w->scl, (int)w->dc, (int)w->rst, (int)w->cs, (int)w->bl);
        plog(line);

        /* 供电提示按档案实际存在的信号生成（不同座子提示不同，避免照抄错接线） */
        pw[0] = '\0';
        if (has_sig(p, P_SIG_VDD)) {
            m += snprintf(pw + m, sizeof(pw) - (size_t)m, "VDD");
            m = clamp_n(m, sizeof(pw));
        }
        if (has_sig(p, P_SIG_VCC)) {
            m += snprintf(pw + m, sizeof(pw) - (size_t)m, "%sVCC", m ? "+" : "");
            m = clamp_n(m, sizeof(pw));
        }
        if (has_sig(p, P_SIG_IOVCC)) {
            m += snprintf(pw + m, sizeof(pw) - (size_t)m, "%sIOVCC", m ? "+" : "");
            m = clamp_n(m, sizeof(pw));
        }
        if (m) {
            m += snprintf(pw + m, sizeof(pw) - (size_t)m, " -> 3V3");
            m = clamp_n(m, sizeof(pw));
        }

        if (has_sig(p, P_SIG_LEDA) || has_sig(p, P_SIG_LEDK)) {
            m += snprintf(pw + m, sizeof(pw) - (size_t)m,
                          "%sLEDA -> 3V3(串限流R) | LEDK -> GND%s",
                          m ? " || " : "",
                          (w->bl >= 0) ? "（LEDK 走 NPN/MOS 由 BL 控亮度）" : "");
            m = clamp_n(m, sizeof(pw));
        } else if (has_sig(p, P_SIG_LEDP) || has_sig(p, P_SIG_LEDM)) {
            m += snprintf(pw + m, sizeof(pw) - (size_t)m,
                          "%sLED+ -> 3V3(串限流R) | LED- -> GND%s",
                          m ? " || " : "",
                          (w->bl >= 0) ? "（LED- 走 NPN/MOS 由 BL 控亮度）" : "");
            m = clamp_n(m, sizeof(pw));
        }
        if (!m) { snprintf(pw, sizeof(pw), "(档案无电源/背光信号)"); }

        snprintf(line, sizeof(line), "panel: power %s", pw);
        plog(line);
    }
}

/* ---------------- 自检 ---------------- */
static int has_sig(const panel_profile_t *p, panel_sig_t s)
{
    uint8_t i;
    for (i = 0; i < p->pinmap_len; i++) {
        if (p->pinmap[i] == (uint8_t)s) { return 1; }
    }
    return 0;
}

int panel_lcd_selftest(const panel_profile_t *p, const panel_wire_t *w)
{
    int fails = 0;

    if (!p) { return 1; }

    if (p->w < 64 || p->h < 64 || p->w > 480 || p->h > 480) { fails++; }
    if (p->colmod != 0x05u) { fails++; }
    if (!panel_is_st7789(p)) { fails++; }
    if (p->pinmap_len == 0 || p->pinmap_len > 24) { fails++; }
    if (p->init_len == 0) { fails++; }
    if (panel_lcd_csum(p) != p->golden) { fails++; }

    /* 必需信号：数据 + 控制 + 电源 + 地 */
    if (!has_sig(p, P_SIG_SDA)) { fails++; }
    if (!has_sig(p, P_SIG_SCL)) { fails++; }
    if (!has_sig(p, P_SIG_DC) && !has_sig(p, P_SIG_RS)) { fails++; }   /* RS / DC 同一功能，收其一 */
    if (!has_sig(p, P_SIG_CS))  { fails++; }
    if (!has_sig(p, P_SIG_RESET) && !has_sig(p, P_SIG_RS)) { fails++; }
    if (!has_sig(p, P_SIG_GND)) { fails++; }
    if (!has_sig(p, P_SIG_VCC) && !has_sig(p, P_SIG_VDD) && !has_sig(p, P_SIG_IOVCC)) { fails++; }

    /* 序列末条必须是 DISPON(0x29) */
    if (p->init[p->init_len - 1].cmd != 0x29u) { fails++; }

    /* 每条命令的参数个数不得超出 data[] 容量 */
    {
        uint16_t k;
        for (k = 0; k < p->init_len; k++) {
            if (p->init[k].len > (uint8_t)(sizeof(p->init[k].data))) { fails++; }
        }
    }

    /* 接线合法性：不得复用同一 GPIO */
    if (w) {
        int pins[5];
        int k, j;
        pins[0] = w->sda; pins[1] = w->scl; pins[2] = w->dc; pins[3] = w->rst; pins[4] = w->cs;
        for (k = 0; k < 5; k++) {
            if (pins[k] < 0) { fails++; continue; }
            for (j = k + 1; j < 5; j++) {
                if (pins[k] == pins[j]) { fails++; }
            }
        }
        if (w->bl >= 0) {
            for (k = 0; k < 5; k++) { if (w->bl == pins[k]) { fails++; } }
        }
    }

    /* 档案表 ID 唯一 */
    {
        int t;
        for (t = 0; t < PANEL_COUNT; t++) {
            if (PANEL_TABLE[t] == p) { continue; }
            if (PANEL_TABLE[t]->id && strcmp(PANEL_TABLE[t]->id, p->id) == 0) { fails++; }
        }
    }

    return fails;
}

/* ================= 协议层自检：SPI 字节流黄金 =================
 * 为什么需要它：panel_lcd_selftest 只锁「档案元数据」（型号/针脚/init 表），
 * 驱动**真正吐给屏的字节**（窗口偏移算法、RGB565 字节序、MADCTL 发射时机、
 * 每帧像素数）此前一路无验——那正是「日志绿 = 屏对」这个假设最脆弱的地方。
 * 本自检把三段固定序列的字节流哈希与黄金表逐条比对：
 *   宿主(C11/C++17) == 真机 == tools/panel_stream_golden.py（独立 Python 模型）
 * 三方一致才算证明；任一环节笔误都会被逐位差异当场抓住。
 */
/* ⚠️ 本表由 tools/gen_stream_golden.py 生成，请勿手改（改了就重跑脚本）。
 * 来源：tools/panel_stream_golden.py（独立模型）经 tools/gen_stream_golden.py 导出。 */
typedef struct {
    const char *id;
    uint32_t    blen, wlen, flen;          /* boot/window/fill 三段流长度
                                            * ⚠️ fill 段可达 15 万+ 字节，
                                            *    **不可**用 uint16 装（会静默截断） */
    uint32_t    boot[4], win[4], fill[4];  /* 对应 rot 0/90/180/270 的 FNV-1a-32 */
} panel_stream_golden_t;

static const panel_stream_golden_t STREAM_GOLDEN[PANEL_COUNT] = {
    /* 0 */ { "GM1020-05-10P", 62, 73, 153684,
            { 0xDABD970Au, 0x3ABE2E2Au, 0x9ABEC54Au, 0x7ABE92EAu },
            { 0x0160574Au, 0xF2CD6BF4u, 0x8005FE8Au, 0xD9A115B4u },
            { 0x8EDE048Au, 0xCCDC3B42u, 0xD2417ECAu, 0xCD672A02u } },
    /* 1 */ { "GM1020-05-14P", 62, 73, 153684,
            { 0xDABD970Au, 0x3ABE2E2Au, 0x9ABEC54Au, 0x7ABE92EAu },
            { 0x0160574Au, 0xF2CD6BF4u, 0x8005FE8Au, 0xD9A115B4u },
            { 0x8EDE048Au, 0xCCDC3B42u, 0xD2417ECAu, 0xCD672A02u } },
    /* 2 */ { "GM13-240x240", 32, 43, 115254,
            { 0x59CAFFA5u, 0xF9CA6885u, 0x99C9D165u, 0xB9CA03C5u },
            { 0x2F56FB9Bu, 0x190C7871u, 0x9A91CBDBu, 0xD913B131u },
            { 0xBF1F041Du, 0xB4400BBDu, 0xEEAC4EDDu, 0x414561FDu } },
};

static void PANEL_UNUSED smismatch(const char *stage, uint32_t got, uint32_t exp)
{
    char line[128];
    snprintf(line, sizeof(line), "  !! %-7s expect 0x%08X got 0x%08X",
             stage, (unsigned)exp, (unsigned)got);
    plog(line);
}

int panel_lcd_stream_selftest(void)
{
    static const uint16_t ROTS[4] = { 0, 90, 180, 270 };
    panel_wire_t    w = { 7, 6, 4, 5, 10, 0 };   /* 流与接线无关；这里只为过 boot 的合法性检查 */
    /* 自检对调用方必须零可见副作用 → 快照全部驱动状态 */
    const panel_profile_t *sv_prof = g_prof;
    panel_wire_t           sv_wire = g_wire;
    uint16_t               sv_w = g_w, sv_h = g_h, sv_rot = g_rot;
    uint8_t                sv_x = g_xoff, sv_y = g_yoff;
    int                    sv_up = g_up, sv_dry = g_dryrun;
    char            line[128];
    int             bad = 0, t, r;

    panel_lcd_dryrun(1);            /* 自检不点屏：只算流，不扰屏 */

    for (t = 0; t < PANEL_COUNT; t++) {
        const panel_stream_golden_t *g = &STREAM_GOLDEN[t];
        const panel_profile_t       *p = PANEL_TABLE[t];
        if (!p || !g->id || strcmp(p->id, g->id) != 0) {
            snprintf(line, sizeof(line), "STREAM: 档案与黄金表顺序不一致 (slot %d)", t);
            plog(line); bad++; continue;
        }
        for (r = 0; r < 4; r++) {
            uint16_t ww = 0, hh = 0;
            uint32_t bl, wl, fl, bc, wc, fc;

            panel_lcd_stream_begin();
            if (panel_lcd_boot(p, &w, ROTS[r]) != 0) {
                snprintf(line, sizeof(line), "STREAM: %s rot %u boot FAILED", p->id, (unsigned)ROTS[r]);
                plog(line); bad++; continue;
            }
            panel_lcd_size(&ww, &hh);
            snprintf(line, sizeof(line), "ARCHIVE %-14s rot %3u  w=%u h=%u",
                     p->id, (unsigned)ROTS[r], (unsigned)ww, (unsigned)hh);
            plog(line);

            bl = panel_lcd_stream_len(); bc = panel_lcd_stream_csum();
            snprintf(line, sizeof(line), "  %-7s len=%u csum=0x%08X", "boot",
                     (unsigned)bl, (unsigned)bc);
            plog(line);

            panel_lcd_set_window(0, 0, (int)ww - 1, (int)hh - 1);
            wl = panel_lcd_stream_len(); wc = panel_lcd_stream_csum();
            snprintf(line, sizeof(line), "  %-7s len=%u csum=0x%08X", "window",
                     (unsigned)wl, (unsigned)wc);
            plog(line);

            panel_lcd_fill(0xF800u);                 /* 纯红 RGB565 */
            fl = panel_lcd_stream_len(); fc = panel_lcd_stream_csum();
            snprintf(line, sizeof(line), "  %-7s len=%u csum=0x%08X", "fill",
                     (unsigned)fl, (unsigned)fc);
            plog(line);

            /* 只在出错时多打 !! 行 → 正确运行时可与 Python 输出直接 diff */
            if (bl != g->blen)  { smismatch("boot-len", bl, g->blen);   bad++; }
            if (bc != g->boot[r]) { smismatch("boot", bc, g->boot[r]);   bad++; }
            if (wl != g->wlen)  { smismatch("win-len", wl, g->wlen);    bad++; }
            if (wc != g->win[r])  { smismatch("window", wc, g->win[r]);  bad++; }
            if (fl != g->flen)  { smismatch("fill-len", fl, g->flen);   bad++; }
            if (fc != g->fill[r]) { smismatch("fill", fc, g->fill[r]);   bad++; }
        }
    }

    /* 复原全部驱动状态：自检对调用方必须零可见副作用
     * ⚠️ 只复原 g_dryrun 不够——否则 g_rot 会残留成最后一组的 270，
     *    随后 panel_lcd_dump 就会打出误导性的 "rotation 270"（实测踩过）。 */
    g_prof   = sv_prof;  g_wire = sv_wire;
    g_w      = sv_w;     g_h    = sv_h;   g_rot = sv_rot;
    g_xoff   = sv_x;     g_yoff = sv_y;
    g_up     = sv_up;    g_dryrun = sv_dry;
    return bad;
}
