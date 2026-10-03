/**
 * prog_api.c — 烧录器后台实现（PROG 页的真后端）
 *
 * 与 hw_pin 的关系：
 *   hw_pin  = 协议/档案/驱动（跨平台，不认识 ESP32）
 *   prog_hw = ESP32-S3 的 BSP（把引脚动作接到真 GPIO / UART）
 *   prog_api（本文件）= 任务化外壳：进度、日志、结论、线程边界
 *
 * 「诚实」是硬约束：
 *   SIM  = 确定性 W25Q 器件模型 → 操作会成功（用来验证链路与 UI）
 *   REAL = 真 GPIO bit-bang     → 没接芯片就报 NOFLASH/IOERR，**绝不假装成功**
 * 这就是为什么 DRIVER 那一行选了 REAL 时值是警示色。
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hw_pin.h"
#include "prog_api.h"
#include "prog_hw.h"

/* ============================================================
 * 状态 + 锁
 * ============================================================ */
static prog_status_t     s_st;
static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_jobq;

/* UI 侧设置（worker 与 UI 共享；只在空闲时改，故无需细粒度保护） */
static int s_tgt = 0;      /* 0=s3-spi 1=w25q 2=s3-isp */
static int s_drv = 0;      /* 0=SIM   1=REAL */
static int s_img = 0;

static const char *g_img_name[] = { "banner-256", "pattern-1k", "rand-2k" };
static const uint32_t g_img_size[] = { 256u, 1024u, 2048u };
#define PROG_IMG_N 3

const char *prog_api_image_name(int i) { return (i >= 0 && i < PROG_IMG_N) ? g_img_name[i] : "?"; }
uint32_t    prog_api_image_size(int i) { return (i >= 0 && i < PROG_IMG_N) ? g_img_size[i] : 0u; }

/* 镜像缓冲：最大 2KB。**必须 static** ——
 * 「>512B 局部数组一律 static」是 hw_flash 那个 25KB 真机栈炸弹换来的纪律。 */
static uint8_t s_image[2048];
#define PROG_IMAGE_MAX ((uint32_t)sizeof(s_image))

static inline void LOCK(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void UNLOCK(void) { xSemaphoreGive(s_lock); }

/* 往日志流里压一行（内部已加锁；同时打到串口便于脚本取证） */
static void log_line(const char *s)
{
    LOCK();
    memmove(s_st.log[1], s_st.log[0],
            (size_t)(PROG_LOG_LINES - 1) * PROG_LOG_WIDTH);
    snprintf(s_st.log[0], PROG_LOG_WIDTH, "%s", s);
    UNLOCK();
    printf("[prog] %s\n", s);
}

static void plogf(const char *fmt, ...)
{
    char b[PROG_LOG_WIDTH];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    log_line(b);
}

void prog_api_log(const char *s) { if (s) log_line(s); }

static void set_pct(int pct)
{
    LOCK();
    s_st.pct = (pct < 0) ? 0 : (pct > 100 ? 100 : pct);
    UNLOCK();
}

static void set_run(int op)
{
    LOCK();
    s_st.state = PROG_ST_RUNNING;
    s_st.op    = op;
    s_st.pct   = 0;
    s_st.rc    = 0;
    snprintf(s_st.msg, sizeof(s_st.msg), "RUNNING");
    UNLOCK();
}

static void set_finish(int rc, const char *msg)
{
    LOCK();
    s_st.rc    = rc;
    s_st.state = (rc == HW_PIN_R_OK) ? PROG_ST_DONE : PROG_ST_FAIL;
    s_st.pct   = 100;
    snprintf(s_st.msg, sizeof(s_st.msg), "%s",
             msg ? msg : hw_pin_result_code_str(rc));
    UNLOCK();
}

static const char *g_op_name[] = { "READ ID", "ERASE", "PROGRAM", "VERIFY", "ISP SYNC" };

/* ============================================================
 * 镜像生成（确定性；同一个 IMAGE 选择永远是同一份字节）
 * ============================================================ */
static void gen_image(int idx, uint8_t *buf, uint32_t n)
{
    uint32_t i;
    if (idx == 0) {
        /* banner-256：可读文本，串口/逻辑分析仪直接能认出来 */
        const char *txt = "XIAOMO FLASHER * ESP32-S3 GPIO-BITBANG * 2026-09-30 ";
        size_t tl = strlen(txt);
        for (i = 0; i < n; i++) buf[i] = (uint8_t)txt[i % tl];
    } else if (idx == 1) {
        /* pattern-1k：递增+异或，便于脚本比对错位 */
        for (i = 0; i < n; i++) buf[i] = (uint8_t)((i ^ (i >> 3)) & 0xFFu);
    } else {
        /* rand-2k：确定性 LCG（不用 rand() → 跨次运行一致，可复现） */
        uint32_t x = 0x1234ABCDu;
        for (i = 0; i < n; i++) {
            x = x * 1103515245u + 12345u;
            buf[i] = (uint8_t)((x >> 16) & 0xFFu);
        }
    }
}

/* ============================================================
 * 各操作
 * ============================================================ */
static const hw_pin_profile_t *pick_profile(void)
{
    if (s_tgt == 2) return prog_hw_profile_isp();
    if (s_tgt == 1) return prog_hw_profile_w25q();
    return prog_hw_profile_spi();
}
static const char *pick_profile_name(void) { return pick_profile()->name; }

/* REAL 模式下失败了，跑一次 MOSI↔MISO 回环，把「引脚/时序对不对」
 * 从「芯片有没有接」里切出来 —— 否则用户永远在猜是哪一头坏了。
 * （SIM 模式没有意义：那里 MISO 由器件模型供，怎么测都是对的。） */
static void diag_real(int rc)
{
    uint8_t tx[8], rx[8];
    uint32_t n = 0;
    int lb;
    if (rc == HW_PIN_R_OK || s_drv != 1) return;
    if (hw_pin_gpio_of(HW_PIN_MOSI) < 0 || hw_pin_gpio_of(HW_PIN_MISO) < 0) {
        plogf("  (no SPI pins in this profile)");
        return;
    }
    lb = prog_hw_loopback(tx, rx, &n);
    if (lb == 0) {
        plogf("  loopback MOSI-MISO OK");
        plogf("  -> pins ok, target silent");
    } else if (lb > 0) {
        plogf("  loopback err@%d %02X!=%02X", lb,
             (unsigned)tx[lb - 1], (unsigned)rx[lb - 1]);
        plogf("  -> check wiring/jumper");
    } else {
        plogf("  loopback n/a (%d)", lb);
    }
}

static void op_rdid(void)
{
    uint32_t jed = 0;
    int rc;
    plogf("rdid: 0x9F @%s", pick_profile_name());
    rc = hw_pin_flash_rdid(&jed);
    if (rc == HW_PIN_R_OK) {
        LOCK(); s_st.jedec = jed; UNLOCK();
        plogf("  jedec %02X %02X %02X", (unsigned)(jed & 0xFFu),
             (unsigned)((jed >> 8) & 0xFFu), (unsigned)((jed >> 16) & 0xFFu));
    } else {
        plogf("  rdid -> %s", hw_pin_result_code_str(rc));
    }
    diag_real(rc);
    set_pct(100);
    set_finish(rc, NULL);
}

static void op_erase(void)
{
    uint32_t sz = prog_api_image_size(s_img);
    uint32_t sectors = (sz + HW_PIN_FLASH_SECTOR - 1u) / HW_PIN_FLASH_SECTOR;
    uint32_t k;
    int rc = HW_PIN_R_OK;

    plogf("erase: %u sector(s)", (unsigned)sectors);
    for (k = 0; k < sectors; k++) {
        plogf("  se 0x%04X", (unsigned)(k * HW_PIN_FLASH_SECTOR));
        rc = hw_pin_flash_sector_erase(k * HW_PIN_FLASH_SECTOR);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_wait(200u);
        if (rc != HW_PIN_R_OK) break;
        set_pct((int)((k + 1u) * 100u / sectors));
        vTaskDelay(1);                       /* 让出：别饿死 IDLE（TG1WDT） */
    }
    if (rc == HW_PIN_R_OK) plogf("  erased -> 0xFF");
    diag_real(rc);
    set_finish(rc, NULL);
}

static void op_program(void)
{
    uint32_t sz = prog_api_image_size(s_img);
    uint32_t sectors, off, k;
    int rc = HW_PIN_R_OK;

    if (sz == 0u || sz > PROG_IMAGE_MAX) { set_finish(HW_PIN_R_BADARG, "BADARG"); return; }
    gen_image(s_img, s_image, sz);

    sectors = (sz + HW_PIN_FLASH_SECTOR - 1u) / HW_PIN_FLASH_SECTOR;
    plogf("program %s %uB", g_img_name[s_img], (unsigned)sz);

    /* ① 先擦：写前必擦，否则 1→0 之外的位写不回去 → verify 必挂 */
    for (k = 0; k < sectors && rc == HW_PIN_R_OK; k++) {
        rc = hw_pin_flash_sector_erase(k * HW_PIN_FLASH_SECTOR);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_wait(200u);
        vTaskDelay(1);
    }
    if (rc != HW_PIN_R_OK) {
        plogf("  erase failed");
        diag_real(rc);
        set_finish(rc, NULL);
        return;
    }

    /* ② 逐页编程（页 ≤256B） */
    for (off = 0; off < sz && rc == HW_PIN_R_OK; off += HW_PIN_FLASH_PAGE) {
        uint32_t n = sz - off;
        if (n > HW_PIN_FLASH_PAGE) n = HW_PIN_FLASH_PAGE;
        rc = hw_pin_flash_page_program(off, s_image + off, n);
        if (rc == HW_PIN_R_OK) rc = hw_pin_flash_wait(200u);
        set_pct((int)((off + n) * 80u / sz));
        vTaskDelay(1);
    }
    if (rc != HW_PIN_R_OK) {
        plogf("  pp failed @0x%04X", (unsigned)off);
        diag_real(rc);
        set_finish(rc, NULL);
        return;
    }
    plogf("  wrote %u page(s)", (unsigned)((sz + 255u) / 256u));

    /* ③ 编程后立刻自检（选 PROGRAM 就是期望「写进去的是对的」） */
    set_pct(85);
    rc = hw_pin_flash_verify(0u, s_image, sz);
    if (rc == HW_PIN_R_OK) plogf("  verify OK");
    else                   plogf("  verify -> %s", hw_pin_result_code_str(rc));
    set_finish(rc, NULL);
}

static void op_verify(void)
{
    uint32_t sz = prog_api_image_size(s_img);
    uint32_t i, bad = 0xFFFFFFFFu;
    static uint8_t rd[HW_PIN_FLASH_PAGE];    /* static：真机栈纪律 */
    int rc;

    if (sz == 0u || sz > PROG_IMAGE_MAX) { set_finish(HW_PIN_R_BADARG, "BADARG"); return; }
    gen_image(s_img, s_image, sz);           /* 同一定义 → 与 PROGRAM 写的必须一致 */

    plogf("verify %s %uB", g_img_name[s_img], (unsigned)sz);
    set_pct(20);
    rc = hw_pin_flash_verify(0u, s_image, sz);
    set_pct(100);
    if (rc == HW_PIN_R_OK) {
        plogf("  byte-for-byte OK");
    } else {
        /* 定位首个不符字节，比一句 FAIL 有用得多 */
        for (i = 0; i < sz; i += HW_PIN_FLASH_PAGE) {
            uint32_t n = sz - i, j;
            if (n > HW_PIN_FLASH_PAGE) n = HW_PIN_FLASH_PAGE;
            if (hw_pin_flash_read(i, rd, n) != HW_PIN_R_OK) break;
            for (j = 0; j < n; j++)
                if (rd[j] != s_image[i + j]) { bad = i + j; break; }
            if (bad != 0xFFFFFFFFu) break;
        }
        if (bad != 0xFFFFFFFFu) plogf("  first diff @0x%04X", (unsigned)bad);
        plogf("  verify -> %s", hw_pin_result_code_str(rc));
    }
    diag_real(rc);
    set_finish(rc, NULL);
}

static void op_ispsync(void)
{
    uint8_t ver = 0, rdp = 0;
    uint16_t pid = 0;
    int rc;

    plogf("isp: sync @%s", prog_hw_profile_isp()->name);
    LOCK(); s_st.pid = 0; s_st.ver = 0; UNLOCK();   /* 每次重读：别把上一轮的值留在屏上 */
    rc = hw_pin_isp_reset_sync();            /* 清握手态 → 每次都真发 0x7F */
    if (rc == HW_PIN_R_OK) {
        plogf("  0x7F -> ACK");
        if (hw_pin_isp_get_version(&ver, &rdp) == HW_PIN_R_OK) {
            plogf("  ver=0x%02X", (unsigned)ver);
            LOCK(); s_st.ver = ver; UNLOCK();
        }
        if (hw_pin_isp_get_id(&pid) == HW_PIN_R_OK) {
            plogf("  pid=0x%04X", (unsigned)pid);
            LOCK(); s_st.pid = pid; UNLOCK();
        } else {
            plogf("  pid read fail");
        }
    } else {
        plogf("  no ACK -> %s", hw_pin_result_code_str(rc));
    }
    set_pct(100);
    set_finish(rc, NULL);
}

/* ============================================================
 * worker
 * ============================================================ */
static void run_op(int op)
{
    prog_status_t s;
    set_run(op);
    printf("[prog] ===== op %s start (%s / %s) =====\n",
           g_op_name[op], pick_profile_name(), s_drv ? "REAL" : "SIM");
    switch (op) {
    case PROG_OP_RDID:    op_rdid();    break;
    case PROG_OP_ERASE:   op_erase();   break;
    case PROG_OP_PROGRAM: op_program(); break;
    case PROG_OP_VERIFY:  op_verify();  break;
    case PROG_OP_ISPSYNC: op_ispsync(); break;
    default:              set_finish(HW_PIN_R_BADARG, "BADARG"); break;
    }
    prog_api_status(&s);
    printf("[prog] ===== op %s end rc=%d (%s) =====\n", g_op_name[op], s.rc, s.msg);
}

static void prog_task(void *arg)
{
    (void)arg;
    for (;;) {
        int op = 0;
        if (xQueueReceive(s_jobq, &op, portMAX_DELAY) == pdTRUE) run_op(op);
    }
}

/* ============================================================
 * 对外 API
 * ============================================================ */
#if PROG_SELFTEST
static void selftest_task(void *arg);   /* 定义在本文件末尾（仅自检构建） */
#endif

void prog_api_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_jobq = xQueueCreate(1, sizeof(int));
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = PROG_ST_IDLE;
    s_st.op = -1;
    snprintf(s_st.msg, sizeof(s_st.msg), "READY");
    snprintf(s_st.log[0], PROG_LOG_WIDTH, "backend up");

    hw_pin_init(NULL);                                 /* 清模拟器 + 复位档案 */
    s_tgt = 0; s_drv = 0; s_img = 0;
    (void)prog_hw_install(pick_profile(), false);      /* 默认 SIM */

    xTaskCreate(prog_task, "prog", 4096, NULL, 5, NULL);

    printf("[prog] backend ready; hw_pin mode=%s golden=0x%08X\n",
           hw_pin_mode_str(hw_pin_mode()),
           (unsigned)hw_pin_profile_checksum());
    prog_hw_dump();

#if PROG_SELFTEST
    xTaskCreate(selftest_task, "progtest", 4096, NULL, 4, NULL);
#endif
}

int prog_api_cfg(int tgt, int drv, int img)
{
    prog_status_t s;
    int rc;

    prog_api_status(&s);
    if (s.state == PROG_ST_RUNNING) return -1;         /* 跑着的时候不换档案 */

    s_tgt = tgt; s_drv = drv; s_img = img;
    rc = prog_hw_install(pick_profile(), drv == 1);
    plogf("cfg %s/%s", pick_profile_name(), drv ? "REAL" : "SIM");
    if (rc != HW_PIN_R_OK) plogf("  install -> %s", hw_pin_result_code_str(rc));
    return (rc == HW_PIN_R_OK) ? 0 : -1;
}

int prog_api_start(int op)
{
    prog_status_t s;
    if (op < 0 || op >= PROG_OP_MAX) return -1;
    prog_api_status(&s);
    if (s.state == PROG_ST_RUNNING) return -1;         /* 不排队：直接拒 */
    if (xQueueSend(s_jobq, &op, 0) != pdTRUE) return -1;
    return 0;
}

void prog_api_status(prog_status_t *out)
{
    if (!out) return;
    LOCK();
    *out = s_st;
    UNLOCK();
}

/* ============================================================
 * 启动自检（仅 -DPROG_SELFTEST=1；产品构建不含）
 *
 * 为什么必须有它：
 *   UI 上的操作要靠**物理按键**触发，没法脚本化 —— 于是「后端到底通不通」
 *   就退化成人肉点击 + 肉眼看结论，不可复现、也不可回归。
 *   这里把「UI 走的那条路」（cfg → start → 轮询 status）在启动时自动跑一遍，
 *   把结论打成机器可读的 PASS/FAIL，串口一抓即可断言。
 *
 * 覆盖点（SIM 真跑 + REAL 诚实）：
 *   S1 hw_pin 器件模型自检（12 项，SIM）
 *   S2 SIM READ ID      → 必须 EF 40 18
 *   S3 SIM ERASE        → OK
 *   S4 SIM PROGRAM      → OK（内含回读校验）
 *   S5 SIM VERIFY       → OK（逐字节）
 *   S6 SIM ISP SYNC     → OK
 *   S7 REAL READ ID     → **必须失败**（没接芯片 → 诚实 NOFLASH，绝不假装成功）
 *   S8 收尾恢复默认 SIM 档
 * ============================================================ */
#if PROG_SELFTEST
static int s_sf_fail;

static void sf(int ok, const char *fmt, ...)
{
    char b[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);

    if (!ok) s_sf_fail++;
    printf("[selftest] %s  %s\n", ok ? "PASS" : "FAIL", b);
}

/* 等一次操作跑完；返回终态（超时也算失败样本，交给断言判） */
static int sf_wait(void)
{
    prog_status_t s;
    int ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        prog_api_status(&s);
        ms += 20;
        if (s.state != PROG_ST_RUNNING) return s.state;
        if (ms >= 8000) return -1;                  /* 卡死 */
    }
}

/* 跑一次操作并返回终态 */
static int sf_run(int op)
{
    prog_status_t s;
    if (prog_api_start(op) != 0) return -2;         /* 没受理 */
    if (sf_wait() < 0) return -1;
    prog_api_status(&s);
    return s.state;
}

static void selftest_task(void *arg)
{
    prog_status_t s;
    int st;

    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));                 /* 让 boot 日志先出干净 */

    printf("\n[selftest] ===== PROG 后端自检开始 =====\n");

    /* S1 器件模型（hw_pin 自带 12 项） */
    {
        int f = hw_pin_selftest(NULL);
        sf(f == 0, "S1 hw_pin device-model selftest (%d fails)", f);
    }

    /* S2 SIM READ ID：s3-spi 档案 + SIM */
    if (prog_api_cfg(0, 0, 0) != 0) sf(0, "S2 pre-cfg s3-spi/SIM failed");
    st = sf_run(PROG_OP_RDID);
    prog_api_status(&s);
    sf(st == PROG_ST_DONE && (s.jedec & 0x00FFFFFFu) == (HW_PIN_FLASH_JEDEC & 0x00FFFFFFu),
       "S2 SIM READ ID jedec=%06X (want EF4018) st=%d",
       (unsigned)(s.jedec & 0x00FFFFFFu), st);

    /* S3 SIM ERASE */
    st = sf_run(PROG_OP_ERASE);
    sf(st == PROG_ST_DONE, "S3 SIM ERASE st=%d", st);

    /* S4 SIM PROGRAM（内置回读校验） */
    st = sf_run(PROG_OP_PROGRAM);
    sf(st == PROG_ST_DONE, "S4 SIM PROGRAM st=%d", st);

    /* S5 SIM VERIFY（逐字节） */
    st = sf_run(PROG_OP_VERIFY);
    sf(st == PROG_ST_DONE, "S5 SIM VERIFY st=%d", st);

    /* S6 SIM ISP SYNC（切到 s3-isp 档案） */
    if (prog_api_cfg(2, 0, 0) != 0) sf(0, "S6 pre-cfg s3-isp/SIM failed");
    st = sf_run(PROG_OP_ISPSYNC);
    sf(st == PROG_ST_DONE, "S6 SIM ISP SYNC st=%d", st);

    /* S7 REAL READ ID：没接芯片 → 必须失败（诚实性铁证） */
    if (prog_api_cfg(0, 1, 0) != 0) sf(0, "S7 pre-cfg s3-spi/REAL failed");
    st = sf_run(PROG_OP_RDID);
    sf(st == PROG_ST_FAIL, "S7 REAL READ ID honestly FAILED (st=%d, no chip wired)", st);

    /* S8 收尾：恢复默认 SIM，别把 REAL 留给用户 */
    (void)prog_api_cfg(0, 0, 0);

    printf("[selftest] ===== PROG 自检结束: %s (%d fails) =====\n\n",
           s_sf_fail == 0 ? "ALL PASS" : "FAIL", s_sf_fail);
    vTaskDelete(NULL);
}
#endif /* PROG_SELFTEST */
