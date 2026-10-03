/*
 * xiaomo - CRW/CPT 分区表层 (hw_dmc_crw)
 * ============================================================================
 * 来源: src/hw/hw_dmc_base.c 里归档的伪代码底稿 (base_pseudo.c) 第 15-52 行。
 *
 * 【为什么不原地修那份归档】
 *   归档区的存在目的就是"证明我们修的是什么" —— tools/dmc_verbatim_check.py
 *   逐字节校验三个区段与原件一字未改。原地改 = 校验器报 diff + 历史记录丢失,
 *   而运行时行为一点都不变 (它被 #if defined(DMC_BASE_PSEUDOCODE) 永久关掉,
 *   全工程零定义点, 实编译符号表为空)。净损失。
 *   ⇒ 本模块是归档【之外】的重写实现, 归档一个字不动。
 *
 * 【伪代码的缺陷 → 本实现的修法, 逐条对应】
 *
 *  原伪代码                                          本实现
 *  ---------------------------------------------------------------------------
 *  #ifdef **std**                                    crw_reg_read() 走 BSP 注入
 *  crw(**std**)                                      标识符基址可传, 零裸解引用
 *
 *  crw(STD) >> 0x00891   (Boot partition)           改 crw_reg_bit():
 *  crw(STD) >> 0x00def1  (Relay partition)             (reg >> BIT) & 1, 且 BIT
 *                                                      强制 < 32 且做参数校验
 *  移位量 2193 / 57073 均 >= 32 = C 未定义行为            —— 原码两个移位量在 x86
 *  x86 把计数掩成 &31 → 都变成 17 → 不报错静默出错       上【都变成 17】, 是本模块
 *                                                      必须绕开的最大暗坑
 *
 *  cpt_t 内 int* handler[32] 与 crw_t* handler       重命名为 handler[] / ent
 *  同名 → C 硬错 (结构体成员重名)                      (原码根本编不过)
 *
 *  DMC_OP_MASK = crw_pop(a,data) | crw_put(b,data)    三个函数都返回 int 状态码
 *  | crw_point(c,data)   crw_pop/crw_put 声明 void     ⇒ 可以 | 聚合
 *                        (void 不能参与 |) 硬错         且 crw_point 原本【无声明】
 *
 *  实参 2 个 vs 声明 3 个 (crw_pop/crw_put)            签名与调用一律对齐
 *
 *  DMC_DATA_1 = 1^0x010;   (给宏赋值 → 非法左值)       DMC_ILLEGAL_ENTRY 改真哨兵
 *  (且 ^ 是异或不是比较, 注释自称 Illegal entry 却用 =)
 *
 *  #define HW_DMC_START 0x01U 紧跟 #ifndef HW_DMC_START  宏不再自我拆台:
 *  → 恒假 ⇒ HW_DMC_TMP/DUMP/... 全得不到定义            HW_DMC_* 是唯一真相源,
 *  → 而 DMC_DATA_1 引用它们 → 展开成 (|) → 语法错        顺序不再自相矛盾
 *
 *  #define DMC_DATA_COUNT 7U 但只定义 DMC_DATA_1..3     DMC_SLOT_MAX=32 与
 *  → 声称 7 实有 3                                      handler[32] 字段一致, 自证
 *
 *  dmc_while(o,x) 丢 x 且展开成值表达式而非循环          crw_wait_trig(): 真轮询 +
 *  → 名字叫 while 却没有循环语义                          真超时 (原码 while(1) 无
 *                                                      超时 = 真机永久挂死, 见
 *                                                      知识页 hw_dmc 坑)
 *
 *  文件自带 3 个未闭合 #if                              本文件是全新文件, 结构自洽;
 *                                                      归档那边的 BALANCE-PAD 仍
 *                                                      承重, 不动
 *
 * 【无真硬件时的诚实失败】
 *   寄存器读一律经 BSP。未装 BSP = 返回 0 并置 crw_s_fpga=0 ⇒ 分区检测报
 *   "不存在", 而不是读一个猜测地址 (原码 crw(0x40000000) 在宿主上 = SIGSEGV)。
 */

#ifndef XIAOMO_HW_DMC_CRW_H
#define XIAOMO_HW_DMC_CRW_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 分区标志位号 (原码 0x00891 / 0x00def1 是移位量, 越界; 此处是位号) ----
 * 纪律: < 32, 且 crw_reg_bit() 对 >= 32 的入参直接判负, 不静默截断。 */
#define CRW_PART_BOOT_BIT   0x11U   /* 0x11 = 17 */
#define CRW_PART_RELAY_BIT  0x1FU   /* 0x1F = 31 */

/* ---- 原码 DMC_DATA_1 = 非法入口标记, 此处定版为真哨兵 ----
 * 原码写 `DMC_DATA_1 = 1^0x010;` 有三层错: ①给宏赋值 = 非法左值
 * ②^ 是异或不是比较, 注释自称 "Illegal entry" 却用 =
 * ③它标的是【载荷地址】⇒ 必须是【指针】哨兵。写 0x010 会在 C++17 下直接
 *   编译失败 (指针与整数比较), 这条是本模块真编译时抓到的。 */
#define DMC_ILLEGAL_ENTRY   ((int*)-1)

/* 槽位数: 与 cpt_t.handler[] 字段长度严格一致 (自证项之一) */
#define CRW_SLOT_MAX        32

/* 返回码 (家族约定: 0 = 成功/就绪) */
#define CRW_OK              0
#define CRW_ERR_PARAM      (-1)
#define CRW_ERR_NOBSP      (-2)   /* 未装 BSP = 无硬件 = 诚实失败, 非崩溃 */
#define CRW_ERR_FULL       (-3)   /* 槽位满 */
#define CRW_ERR_EMPTY      (-4)   /* 槽位空 */
#define CRW_ERR_TIMEOUT    (-5)   /* 等待触发超时 (原码是永久挂死) */
#define CRW_ERR_NOPART     (-6)   /* 分区不存在 */

/* ---- crw_t: 单个控制字 = 数据指针 + 处理函数 + 长度 ----
 * 原码: typedef struct{ int* point; int* handler; size_t size; } crw_t; */
typedef struct {
    int*   point;      /* 数据载荷 */
    int*   handler;    /* 处理函数 */
    size_t size;       /* 载荷字节数 */
} crw_t;

/* ---- cpt_t: 控制分区表 ----
 * 原码的 handler[32] 与 crw_t* handler 同名 = C 硬错, 此处 ent 为归档视图。 */
typedef struct {
    void*   std;               /* 标识/基址 */
    size_t* size;              /* 容量 */
    uint16_t point;            /* 当前位置 */
    int*    handler[CRW_SLOT_MAX]; /* 处理函数槽表 */
    int*    pos;               /* 游标 */
    int*    offset;            /* 偏移 */
    crw_t*  ent;               /* 【原码此处叫 handler, 与上面成员重名】 */
} cpt_t;

/* ---- BSP: 寄存器读注入 ----
 * 原码 crw(x) 是裸 volatile 解引用一个由 markdown 粗体 (**std**) 残留下来的
 * 标识符, 在任何真实平台上都编不过。改为回调注入:
 *   NULL = 无硬件 → 返回 0 + 标志位全 0 ⇒ 分区检测诚实报"不存在"。 */
typedef uint32_t (*crw_reg_read_fn)(uint32_t addr, void* user);
void        crw_bsp_install(crw_reg_read_fn fn, void* user);
void        crw_bsp_uninstall(void);
int         crw_bsp_installed(void);
/* 是否为真硬件 (装过 BSP), 供 selftest 做 SIM/REAL 分水岭 */
int         crw_s_fpga(void);

/* ---- 状态寄存器原语 ---- */
/* 原码 crw(x): 裸解引用。此处读 (addr << 2) 处的字。 */
uint32_t    crw_reg_read(uint32_t addr);
/* 位测试。bit 必须 < 32 —— 原码的 >>0x00891 在 x86 上被掩成 >>17, 静默错。 */
int         crw_reg_bit(uint32_t addr, uint32_t bit);
/* 分区检测: 原码的 crw(STD)>>0x00891 / cpt(STD)>>0x00def1 */
int         crw_boot_present(void);
int         crw_relay_present(void);

/* ---- crw_t 存取 (原码 crw_dump / crw_put / crw_pop / crw_point) ----
 * 原码这四个: 两个声明 void (不能参与 | 聚合)、一个 crw_point 根本没声明、
 * 调用点与声明的实参个数也对不上。此处一律 int 返回码, 签名自洽。 */
int         crw_dump(const crw_t* v);
/* 写: 原码 crw_put(crw_t* value, crw_t* point, int* handler) 声明 3 参却 2 参调用 */
int         crw_put(crw_t* v, int* point, int* handler, size_t size);
/* 读: 原码 crw_pop(int* handler, crw_t* point, crw_t* size) 参数顺序与语义都错乱 */
int         crw_pop(int* handler, crw_t* point, size_t* size);
/* 原码 crw_point 恒被调用却从未声明 (隐式声明) —— 此处给真定义 */
int         crw_point(const crw_t* v);

/* ---- CPT 槽表 ---- */
int         crw_cpt_init(cpt_t* t, void* std, size_t* size, int* pos, int* offset);
int         crw_cpt_push(cpt_t* t, int* handler, int* point);
int         crw_cpt_pop(cpt_t* t);
/* 等待触发区: 原码 dmc_while(o,x) 丢参数 + 无超时 ⇒ 真轮询 + 真超时 */
int         crw_wait_trig(uint32_t addr, uint32_t bit, uint32_t max_polls);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_HW_DMC_CRW_H */
