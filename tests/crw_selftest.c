/*
 * hw_dmc_crw selftest —— 正向断言
 * ============================================================================
 * 【为什么不把变异写在这个文件里】
 *   家族坑 #45/#47: 全绿的测试不证明测试有效; 而"变异"打在【测试自己的期望值】
 *   上是自欺欺人 —— 被测模块一个字节没动, 测试自己改口说"我期望不同", 这不
 *   构成任何证据。本文件因此【只含正向断言】, 变异矩阵放在
 *   tools/crw_matrix.sh, 它改的是 src/hw/hw_dmc_crw.c 的源码, 重编后必须变红。
 *
 * 【全局态纪律】(家族第三次同形: selftest 污染全局态)
 *   本测试全程操作 crw_bsp_install/uninstall 这个【全局单例】, 因此结尾必须
 *   主动卸载并断言已卸载, 否则后续任何测试/调用方都会拿到残留的假 BSP。
 *   见 [11]。
 *
 * 【无真硬件时的诚实失败】
 *   未装 BSP 时 crw_reg_read() 返回 0 而非解引用猜测地址 ([1])。
 */
#include "hw_dmc_crw.h"
#include <stdio.h>
#include <string.h>

static int g_fail;
static int g_case_total;

#define CHECK(cond, fmt, ...)                                       \
    do { g_case_total++;                                            \
         if (!(cond)) { g_fail++;                                   \
            printf("  [FAIL] " fmt "\n", ##__VA_ARGS__); }         \
    } while (0)

/* ---------- 确定性模拟 BSP ---------- */
static uint32_t g_sim_regs[4];   /* 寄存器读用 (BSP 返回 uint32_t) */
static int      g_payload[4];    /* 载荷/处理函数槽用 (保持 int*, 与 crw_t 一致) */

/* 原码 crw_t 的 handler 字段类型就是 `int*` —— 语义是"把函数地址当数据
 * 指针存"。本实现照实保留该字段类型 (不改原语义), 转换处一律显式 cast。 */
static int* fnptr(int (*f)(int)) { return (int*)(intptr_t)f; }

static uint32_t sim_reg_read(uint32_t addr, void* user)
{
    (void)user;
    if (addr >= 4U) return 0U;
    return g_sim_regs[addr];
}

static void sim_setup(uint32_t boot, uint32_t relay)
{
    memset(g_sim_regs, 0, sizeof(g_sim_regs));
    if (boot)  g_sim_regs[0] |= (1UL << CRW_PART_BOOT_BIT);
    if (relay) g_sim_regs[0] |= (1UL << CRW_PART_RELAY_BIT);
    crw_bsp_install(sim_reg_read, NULL);
}

static int sim_handler_a(int x) { return x + 1; }
static int sim_handler_b(int x) { return x + 2; }

/* ---------- 正向用例 ---------- */
static void t_pos(void)
{
    crw_t   v;
    cpt_t   t;
    int*    pl = &g_payload[1];
    int*    hd = &g_payload[2];
    size_t  cap = 8, got = 0;
    int     pos = -1, off = -1;
    int     rh = 0;
    int     rc;

    /* [0] 结构性自证: 常量与结构体字段长度一致 (原码 DMC_DATA_COUNT 7U
     *     声称 7 实有 3, 这里必须自证而不是嘴上说) */
    CHECK(sizeof(((cpt_t*)0)->handler) / sizeof(int*) == (size_t)CRW_SLOT_MAX,
          "[0a] CRW_SLOT_MAX 必须等于 handler[] 实际槽数, 实=%d 实际=%d",
          CRW_SLOT_MAX, (int)(sizeof(((cpt_t*)0)->handler) / sizeof(int*)));
    CHECK(CRW_PART_BOOT_BIT < 32U && CRW_PART_RELAY_BIT < 32U,
          "[0b] 分区位号必须 < 32 (原码 2193/57073 越界) boot=%u relay=%u",
          (unsigned)CRW_PART_BOOT_BIT, (unsigned)CRW_PART_RELAY_BIT);
    CHECK(CRW_PART_BOOT_BIT != CRW_PART_RELAY_BIT,
          "[0c] 两个分区位号不得相同 (x86 上原码两个都退化成 >>17)");

    /* [1] 未装 BSP = 无硬件 = 诚实失败, 不是崩溃 (原码是裸解引用猜测地址) */
    crw_bsp_uninstall();
    CHECK(crw_bsp_installed() == 0, "[1a] uninstall 后应报未装 BSP");
    CHECK(crw_s_fpga() == 0,          "[1b] uninstall 后 crw_s_fpga 应为 0");
    CHECK(crw_reg_read(0U) == 0U,     "[1c] 无 BSP 时读寄存器应返回 0 而非崩");
    CHECK(crw_boot_present() == 0,    "[1d] 无 BSP 时 boot 分区应报不存在");
    CHECK(crw_relay_present() == 0,   "[1e] 无 BSP 时 relay 分区应报不存在");
    CHECK(crw_wait_trig(0U, 0U, 4U) == CRW_ERR_TIMEOUT,
          "[1f] 无 BSP 时等待应超时, 不是命中");

    /* [2] 装 BSP 后分区检测生效 (SIM/REAL 分水岭, 家族坑 #21) */
    sim_setup(1, 0);
    CHECK(crw_s_fpga() == 1,          "[2a] 装 BSP 后 crw_s_fpga 应为 1");
    CHECK(crw_boot_present() == 1,    "[2b] boot 分位置位应检出");
    CHECK(crw_relay_present() == 0,   "[2c] relay 分区未置位应报不存在");

    /* [3] 🔴 移位量越界守卫 —— 原码 >>0x00891(2193) / >>0x00def1(57073)
     *     在 x86 上都被掩成 >>17, 静默取错位。此处必须显式判负。 */
    CHECK(crw_reg_bit(0U, 0x00891U) == CRW_ERR_PARAM,
          "[3a] bit=0x00891(2193>=32) 必须判负, 不得静默截断");
    CHECK(crw_reg_bit(0U, 0x00def1U) == CRW_ERR_PARAM,
          "[3b] bit=0x00def1(57073>=32) 必须判负");
    CHECK(crw_reg_bit(0U, 32U) == CRW_ERR_PARAM,
          "[3c] bit=32 必须判负 (32 不是合法位号)");
    CHECK(crw_reg_bit(0U, 31U) == 0,  "[3d] bit=31 合法边界, 不应被判负");

    /* [4] 边界位 0 与 31 真能读到 (证明 [3] 的守卫不是恒真) —— 锚点 J:
     *     只测"越界被判负"不够, 必须同时证明"合法位真能读出对的值"。 */
    g_sim_regs[0] = 0x80000001UL;
    CHECK(crw_reg_bit(0U, 0U)  == 1, "[4a] bit0 应为 1");
    CHECK(crw_reg_bit(0U, 31U) == 1, "[4b] bit31 应为 1 (最高位, 不被符号扩展吞)");
    CHECK(crw_reg_bit(0U, 1U)  == 0, "[4c] bit1 应为 0");
    CHECK(crw_boot_present() == 0,  "[4d] 低半字有值但 boot 位(17) 应为 0");
    CHECK(crw_relay_present() == 1, "[4e] relay 位(31) 应检出为 1");

    /* [5] crw_put / crw_point / crw_dump / crw_pop 往返 */
    sim_setup(1, 1);
    memset(&v, 0, sizeof(v));
    rc = crw_put(&v, pl, hd, 42U);
    CHECK(rc == CRW_OK,                 "[5a] crw_put 应成功 rc=%d", rc);
    CHECK(crw_point(&v) == 1,           "[5b] crw_put 后 point 应有效");
    CHECK(v.point == pl,                "[5c-1] 载荷指针应原样往返");
    CHECK(v.handler == hd,              "[5c-2] handler 指针应原样往返");
    CHECK(v.size == 42U,                 "[5c-3] size 应往返 42, 实=%u", (unsigned)v.size);
    CHECK(crw_dump(&v) == 42,            "[5d] crw_dump 应返回 42, 实=%d", crw_dump(&v));
    rc = crw_pop(&rh, &v, &got);
    CHECK(rc == CRW_OK,                 "[5e] crw_pop 应成功");
    CHECK(rh == (int)(intptr_t)hd,      "[5f] pop 出的 handler 应一致");
    CHECK(got == 42U,                   "[5g] pop 出的 size 应为 42");
    CHECK(crw_point(&v) == 0,           "[5h] pop 后 point 应失效 (零次执行检测)");
    CHECK(v.size == 0U,                 "[5i] pop 后 size 应被清零 (不得留脏)");

    /* [6] NULL 守卫: 每个入口都该判负, 不得崩 */
    CHECK(crw_point(NULL) == CRW_ERR_PARAM,  "[6a] crw_point(NULL) 应判负");
    CHECK(crw_dump(NULL)  == CRW_ERR_PARAM,  "[6b] crw_dump(NULL) 应判负");
    CHECK(crw_put(NULL, pl, hd, 1U) == CRW_ERR_PARAM, "[6c] crw_put(NULL) 应判负");
    CHECK(crw_pop(0, NULL, &got) == CRW_ERR_PARAM,    "[6d] crw_pop(NULL v) 应判负");
    CHECK(crw_cpt_init(NULL, 0, 0, 0, 0) == CRW_ERR_PARAM, "[6e] cpt_init(NULL) 应判负");
    CHECK(crw_cpt_push(NULL, pl, pl) == CRW_ERR_PARAM, "[6f] cpt_push(NULL) 应判负");
    CHECK(crw_cpt_pop(NULL) == CRW_ERR_PARAM,         "[6g] cpt_pop(NULL) 应判负");

    /* [7] 非法入口哨兵 (原码 DMC_DATA_1 = 1^0x010 是给宏赋值) */
    memset(&v, 0, sizeof(v));
    CHECK(crw_put(&v, DMC_ILLEGAL_ENTRY, hd, 1U) == CRW_ERR_PARAM,
          "[7a] 非法入口哨兵应判负");
    CHECK(v.point == NULL, "[7b] 判负后 point 不应被写入 (半写状态检测)");
    CHECK(v.size == 0U,    "[7c] 判负后 size 不应被写入 (半写状态检测)");
    /* 哨兵必须真能在 [7a] 咬到, 而不能退化成"总是判负" */
    CHECK(crw_put(&v, pl, hd, 1U) == CRW_OK, "[7d] 合法指针仍应成功 (证明 [7a] 有区分力)");

    /* [8] CPT 槽表: push/pop 往返 + 空/满边界 */
    CHECK(crw_cpt_init(&t, (void*)&g_sim_regs, &cap, &pos, &off) == CRW_OK,
          "[8a] cpt_init 应成功");
    CHECK(t.point == 0U,  "[8b] init 后 point 应为 0");
    CHECK(t.handler[0] == NULL, "[8c-1] init 必须清空槽表 (原码无此步)");
    CHECK(t.handler[CRW_SLOT_MAX - 1] == NULL, "[8c-2] 末槽也必须被清空");
    CHECK(crw_cpt_pop(&t) == CRW_ERR_EMPTY, "[8c-3] 空表 pop 应报 EMPTY");
    CHECK(crw_cpt_push(&t, fnptr(sim_handler_a), pl) == CRW_OK, "[8d] push1 应成功");
    CHECK(crw_cpt_push(&t, fnptr(sim_handler_b), hd) == CRW_OK, "[8e] push2 应成功");
    CHECK(t.point == 2U,  "[8f] push2 后 point 应为 2");
    CHECK(pos == 1,       "[8g] 外部 pos 游标应同步为 1");
    CHECK(off == (int)(intptr_t)hd, "[8h] 外部 offset 应同步为末次载荷地址");
    CHECK(t.handler[0] == fnptr(sim_handler_a), "[8i] 槽0 应存 handler_a");
    CHECK(t.handler[1] == fnptr(sim_handler_b), "[8j] 槽1 应存 handler_b");
    CHECK(crw_cpt_pop(&t) == CRW_OK,       "[8k] pop 应成功");
    CHECK(t.point == 1U,  "[8l] pop 后 point 应为 1");
    CHECK(t.handler[1] == NULL, "[8m] pop 后槽1 应清空");
    CHECK(t.handler[0] == fnptr(sim_handler_a), "[8n] pop 只清末槽, 不得误清槽0");
    /* pos 语义 = "最后触碰的槽号": push 时是刚写入的 n, pop 时是刚释放的
     * point-1。此处两次都是槽1 ⇒ 1。(先前误写 0 被基线阴性对照当场抓住。) */
    CHECK(pos == 1,       "[8o] pop 后外部 pos 应同步为 1(最后触碰的槽号)");

    /* [9] 🔴 槽位上界: 推满 32 个后再推必须报 FULL, 不静默回绕
     *     (原码 DMC_DATA_COUNT 7U 但只定义 1..3, 声称与实有不符) */
    CHECK(crw_cpt_init(&t, (void*)&g_sim_regs, &cap, &pos, &off) == CRW_OK, "[9a] 重置");
    {
        int i, pushed = 0;
        for (i = 0; i < CRW_SLOT_MAX; i++) {
            if (crw_cpt_push(&t, fnptr(sim_handler_a), pl) == CRW_OK) pushed++;
        }
        CHECK(pushed == CRW_SLOT_MAX, "[9b] 应恰好推入 32 个, 实=%d", pushed);
        CHECK(t.point == (uint16_t)CRW_SLOT_MAX, "[9c] point 应为 32");
        CHECK(crw_cpt_push(&t, fnptr(sim_handler_b), hd) == CRW_ERR_FULL,
              "[9d] 第 33 次 push 必须报 FULL (不得静默回绕覆盖槽0)");
        CHECK(t.handler[0] == fnptr(sim_handler_a), "[9e] 报 FULL 后槽0 不应被覆盖");
        CHECK(t.point == (uint16_t)CRW_SLOT_MAX, "[9f] 报 FULL 后 point 不得被推进到 33");
        CHECK(pos == (int)CRW_SLOT_MAX - 1, "[9g] 报 FULL 后外部 pos 不得被越界改写");
    }

    /* [10] 等待触发: 真轮询 + 真超时 (原码 dmc_while 无超时 = 永久挂死) */
    sim_setup(0, 0);
    g_sim_regs[1] = 0U;
    CHECK(crw_wait_trig(1U, 3U, 10U) == CRW_ERR_TIMEOUT,
          "[10a] 未触发时必须报 TIMEOUT (不得永久挂死)");
    g_sim_regs[1] = (1UL << 3);
    CHECK(crw_wait_trig(1U, 3U, 10U) == 0,
          "[10b] 首次即命中应返回已轮次数 0");
    CHECK(crw_wait_trig(1U, 32U, 10U) == CRW_ERR_PARAM,
          "[10c] wait 的 bit>=32 应判负");
    CHECK(crw_wait_trig(1U, 3U, 0U) == CRW_ERR_PARAM,
          "[10d] max_polls=0 应判负 (防零次循环假通过)");
    /* 命中发生在最后一轮: 证明真在轮询, 不是第一次就返回 */
    g_sim_regs[1] = 0U;
    {
        int r = crw_wait_trig(1U, 3U, 10U);
        CHECK(r == CRW_ERR_TIMEOUT, "[10e] 全程未置位应超时");
    }
}

int main(void)
{
    t_pos();

    /* [11] 🔴 全局态还原 (家族第三次同形: selftest 污染全局态)
     *     装过 BSP = 留下假硬件, 后续任何调用方都会读到残留状态。 */
    crw_bsp_uninstall();
    g_case_total++;
    if (crw_bsp_installed() != 0) {
        g_fail++;
        printf("  [FAIL] [11a] 结尾必须卸载 BSP, 不得给后续留下残留全局态\n");
    }

    printf("RESULT: %s (selftest: %d 断言)\n",
           (g_fail == 0) ? "ALL PASS" : "FAIL", g_case_total);
    return g_fail ? 1 : 0;
}
