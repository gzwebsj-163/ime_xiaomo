/* hw_wdbg 宿主验证驱动: 只调 CLI 入口, 不重复实现任何逻辑
 * 纪律: 驱动不实现任何协议逻辑, 只做转发 —— 否则驱动自己会成为第二个真相源
 *       (锚点: 自证工具与被测物共享盲点; dmc 已有同款先例 tests/dmc_host_main.c)
 *
 * 存在理由: hw_wdbg.c 是纯模块文件, 没有 main (CLI 入口由 src/main.c 转调),
 *           而矩阵脚本需要独立二进制跑「C11 vs C++17 逐位对拍」。
 *           若直接链接 src/main.c 就把整个 VM 拉进来, 双形态对拍会掺入
 *           与 wdbg 无关的变量 —— 故用本驱动单独把 CLI 提出来。 */
#include <stdio.h>
#include "hw_wdbg.h"

int main(int argc, char** argv)
{
    return hw_wdbg_cli(argc, argv);
}
