/*
 * linux_embed_demo.c — 嵌入式 Linux 内核核心的验证驱动
 *
 * 流程：启动内核 → 等 busybox 提示符 → 逐条注入命令 → 收集输出 → 验证 → 退出
 *
 * 编译后直接运行 ./linux_embed_demo
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linux_embed.h"

#define CFG_PATH "../diskimage-linux-riscv-2018-09-23/root-riscv64-embed.cfg"

static int total_ok = 0, total_fail = 0;

/* 发一条命令，expect 为预期输出子串；打印结果并统计 */
static void run_cmd(LinuxVM *vm, const char *cmd, const char *expect,
                    int timeout_ms)
{
    char buf[65536];
    int n, rc;

    printf("\n\033[1;36m$ %s\033[0m\n", cmd);
    fflush(stdout);

    rc = linux_exec(vm, cmd, expect, timeout_ms);
    n = linux_read_output(vm, buf, (int)sizeof(buf));

    if (n > 0)
        printf("%s", buf);
    else
        printf("(no output)\n");

    if (rc == 0) {
        printf("  \033[1;32m[PASS]\033[0m 出现预期输出: %s\n", expect);
        total_ok++;
    } else if (rc == 1) {
        printf("  \033[1;33m[FAIL]\033[0m 超时未出现: %s\n", expect);
        total_fail++;
    } else {
        printf("  \033[1;31m[ERROR]\033[0m rc=%d\n", rc);
        total_fail++;
    }
}

int main(int argc, char **argv)
{
    LinuxVM *vm;
    const char *cfg = (argc > 1) ? argv[1] : CFG_PATH;

    printf("=== xiaomo 嵌入式 Linux 内核核心 demo ===\n");
    printf("[init] cfg = %s\n", cfg);

    vm = linux_init(cfg);
    if (!vm) {
        fprintf(stderr, "[init] 失败!\n");
        return 1;
    }
    printf("[init] VM 已创建，等待 Linux 内核 boot ...\n");

    /* 阶段 0: 等 busybox 提示符（boot 完成，最长 120s） */
    if (linux_exec(vm, "", "~ #", 120000) != 0) {
        fprintf(stderr, "[boot] 超时未等到 shell 提示符!\n");
        linux_dump_output(vm);
        linux_end(vm);
        return 1;
    }
    printf("[boot] ✅ shell 就绪\n");

    /* 阶段 1: 系统信息 */
    run_cmd(vm, "uname -a", "Linux", 15000);
    run_cmd(vm, "id", "uid=0", 15000);
    run_cmd(vm, "cat /proc/version", "Linux version", 15000);
    run_cmd(vm, "cat /proc/cpuinfo", "rv64", 15000);
    run_cmd(vm, "free", "Mem", 15000);
    run_cmd(vm, "ls /", "bin", 15000);

    /* 阶段 2: 计算能力验证（内核里跑点活）
     * ⚠️ 嵌入 rootfs 的 busybox 没有 bc — 旧用例 echo '2+2=4' | bc 是假通过
     *  (expect="4" 匹配的是命令回显里的 '4', bc 从未运行)。
     * 改用 ash 内建算术 $((...)): 回显 echo $((7*6)) 不含 "42", 只有真实算出
     *  的 42 才会命中 expect, 唯一对应真实计算输出。 */
    run_cmd(vm, "echo $((7*6))", "42", 15000);
    run_cmd(vm, "echo LINUX-EMBED-OK", "LINUX-EMBED-OK", 15000);

    printf("\n=== 结果汇总: PASS=%d FAIL=%d ===\n", total_ok, total_fail);

    linux_end(vm);
    printf("[end] VM 已关闭\n");
    return (total_fail == 0) ? 0 : 2;
}
