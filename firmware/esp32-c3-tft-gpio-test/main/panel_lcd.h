/**
 * panel_lcd.h — 屏幕「面板档案」+ 软件 SPI 驱动（平台无关）
 *
 * 立项目标：把「换一块屏」变成「换一条档案」，而不是散落各处的魔数。
 *
 * 档案（panel_profile_t）承载一块屏的全部不变信息：
 *   型号 / 驱动 IC / 分辨率 / RAM 偏移 / MADCTL / 是否反色 / 像素格式 /
 *   接口类型 / **座子针脚定义（座号 → 信号）** / 初始化序列 / 档案校验和(黄金值)
 *
 * 接线（panel_wire_t）承载「这块屏接在本板哪些 GPIO 上」，与档案解耦：
 *   同一块屏换块板 → 只改 wire，不动档案。
 *
 * 平台无关：不依赖 FreeRTOS/IDF，延时与日志走注入回调 panel_lcd_env()，
 * 因此同一份档案可在 ESP32 / ESP8266 / 宿主模拟器 / KELL 内核态复用。
 */
#ifndef PANEL_LCD_H
#define PANEL_LCD_H

#include <stdint.h>

/* 座子引脚信号（一个座子引脚 = 一个信号；NC = 空脚）
 * 注：RS 与 DC 是同一根线的两种叫法（命令/数据选择），两条都收。 */
typedef enum {
    P_SIG_NC = 0, P_SIG_GND,  P_SIG_LEDK, P_SIG_LEDA, P_SIG_RESET, P_SIG_RS,
    P_SIG_DC,     P_SIG_SDA,  P_SIG_SCL,  P_SIG_VCC,  P_SIG_IOVCC, P_SIG_VDD,
    P_SIG_CS,     P_SIG_LEDP, P_SIG_LEDM, P_SIG__MAX
} panel_sig_t;

/* 初始化序列单条：cmd=0xFF 表示「纯延时」。
 * data[] 必须能装下单条命令的全部参数：ST7789 的 gamma(0xE0/0xE1) 是 14 字节，
 * 且同一命令不可拆成多条重发（IC 会把参数指针复位→后半段覆盖前段）。 */
#define PANEL_INIT_DATA_MAX 14
typedef struct {
    uint8_t  cmd;
    uint8_t  len;
    uint8_t  data[PANEL_INIT_DATA_MAX];
    uint16_t delay_ms;
} panel_init_cmd_t;

/* 面板档案 */
typedef struct {
    const char *id;          /* 模组编号，如 "GM1020-05-10P" */
    const char *vendor;      /* 厂商 */
    const char *ic;          /* 驱动 IC，如 "ST7789P3" */
    const char *iface;       /* 接口，如 "4wireSPI" */
    uint16_t    w, h;        /* 原生分辨率（竖屏） */
    uint8_t     x_off, y_off;/* 可见区在 IC RAM 中的偏移 */
    uint8_t     madctl;      /* 基础 MADCTL(0x36) */
    uint8_t     invert;      /* 1 = 发 INVON(0x21) */
    uint8_t     colmod;      /* 像素格式，0x05 = RGB565 */
    const char *pin_id;      /* 座子标识，如 "10P" / "14P" */
    const uint8_t *pinmap;   /* 座号 1..n → panel_sig_t 序列 */
    uint8_t     pinmap_len;
    const panel_init_cmd_t *init;
    uint16_t    init_len;
    uint32_t    golden;      /* 档案规范串的 FNV-1a-32（Python 独立对拍锁定） */
} panel_profile_t;

/* 主机侧接线（-1 = 不用该脚） */
typedef struct {
    int8_t sda;   /* MOSI */
    int8_t scl;   /* SCLK */
    int8_t dc;    /* 数据/命令 */
    int8_t rst;   /* 复位 */
    int8_t cs;    /* 片选 */
    int8_t bl;    /* 背光使能（-1 = 背光常亮/直连 3V3） */
} panel_wire_t;

/* 注入环境（延时 + 日志），保持本模块平台无关 */
typedef struct {
    void (*delay_ms)(uint32_t ms);
    void (*log)(const char *line);
} panel_lcd_env_t;

void panel_lcd_env(const panel_lcd_env_t *e);

/* 上电 + 复位 + 初始化（rotation: 0/90/180/270） */
int  panel_lcd_boot(const panel_profile_t *p, const panel_wire_t *w, uint16_t rotation);
/* 整屏纯色（RGB565，大端发送） */
void panel_lcd_fill(uint16_t rgb565);
/* 设置显示窗口（自动加上档案的 RAM 偏移） */
void panel_lcd_set_window(int x0, int y0, int x1, int y1);
/* 打印档案 + 接线 + 供电提示（走注入的 log） */
void panel_lcd_dump(const panel_profile_t *p, const panel_wire_t *w);
/* 档案规范串的 FNV-1a-32（应与 p->golden 相等） */
uint32_t panel_lcd_csum(const panel_profile_t *p);
/* 档案自检：返回失败项数（0 = 全绿） */
int  panel_lcd_selftest(const panel_profile_t *p, const panel_wire_t *w);

const panel_profile_t *panel_lcd_by_id(const char *id);

/* ---------- 协议层：SPI 字节流黄金哈希 ----------
 * 「日志绿」只证明没崩；「流哈希」才证明驱动吐给屏的字节**逐字节正确** ——
 * 窗口偏移算法、RGB565 字节序、MADCTL 发射时机、每帧像素数，全都落在流里。
 * 宿主（gcc -std=c11 / g++ -std=c++17）与真机共用同一套哈希，
 * 且与 tools/panel_stream_golden.py 的独立实现逐位对拍。
 *
 *   panel_lcd_dryrun(1)      → boot 不碰 GPIO（宿主纯模型；真机=只算哈希不点屏）
 *   panel_lcd_stream_begin() → 清空流
 *   跑固定序列（boot + set_window + fill）
 *   panel_lcd_stream_csum()  → 该序列的 FNV-1a-32
 *
 * ⚠️ 流里**只含 SDA 上的字节**（含命令/数据、大端像素），不含延时与 CS/DC 时序；
 *    时序由 spi_send8 的 mode-0 相位保证，属另一维（真机示波器/逻辑分析仪范畴）。
 */
void     panel_lcd_dryrun(int on);
void     panel_lcd_stream_begin(void);
uint32_t panel_lcd_stream_csum(void);
uint32_t panel_lcd_stream_len(void);
/* 旋转后的有效绘制尺寸（boot 后可用；未 boot 返回 0,0） */
void     panel_lcd_size(uint16_t *w, uint16_t *h);
/* 协议层自检：3 档案 × 4 旋转逐条比对字节流黄金 → 返回失败项数（0 = 全绿）。
 * 自检期间自动切 dry-run（不点屏、不扰屏），结束恢复原状态。
 * 输出与 tools/panel_stream_golden.py 逐字节可 diff；只在**不匹配**时多打 !! 行。 */
int      panel_lcd_stream_selftest(void);

/* ---------- 内置档案 ---------- */
/* 2.0" 竖屏 240x320（ST7789P3）—— 同一块屏两种座子，换屏只改 ACTIVE_PANEL 一行 */
extern const panel_profile_t PANEL_GM1020_05_10P;  /* 10 脚：GND RS CS SCL SDA RESET VDD GND LED+ LED- */
extern const panel_profile_t PANEL_GM1020_05_14P;  /* 14 脚：NC GND LEDK LEDA GND RESET DC SDA SCL VCC IOVCC CS GND NC */
/* 1.3" 方屏 240x240（ST7789V）10 脚，RAM 偏移 0,80 */
extern const panel_profile_t PANEL_GM13_240x240;

#endif /* PANEL_LCD_H */
