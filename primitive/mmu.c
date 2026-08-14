/**
 * mmu.c — 内存保护单元实现
 *
 * 三个核心仲裁函数 (mmu_read / mmu_write / mmu_exec) 是所有内存
 * 访问的必经关卡。任何越权/越界/无权访问都会被拦下并置 trap。
 * ================================================================ */

#include "mmu.h"
#include <string.h>
#include <stdio.h>

/* 初始化：清空段表 */
void mmu_init(Mmu* m, uint8_t* mem, uint32_t size) {
    memset(m, 0, sizeof(Mmu));
    m->mem      = mem;
    m->mem_size = size;
    m->cpl      = RING_USER;   /* 默认用户态 (保守: 从低特权启动) */
}

/* 设置段描述符 */
void mmu_set_segment(Mmu* m, int idx, uint8_t type,
                     uint16_t base, uint16_t limit,
                     uint8_t perm, uint8_t dpl) {
    if (idx < 0 || idx >= 8) return;
    m->sdt[idx].type    = type;
    m->sdt[idx].base    = base;
    m->sdt[idx].limit   = limit;
    m->sdt[idx].perm    = perm;
    m->sdt[idx].dpl     = dpl;
    m->sdt[idx].present = true;
}

/* 切换特权级 */
void mmu_set_cpl(Mmu* m, uint8_t cpl) {
    m->cpl = cpl;
}

/* 定位负责 addr 的段 (返回下标, 找不到返回 -1) */
int mmu_find_segment(Mmu* m, uint16_t addr) {
    for (int i = 0; i < 8; i++) {
        Segment* s = &m->sdt[i];
        if (!s->present) continue;
        if (addr >= s->base && addr < (uint32_t)s->base + s->limit) {
            return i;
        }
    }
    return -1;
}

/* 触发保护异常 */
static void trap(Mmu* m, uint16_t addr, const char* msg) {
    m->trap      = true;
    m->trap_addr = addr;
    snprintf(m->trap_msg, sizeof(m->trap_msg), "%s", msg);
}

/* 清除异常 */
void mmu_clear_trap(Mmu* m) {
    m->trap = false;
}

/* ================================================================
 * 核心: 读内存 — MMU 仲裁
 * 检查: ①段存在  ②不越界  ③特权级够  ④有读权限
 * ================================================================ */
bool mmu_read(Mmu* m, uint16_t addr, uint8_t* out) {
    if (addr >= m->mem_size) {
        trap(m, addr, "地址超出物理内存");
        return false;
    }
    int idx = mmu_find_segment(m, addr);
    if (idx < 0) {
        trap(m, addr, "未映射的内存区域 (缺段)");
        return false;
    }
    Segment* s = &m->sdt[idx];
    /* 特权级检查: cpl 数值必须 <= dpl (数值小=特权高) */
    if (m->cpl > s->dpl) {
        trap(m, addr, "特权不足 (Ring 拒绝访问)");
        return false;
    }
    if (!(s->perm & SEG_READ)) {
        trap(m, addr, "该段无读权限");
        return false;
    }
    *out = m->mem[addr];
    return true;
}

/* ================================================================
 * 核心: 写内存 — MMU 仲裁
 * ================================================================ */
bool mmu_write(Mmu* m, uint16_t addr, uint8_t val) {
    if (addr >= m->mem_size) {
        trap(m, addr, "地址超出物理内存");
        return false;
    }
    int idx = mmu_find_segment(m, addr);
    if (idx < 0) {
        trap(m, addr, "未映射的内存区域 (缺段)");
        return false;
    }
    Segment* s = &m->sdt[idx];
    if (m->cpl > s->dpl) {
        trap(m, addr, "特权不足 (Ring 拒绝访问)");
        return false;
    }
    if (!(s->perm & SEG_WRITE)) {
        trap(m, addr, "该段无写权限 (只读段)");
        return false;
    }
    m->mem[addr] = val;
    return true;
}

/* ================================================================
 * 核心: 取指(执行) — 同样要过 MKU 的权限检查
 * 代码段应有 EXEC 权限; 数据段不允许执行 (防 NX 攻击的雏形)
 * ================================================================ */
bool mmu_exec(Mmu* m, uint16_t addr) {
    if (addr >= m->mem_size) {
        trap(m, addr, "地址超出物理内存");
        return false;
    }
    int idx = mmu_find_segment(m, addr);
    if (idx < 0) {
        trap(m, addr, "未映射的内存区域 (缺段)");
        return false;
    }
    Segment* s = &m->sdt[idx];
    if (m->cpl > s->dpl) {
        trap(m, addr, "特权不足 (Ring 拒绝执行)");
        return false;
    }
    if (!(s->perm & SEG_EXEC)) {
        trap(m, addr, "非代码段禁止执行 (NX 保护)");
        return false;
    }
    return true;
}
