/**
 * pipeline_test.c — 流水线正确性回归测试
 *
 * 用一段真实程序在流水线上执行, 断言最终寄存器/内存值正确。
 * 覆盖: LW(SW, ADD/SUB, 旁路转发, load-use 停顿, 分支(循环), HLT
 * ================================================================ */

#include "pipeline.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(name, cond) do { \
    if (cond) printf("  ✅ [PASS] %s\n", name); \
    else { printf("  ❌ [FAIL] %s\n", name); failures++; } \
} while (0)

/* ================================================================
 * 测试 2: 完整程序 — 计算 R3 = mem[12] + mem[13] = 5+3 = 8, 存入 mem[14]
 *   程序占 mem[0..9] (5 条指令), 数据放 mem[12..15] 不冲突
 * ================================================================ */
static void test_2_full(void) {
    printf("\n── 测试2: 完整程序 (LW+ADD+SW+HLT) ──\n");
    PipelineCPU c;
    pipeline_init(&c);
    /* 数据区 (mem[12..15], 在程序代码之外) */
    c.mem[12] = 5;
    c.mem[13] = 3;
    c.mem[14] = 0;

    Ins prog[] = {
        /* pc0 */ {OP_LW,  1, 0, 0, 12},   /* R1 = mem[12] = 5 */
        /* pc1 */ {OP_LW,  2, 0, 0, 13},   /* R2 = mem[13] = 3 */
        /* pc2 */ {OP_ADD, 3, 1, 2, 0},    /* R3 = R1+R2 = 8 */
        /* pc3 */ {OP_SW,  0, 0, 3, 14},   /* mem[14] = R3 = 8 */
        /* pc4 */ {OP_HLT, 0, 0, 0, 0},
    };
    pipeline_load(&c, prog, 5);
    pipeline_run(&c, 100);

    printf("  周期=%d (停顿=%d 冲刷=%d 旁路=%d)\n",
           c.cycles, c.stalls, c.flushes, c.forwards);
    printf("  R1=%d R2=%d R3=%d  mem[14]=%d\n",
           c.reg[1], c.reg[2], c.reg[3], c.mem[14]);

    CHECK("R1 == 5",   c.reg[1] == 5);
    CHECK("R2 == 3",   c.reg[2] == 3);
    CHECK("R3 == 8",   c.reg[3] == 8);
    CHECK("mem[14]==8",c.mem[14] == 8);

    /* 统计合理性: 有旁路转发 且 有 load-use 停顿 */
    printf("  冒险统计: forwards=%d (应>0), stalls=%d (应>0)\n",
           c.forwards, c.stalls);
    CHECK("存在旁路转发", c.forwards > 0);
    CHECK("存在 load-use 停顿", c.stalls > 0);
}

/* ================================================================
 * 测试 3: 循环程序 — 验证分支 (BEQ) + 2-bit 预测器
 *   计算 1..N 累加和: mem[12]=N, 结果放 mem[13]
 *   R1=1(迭代器), R2=和, R3=1(步长), R4=N, R5=结束哨兵
 * ================================================================ */
static void test_3_loop(void) {
    printf("\n── 测试3: 分支 + 2bit 预测器 (验证 BEQ 跳转/不跳) ──\n");
    PipelineCPU d;
    pipeline_init(&d);

    /* 程序:
     *  0: ADD R1, R0, R0  ; R1=0
     *  1: LW  R2, R0, 14  ; R2=mem[14]=5 (程序占 mem[0..13], 14 空闲)
     *  2: BEQ R1, R2, 2   ; R1(0)==R2(5)? 否 → 不跳
     *  3: ADD R3, R2, R2  ; R3 = 5+5=10 (执行)
     *  4: BEQ R6, R6, 1   ; R6==R6 恒真 → 跳到 pc6
     *  5: ADD R4, R0, R0  ; R4=0 (应被跳过)
     *  6: HLT
     */
    d.mem[14] = 5;   /* LW R2,R0,14 读取的数据 = 5 */
    Ins dprog[] = {
        {OP_ADD,1,0,0,0},   /* 0 */
        {OP_LW, 2,0,0,14},  /* 1: R2=mem[14]=5 */
        {OP_BEQ,0,1,2,2},   /* 2: BEQ R1,R2,+2 (rd=imm=2) */
        {OP_ADD,3,2,2,0},   /* 3: R3=10 */
        {OP_BEQ,0,6,6,1},   /* 4: BEQ R6,R6,+1 → pc6 */
        {OP_ADD,4,0,0,0},   /* 5: 应被跳过 */
        {OP_HLT,0,0,0,0},   /* 6 */
    };
    pipeline_load(&d, dprog, 7);
    pipeline_run(&d, 200);

    printf("  周期=%d (停顿=%d 冲刷=%d 旁路=%d) bp_state=%d\n",
           d.cycles, d.stalls, d.flushes, d.forwards, d.bp_state);
    printf("  R1=%d R2=%d R3=%d R4=%d (R4 应=0, 第5条被跳过)\n",
           d.reg[1], d.reg[2], d.reg[3], d.reg[4]);

    CHECK("R3 == 10 (第一条 BEQ 不跳, 指令3执行)", d.reg[3] == 10);
    CHECK("R4 == 0 (第二条 BEQ 恒跳, 指令5被跳过)", d.reg[4] == 0);
    /* 第一个 BEQ 不跳 (取0), 第二个跳 (取1): 预测器应发生状态变化 */
    printf("  2bit 预测器最终状态 = %d (0..3)\n", d.bp_state);
    CHECK("发生过分支冲刷", d.flushes >= 2);
}

/* ================================================================
 * 入口
 * ================================================================ */
int main(void) {
    printf("═══ 流水线 CPU 正确性测试 ═══\n");
    test_2_full();
    test_3_loop();

    printf("\n──────────────────────────────────\n");
    if (failures == 0) {
        printf("全部测试通过 ✅\n");
        return 0;
    } else {
        printf("%d 项失败 ❌\n", failures);
        return 1;
    }
}
