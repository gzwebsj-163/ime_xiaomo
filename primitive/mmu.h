/**
 * mmu.h — 内存保护单元 (第二阶段: 内存保护与特权级)
 *
 * 这一层把「能跑程序的计算机」升级为「有保护机制的计算机」。
 * 核心思想来自真实 CPU (x86 的保护模式 / ARM 的 PLD)：
 *
 *   1. 特权级 (Ring) — CPU 当前的运行级别:
 *        Ring 0 = 内核 (最高特权, 可访问一切)
 *        Ring 3 = 用户 (最低特权, 受限访问)
 *      数值越小特权越高。cpl 字段保存当前特权级。
 *
 *   2. 段描述符表 (SDT) — 每个段描述一段内存的权限:
 *        base   起始地址
 *        limit  长度 (界限)
 *        perm   读/写/执行 权限
 *        dpl    描述符特权级 (谁的级别以上才能访问它)
 *
 *   3. 访问检查 — 每次内存访问都经过 MMU 仲裁:
 *        cpl(数值) 必须 <= 段的 dpl(数值)  → 否则拒绝 (特权不够)
 *        地址必须在 [base, base+limit] 内   → 否则拒绝 (越界)
 *        操作必须有对应 权限位              → 否则拒绝 (权限不足)
 *
 *  联想真实世界: 用户程序是 "普通市民" (Ring 3)，
 *  内核是 "政府大楼" (Ring 0)。市民不能随便进大楼的机要室，
 *  但可以经过指定的 "前台" (SYSCALL 门) 提交办事申请。
 * ================================================================ */

#ifndef MMU_H
#define MMU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 特权级 */
#define RING_KERNEL  0   /* 内核 */
#define RING_USER    3   /* 用户 */

/* 段权限位 */
#define SEG_READ     (1 << 0)
#define SEG_WRITE    (1 << 1)
#define SEG_EXEC     (1 << 2)

/* 段类型 */
#define SEG_NULL     0   /* 空描述符 (哨兵) */
#define SEG_CODE     1   /* 代码段 */
#define SEG_DATA     2   /* 数据段 */
#define SEG_STACK    3   /* 栈段 */
#define SEG_KERNEL   4   /* 内核段 (最高保护) */

/* 段描述符 — 描述一块受保护内存 */
typedef struct {
    uint8_t  type;    /* 段类型 */
    uint16_t base;    /* 起始地址 */
    uint16_t limit;   /* 长度 (界限) */
    uint8_t  perm;    /* 权限位 (READ/WRITE/EXEC) */
    uint8_t  dpl;     /* 描述符特权级: 谁的级别(数值<=)才能访问 */
    bool     present; /* 是否存在 */
} Segment;

/* ================================================================
 * MMU — 内存保护的核心仲裁者
 * ================================================================ */
typedef struct {
    Segment    sdt[8];      /* 段描述符表 */
    uint8_t*   mem;         /* 物理内存指针 */
    uint32_t   mem_size;    /* 物理内存大小 */
    uint8_t    cpl;         /* 当前特权级 (Current Privilege Level) */
    bool       trap;        /* 是否发生保护异常 */
    uint16_t   trap_addr;   /* 引发异常的地址 */
    char       trap_msg[64];/* 异常描述 */
} Mmu;

/* 初始化 (清空段表) */
void mmu_init(Mmu* m, uint8_t* mem, uint32_t size);

/* 设置一个段描述符 */
void mmu_set_segment(Mmu* m, int idx, uint8_t type,
                     uint16_t base, uint16_t limit,
                     uint8_t perm, uint8_t dpl);

/* 切换特权级 */
void mmu_set_cpl(Mmu* m, uint8_t cpl);

/* 带权限检查的内存访问 — 核心仲裁
 * 返回 true=允许, false=拒绝(置 trap) */
bool mmu_read (Mmu* m, uint16_t addr, uint8_t* out);   /* 读 */
bool mmu_write(Mmu* m, uint16_t addr, uint8_t val);     /* 写 */
bool mmu_exec (Mmu* m, uint16_t addr);                  /* 取指(执行) */

/* 定位负责 addr 的段 (返回下标, 未找到返回 -1) */
int  mmu_find_segment(Mmu* m, uint16_t addr);

/* 重置异常标志 */
void mmu_clear_trap(Mmu* m);

#endif /* MMU_H */
