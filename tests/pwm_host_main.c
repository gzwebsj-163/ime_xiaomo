/* ============================================================================
 * tests/pwm_host_main.c —— hw_pwm 宿主 harness
 * ----------------------------------------------------------------------------
 * 家族坑 #29: harness 只许【转发】, 绝不许实现任何协议/判定逻辑。
 * 一旦它顺手写点"便捷逻辑"就成为第二个真相源, 测试结果不再代表模块行为。
 * 所以本文件只做三件事: 转发 pwm_cli / 转发 pwm_selftest / 转发 pwm_cmd。
 * ==========================================================================*/
#include "hw_pwm.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    int rc;

    if (argc >= 2 && strcmp(argv[1], "selftest") == 0) {
        /* 失败计数【直通退出码】(坑 #50): 不加这层, 自检红了进程仍报成功 */
        rc = pwm_selftest(NULL);
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "cmd") == 0) {
        if (argc < 3) { printf("usage: %s cmd <command>\n", argv[0]); return -1; }
        rc = pwm_cmd(argv[2]);
        printf("rc=%d\n", rc);
        return (rc == PWM_OK) ? 0 : 1;
    }
    return pwm_cli(argc, argv);
}
