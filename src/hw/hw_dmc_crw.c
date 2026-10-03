/*
 * xiaomo - CRW/CPT 分区表层 (hw_dmc_crw) —— 实现
 *
 * 语义来源: src/hw/hw_dmc_base.c 归档区段 base_pseudo.c:15-52 (伪代码底稿)。
 * 逐条缺陷与修法见 include/hw_dmc_crw.h 顶部对照表。归档本体一个字未动。
 *
 * 依赖纪律 (家族规范):
 *   - 只用标准函数, 无 malloc / 无 pthread / 无文件 IO
 *   - 硬件差异一律走 BSP 回调注入; 默认 = 表驱动模拟, 全平台行为一致可回归
 *   - 大局部数组一律 static (真机栈纪律, 家族坑 #12)
 *   - 局部变量名避开 C++ 保留字 (家族坑 #19)
 */
#include "hw_dmc_crw.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================= BSP ================= */

static crw_reg_read_fn g_reg_read;
static void*           g_reg_user;
static int             g_s_fpga;      /* 装过 BSP = 真硬件 */

void crw_bsp_install(crw_reg_read_fn fn, void* user)
{
    g_reg_read = fn;
    g_reg_user = user;
    g_s_fpga   = (fn != NULL) ? 1 : 0;
}

void crw_bsp_uninstall(void)
{
    g_reg_read = NULL;
    g_reg_user = NULL;
    g_s_fpga   = 0;
}

int  crw_bsp_installed(void) { return (g_reg_read != NULL) ? 1 : 0; }
int  crw_s_fpga(void)        { return g_s_fpga; }

/* ================= 状态寄存器原语 =================
 * 原码: #define crw(x) ((volatile uint32_t)(*(volatile uint32_t *)(x)))
 *       然后 crw(**std**) —— **std** 是 markdown 粗体残留, 不是 C 标识符。
 * 修法: 读 (addr << 2) 处的字; 无 BSP 时返回 0 而不是解引用猜测地址。 */

uint32_t crw_reg_read(uint32_t addr)
{
    if (g_reg_read == NULL) return 0U;      /* 无硬件: 诚实 0, 不碰野地址 */
    return g_reg_read(addr, g_reg_user);
}

/* 位号必须 < 32。原码 >>0x00891(=2193) / >>0x00def1(=57073) 均越界,
 * x86 把计数掩成 &31 → 两个都变成 >>17, 不报错但静默取错位。 */
int crw_reg_bit(uint32_t addr, uint32_t bit)
{
    if (bit >= 32U) return CRW_ERR_PARAM;   /* 越界直接判负, 不静默截断 */
    return (int)((crw_reg_read(addr) >> bit) & 1U);
}

int crw_boot_present(void)  { return crw_reg_bit(0U, CRW_PART_BOOT_BIT); }
int crw_relay_present(void) { return crw_reg_bit(0U, CRW_PART_RELAY_BIT); }

/* ================= crw_t 存取 =================
 * 原码四个函数全部有硬伤: crw_pop/crw_put 声明 void 却要参与 | 聚合;
 * crw_point 恒被调用却从未声明; 调用点实参个数与声明对不上。 */

int crw_point(const crw_t* v)
{
    if (v == NULL) return CRW_ERR_PARAM;
    return (v->point != NULL) ? 1 : 0;
}

int crw_dump(const crw_t* v)
{
    if (v == NULL) return CRW_ERR_PARAM;
    return (int)(v->size & 0x7FFFFFFFU);   /* 掩高位: 同族口径 (hw_main 坑 #48) */
}

int crw_put(crw_t* v, int* point, int* handler, size_t size)
{
    if (v == NULL) return CRW_ERR_PARAM;
    if (point == DMC_ILLEGAL_ENTRY) return CRW_ERR_PARAM; /* 真比较, 原码写成 = */
    v->point   = point;
    v->handler = handler;
    v->size    = size;
    return CRW_OK;
}

int crw_pop(int* handler, crw_t* point, size_t* size)
{
    if (point == NULL) return CRW_ERR_PARAM;
    if (handler != NULL) *handler = (int)(intptr_t)point->handler;
    if (size != NULL)     *size    = point->size;
    point->point   = NULL;
    point->handler = NULL;
    point->size    = 0U;
    return CRW_OK;
}

/* ================= CPT 槽表 ================= */

int crw_cpt_init(cpt_t* t, void* std, size_t* size, int* pos, int* offset)
{
    if (t == NULL) return CRW_ERR_PARAM;
    memset(t->handler, 0, sizeof(t->handler));   /* handler 槽表必须清空 */
    t->std    = std;
    t->size   = size;
    t->point  = 0U;
    t->pos    = pos;
    t->offset = offset;
    t->ent    = NULL;
    return CRW_OK;
}

int crw_cpt_push(cpt_t* t, int* handler, int* point)
{
    uint16_t n;
    if (t == NULL) return CRW_ERR_PARAM;
    n = t->point;
    if (n >= (uint16_t)CRW_SLOT_MAX) return CRW_ERR_FULL;   /* 上界, 不静默回绕 */
    t->handler[n] = handler;
    if (t->pos != NULL)   *t->pos   = (int)n;
    if (t->offset != NULL)*t->offset = (int)(intptr_t)point;
    t->point = (uint16_t)(n + 1U);
    return CRW_OK;
}

int crw_cpt_pop(cpt_t* t)
{
    if (t == NULL) return CRW_ERR_PARAM;
    if (t->point == 0U) return CRW_ERR_EMPTY;
    t->point = (uint16_t)(t->point - 1U);
    t->handler[t->point] = NULL;
    if (t->pos != NULL) *t->pos = (int)t->point;
    return CRW_OK;
}

/* 原码 dmc_while(o,x) 声明 2 参只用 o (x 被静默丢弃), 展开成值表达式而非
 * 循环, 名字叫 while 却没有循环语义。此处给真轮询 + 真超时 —— 原码的
 * while(1) 无超时在真机上等于永久挂死。 */
int crw_wait_trig(uint32_t addr, uint32_t bit, uint32_t max_polls)
{
    uint32_t i;
    if (bit >= 32U) return CRW_ERR_PARAM;
    if (max_polls == 0U) return CRW_ERR_PARAM;
    for (i = 0U; i < max_polls; i++) {
        if (crw_reg_bit(addr, bit) == 1) return (int)i;   /* 命中: 已轮次 */
    }
    return CRW_ERR_TIMEOUT;
}

#ifdef __cplusplus
}
#endif
