/*
 * xiaomo debug 工具 —— 排障三层证据链统一入口 (2026-09-28)
 *
 * 历史排障手法工具化 (AST dump → 反汇编 → 运行期 trace 曾靠三处散装环境变量):
 *   1) xiaomo debug ast  <file.mo>              AST dump (语义层)
 *   2) xiaomo debug bc   <file.kbc|file.mo>     字节码反汇编 + 常量池/函数表 (编译层)
 *   3) xiaomo debug tr   <file> [opts]          运行期指令级 trace/断点/watch (执行层)
 *      --max N         最多 trace N 步后停 (默认 40, 0=跑完)
 *      --bp pc[,pc..]  断点: 落到指定 pc 停机并 dump 现场
 *      --watch rN[..]  观察寄存器: 值变化即打点 (r3 或 3 均可)
 *      --calltrace     只打控制流 (CALL/RET/跳转/HALT)
 *   4) xiaomo debug regs <file> [--all]         跑到底 dump 终态
 *
 * 双格式加载: .kbc 直接反序列化; 其他后缀 (.mo) 先解析再 mo2kbc 编译。
 * 停机四态: HALT 跑完 / 断点命中 / trace 上限 / 错误 (error_msg)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debug_tool.h"
#include "parser.h"    /* xiaomo_parse_source */
#include "ast.h"       /* ast_dump */
#include "vm_core.h"    /* KillsVM/KillsProgram/kvm_* */
#include "vm_stack.h"   /* vstack_used */
#include "mo2kbc.h"     /* mo2kbc_compile */

/* ================= 加载 ================= */

static char* dbg_read_file(const char* path, long* out_sz) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = '\0';
    fclose(f);
    if (out_sz) *out_sz = (long)rd;
    return buf;
}

/* 双格式: .kbc 反序列化; .mo 解析+编译 */
static KillsProgram* dbg_load(const char* path, char* err, int errlen) {
    size_t n = strlen(path);
    if (n >= 4 && strcmp(path + n - 4, ".kbc") == 0) {
        long sz = 0;
        char* buf = dbg_read_file(path, &sz);
        if (!buf) { snprintf(err, (size_t)errlen, "无法读取文件: %s", path); return NULL; }
        KillsProgram* p = kprog_deserialize((const uint8_t*)buf, sz, err, errlen);
        free(buf);
        if (!p) snprintf(err, (size_t)errlen, "kbc 反序列化失败: %s", err);
        return p;
    }
    char* src = dbg_read_file(path, NULL);
    if (!src) { snprintf(err, (size_t)errlen, "无法读取文件: %s", path); return NULL; }
    AstNode* program = xiaomo_parse_source(src, err, (size_t)errlen);
    free(src);
    if (!program) { snprintf(err, (size_t)errlen, "解析失败: %s", err); return NULL; }
    KillsProgram* kp = mo2kbc_compile(program, err, (size_t)errlen);
    ast_free(program);
    if (!kp) snprintf(err, (size_t)errlen, "编译失败: %s", err);
    return kp;
}

/* ================= 状态 dump ================= */

static void dbg_print_outputs(struct KillsVM* vm, int tail) {
    int cnt = (int)kvm_output_count(vm);
    if (cnt == 0) { printf("(无程序输出)\n"); return; }
    int from = tail > 0 && cnt > tail ? cnt - tail : 0;
    for (int i = from; i < cnt; i++) printf("[out] %s\n", kvm_output(vm, i));
    if (from > 0) printf("[out] ... (共 %d 条, 显示尾部 %d 条)\n", cnt, cnt - from);
}

static void dbg_dump_regs(struct KillsVM* vm, int all) {
    int col = 0;
    for (int i = 0; i < (int)KILLS_NREG; i++) {
        int64_t v = vm->regs[i];
        if (!all && v == 0) continue;
        printf("R%d=%lld%s", i, (long long)v, (++col % 4 == 0) ? "\n" : "  ");
    }
    if (col % 4 != 0) printf("\n");
    if (col == 0) printf("(所有寄存器为 0)\n");
}

/* ================= trace 钩子 ================= */

#define DBG_MAX_BP   64
#define DBG_MAX_WATCH 32

typedef struct {
    uint32_t max_steps;   /* 0 = 无限制 */
    int      limit_hit;   /* trace 达上限停机 */
    int      bp_hit;      /* 断点命中停机 */
    uint32_t bps[DBG_MAX_BP];
    int      nbp;
    int      watch[DBG_MAX_WATCH];   /* 寄存器下标 */
    int64_t  wbase[DBG_MAX_WATCH];   /* watch 基线值 */
    int      nwatch;
    int      calltrace;
    uint64_t traced;       /* 已打步数 */
    uint64_t steps_done;   /* 总步数 */
} DbgState;

static int dbg_is_ctrl(uint8_t op) {
    switch (op) {
    case OP_JMP: case OP_JZ: case OP_JNZ: case OP_JE: case OP_JNE:
    case OP_JG: case OP_JGE: case OP_JL: case OP_JLE:
    case OP_CALL: case OP_RET: case OP_HALT:
        return 1;
    default:
        return 0;
    }
}

/* 返回非 0 -> kvm_run 提前停机 */
static int dbg_hook(void* ud, struct KillsVM* vm, const KillsIns* ins,
                    uint32_t next_pc, uint64_t steps) {
    DbgState* st = (DbgState*)ud;
    (void)vm;
    st->steps_done = steps;

    /* 1) watchpoint: 值变化打点 (基线比较, 与步末语义等价) */
    for (int i = 0; i < st->nwatch; i++) {
        int r = st->watch[i];
        int64_t v = vm->regs[r];
        if (v != st->wbase[i]) {
            printf("      [W] r%d: %lld -> %lld\n", r,
                   (long long)st->wbase[i], (long long)v);
            st->wbase[i] = v;
        }
    }

    /* 2) 单行 trace */
    uint32_t pc = vm->pc;
    if (!st->calltrace || dbg_is_ctrl(ins->op)) {
        if (st->max_steps == 0 || st->traced < st->max_steps) {
            printf("[#%llu] pc=%-4u %-12s a=%d b=%d imm=%lld",
                   (unsigned long long)steps, pc, kvm_op_name(ins->op),
                   ins->a, ins->b, (long long)ins->imm);
            if (dbg_is_ctrl(ins->op) && ins->op != OP_RET && ins->op != OP_HALT)
                printf(" -> pc=%u%s", next_pc, next_pc != pc + 1 ? " *" : "");
            printf("\n");
            st->traced++;
        }
    }

    /* 3) trace 上限停机 */
    if (st->max_steps > 0 && steps >= st->max_steps) {
        st->limit_hit = 1;
        return 1;
    }

    /* 4) 断点: 命中 dump 现场并停机 */
    for (int i = 0; i < st->nbp; i++) {
        if (st->bps[i] == pc) {
            st->bp_hit = 1;
            printf("== 断点命中: pc=%u (%s) step=%llu | 栈 op=%d call=%d\n",
                   pc, kvm_op_name(ins->op), (unsigned long long)steps,
                   (int)(vstack_used(&vm->operand) / 8),
                   (int)(vstack_used(&vm->callstack) / 8));
            return 1;
        }
    }
    return 0;
}

static void dbg_report(struct KillsVM* vm, const KillsProgram* kp,
                       DbgState* st, int rc) {
    printf("-- 停机报告 ----------------------------------------\n");
    if (rc != 0)                 printf("状态: 错误 (%s)\n", vm->error_msg);
    else if (st && st->bp_hit)   printf("状态: 断点命中, 停于 pc=%u (代码共 %u 条)\n",
                                        vm->pc, kp->code_count);
    else if (st && st->limit_hit) printf("状态: trace 上限 (%u 步), 停于 pc=%u\n",
                                         st->max_steps, vm->pc);
    else if (vm->halted)         printf("状态: HALT 正常跑完 (%llu 步)\n",
                                        (unsigned long long)vm->steps);
    else                         printf("状态: 退出 (pc=%u/%u)\n", vm->pc, kp->code_count);
    printf("pc=%u steps=%llu 栈: op=%d call=%d | 错误: %d\n",
           vm->pc, (unsigned long long)vm->steps,
           (int)(vstack_used(&vm->operand) / 8),
           (int)(vstack_used(&vm->callstack) / 8), vm->error_count);
    if (vm->sig_violations)
        printf("R127 越权写计数: %u (写屏蔽生效)\n", vm->sig_violations);
    printf("非零寄存器:\n");
    dbg_dump_regs(vm, 0);
}

/* ================= 子命令 ================= */

static int dbg_usage(void) {
    printf("xiaomo debug —— 排障三层证据链 (ast -> bc -> tr)\n");
    printf("用法:\n");
    printf("  xiaomo debug ast  <file.mo>              AST dump (第 1 层)\n");
    printf("  xiaomo debug bc   <file.kbc|file.mo> [--limit N]\n");
    printf("                                          反汇编+常量池+函数表 (第 2 层)\n");
    printf("  xiaomo debug tr   <file.kbc|file.mo> [--max N] [--bp pc[,pc..]]\n");
    printf("                    [--watch r3[,r5..]] [--calltrace]\n");
    printf("                                          运行期 trace/断点/watch (第 3 层)\n");
    printf("  xiaomo debug regs <file.kbc|file.mo> [--all]\n");
    printf("                                          跑到底 dump 终态寄存器\n");
    return 1;
}

static int cmd_ast(const char* path) {
    char errbuf[1024] = "";
    char* src = dbg_read_file(path, NULL);
    if (!src) { fprintf(stderr, "无法读取文件: %s\n", path); return 1; }
    AstNode* program = xiaomo_parse_source(src, errbuf, sizeof(errbuf));
    free(src);
    if (!program) { fprintf(stderr, "解析失败: %s\n", errbuf); return 1; }
    ast_dump(program, 0);
    ast_free(program);
    return 0;
}

static int cmd_bc(const char* path, int limit) {
    char errbuf[1024] = "";
    KillsProgram* kp = dbg_load(path, errbuf, (int)sizeof(errbuf));
    if (!kp) { fprintf(stderr, "%s\n", errbuf); return 1; }
    printf("; Kills program: %u ins, %u consts, %u funcs, data=%u\n",
           kp->code_count, kp->const_count, kp->func_count, kp->data_size);
    if (limit <= 0 || (uint32_t)limit > kp->code_count) limit = (int)kp->code_count;
    for (uint32_t i = 0; i < (uint32_t)limit; i++) {
        KillsIns* ins = &kp->code[i];
        printf("  %4u: %-12s a=%d b=%d imm=%lld\n",
               i, kvm_op_name(ins->op), ins->a, ins->b, (long long)ins->imm);
    }
    if ((uint32_t)limit < kp->code_count)
        printf("  ... (共 %u 条, 显示前 %d)\n", kp->code_count, limit);
    if (kp->const_count > 0) {
        printf("常量池:\n");
        for (uint32_t i = 0; i < kp->const_count && i < 32; i++) {
            KillsConst* c = &kp->consts[i];
            if (c->type == 1)      printf("  [%u] str \"%s\"\n", i, c->sv ? c->sv : "");
            else if (c->type == 2) printf("  [%u] float %f\n", i, c->fv);
            else                   printf("  [%u] int %lld\n", i, (long long)c->iv);
        }
        if (kp->const_count > 32) printf("  ... (共 %u 条)\n", kp->const_count);
    }
    if (kp->func_count > 0) {
        printf("函数表:\n");
        for (uint32_t i = 0; i < kp->func_count && i < 32; i++)
            printf("  [%u] %s pc=%u nparams=%u\n", i,
                   kp->funcs[i].name, kp->funcs[i].pc, kp->funcs[i].nparams);
        if (kp->func_count > 32) printf("  ... (共 %u 条)\n", kp->func_count);
    }
    kprog_free(kp);
    return 0;
}

/* 解析逗号分隔整型列表: "12,34" -> {12,34}, 带 r 前缀兼容 (r3 或 3) */
static int dbg_parse_list(const char* s, int64_t* out, int maxn) {
    int n = 0;
    while (s && *s && n < maxn) {
        if (*s == 'r' || *s == 'R') s++;
        char* end = NULL;
        long long v = strtoll(s, &end, 0);
        if (end == s) break;
        out[n++] = v;
        s = (*end == ',') ? end + 1 : end;
    }
    return n;
}

static int cmd_tr(const char* path, uint32_t max_steps, const char* bps_s,
                  const char* watch_s, int calltrace) {
    char errbuf[1024] = "";
    KillsProgram* kp = dbg_load(path, errbuf, (int)sizeof(errbuf));
    if (!kp) { fprintf(stderr, "%s\n", errbuf); return 1; }

    DbgState st;
    memset(&st, 0, sizeof(st));
    st.max_steps = max_steps;
    st.calltrace = calltrace;
    if (bps_s) {
        int64_t tmp[DBG_MAX_BP];
        int n = dbg_parse_list(bps_s, tmp, DBG_MAX_BP);
        for (int i = 0; i < n; i++) st.bps[st.nbp++] = (uint32_t)tmp[i];
    }
    if (watch_s) {
        int64_t tmp[DBG_MAX_WATCH];
        int n = dbg_parse_list(watch_s, tmp, DBG_MAX_WATCH);
        for (int i = 0; i < n && i < (int)KILLS_NREG; i++) st.watch[st.nwatch++] = (int)tmp[i];
    }

    KillsVM vm;
    kvm_init(&vm);
    vm.dbg_ud = &st;
    vm.dbg_hook = dbg_hook;

    printf("; trace %s: %u ins | max=%u bp=%d watch=%d%s\n",
           path, kp->code_count, st.max_steps, st.nbp, st.nwatch,
           calltrace ? " calltrace" : "");
    for (int i = 0; i < st.nwatch; i++) st.wbase[i] = vm.regs[st.watch[i]];
    int rc = kvm_run(&vm, kp);

    dbg_report(&vm, kp, &st, rc);

    printf("程序输出:\n");
    dbg_print_outputs(&vm, 8);

    kvm_free(&vm);
    kprog_free(kp);
    return rc;
}

static int cmd_regs(const char* path, int all) {
    char errbuf[1024] = "";
    KillsProgram* kp = dbg_load(path, errbuf, (int)sizeof(errbuf));
    if (!kp) { fprintf(stderr, "%s\n", errbuf); return 1; }
    KillsVM vm;
    kvm_init(&vm);
    int rc = kvm_run(&vm, kp);
    if (rc != 0)
        fprintf(stderr, "执行错误: %s\n", vm.error_msg);
    printf("; regs %s: %u ins | %s (steps=%llu)\n", path, kp->code_count,
           vm.halted ? "HALT 跑完" : "中途退出",
           (unsigned long long)vm.steps);
    dbg_report(&vm, kp, NULL, rc);
    printf("程序输出:\n");
    dbg_print_outputs(&vm, 8);
    if (all) { printf("全寄存器 (--all):\n"); dbg_dump_regs(&vm, 1); }
    kvm_free(&vm);
    kprog_free(kp);
    return rc;
}

int debug_tool_main(int argc, char** argv) {
    /* 约定: argv[0]="debug", argv[1]=子命令 */
    if (argc < 2) return dbg_usage();
    const char* sub = argv[1];
    if (strcmp(sub, "ast") == 0 && argc >= 3) return cmd_ast(argv[2]);
    if (strcmp(sub, "bc") == 0 && argc >= 3) {
        int limit = 0;
        for (int i = 3; i + 1 < argc; i++)
            if (strcmp(argv[i], "--limit") == 0) limit = atoi(argv[i + 1]);
        return cmd_bc(argv[2], limit <= 0 ? 50 : limit);
    }
    if (strcmp(sub, "tr") == 0 && argc >= 3) {
        uint32_t max_steps = 40;
        const char* bps_s = NULL, *watch_s = NULL;
        int calltrace = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "--max") == 0 && i + 1 < argc)
                max_steps = (uint32_t)strtoul(argv[++i], NULL, 0);
            else if (strcmp(argv[i], "--bp") == 0 && i + 1 < argc)
                bps_s = argv[++i];
            else if (strcmp(argv[i], "--watch") == 0 && i + 1 < argc)
                watch_s = argv[++i];
            else if (strcmp(argv[i], "--calltrace") == 0)
                calltrace = 1;
        }
        return cmd_tr(argv[2], max_steps, bps_s, watch_s, calltrace);
    }
    if (strcmp(sub, "regs") == 0 && argc >= 3) {
        int all = 0;
        for (int i = 3; i < argc; i++)
            if (strcmp(argv[i], "--all") == 0) all = 1;
        return cmd_regs(argv[2], all);
    }
    if (strcmp(sub, "help") == 0 || strcmp(sub, "-h") == 0) return dbg_usage();
    fprintf(stderr, "未知 debug 子命令: %s\n", sub);
    return dbg_usage();
}
