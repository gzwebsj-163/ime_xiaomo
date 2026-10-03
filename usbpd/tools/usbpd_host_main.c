/* usbpd_host_main.c —— 宿主验证驱动: 只调 CLI 入口, 不重复实现任何逻辑
 *
 * 纪律 (对齐 tests/dmc_host_main.c): 驱动不实现任何协议逻辑, 只做转发。
 *   否则驱动自己会成为第二个真相源 —— 本轮矩阵要靠 C11/C++17 两套二进制的
 *   输出 md5 逐位一致来判定, 若驱动里有一份"自己的实现", 就不存在对拍可言。
 *
 * 用法: usbpd_host_main usbpd <cmd> [args...]
 *   argv[1] 恒为模块名 (家族约定: main.c 原样传 (argc, argv), 子命令在 argv[2])
 *   见 hw_usbpd_cli() 开头关于 argv 偏移 1 契约的注释。
 *
 * 例: usbpd_host_main usbpd selftest
 *     usbpd_host_main usbpd at 2700
 */
#include <stdio.h>
#include "hw_usbpd.h"

int main(int argc, char** argv)
{
    /* 无参数时给个体面输出, 而不是 usage 洪水 */
    if (argc < 2) {
        printf("usage: usbpd_host_main <module> <cmd> [args...]\n");
        printf("  例: usbpd_host_main usbpd selftest\n");
        return 2;
    }
    return hw_usbpd_cli(argc, argv);
}
