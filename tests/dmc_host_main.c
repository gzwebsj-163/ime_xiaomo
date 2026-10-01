/* hw_dmc 宿主验证驱动: 只调 CLI 入口, 不重复实现任何逻辑
 * 纪律: 驱动不实现任何协议逻辑, 只做转发 —— 否则驱动自己会成为第二个真相源 */
#include <stdio.h>
#include "hw_dmc.h"

int main(int argc, char** argv)
{
    return hw_dmc_cli(argc, argv);
}
