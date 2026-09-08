/**
 * ctx_android.c — Android/bionic 版 ucontext 三件套（aarch64 汇编实现）
 *
 * bionic 只定义 ucontext_t 结构（<sys/ucontext.h>），函数符号
 * getcontext/makecontext/swapcontext/setcontext 在 libc 中缺失，
 * 这里用 arm64 裸汇编实现真正的独立栈上下文切换。
 *
 * ══ bionic aarch64 布局（2026-08-25 探针实测）══
 *   ucontext_t:  uc_flags(0) uc_link(8) uc_stack(16, stack_t=24B)
 *                uc_sigmask(40, 136B) → uc_mcontext(176)   size=4560
 *   sigcontext:  fault_address(0) regs[31](8) sp(256) pc(264)
 *                pstate(272) __reserved[4096](288, 16 对齐) size=4384
 *   ⚠️ ARM64 浮点寄存器不走 sigcontext（内核用独立扩展 record），
 *      sigcontext 内无 fpregs 成员 → 浮点区挪进 __reserved（289 起 16 对齐）。
 *
 *   相对 ucontext_t: R0=184 LR=424 SP=432 PC=440 RESV=464
 *   浮点存储（reserved 内）: Q8=464(16对齐) FPSR=592(8对齐) FPCR=600(8对齐)
 *   ⚠️ bionic/clang 编码陷阱: `str sp,#imm` 非法(需 mov 中转)、
 *      msr fpsr/fpcr 必须 X 寄存器、LDR/STR 大偏移须 8 字节对齐。
 *
 * 仅 __ANDROID__/__BIONIC__ + __aarch64__ 编译；其余平台此文件为空。
 */
#if (defined(__ANDROID__) || defined(__BIONIC__)) && defined(__aarch64__)

#include "ctx_android.h"
#include <stddef.h>
#include <signal.h>
#include <sys/ucontext.h>

/* ── bionic aarch64 布局常量（探针实测）── */
#define MC         176u        /* uc_mcontext 偏移 */
#define R0         (MC + 8u)                      /* 184: sigcontext.regs[0]=x0 */
#define LR         (MC + 8u + 30u*8u)             /* 424: regs[30]=x30 */
#define SP         (MC + 256u)                    /* 432: sigcontext.sp */
#define PC         (MC + 264u)                    /* 440: sigcontext.pc */
#define RESV       (MC + 288u)                    /* 464: sigcontext.__reserved[0](16对齐) */
#define Q8         (RESV + 0u)                    /* 464: 16 对齐，q8..q15 共 128B */
#define FPSR       (RESV + 128u)                  /* 592: 16 对齐 */
#define FPCR       (RESV + 136u)                  /* 600: 8 对齐(str x2 需 8 倍数) */

/* 编译期断言：防止平台头变化后静默越界 */
_Static_assert(offsetof(ucontext_t, uc_link) == 8u,    "bionic uc_link=8");
_Static_assert(offsetof(ucontext_t, uc_mcontext) == 176u, "bionic uc_mcontext=176");
_Static_assert(offsetof(struct sigcontext, sp) == 256u,   "bionic sigcontext.sp=256");
_Static_assert(offsetof(struct sigcontext, pc) == 264u,   "bionic sigcontext.pc=264");
_Static_assert((Q8 % 16u) == 0u, "Q8 必须 16 字节对齐(stp q)");

#define STR2(x) #x
#define STR(x) STR2(x)

/* ════════════════════════════════════════════════════════════════
 * __ctx_link_entry — makecontext 铺设的任务跳入点
 *   新栈 [sp]=ucp  [sp+8]=func
 *   1) 调用 func()（任务函数返回 = 任务结束）
 *   2) 经 uc_link 跳回调度器（等价标准 ucontext 语义）
 * ════════════════════════════════════════════════════════════════ */
__asm__(
".global __ctx_link_entry\n"
".type __ctx_link_entry, %function\n"
"__ctx_link_entry:\n"
"    ldr  x0, [sp]            /* ucp */\n"
"    ldr  x1, [sp, #8]        /* func */\n"
"    cbz  x1, .Lctx_nofunc\n"
"    blr  x1                  /* func() — 任务函数入口 */\n"
"    ldr  x0, [sp]            /* func 返回后重取 ucp */\n"
".Lctx_nofunc:\n"
"    ldr  x30, [x0, #8]       /* uc_link */\n"
"    cbz  x30, .Lctx_done\n"
"    mov  x0, x30\n"
"    b    setcontext          /* 切到 uc_link(调度器) — 不返回 */\n"
".Lctx_done:\n"
"    ret\n"
".size __ctx_link_entry, .-__ctx_link_entry\n");

void __ctx_link_entry(void);

/* ════════════════════════════════════════════════════════════════
 * 恢复段（setcontext / swapcontext 共用）
 *   x0 = 目标 ucontext；恢复寄存器后 x0=0，br x16(=LR 字段)
 * ════════════════════════════════════════════════════════════════ */
#define RESTORE \
        "ldp q8, q9,   [x0, #" STR(Q8) "]\n" \
        "ldp q10, q11, [x0, #" STR(Q8) "+32]\n" \
        "ldp q12, q13, [x0, #" STR(Q8) "+64]\n" \
        "ldp q14, q15, [x0, #" STR(Q8) "+96]\n" \
        "ldr x2, [x0, #" STR(FPSR) "]\n" \
        "msr fpsr, x2\n" \
        "ldr x2, [x0, #" STR(FPCR) "]\n" \
        "msr fpcr, x2\n" \
        "ldr x16, [x0, #" STR(LR) "]\n" \
        "ldr x17, [x0, #" STR(SP) "]\n" \
        "ldp x2, x3,   [x0, #" STR(R0) "+16]\n" \
        "ldp x4, x5,   [x0, #" STR(R0) "+32]\n" \
        "ldp x6, x7,   [x0, #" STR(R0) "+48]\n" \
        "ldp x8, x9,   [x0, #" STR(R0) "+64]\n" \
        "ldp x10,x11,  [x0, #" STR(R0) "+80]\n" \
        "ldp x12,x13,  [x0, #" STR(R0) "+96]\n" \
        "ldp x14,x15,  [x0, #" STR(R0) "+112]\n" \
        "ldp x18,x19,  [x0, #" STR(R0) "+144]\n" \
        "ldp x20,x21,  [x0, #" STR(R0) "+160]\n" \
        "ldp x22,x23,  [x0, #" STR(R0) "+176]\n" \
        "ldp x24,x25,  [x0, #" STR(R0) "+192]\n" \
        "ldp x26,x27,  [x0, #" STR(R0) "+208]\n" \
        "ldp x28,x29,  [x0, #" STR(R0) "+224]\n" \
        "mov sp, x17\n" \
        "ldp x0, x1,   [x0, #" STR(R0) "]\n" \
        "mov x0, #0\n" \
        "br  x16\n"

/* ════════════════════════════════════════════════════════════════
 * getcontext — 保存当前现场 (x0-x29, x30, sp, pc, q8-q15, fpsr, fpcr)
 * ════════════════════════════════════════════════════════════════ */
__attribute__((naked, noinline)) int getcontext(ucontext_t *ucp) {
    __asm__ __volatile__(
        "stp x0, x1, [x0, #" STR(R0) "]\n"
        "stp x2, x3, [x0, #" STR(R0) "+16]\n"
        "stp x4, x5, [x0, #" STR(R0) "+32]\n"
        "stp x6, x7, [x0, #" STR(R0) "+48]\n"
        "stp x8, x9, [x0, #" STR(R0) "+64]\n"
        "stp x10,x11,[x0, #" STR(R0) "+80]\n"
        "stp x12,x13,[x0, #" STR(R0) "+96]\n"
        "stp x14,x15,[x0, #" STR(R0) "+112]\n"
        "stp x16,x17,[x0, #" STR(R0) "+128]\n"
        "stp x18,x19,[x0, #" STR(R0) "+144]\n"
        "stp x20,x21,[x0, #" STR(R0) "+160]\n"
        "stp x22,x23,[x0, #" STR(R0) "+176]\n"
        "stp x24,x25,[x0, #" STR(R0) "+192]\n"
        "stp x26,x27,[x0, #" STR(R0) "+208]\n"
        "stp x28,x29,[x0, #" STR(R0) "+224]\n"
        "str x30, [x0, #" STR(LR) "]\n"
        "mov x9, sp\n"
        "str x9,  [x0, #" STR(SP) "]\n"
        "adr x1, 1f\n"
        "str x1,  [x0, #" STR(PC) "]\n"
        /* 浮点: q8..q15 + fpsr/fpcr (存 reserved 区) */
        "stp q8, q9,   [x0, #" STR(Q8) "]\n"
        "stp q10, q11, [x0, #" STR(Q8) "+32]\n"
        "stp q12, q13, [x0, #" STR(Q8) "+64]\n"
        "stp q14, q15, [x0, #" STR(Q8) "+96]\n"
        "mrs x2, fpsr\n"
        "str x2, [x0, #" STR(FPSR) "]\n"
        "mrs x2, fpcr\n"
        "str x2, [x0, #" STR(FPCR) "]\n"
        "mov x0, #0\n"
        "ret\n"
        "1:\n");
}

/* ════════════════════════════════════════════════════════════════
 * makecontext — 在新栈上铺设任务入口
 *   非 naked + 内联汇编：签名含变参 `...`，clang 对 naked+变参 会注入
 *   变参保存 prologue（str x3-x7/q0-q7 写调用者栈）→ 实测 SIGSEGV。
 *   这里让编译器管理寄存器分配，绝对安全。
 *   仅支持 argc==0（freertos 用法）。
 *   新栈顶 16 对齐向下放 [ucp, func]，PC/LR = __ctx_link_entry
 * ════════════════════════════════════════════════════════════════ */
__attribute__((noinline)) void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...) {
    (void)argc;  /* 变参暂不支持：freertos 只用 argc=0 */
    __asm__ __volatile__(
        "ldr  x3, [%0, #16]\n"           /* uc_stack.ss_sp */
        "ldr  x4, [%0, #32]\n"           /* uc_stack.ss_size */
        "add  x3, x3, x4\n"              /* 栈顶 */
        "and  x3, x3, #-16\n"
        "sub  x3, x3, #16\n"             /* [sp]=ucp [sp+8]=func */
        "str  %0, [x3]\n"
        "str  %1, [x3, #8]\n"
        "str  %2, [%0, #" STR(LR) "]\n"  /* 跳入点 → x30 */
        "str  %2, [%0, #" STR(PC) "]\n"
        "str  x3, [%0, #" STR(SP) "]\n"  /* sp = 新栈 */
        "str  xzr, [%0, #" STR(R0) "+232]\n"  /* 清 x29(fp) 防回溯 */
        "mrs  x6, fpcr\n"
        "str  x6, [%0, #" STR(FPCR) "]\n"
        :: "r"(ucp), "r"(func), "r"((void*)__ctx_link_entry)
        : "x3", "x4", "x6", "memory");
}

/* ════════════════════════════════════════════════════════════════
 * setcontext — 切到 ucp（不返回）
 * ════════════════════════════════════════════════════════════════ */
__attribute__((naked, noinline)) int setcontext(const ucontext_t *ucp) {
    __asm__ __volatile__(RESTORE);
}

/* ════════════════════════════════════════════════════════════════
 * swapcontext — 保存 oucp 现场，切到 ucp
 * ════════════════════════════════════════════════════════════════ */
__attribute__((naked, noinline)) int swapcontext(ucontext_t *oucp, const ucontext_t *ucp) {
    __asm__ __volatile__(
        "stp x0, x1, [x0, #" STR(R0) "]\n"
        "stp x2, x3, [x0, #" STR(R0) "+16]\n"
        "stp x4, x5, [x0, #" STR(R0) "+32]\n"
        "stp x6, x7, [x0, #" STR(R0) "+48]\n"
        "stp x8, x9, [x0, #" STR(R0) "+64]\n"
        "stp x10,x11,[x0, #" STR(R0) "+80]\n"
        "stp x12,x13,[x0, #" STR(R0) "+96]\n"
        "stp x14,x15,[x0, #" STR(R0) "+112]\n"
        "stp x16,x17,[x0, #" STR(R0) "+128]\n"
        "stp x18,x19,[x0, #" STR(R0) "+144]\n"
        "stp x20,x21,[x0, #" STR(R0) "+160]\n"
        "stp x22,x23,[x0, #" STR(R0) "+176]\n"
        "stp x24,x25,[x0, #" STR(R0) "+192]\n"
        "stp x26,x27,[x0, #" STR(R0) "+208]\n"
        "stp x28,x29,[x0, #" STR(R0) "+224]\n"
        "str x30, [x0, #" STR(LR) "]\n"
        "mov x8, x1\n"      /* ucp 中转（x1 原值已存入 oucp，可复用）*/
        "mov x9, sp\n"
        "str x9,  [x0, #" STR(SP) "]\n"
        "adr x1, 1f\n"
        "str x1,  [x0, #" STR(PC) "]\n"
        "stp q8, q9,   [x0, #" STR(Q8) "]\n"
        "stp q10, q11, [x0, #" STR(Q8) "+32]\n"
        "stp q12, q13, [x0, #" STR(Q8) "+64]\n"
        "stp q14, q15, [x0, #" STR(Q8) "+96]\n"
        "mrs x2, fpsr\n"
        "str x2, [x0, #" STR(FPSR) "]\n"
        "mrs x2, fpcr\n"
        "str x2, [x0, #" STR(FPCR) "]\n"
        "1:\n"
        "mov x0, x8\n"      /* 真正的 ucp → x0 */
        RESTORE);
}

#else  /* 非 Android aarch64：文件为空 */
#endif
