/*
 * ch340_demo.c - CH340 4路编程扩展坞 仿真演示
 * 演示完整工作流: 上电 → HUB 枚举 → 串口配置 →
 *                 STM32 ISP 烧录 + 8051 ISP 烧录 → 状态转储
 * 编译: gcc -I. ch340_demo.c ch340_sim.c -o ch340_demo
 */
#include "ch340_sim.h"
#include <stdio.h>

int main(void)
{
    ch_dock_t dock;
    int i, ret;

    printf("======== CH340 4路编程扩展坞 · 全流程仿真 ========\n\n");

    /* 1. 初始化 */
    ch_dock_init(&dock);

    /* 2. 上电 (正常 5V) */
    ret = ch_power_on(&dock, 5.0f);
    printf("[1] 上电 5V -> ret=%d %s\n", ret, ch_power_status(&dock));

    /* 3. HUB 枚举 */
    ch_hub_enumerate(&dock);
    printf("[2] HUB 枚举成功端口数=%d usb_detected=%s\n",
           ch_hub_enumerate(&dock), dock.usb_detected ? "YES" : "NO");

    /* 4. 配置 4 路通道 */
    ch_chan_set_target(&dock, 0, CH_TARGET_STM32);
    ch_chan_set_target(&dock, 1, CH_TARGET_8051);
    ch_chan_set_target(&dock, 2, CH_TARGET_STM32);
    ch_chan_set_target(&dock, 3, CH_TARGET_DEBUG);
    ch_chan_set_baud(&dock, 0, 115200);
    ch_chan_set_baud(&dock, 1, 9600);
    ch_chan_set_baud(&dock, 2, 115200);
    ch_chan_set_baud(&dock, 3, 115200);

    printf("[3] 4 路通道配置完成: CH1=STM32 ISP, CH2=8051 ISP, CH3=STM32 ISP, CH4=DEBUG\n");

    /* 5. STM32 烧录 (CH1) */
    ret = ch_prog_stm32(&dock, 0, 0x8000);
    printf("[4] STM32 ISP 烧录 32KB -> ret=%d\n", ret);

    /* 6. 8051 烧录 (CH2) */
    ret = ch_prog_8051(&dock, 1, 0x1000);
    printf("[5] 8051 ISP 烧录 4KB -> ret=%d\n", ret);

    /* 7. 状态转储 */
    printf("\n[6] 扩展坞最终状态:\n");
    ch_dock_dump(&dock);

    /* 8. 过压保护测试 */
    ret = ch_power_on(&dock, 12.0f);
    printf("\n[7] 过压 12V 测试 -> ret=%d %s\n", ret, ch_power_status(&dock));

    return 0;
}
