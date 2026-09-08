/*
 * xiaomo - 硬件设备命令分发层 (hw_dev)
 *
 * 职责: 统一设备命令表 + 前缀分发 + 动态命令注册。
 *   - 静态命令表 g_dev_table: cmd/echo/self/send/tx/rx/i2c/pwm
 *     (命令名带 "\n\r " 尾缀 = 串口风格行协议, dispatch 按前缀匹配)
 *   - 动态命令容器 dev_bin: 运行时 hw_dev_register() 注册, hw_dev_hook() 释放
 *   - 2026-09-07 全跨式接入 xiaomo 项目 (六层):
 *       Makefile:  HW_SRCS 收编
 *       VM 内核:   OP_HW_DEV_CALL (vm_core.c, kvm_run 上电自动 hw_dev_init)
 *       编译器:    mo2kbc 内置 hw_dev("...") → OP_HW_DEV_CALL
 *       CLI:       ./xiaomo hwdev ["cmd..."]
 *       示例:      examples/hwdev_test.mo (.kbc 端到端)
 *       测试:      tests/run_tests.sh hwdev 块
 *
 * 修复记录 (2026-09-07, 用户初版 → 可编译版, 语义零改动):
 *   ① g_dev_table i2c/pwm 两项缺逗号分隔 (语法错)
 *   ② dev_i2c_handler/dev_pwm_handler 只有声明无定义 (链接错)
 *   ③ hw_dev_init 误加 auto 存储类 (C/C++ 函数均不允许)
 *   ④ 实现名 dev_hook 与头文件声明的 hw_dev_hook 不一致 (链接错), 统一为 hw_dev_hook
 * 新增 API (不动原设计):
 *   hw_dev_register(name, mask, cb) — 动态命令注册进 dev_bin
 *   hw_dev_registered()             — 动态表当前条数
 */
#include "hw_dev.h"
#include "hw_direct.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* 各个命令处理函数声明 */
static uint16_t dev_cmd_handler(void *arg);
static uint16_t dev_echo_handler(void *arg);
static uint16_t dev_self_handler(void *arg);
static uint16_t dev_send_handler(void *arg);
static uint16_t dev_tx_handler(void *arg);
static uint16_t dev_rx_handler(void *arg);
static uint16_t dev_i2c_handler(void *arg);
static uint16_t dev_pwm_handler(void *arg);

static const hw_dev_address_t g_dev_table[] =
{
    {
        .cmd_name = "cmd\n\r ",
        .mask     = 0x000010U,
        .handler  = dev_cmd_handler
    },
    {
        .cmd_name = "echo\n\r ",
        .mask     = 0x00001eU,
        .handler  = dev_echo_handler
    },
    {
        .cmd_name = "self\n\r ",
        .mask     = 0x0001edU,
        .handler  = dev_self_handler
    },
    {
        .cmd_name = "send\n\r ",
        .mask     = 0x002580U,
        .handler  = dev_send_handler
    },
    {
        .cmd_name = "tx\n\r ",
        .mask     = 0x010000U,
        .handler  = dev_tx_handler
    },
    {
        .cmd_name = "rx\n\r ",
        .mask     = 0x100000U,
        .handler  = dev_rx_handler
    },
    {
        .cmd_name = "i2c\n\r ",
        .mask     = 0x1ed000U,
        .handler  = dev_i2c_handler
    },
    {
        .cmd_name = "pwm\n\r ",
        .mask     = 0xdef000U,
        .handler  = dev_pwm_handler
    }
};

/* 动态命令容器 dev_bin：运行时可动态分配table */
static DEV_POP_t dev_bin = {
    .table = NULL,
    .count = 0U
};

/* 动态表已分配容量 (count 为实际条数) */
static uint32_t dev_bin_cap = 0U;

/* hw_dev_hook：动态表资源释放钩子 (与头文件声明同名, 原 dev_hook) */
void hw_dev_hook(void *arg)
{
    (void)arg;
    if(dev_bin.table != NULL)
    {
        free((hw_dev_address_t*)dev_bin.table);
        dev_bin.table = NULL;
        dev_bin.count = 0U;
        dev_bin_cap = 0U;
    }
}

/* handler 文本结果缓冲 (echo 回显 / i2c 读数 / pwm 状态, hw_dev_result 取走) */
static char g_result[256] = "";
static void dev_result_set(const char* fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_result, sizeof(g_result), fmt, ap);
    va_end(ap);
}
void hw_dev_result(char *buf, uint32_t cap)
{
    if (buf == NULL || cap == 0U) return;
    snprintf(buf, cap, "%s", g_result);
}

/* 全局静态命令集合 */
static const DEV_POP_t g_dev_pop =
{
    .table  = g_dev_table,
    .count  = sizeof(g_dev_table) / sizeof(hw_dev_address_t)
};

static uint16_t dev_cmd_handler(void *arg)
{
    (void)arg;
    return 0x00U;
}
static uint16_t dev_echo_handler(void *arg)
{
    /* 2026-09-07 真实化: 回显参数部分 (原恒 0x00 桩) */
    const hw_dev_req_t* rq = (const hw_dev_req_t*)arg;
    dev_result_set("%s", (rq != NULL && rq->params != NULL) ? rq->params : "");
    return HWDEV_R_OK;
}
static uint16_t dev_self_handler(void *arg)
{
    (void)arg;
    return 0x00U;
}
static uint16_t dev_send_handler(void *arg)
{
    (void)arg;
    return 0x00U;
}
static uint16_t dev_tx_handler(void *arg)
{
    (void)arg;
    return 0x00U;
}
static uint16_t dev_rx_handler(void *arg)
{
    (void)arg;
    return 0x00U;
}
/* ========== i2c/pwm 真实外设 (2026-09-07, 原 TODO 桩) ==========
 * 命令协议 (params 为前缀 "i2c\n\r " 之后的空格分词):
 *   i2c\n\r list                    → 枚举 /dev/i2c-*
 *   i2c\n\r open <dev> <addr>       → 打开从站, result "h=<idx>"
 *   i2c\n\r write <h> <hex...>      → 纯写 (hex 字节序列)
 *   i2c\n\r read <h> <len>          → 纯读, result 为 hex 串
 *   i2c\n\r xfer <h> <hex...> <len> → 写-读组合 (RDWR), result 为读到的 hex
 *   i2c\n\r close <h>               → 关闭
 *   pwm\n\r list                    → 枚举 /sys/class/pwm/pwmchip*
 *   pwm\n\r set <chip> <ch> <period_ns> <duty_ns> → 导出+周期+占空+使能
 *   pwm\n\r read <chip> <ch> <attr> → 读 period/duty_cycle/enable
 *   pwm\n\r off <chip> <ch>         → 禁能 + unexport
 * 返回码: HWDEV_R_* (0x00 成功 / 0x01 缺参 / 0x02 参数非法
 *         / 0x03 IO 错 / 0x04 句柄无效), 未命中仍由 dispatch 返回 0xFF。 */

/* i2c 句柄表: 0..15 → hw_direct fd (open 时登记) */
#define HWDEV_I2C_SLOTS 16
static int g_i2c_fd[HWDEV_I2C_SLOTS];
static int g_i2c_seeded = 0;

static void hwdev_i2c_seed(void)
{
    if (g_i2c_seeded) return;
    for (int i = 0; i < HWDEV_I2C_SLOTS; i++) g_i2c_fd[i] = -1;
    g_i2c_seeded = 1;
}
static int hwdev_i2c_slot(int fd)
{
    for (int i = 0; i < HWDEV_I2C_SLOTS; i++)
        if (g_i2c_fd[i] == fd) return i;
    return -1;
}
/* hex 串 ("3c 00 01" / "3c0001") → 字节; 回写消费长度, 失败 -1 */
static int hwdev_hex_parse(const char* s, uint8_t* out, int maxn)
{
    int n = 0; unsigned v; int have = 0;
    while (*s && n < maxn) {
        if (*s == ' ' || *s == ',') { if (have) return -1; s++; continue; }
        if (sscanf(s, "%2x", &v) != 1) return -1;
        out[n++] = (uint8_t)v; s += 2; have = 1;
        if (*s == ' ' || *s == ',' || *s == '\0') { have = 0; s++; }
    }
    return (have && n < maxn) ? -1 : n;
}
static void hwdev_hex_dump(const uint8_t* b, int n, char* out, int cap)
{
    int o = 0;
    for (int i = 0; i < n && o < cap - 4; i++) {
        o += snprintf(out + o, (size_t)(cap - o), "%02x%s",
                      b[i], (i + 1 < n) ? " " : "");
    }
    out[o] = '\0';
}

/* token 扫描: q 前进一个 token, 再跳过空白; 返回 NULL 表示到头 */
static const char* hwdev_tok_next(const char* q)
{
    while (*q && *q != ' ' && *q != '\t') q++;
    while (*q == ' ' || *q == '\t') q++;
    return (*q == '\0') ? NULL : q;
}
static const char* hwdev_trim_end(const char* p)
{
    const char* e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
    return e;
}

static uint16_t dev_i2c_handler(void *arg)
{
    const hw_dev_req_t* rq = (const hw_dev_req_t*)arg;
    const char* p = (rq != NULL && rq->params != NULL) ? rq->params : "";
    char sub[24], a1[64], a2[24], a3[24], a4[24];
    int k = sscanf(p, "%23s %63s %23s %23s", sub, a1, a2, a3);
    (void)a4;
    if (k <= 0) { dev_result_set("i2c: 缺子命令 (list/open/write/read/xfer/close)"); return HWDEV_R_NOARGS; }
    hwdev_i2c_seed();
    if (strcmp(sub, "list") == 0) {
        char lst[192];
        int n = hw_i2c_enumerate(lst, (int)sizeof(lst));
        if (n < 0) { dev_result_set("i2c list: 枚举失败 (无 /dev/i2c-*)"); return HWDEV_R_IOERR; }
        dev_result_set("%s", (n == 0) ? "(无 i2c 总线)" : lst);
        return HWDEV_R_OK;
    }
    if (strcmp(sub, "open") == 0) {
        if (k < 3) { dev_result_set("i2c open: 用法 open <dev> <addr>"); return HWDEV_R_NOARGS; }
        uint16_t addr = (uint16_t)strtoul(a2, NULL, 0);
        int fd = hw_i2c_open(a1, addr);
        if (fd < 0) { dev_result_set("i2c open %s 失败", a1); return HWDEV_R_IOERR; }
        int slot = hwdev_i2c_slot(fd); /* fd 刚登记必命中 */
        if (slot < 0) { hw_i2c_close(fd); dev_result_set("i2c 句柄表满"); return HWDEV_R_IOERR; }
        dev_result_set("h=%d fd=%d addr=0x%02x", slot, fd, addr);
        return HWDEV_R_OK;
    }
    /* write/read/xfer/close 第一个参数都是句柄 */
    if (k < 2) { dev_result_set("i2c %s: 缺句柄", sub); return HWDEV_R_NOARGS; }
    int h = (int)strtol(a1, NULL, 0);
    if (h < 0 || h >= HWDEV_I2C_SLOTS || g_i2c_fd[h] < 0) {
        dev_result_set("i2c %s: 句柄 h=%d 无效 (先 open)", sub, h);
        return HWDEV_R_NOHANDLE;
    }
    if (strcmp(sub, "close") == 0) {
        if (hw_i2c_close(g_i2c_fd[h]) != 0) { dev_result_set("i2c close 失败"); return HWDEV_R_IOERR; }
        g_i2c_fd[h] = -1;
        dev_result_set("h=%d closed", h);
        return HWDEV_R_OK;
    }
    if (strcmp(sub, "write") == 0) {
        uint8_t buf[64];
        /* 定位 hex 起点: 跳过 sub 与 h 两个 token (对多空格健壮) */
        const char* hexs = hwdev_tok_next(hwdev_tok_next(p));
        if (hexs == NULL) { dev_result_set("i2c write: 用法 write <h> <hex...>"); return HWDEV_R_NOARGS; }
        int n = hwdev_hex_parse(hexs, buf, (int)sizeof(buf));
        if (n <= 0) { dev_result_set("i2c write: hex 解析失败"); return HWDEV_R_BADARG; }
        if (hw_i2c_write(g_i2c_fd[h], buf, n) != n) { dev_result_set("i2c w h=%d 失败", h); return HWDEV_R_IOERR; }
        dev_result_set("h=%d wrote %d byte(s)", h, n);
        return HWDEV_R_OK;
    }
    if (strcmp(sub, "read") == 0) {
        if (k < 3) { dev_result_set("i2c read: 用法 read <h> <len>"); return HWDEV_R_NOARGS; }
        int len = atoi(a2);
        if (len <= 0 || len > 80) { dev_result_set("i2c read: len 1..80"); return HWDEV_R_BADARG; }
        uint8_t buf[80]; char hex[192];
        memset(buf, 0, sizeof(buf));
        int n = hw_i2c_read(g_i2c_fd[h], buf, len);
        if (n < 0) { dev_result_set("i2c r h=%d 失败", h); return HWDEV_R_IOERR; }
        hwdev_hex_dump(buf, n, hex, (int)sizeof(hex));
        dev_result_set("h=%d [%s]", h, hex);
        return HWDEV_R_OK;
    }
    if (strcmp(sub, "xfer") == 0) {
        /* rxlen = 末 token; tx hex = sub/h 两个 token 之后到末 token 之间
         * (token 扫描定位, 对多字节 hex 与多空格健壮) */
        const char* end = hwdev_trim_end(p);
        const char* lbeg = end;
        while (lbeg > p && lbeg[-1] != ' ' && lbeg[-1] != '\t') lbeg--;
        size_t ll = (size_t)(end - lbeg);
        char lasttok[24];
        if (ll == 0 || ll >= sizeof(lasttok)) { dev_result_set("i2c xfer: 用法 xfer <h> <txhex...> <rxlen>"); return HWDEV_R_NOARGS; }
        memcpy(lasttok, lbeg, ll); lasttok[ll] = '\0';
        int rxlen = atoi(lasttok);
        if (rxlen <= 0 || rxlen > 80) { dev_result_set("i2c xfer: rxlen 1..80"); return HWDEV_R_BADARG; }
        const char* hexs = hwdev_tok_next(hwdev_tok_next(p));
        if (hexs == NULL || (size_t)(lbeg - hexs) < ll + 1) { dev_result_set("i2c xfer: 缺 tx hex"); return HWDEV_R_NOARGS; }
        size_t hl = (size_t)(lbeg - hexs) - ll - 1; /* 末 token 前的空格不算 hex */
        if (hl >= 192) { dev_result_set("i2c xfer: tx hex 过长"); return HWDEV_R_BADARG; }
        char hexsrc[192];
        memcpy(hexsrc, hexs, hl); hexsrc[hl] = '\0';
        uint8_t tx[64], rx[80]; char hex[192];
        int tn = hwdev_hex_parse(hexsrc, tx, (int)sizeof(tx));
        if (tn <= 0) { dev_result_set("i2c xfer: tx hex 解析失败"); return HWDEV_R_BADARG; }
        if (hw_i2c_xfer(g_i2c_fd[h], tx, tn, rx, rxlen) < 0) { dev_result_set("i2c xfer h=%d 失败", h); return HWDEV_R_IOERR; }
        hwdev_hex_dump(rx, rxlen, hex, (int)sizeof(hex));
        dev_result_set("h=%d [%s]", h, hex);
        return HWDEV_R_OK;
    }
    dev_result_set("i2c: 未知子命令 %s", sub);
    return HWDEV_R_BADARG;
}

static uint16_t dev_pwm_handler(void *arg)
{
    const hw_dev_req_t* rq = (const hw_dev_req_t*)arg;
    const char* p = (rq != NULL && rq->params != NULL) ? rq->params : "";
    char sub[24], a1[24], a2[24], a3[24], a4[24];
    int k = sscanf(p, "%23s %23s %23s %23s %23s", sub, a1, a2, a3, a4);
    if (k <= 0) { dev_result_set("pwm: 缺子命令 (list/set/read/off)"); return HWDEV_R_NOARGS; }
    if (strcmp(sub, "list") == 0) {
        char lst[192];
        int n = hw_pwm_enumerate(lst, (int)sizeof(lst));
        if (n < 0) { dev_result_set("pwm list: 枚举失败 (无 /sys/class/pwm)"); return HWDEV_R_IOERR; }
        dev_result_set("%s", (n == 0) ? "(无 pwmchip)" : lst);
        return HWDEV_R_OK;
    }
    if (k < 3) { dev_result_set("pwm %s: 用法 %s <chip> <ch> ...", sub, sub); return HWDEV_R_NOARGS; }
    int chip = atoi(a1), ch = atoi(a2);
    if (chip < 0 || ch < 0) { dev_result_set("pwm %s: chip/ch 非法", sub); return HWDEV_R_BADARG; }
    if (strcmp(sub, "set") == 0) {
        if (k < 5) { dev_result_set("pwm set: 用法 set <chip> <ch> <period_ns> <duty_ns>"); return HWDEV_R_NOARGS; }
        int pn = atoi(a3), dn = atoi(a4);
        if (pn <= 0 || dn < 0 || dn > pn) { dev_result_set("pwm set: 0<=duty<=period 非法"); return HWDEV_R_BADARG; }
        if (hw_pwm_export(chip, ch) != 0) { dev_result_set("pwm export chip%d/ch%d 失败", chip, ch); return HWDEV_R_IOERR; }
        if (hw_pwm_set_period(chip, ch, pn) != 0) { dev_result_set("pwm period 写入失败 (需 root?)"); return HWDEV_R_IOERR; }
        if (hw_pwm_set_duty(chip, ch, dn) != 0)   { dev_result_set("pwm duty 写入失败"); return HWDEV_R_IOERR; }
        if (hw_pwm_set_enable(chip, ch, 1) != 0)  { dev_result_set("pwm enable 失败"); return HWDEV_R_IOERR; }
        dev_result_set("pwmchip%d/ch%d period=%d duty=%d ON", chip, ch, pn, dn);
        return HWDEV_R_OK;
    }
    if (strcmp(sub, "read") == 0) {
        if (k < 4) { dev_result_set("pwm read: 用法 read <chip> <ch> <attr>"); return HWDEV_R_NOARGS; }
        char v[64];
        if (hw_pwm_read(chip, ch, a3, v, (int)sizeof(v)) < 0) { dev_result_set("pwm read %s 失败", a3); return HWDEV_R_IOERR; }
        dev_result_set("pwmchip%d/ch%d %s=%s", chip, ch, a3, v);
        return HWDEV_R_OK;
    }
    if (strcmp(sub, "off") == 0) {
        (void)hw_pwm_set_enable(chip, ch, 0);
        if (hw_pwm_unexport(chip, ch) != 0) { dev_result_set("pwm unexport chip%d/ch%d 失败", chip, ch); return HWDEV_R_IOERR; }
        dev_result_set("pwmchip%d/ch%d OFF", chip, ch);
        return HWDEV_R_OK;
    }
    dev_result_set("pwm: 未知子命令 %s", sub);
    return HWDEV_R_BADARG;
}

uint16_t hw_dev_ctrl(hw_dev_cmd_t cmd, hw_dev_cb_t cb, void *arg)
{
    (void)cmd;
    if (cb != NULL)
    {
        return cb(arg);
    }
    return 0x01U;
}

/**
 * @brief 命令分发：前缀匹配，支持命令后面携带参数
 * cmd_str: 输入完整字符串，例："echo\n\r hello"
 */
uint16_t hw_dev_dispatch(const char *cmd_str, void *arg)
{
    if (cmd_str == NULL)
    {
        return 0xFFU;
    }
    /* 2026-09-07 参数通道: 组 hw_dev_req_t 传 handler
     * (params = 前缀之后的参数部分; handler 按需读取, 忽略 arg 则兼容旧版) */
    g_result[0] = '\0';
    hw_dev_req_t rq;
    rq.cmd  = cmd_str;
    rq.ctx  = arg;
    rq.params = NULL;
    /* 先遍历静态命令表 */
    for (uint32_t i = 0; i < g_dev_pop.count; i++)
    {
        const hw_dev_address_t *entry = &g_dev_pop.table[i];
        size_t prefix_len = strlen(entry->cmd_name);
        /* 判断输入是否以命令前缀开头 */
        if(strncmp(cmd_str, entry->cmd_name, prefix_len) == 0)
        {
            if (entry->handler != NULL)
            {
                rq.params = cmd_str + prefix_len;
                return entry->handler(&rq);
            }
        }
    }
    /* 再遍历动态dev_bin表 */
    for(uint32_t i = 0; i < dev_bin.count; i++)
    {
        const hw_dev_address_t *entry = &dev_bin.table[i];
        size_t prefix_len = strlen(entry->cmd_name);
        if(strncmp(cmd_str, entry->cmd_name, prefix_len) == 0)
        {
            if(entry->handler != NULL)
            {
                rq.params = cmd_str + prefix_len;
                return entry->handler(&rq);
            }
        }
    }
    return 0xFFU; /* 未找到命令 */
}

uint16_t hw_dev_main(void *arg)
{
    (void)arg;
    return 0x00U;
}

void hw_dev_init(void *arg)
{
    (void)arg;
    /* 初始化时调用钩子，释放旧动态表 */
    hw_dev_hook(NULL);
}

/* ================= 新增: 动态命令注册 (dev_bin) ================= */

/**
 * @brief 注册动态命令进 dev_bin (运行时可扩展命令表)
 * @return 0 成功; -1 参数非法; -2 命令名已存在 (静态表或动态表撞名); -3 内存不足
 */
int hw_dev_register(const char *cmd_name, uint32_t mask, hw_dev_cb_t cb)
{
    if (cmd_name == NULL || cmd_name[0] == '\0')
    {
        return -1;
    }
    /* 撞名检查: 与静态表前缀互撞或已有动态命令重名 → 拒绝 */
    if (hw_dev_dispatch(cmd_name, NULL) != 0xFFU)
    {
        return -2;
    }
    if (dev_bin.count >= dev_bin_cap)
    {
        uint32_t ncap = dev_bin_cap ? dev_bin_cap * 2U : 4U;
        hw_dev_address_t *nt =
            (hw_dev_address_t*)realloc((void*)dev_bin.table, ncap * sizeof(hw_dev_address_t));
        if (nt == NULL)
        {
            return -3;
        }
        dev_bin.table = nt;
        dev_bin_cap   = ncap;
    }
    hw_dev_address_t *e = (hw_dev_address_t*)&dev_bin.table[dev_bin.count];
    e->cmd_name = cmd_name;
    e->mask     = mask;
    e->handler  = cb;
    dev_bin.count++;
    return 0;
}

/**
 * @brief 当前动态命令表条数 (0 = 无动态命令)
 */
uint32_t hw_dev_registered(void)
{
    return dev_bin.count;
}
