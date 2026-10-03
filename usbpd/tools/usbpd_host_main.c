/* usbpd_host_main.c —— 宿主测试驱动 (SIM 侧可测性的证据) */
#include "hw_usbpd.h"
#include <stdio.h>
int hw_usbpd_selftest(void);
int main(void)
{
    printf("=== xiaomo hw_usbpd (USB-C 快充协议采集) 宿主自校验 ===\n\n");
    int fails = hw_usbpd_selftest();
    printf("\n%s\n", fails == 0 ? "SELFTEST ALL PASS" : "SELFTEST FAILED");
    return fails;
}
