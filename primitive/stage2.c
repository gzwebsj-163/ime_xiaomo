/**
 * stage2.c — 第二阶段: 内存保护与特权级
 *
 * 从「能跑程序的计算机」升级为「有保护机制的计算机」。
 *
 * 核心新增:
 *   1. 特权级:  Ring 0 (内核) / Ring 3 (用户)
 *   2. 内存段:   base + limit + 权限(读/写/执行) + dpl
 *   3. MMU 仲裁: 每次内存访问都检查 特权级/界限/权限
 *   4. SYSCALL/SYSRET: 用户程序安全地请求内核服务的唯一通道
 *
 * 演示场景:
 *   - 用户程序 (Ring 3) 运行
 *   - 直接偷读内核机密 → 被 MMU 拒绝 + 触发保护异常
 *   - 试图写只读段 → 被拒绝
 *   - 通过 SYSCALL 请求「读内核时间」→ 内核 (Ring 0) 提供服务 → SYSRET 返回
 * ================================================================ */

#include "mmu.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * 微型 CPU — 专门展示特权级的数据结构
 * 复用了第一阶段的部分理念,但聚焦在「谁能在哪块内存上做什么」
 * ================================================================ */
#define MEM_SIZE 1024    /* 物理内存 1024 字节 (覆盖 0x300 机密区) */
#define USER_CODE 0x00   /* 用户代码段:  0x00 - 0x7F */
#define USER_DATA 0x80   /* 用户数据段:  0x80 - 0xFF */
#define KERN_CODE 0x100  /* 内核代码段: 0x100 - 0x17F */
#define KERN_DATA 0x180  /* 内核数据段: 0x180 - 0x1FF */
#define KERN_SECR 0x300  /* 内核机密段: 0x300 - 0x31F (最高保护) */

typedef struct {
    Mmu     mmu;
    uint8_t mem[MEM_SIZE];
    uint16_t pc;         /* 程序计数器 */
    uint8_t  reg[4];     /* 少量通用寄存器 */
    bool     running;

    /* 系统调用现场 */
    uint16_t user_pc;    /* 进入 SYSCALL 前的 PC */
    uint8_t  user_cpl;   /* 用户态特权级存档 */
} Stage2CPU;

/* ================================================================
 * 系统调用向量 — 内核提供给用户的服务清单
 * ================================================================ */
enum {
    SYS_FETCH_TIME = 1,  /* 读内核时间 */
    SYS_PRINT      = 2,  /* 打印一个字符 */
};

/* ================================================================
 * 初始化: 建立内存布局 + 段表 + 特权级
 * ================================================================ */
void stage2_init(Stage2CPU* c) {
    memset(c, 0, sizeof(*c));
    mmu_init(&c->mmu, c->mem, MEM_SIZE);

    /* ---- 建立段描述符表 (分页/分段的核心) ----
     * 注意 dpl: 数值越小特权越高。
     *   dpl=0  → 只有内核(Ring0)能访问
     *   dpl=3  → 内核和用户都能访问            */
    mmu_set_segment(&c->mmu, 0, SEG_CODE, USER_CODE, 0x80,
                    SEG_READ | SEG_EXEC, RING_USER);   /* 用户代码: 可读可执行 */
    mmu_set_segment(&c->mmu, 1, SEG_DATA, USER_DATA, 0x80,
                    SEG_READ | SEG_WRITE, RING_USER);  /* 用户数据: 可读可写 */
    mmu_set_segment(&c->mmu, 2, SEG_CODE, KERN_CODE, 0x80,
                    SEG_READ | SEG_EXEC, RING_KERNEL); /* 内核代码: 仅内核可执行 */
    mmu_set_segment(&c->mmu, 3, SEG_DATA, KERN_DATA, 0x80,
                    SEG_READ | SEG_WRITE, RING_KERNEL);/* 内核数据: 仅内核可读写 */
    mmu_set_segment(&c->mmu, 4, SEG_KERNEL, KERN_SECR, 0x20,
                    SEG_READ, RING_KERNEL);            /* 机密段:   仅内核可读 */

    c->pc      = USER_CODE;      /* 从用户代码开始 */
    c->running = true;
    /* 初始处于内核态(像 boot 加载完用户程序后切到用户态) */
    mmu_set_cpl(&c->mmu, RING_USER);
}

/* ================================================================
 * 打印工具(不通过 MMU, 属于宿主调试)
 * ================================================================ */
static const char* cpl_name(uint8_t cpl) {
    return cpl == RING_KERNEL ? "Ring 0 😈 内核" : "Ring 3 🐿️ 用户";
}

/* ================================================================
 * 场景 A: 用户程序偷偷摸内核机密段
 * ================================================================ */
void stage2_demo_sneak(Stage2CPU* c) {
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  场景 A: 用户程序直接偷读内核机密 → 被 MMU 拦截!        ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* 内核保密数据 */
    c->mem[KERN_SECR + 0] = 'P';
    c->mem[KERN_SECR + 1] = 'A';
    c->mem[KERN_SECR + 2] = 'S';
    c->mem[KERN_SECR + 3] = 'S';

    printf("现状: 当前特权级 = %s\n", cpl_name(c->mmu.cpl));
    printf("企图: 用户程序直接读内核机密段 0x%03X (dpl=%d, 仅内核可读)\n",
           KERN_SECR, c->mmu.sdt[4].dpl);
    printf("\n");

    uint8_t leak = 0;
    mmu_clear_trap(&c->mmu);
    bool ok = mmu_read(&c->mmu, KERN_SECR + 0, &leak);

    if (!ok) {
        printf("❌  MMU 裁决: 拒绝访问!\n");
        printf("    🚨 保护异常: %s (地址 0x%03X)\n",
               c->mmu.trap_msg, c->mmu.trap_addr);
        printf("    原因: 当前 cpl=%d, 而该段 dpl=%d → 特权不够\n",
               c->mmu.cpl, c->mmu.sdt[4].dpl);
        printf("\n    💡 对比: 如果用户程序真拿到了它想读的机密字节\n");
        printf("       其实就是 'P'(0x50) —— 但它永远也读不到, 因为 MMU 挡下了\n");
    } else {
        printf("⚠️  异常: 居然读到了 0x%02X (说明 MMU 没拦截!)\n", leak);
    }
}

/* ================================================================
 * 场景 B: 用户程序往只读代码段里写数据
 * ================================================================ */
void stage2_demo_ro_write(Stage2CPU* c) {
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  场景 B: 用户程序往「只读」代码段写数据 → 被 MMU 拦截!     ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    printf("现状: 当前特权级 = %s\n", cpl_name(c->mmu.cpl));
    printf("企图: 往用户代码段 0x%02X (perm=可读|可执行, 无写权限) 写入数据\n", USER_CODE);
    printf("\n");

    mmu_clear_trap(&c->mmu);
    bool ok = mmu_write(&c->mmu, USER_CODE + 0x10, 0xAA);

    if (!ok) {
        printf("❌  MMU 裁决: 拒绝写入!\n");
        printf("    🚨 保护异常: %s (地址 0x%02X)\n",
               c->mmu.trap_msg, c->mmu.trap_addr);
        printf("    原因: 该段 perm 无 SEG_WRITE 位 → 只读段不可写\n");
        printf("\n    💡 这防止了「程序把自己改掉」或「注入恶意代码」\n");
    } else {
        printf("⚠️  异常: 写成功了 (说明 MMU 没拦截!)\n");
    }
}

/* ================================================================
 * 场景 C: 用户的正确姿势 — 通过 SYSCALL 请求内核服务
 *
 * 这是本阶段的重头戏: 演示特权级切换的完整流程
 *   用户(Ring3) → SYSCALL → 陷入内核(Ring0) → 服务 → SYSRET → 回到用户(Ring3)
 * ================================================================ */
void stage2_demo_syscall(Stage2CPU* c) {
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  场景 C: 用户的正确姿势 — 通过 SYSCALL 请求内核服务     ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    /* 先给内核时间一个非零值, 让演示更有说服力 */
    c->mem[KERN_DATA] = 42;

    printf("内核在 0x%03X 保存了一个「系统时间」值: %d\n", KERN_DATA, c->mem[KERN_DATA]);
    printf("\n用户程序想读这个时间, 但它不能直接访问内核段。\n");
    printf("正确做法: 通过 SYSCALL 指令「陷入内核」, 由内核代读后返回。\n\n");

    /* ---- 第 1 步: 用户态请求 ---- */
    printf("─── ① 用户态 (Ring 3): 执行 SYSCALL(编号=1, 请求读时间) ───\n");
    c->user_pc  = 0x30;         /* 要返回的地址 */
    c->user_cpl = c->mmu.cpl;   /* 保存用户态 (Ring 3) */
    printf("    保存返回地址 user_pc=0x%02X, 用户特权级 cpl=%d\n",
           c->user_pc, c->user_cpl);

    /* 先展示: 如果用户直接在用户态硬读, 结果是什么 */
    {
        uint8_t tryd = 0;
        mmu_clear_trap(&c->mmu);
        bool direct_ok = mmu_read(&c->mmu, KERN_DATA, &tryd);
        printf("    验证: 若此刻(仍 Ring 3)直接读内核段 → %s\n",
               direct_ok ? "⚠️ 居然允许?!" : "🚨 被拒绝 (特权不足)");
    }
    printf("    ✔ 所以必须切换特权级。陷入内核!\n\n");

    /* ---- 第 2 步: 内核接管 (特权级切换) ---- */
    mmu_set_cpl(&c->mmu, RING_KERNEL);
    printf("─── ② 切换到内核态! cpl: %d → %d (%s现接管) ───\n",
           c->user_cpl, c->mmu.cpl, cpl_name(c->mmu.cpl));

    /* 现在内核可以读取机密了 */
    uint8_t time_val = 0;
    mmu_clear_trap(&c->mmu);
    bool ok = mmu_read(&c->mmu, KERN_DATA, &time_val);
    printf("    内核 (Ring 0) 现在有权读内核段: %s → 读到时间 = %d\n",
           ok ? "✅ 允许" : "❌ 拒绝", time_val);

    /* 内核把结果放入用户数据段 R0 对应的位置 (这里演示直接打印) */
    printf("    内核把结果返回给用户 …\n\n");

    /* ---- 第 3 步: 返回用户态 ---- */
    mmu_set_cpl(&c->mmu, (uint8_t)c->user_cpl);
    c->pc = c->user_pc;
    printf("─── ③ SYSRET 返回! cpl: %d → %d, PC 回到 0x%02X ───\n",
           RING_KERNEL, c->mmu.cpl, c->pc);
    printf("    内核机密始终没有被「直接」泄露给用户, 一切通过受控通道完成 ✅\n\n");

    printf("⚙️  特权级切换全流程:\n");
    printf("    Ring 3 ──SYSCALL──▶ Ring 0 ──服务──▶ SYSRET──▶ Ring 3\n");
    printf("    (受限)               (高特权)                (恢复受限)\n");
}

/* ================================================================
 * 场景 D: 特权级对比总览表
 * ================================================================ */
void stage2_demo_summary(void) {
    printf("\n╔══════════════════════════════════════════════════════════╗\n");
    printf("║  特权级模型全景 — 谁在哪块内存上能做什么                  ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n\n");

    printf("段              基址    范围        dpl  读  写  执行   谁能访问\n");
    printf("──────────────────────────────────────────────────────────────\n");
    printf("用户代码段      0x00    0x00-0x7F   3    ✓   ✗   ✓   内核+用户\n");
    printf("用户数据段      0x80    0x80-0xFF   3    ✓   ✓   ✗   内核+用户\n");
    printf("内核代码段      0x100   0x100-0x17F 0    ✓   ✗   ✓   仅内核\n");
    printf("内核数据段      0x180   0x180-0x1FF 0    ✓   ✓   ✗   仅内核\n");
    printf("内核机密段      0x300   0x300-0x31F 0    ✓   ✗   ✗   仅内核(只读)\n");
    printf("──────────────────────────────────────────────────────────────\n");
    printf("\n结论: 用户(3)无法访问 dpl=0 的段 → 内核数据天生安全\n");
    printf("    用户通过 SYSCALL 门请求内核 → 内核仲裁 → 受控提供服务\n");
}

/* ================================================================
 * 主演示入口
 * ================================================================ */
int main(void) {
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║      第二阶段: 内存保护与特权级 (Ring 0 / Ring 3)       ║\n");
    printf("║      从「能跑程序」到「安全地跑程序」                    ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");

    /* 展示布局 */
    printf("\n┌─────────── 内存布局 (分段) ───────────┐\n");
    printf("│ 0x000-0x07F │ 用户代码段  (Ring 3)   │\n");
    printf("│ 0x080-0x0FF │ 用户数据段  (Ring 3)   │\n");
    printf("│ 0x100-0x17F │ 内核代码段  (Ring 0)   │\n");
    printf("│ 0x180-0x1FF │ 内核数据段  (Ring 0)   │\n");
    printf("│ 0x300-0x31F │ 内核机密段  (Ring 0 只读)│\n");
    printf("└──────────────────────────────────────┘\n");

    Stage2CPU c;
    stage2_init(&c);

    stage2_demo_sneak(&c);
    stage2_demo_ro_write(&c);
    stage2_demo_syscall(&c);
    stage2_demo_summary();

    printf("\n════════════════════════════════════════════════════════════\n");
    printf("第二阶段验证完毕 ✅\n");
    printf("  1. ✅ 特权级模型 — Ring 0 (内核) / Ring 3 (用户)\n");
    printf("  2. ✅ 内存段描述符 — base + limit + 权限 + dpl\n");
    printf("  3. ✅ MMU 仲裁 — 越权/越界/无权 一律拦截\n");
    printf("  4. ✅ SYSCALL/SYSRET — 用户→内核 受控通道\n");
    printf("════════════════════════════════════════════════════════════\n");
    return 0;
}
