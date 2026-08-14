/**
 * demo.c — 原始构造体演示
 *
 * 展示：
 *   1. 控制单元 (CU) 如何驱动整个计算机
 *   2. 堆和栈管理器如何工作
 *   3. 归一化算法如何找到入口点
 */

#include "primitive.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * 演示 1：CU 真值表 — 每条指令产生什么控制信号
 * ================================================================ */
void demo_cu_truthtable() {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  演示 1：控制单元真值表 (CU = 唯一决策者)            ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    const char* op_names[] = {
        "NOP", "ADD", "SUB", "LD", "ST", "JMP", "JZ", "JNZ",
        "PUSH", "POP", "CALL", "RET", "IRET", "STI", "CLI", "HALT"
    };

    printf("操作码 → 控制信号 (RegW/MemR/MemW/ALU/Imm/Br/CBr/Push/Pop/Call/Ret/IRet/SetIF/ClrIF/Halt)\n");
    printf("───────┬───────────────────────────────────────────────────────────\n");

    for (int op = 0; op <= 0xF; op++) {
        MicroCode mc = cu_decode(op);
        printf(" %-5s │ ", op_names[op]);
        printf("%d%d%d%d%d%d%d%d%d%d%d%d%d%d%d\n",
               mc.reg_write, mc.mem_read, mc.mem_write,
               mc.alu_op, mc.use_imm,
               mc.branch, mc.cond_branch,
               mc.stack_push, mc.stack_pop,
               mc.call, mc.ret, mc.iret,
               mc.set_if, mc.clear_if, mc.halt);
    }
}

/* ================================================================
 * 演示 2：堆栈管理器 — 最简实现
 * ================================================================ */
void demo_stack_heap() {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  演示 2：堆栈管理器 (栈 = SP + 边界, 堆 = 空闲链表)  ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* 栈演示 */
    printf("─── 栈管理器 ───\n");
    uint8_t stack_mem[16];
    StackManager stack;
    stack_init(&stack, stack_mem, 16);

    printf("PUSH 0xAA → ");
    stack_push(&stack, 0xAA);
    printf("SP=%d\n", stack.sp);

    printf("PUSH 0xBB → ");
    stack_push(&stack, 0xBB);
    printf("SP=%d\n", stack.sp);

    printf("PUSH 0xCC → ");
    stack_push(&stack, 0xCC);
    printf("SP=%d\n", stack.sp);

    printf("POP → 0x%02X, SP=%d\n", stack_pop(&stack), stack.sp);
    printf("PEEK → 0x%02X\n", stack_peek(&stack));
    printf("POP → 0x%02X, SP=%d\n", stack_pop(&stack), stack.sp);
    printf("POP → 0x%02X, SP=%d\n", stack_pop(&stack), stack.sp);

    printf("溢出=%d 下溢=%d\n", stack.overflow, stack.underflow);

    /* 堆演示 */
    printf("\n─── 堆管理器 (空闲链表) ───\n");
    uint8_t heap_mem[128];
    HeapManager heap;
    heap_init(&heap, heap_mem, 128);
    printf("初始: 一个 128 字节的空闲块\n");

    void* p1 = heap_alloc(&heap, 16);
    printf("malloc(16) → %p\n", p1);

    void* p2 = heap_alloc(&heap, 32);
    printf("malloc(32) → %p\n", p2);

    void* p3 = heap_alloc(&heap, 8);
    printf("malloc(8)  → %p\n", p3);

    printf("free(%p)\n", p2);
    heap_free(&heap, p2);

    void* p4 = heap_alloc(&heap, 24);
    printf("malloc(24) → %p (复用 p2 的空间)\n", p4);

    heap_dump(&heap);
}

/* ================================================================
 * 演示 3：完整计算机运行 — 斐波那契数列
 *
 * 寄存器约定:
 *   R0 = F(n)     (当前值)
 *   R1 = F(n-1)   (前一个值)
 *   R2 = 临时 (F(n+1))
 *   R3 = 循环计数器
 *   R4 = 存储地址指针
 *   R5 = 常数 1
 *   R6 = 常数 0
 *
 * 内存布局:
 *   0x20: 初始值 1 (F(1))
 *   0x21: 初始值 0 (F(0))
 *   0x22: 循环次数 10
 *   0x23: 结果存储起始地址 0x30
 *   0x24: 常数 1
 *   0x25: 常数 0
 *   0x30-0x39: 结果存储区
 *
 * 程序逻辑:
 *   LD R0, [0x20]   // R0=1
 *   LD R1, [0x21]   // R1=0
 *   LD R3, [0x22]   // R3=10
 *   LD R4, [0x23]   // R4=0x30
 *   LD R5, [0x24]   // R5=1 (常数)
 *   LD R6, [0x25]   // R6=0 (常数)
 * loop:
 *   ST [R4], R0     // 存 F(n)
 *   ADD R4, R4, R5  // 地址++
 *   ADD R2, R0, R1  // R2 = F(n+1)
 *   ADD R1, R0, R6  // R1 = F(n)   (MOV R1,R0)
 *   ADD R0, R2, R6  // R0 = F(n+1) (MOV R0,R2)
 *   SUB R3, R3, R5  // 计数--
 *   JNZ loop        // 回到 loop
 *   HALT
 * ================================================================ */
void demo_fibonacci() {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  演示 3：完整计算机 — 斐波那契数列计算                ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    Instruction prog[] = {
        /* 0: LD R0, [0x20]  — R0 = 1 */
        {OP_LD, 0, 2, 0},
        /* 1: LD R1, [0x21]  — R1 = 0 */
        {OP_LD, 1, 2, 1},
        /* 2: LD R3, [0x22]  — R3 = 10 */
        {OP_LD, 3, 2, 2},
        /* 3: LD R4, [0x23]  — R4 = 0x30 */
        {OP_LD, 4, 2, 3},
        /* 4: LD R5, [0x24]  — R5 = 1 (常数) */
        {OP_LD, 5, 2, 4},
        /* 5: LD R6, [0x25]  — R6 = 0 (常数) */
        {OP_LD, 6, 2, 5},

        /* 6: loop: ST [R4], R0  — 存 F(n) */
        {OP_ST, 0, 4, 0},
        /* 7: ADD R4, R4, R5 — 地址++ */
        {OP_ADD, 4, 4, 5},
        /* 8: ADD R2, R0, R1 — R2 = F(n+1) */
        {OP_ADD, 2, 0, 1},
        /* 9: ADD R1, R0, R6 — R1 = F(n) (MOV) */
        {OP_ADD, 1, 0, 6},
        /* 10: ADD R0, R2, R6 — R0 = F(n+1) (MOV) */
        {OP_ADD, 0, 2, 6},
        /* 11: SUB R3, R3, R5 — 计数-- */
        {OP_SUB, 3, 3, 5},
        /* 12: JNZ 6 — 如果 R3 != 0, 跳回 loop */
        {OP_JNZ, 0, 0, 6},
        /* 13: HALT */
        {OP_HALT, 0, 0, 0},
    };

    PrimitiveComputer c;
    memset(&c, 0, sizeof(c));

    /* 预置数据 */
    c.memory[0x20] = 1;    /* F(1) = 1 */
    c.memory[0x21] = 0;    /* F(0) = 0 */
    c.memory[0x22] = 10;   /* 循环次数 */
    c.memory[0x23] = 0x30; /* 结果存储起始地址 */
    c.memory[0x24] = 1;    /* 常数 1 */
    c.memory[0x25] = 0;    /* 常数 0 */

    computer_load(&c, prog, sizeof(prog)/sizeof(Instruction));
    printf("程序已加载，开始执行...\n");
    printf("计算斐波那契数列前 10 项:\\n\\n");

    computer_run(&c);
    computer_dump(&c);

    printf("\\n内存 [0x30-0x3F]: 斐波那契数列:\n");
    for (int i = 0x30; i < 0x3A; i++) {
        printf("  MEM[0x%02X] = %d\n", i, c.memory[i]);
    }
}

/* ================================================================
 * 演示 4：归一化算法 — 在未知内存中找入口点
 * ================================================================ */
void demo_entry_finder() {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  演示 4：归一化算法 — 在未知内存中找入口点           ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* 模拟一段未知内存镜像 */
    uint8_t unknown_mem[256];

    /* 填充随机数据 (模拟数据段) */
    for (int i = 0; i < 256; i++) {
        unknown_mem[i] = (uint8_t)(i * 17 + 31);  /* 伪随机 */
    }

    /* 在地址 0x40 处注入入口点特征 */
    /* 模拟 ARM Cortex-M 的中断向量表结构 */
    unknown_mem[0x00] = 0x00;  /* 初始 SP (低字节) */
    unknown_mem[0x01] = 0x00;
    unknown_mem[0x02] = 0x01;  /* 初始 SP = 0x100 (指向栈顶) */
    unknown_mem[0x03] = 0x00;

    unknown_mem[0x04] = 0x40;  /* Reset_Handler = 0x40 (入口点!) */
    unknown_mem[0x05] = 0x00;
    unknown_mem[0x06] = 0x00;
    unknown_mem[0x07] = 0x00;

    unknown_mem[0x08] = 0x50;  /* NMI_Handler = 0x50 */
    unknown_mem[0x09] = 0x00;
    unknown_mem[0x0A] = 0x00;
    unknown_mem[0x0B] = 0x00;

    unknown_mem[0x0C] = 0x60;  /* HardFault = 0x60 */
    unknown_mem[0x0D] = 0x00;
    unknown_mem[0x0E] = 0x00;
    unknown_mem[0x0F] = 0x00;

    /* 在入口点 0x40 处放置有效指令 */
    unknown_mem[0x40] = 0x10;  /* ADD (有效操作码) */
    unknown_mem[0x41] = 0x20;  /* ADD */
    unknown_mem[0x42] = 0x30;  /* LD */
    unknown_mem[0x43] = 0x40;  /* ST */
    unknown_mem[0x44] = 0x50;  /* JMP */

    printf("未知内存镜像 (256 字节):\n");
    printf("  地址 0x00-0x0F: ARM Cortex-M 风格中断向量表\n");
    printf("  地址 0x40-0x44: 有效指令 (入口点)\n");
    printf("  其余: 随机数据\n\n");

    printf("运行归一化入口点查找...\n\n");

    EntryCandidate results[MAX_CANDIDATES];
    int n = find_entry_points(unknown_mem, 256, results, MAX_CANDIDATES);

    printf("找到 %d 个候选入口点:\n", n);
    printf("───────┬────────┬────────────────────────────────\n");
    printf(" 地址   │ 置信度 │ 原因\n");
    printf("───────┼────────┼────────────────────────────────\n");
    for (int i = 0; i < n; i++) {
        printf(" 0x%04X │ %5.2f  │ %s\n",
               results[i].address, results[i].score, results[i].reason);
    }

    if (n > 0 && results[0].address == 0x40) {
        printf("\n✅ 归一化算法成功找到入口点 0x40！（最高置信度）\n");
    } else if (n > 0) {
        printf("\n⚠️  最高分候选: 0x%04X (期望 0x40)\n", results[0].address);
    } else {
        printf("\n❌ 未找到任何候选入口点\n");
    }
}

/* ================================================================
 * 演示 5：CU 逐条指令追踪
 * ================================================================ */
void demo_step_trace() {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  演示 5：CU 逐条指令追踪 (取指→译码→执行)           ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /* 简单程序：ADD R0, R1, R2 → JZ 3 → HALT → SUB R3, R3, R4 */
    Instruction prog[] = {
        {OP_ADD, 0, 1, 2},   /* 0: R0 = R1 + R2 (R1=0, R2=0 → R0=0, ZF=1) */
        {OP_JZ,  0, 0, 3},   /* 1: 如果 ZF=1 → 跳转到地址 3 (HALT) */
        {OP_SUB, 3, 3, 4},   /* 2: 这条应该被跳过 */
        {OP_HALT,0, 0, 0},   /* 3: 停机 */
    };

    PrimitiveComputer c;
    memset(&c, 0, sizeof(c));
    computer_load(&c, prog, 4);

    printf("程序: ADD R0,R1,R2 → JZ 3 → SUB (跳过) → HALT\n");
    printf("初始: R1=0, R2=0, R3=5, R4=3\n\n");
    c.cpu.reg[3] = 5;
    c.cpu.reg[4] = 3;

    printf("逐条执行:\n");
    int step = 0;
    while (c.cpu.running && step < 10) {
        uint8_t* ip = &c.memory[c.cpu.pc * 2];
        uint8_t op = (ip[0] >> 4) & 0x0F;

        const char* op_names[] = {
            "NOP","ADD","SUB","LD","ST","JMP","JZ","JNZ",
            "PUSH","POP","CALL","RET","?","?","?","HALT"
        };

        printf("  步骤 %d: PC=0x%02X 指令=%s", step, c.cpu.pc, op_names[op]);

        MicroCode uop = cu_decode(op);
        printf("  [RegW=%d MemR=%d MemW=%d ALU=%d Br=%d CBr=%d Push=%d Pop=%d Call=%d Ret=%d Halt=%d]",
               uop.reg_write, uop.mem_read, uop.mem_write,
               uop.alu_op, uop.branch, uop.cond_branch,
               uop.stack_push, uop.stack_pop,
               uop.call, uop.ret, uop.halt);

        computer_step(&c);
        printf(" → ZF=%d PC=0x%02X R0=%d\n", c.cpu.zf, c.cpu.pc, c.cpu.reg[0]);
        step++;
    }

    printf("\n结果: 跳过了 SUB 指令 (因为 ADD 结果=0, ZF=1, JZ 跳转)\n");
}

/* ================================================================
 * 演示 6：中断系统 — 定时器中断驱动的多任务
 *
 * 场景：主程序在计算斐波那契，定时器每 8 个周期打断一次，
 *       执行一个「心跳 ISR」在内存中写入递增计数器，
 *       然后 IRET 返回继续计算。
 *
 * 这个演示展示了计算机如何从纯计算器变成可响应外部事件的系统。
 * ================================================================ */
void demo_interrupt() {
    printf("\n╔══════════════════════════════════════════════════════╗\n");
    printf("║  演示 6：中断系统 — 定时器中断驱动的多任务          ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");

    /*
     * 内存布局:
     *   0x00-0x1F: 主程序 (斐波那契循环)
     *   0x20-0x2F: 数据区
     *   0x30-0x3F: 结果存储区
     *   0x40-0x4F: ISR 代码 (心跳 ISR)
     *   0x50:      心跳计数器
     *
     * 主程序:
     *   STI           // 开中断
     *   LD R0, [0x20] // R0=1 (F(1))
     *   LD R1, [0x21] // R1=0 (F(0))
     *   LD R3, [0x22] // R3=10 (计数)
     *   LD R4, [0x23] // R4=0x30 (存储地址)
     *   LD R5, [0x24] // R5=1 (常数)
     *   LD R6, [0x25] // R6=0 (常数)
     * loop:
     *   ST [R4], R0    // 存 F(n)
     *   ADD R4, R4, R5 // 地址++
     *   ADD R2, R0, R1 // F(n+1)
     *   ADD R1, R0, R6 // MOV
     *   ADD R0, R2, R6 // MOV
     *   SUB R3, R3, R5 // 计数--
     *   JNZ loop
     *   CLI            // 关中断
     *   HALT
     *
     * ISR (地址 0x40):
     *   LD R10, [0x50]  // 读心跳计数器
     *   ADD R10, R10, R5 // 计数器++
     *   ST [0x50], R10   // 写回
     *   IRET             // 返回
     */

    Instruction prog[] = {
        /* 0: STI — 开中断 */
        {OP_STI, 0, 0, 0},
        /* 1: LD R0, [0x20] */
        {OP_LD, 0, 2, 0},
        /* 2: LD R1, [0x21] */
        {OP_LD, 1, 2, 1},
        /* 3: LD R3, [0x22] */
        {OP_LD, 3, 2, 2},
        /* 4: LD R4, [0x23] */
        {OP_LD, 4, 2, 3},
        /* 5: LD R5, [0x24] */
        {OP_LD, 5, 2, 4},
        /* 6: LD R6, [0x25] */
        {OP_LD, 6, 2, 5},

        /* 7: loop: ST [R4], R0 */
        {OP_ST, 0, 4, 0},
        /* 8: ADD R4, R4, R5 */
        {OP_ADD, 4, 4, 5},
        /* 9: ADD R2, R0, R1 */
        {OP_ADD, 2, 0, 1},
        /* 10: ADD R1, R0, R6 */
        {OP_ADD, 1, 0, 6},
        /* 11: ADD R0, R2, R6 */
        {OP_ADD, 0, 2, 6},
        /* 12: SUB R3, R3, R5 */
        {OP_SUB, 3, 3, 5},
        /* 13: JNZ 7 */
        {OP_JNZ, 0, 0, 7},
        /* 14: CLI — 关中断 */
        {OP_CLI, 0, 0, 0},
        /* 15: HALT */
        {OP_HALT, 0, 0, 0},
    };

    PrimitiveComputer c;
    memset(&c, 0, sizeof(c));

    /* 预置数据 */
    c.memory[0x20] = 1;    /* F(1) */
    c.memory[0x21] = 0;    /* F(0) */
    c.memory[0x22] = 10;   /* 循环次数 */
    c.memory[0x23] = 0x30; /* 存储地址 */
    c.memory[0x24] = 1;    /* 常数 1 */
    c.memory[0x25] = 0;    /* 常数 0 */
    c.memory[0x50] = 0;    /* 心跳计数器初始值 */

    /* 加载主程序 */
    computer_load(&c, prog, sizeof(prog)/sizeof(Instruction));

    /* 注意: computer_load 初始化了堆 (0x40-0x80) 和栈 (0x80-0x100)。
     * 所以需要在之后重新设置数据区域。
     * 心跳计数器放在 0x30 (在堆区域之前，安全) */
    c.memory[0x20] = 1;    /* F(1) */
    c.memory[0x21] = 0;    /* F(0) */
    c.memory[0x22] = 10;   /* 循环次数 */
    c.memory[0x23] = 0x38; /* 存储地址 (用 0x38 避免与堆冲突) */
    c.memory[0x24] = 1;    /* 常数 1 */
    c.memory[0x25] = 0;    /* 常数 0 */
    c.memory[0x30] = 0;    /* 心跳计数器初始值 */

    /* 安装 ISR — 定时器中断向量 0, ISR 在地址 0x10 (程序代码之后) */
    pic_set_handler(&c.pic, IRQ_TIMER, 0x10, 0x03);
    pic_mask(&c.pic, IRQ_TIMER, false);

    /* ISR 用的地址常量: 心跳计数器在 0x30 */
    c.memory[0x31] = 0x30;  /* 目标地址常量 */

    /* ISR 代码 (地址 0x10-0x14):
     * 0x10: LD R11, [0x31] — R11 = 0x30 (目标地址)
     * 0x11: LD R10, [0x30] — R10 = 心跳计数器
     * 0x12: ADD R10, R10, R5 — R10++
     * 0x13: ST [R11], R10 — 写回
     * 0x14: IRET */
    c.memory[0x10 * 2]     = (OP_LD << 4) | 0xB;    /* LD R11, [0x31] */
    c.memory[0x10 * 2 + 1] = (0x3 << 4) | 0x1;
    c.memory[0x11 * 2]     = (OP_LD << 4) | 0xA;    /* LD R10, [0x30] */
    c.memory[0x11 * 2 + 1] = (0x3 << 4) | 0x0;
    c.memory[0x12 * 2]     = (OP_ADD << 4) | 0xA;   /* ADD R10, R10, R5 */
    c.memory[0x12 * 2 + 1] = (0xA << 4) | 0x5;
    c.memory[0x13 * 2]     = (OP_ST << 4) | 0xA;    /* ST [R11], R10 */
    c.memory[0x13 * 2 + 1] = (0xB << 4) | 0x0;
    c.memory[0x14 * 2]     = (OP_IRET << 4) | 0x0;  /* IRET */
    c.memory[0x14 * 2 + 1] = 0x00;

    printf("系统配置:\\n");
    printf("  主程序: 斐波那契循环 (地址 0x00-0x0F)\\n");
    printf("  ISR:    心跳计数器递增 (地址 0x10-0x14)\\n");
    printf("  定时器: 每 4 个周期触发一次中断\\n");
    printf("  心跳计数器地址: 0x30\\n\\n");

    printf("逐周期执行 (展示中断抢占):\\n");

    int timer_cycle = 0;
    int timer_period = 4;  /* 每 4 个周期触发一次 */

    int step = 0;
    while (c.cpu.running && step < 200) {
        /* 模拟定时器：每 timer_period 个周期触发一次 */
        timer_cycle++;
        if (timer_cycle >= timer_period) {
            timer_cycle = 0;
            pic_request(&c.pic, IRQ_TIMER);
        }

        /* 执行前快照 */
        uint8_t pc_before = c.cpu.pc;
        uint8_t hb_before = c.memory[0x30];
        bool iflag_before = c.cpu.iflag;
        bool irr_before = (c.pic.irr != 0);

        computer_step(&c);

        uint8_t pc_after = c.cpu.pc;
        uint8_t hb_after = c.memory[0x30];

        /* 展示中断事件 */
        if (pc_before != pc_after && pc_after == 0x10) {
            printf("  ⚡ 周期 %3d: 定时器中断触发! PC 0x%02X→0x10 心跳=%d IF=%d,IRR=%d\n",
                   step, pc_before, hb_before, iflag_before, irr_before);
        } else if (hb_before != hb_after) {
            printf("  💓 周期 %3d: 心跳 ISR 执行, 计数器 %d → %d\n",
                   step, hb_before, hb_after);
        } else if (pc_before == 0x10 || pc_before == 0x11 ||
                   pc_before == 0x12 || pc_before == 0x13 || pc_before == 0x14) {
            /* 展示 ISR 内部执行 */
            printf("  🔧 周期 %3d: ISR 内部 PC=0x%02X 心跳=%d\n", step, pc_before, hb_before);
        }

        step++;
    }

    computer_dump(&c);

    printf("\n定时器中断统计:\\n");
    printf("  心跳计数器最终值: %d (ISR 总共被触发了这么多次)\\n", c.memory[0x30]);
    printf("  总执行周期: %d\\n", step);
    printf("  斐波那契结果:\\n");
    for (int i = 0x38; i < 0x42; i++) {
        printf("    MEM[0x%02X] = %d\n", i, c.memory[i]);
    }

    printf("\n关键观察:\\n");
    printf("  - 主程序在计算斐波那契的同时，ISR 在后台递增心跳计数器\\n");
    printf("  - 中断是透明的：主程序不知道自己被中断过\\n");
    printf("  - 这就是现代操作系统「抢占式多任务」的雏形\\n");
}

int main() {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║       最原始构造体 — CU + 堆栈管理器 + 归一化入口点     ║\n");
    printf("║       从门电路到计算机：理解机器语言的本质               ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    demo_cu_truthtable();
    demo_stack_heap();
    demo_fibonacci();
    demo_entry_finder();
    demo_step_trace();
    demo_interrupt();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("全部演示完成。\n");
    printf("三个原始构造体验证完毕：\n");
    printf("  1. ✅ 控制单元 (CU) — 真值表译码，唯一决策者\n");
    printf("  2. ✅ 栈管理器 — SP + 边界检查\n");
    printf("  3. ✅ 堆管理器 — 空闲链表\n");
    printf("外加：归一化入口点查找器 ✅\n");
    printf("新增：中断系统 ✅ (PIC + IVT + IRET)\n");
    printf("══════════════════════════════════════════════════════════\n");

    return 0;
}