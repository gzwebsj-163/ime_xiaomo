/*
 * xiaomo - 硬件探测演示 (Kills 字节码)
 *
 * 演示用 Kills VM 指令枚举 PCI 设备 / USB 设备 / 串口 / CPU 信息
 * 通过 ./xiaomo hwprobe 运行
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "vm_core.h"
#include <stdio.h>

/*
 * 构建硬件探测演示程序
 * 返回 KillsProgram*, 调用方负责 kprog_free
 */
KillsProgram* hw_demo_build(void) {
    KillsProgram* p = kprog_new();

    /* -- 添加常量 (标题字符串) -- */
    int c_title_pci    = kprog_add_const(p, 1, 0, 0, "=== PCI 设备 ===");
    int c_title_usb    = kprog_add_const(p, 1, 0, 0, "=== USB 设备 ===");
    int c_title_serial = kprog_add_const(p, 1, 0, 0, "=== 串口设备 ===");
    int c_title_cpu    = kprog_add_const(p, 1, 0, 0, "=== CPU 信息 ===");
    int c_title_mem    = kprog_add_const(p, 1, 0, 0, "=== 系统内存 ===");
    int c_title_gpu    = kprog_add_const(p, 1, 0, 0, "=== GPU 探测 (PCI 00:02.0) ===");
    int c_sep          = kprog_add_const(p, 1, 0, 0, "---");
    int c_fmt_vendor   = kprog_add_const(p, 1, 0, 0, "Vendor+Device: ");
    int c_fmt_class    = kprog_add_const(p, 1, 0, 0, "Class: ");
    int c_fmt_total    = kprog_add_const(p, 1, 0, 0, "Total: ");
    int c_fmt_free     = kprog_add_const(p, 1, 0, 0, "Free: ");
    int c_fmt_gb       = kprog_add_const(p, 1, 0, 0, " GB");
    int c_fmt_mb       = kprog_add_const(p, 1, 0, 0, " MB");

    /*
     * 程序流程:
     *   0: 打印标题 PCI
     *   1: HW_PCI_ENUM
     *   2: 打印分隔线
     *   3: 打印标题 USB
     *   4: HW_USB_ENUM
     *   5: 打印分隔线
     *   6: 打印标题 Serial
     *   7: HW_SERIAL_ENUM
     *   8: 打印分隔线
     *   9: 打印标题 CPU
     *  10: HW_CPU_INFO
     *  11: 打印分隔线
     *  12: 打印标题 Mem
     *  13: HW_SYS_INFO (R0=total, R1=free)
     *  14: PRINT R0
     *  15: PRINT R1
     *  16: 打印标题 GPU
     *  17: HW_PCI_RD bus=0,dev=2,func=0,offset=0 → R2
     *  18: PRINT R2
     *  19: HW_PCI_RD bus=0,dev=2,func=0,offset=8 → R3
     *  20: PRINT R3
     *  21: HALT
     */

    int pc = 0;
    /* 0: PRINT "=== PCI 设备 ===" */
    kprog_add_ins(p, OP_PRINT, -1, c_title_pci, 0);
    /* 1: HW_PCI_ENUM */
    kprog_add_ins(p, OP_HW_PCI_ENUM, 0, 0, 0);
    /* 2: PRINT "---" */
    kprog_add_ins(p, OP_PRINT, -1, c_sep, 0);
    /* 3: PRINT "=== USB 设备 ===" */
    kprog_add_ins(p, OP_PRINT, -1, c_title_usb, 0);
    /* 4: HW_USB_ENUM */
    kprog_add_ins(p, OP_HW_USB_ENUM, 0, 0, 0);
    /* 5: PRINT "---" */
    kprog_add_ins(p, OP_PRINT, -1, c_sep, 0);
    /* 6: PRINT "=== 串口设备 ===" */
    kprog_add_ins(p, OP_PRINT, -1, c_title_serial, 0);
    /* 7: HW_SERIAL_ENUM */
    kprog_add_ins(p, OP_HW_SERIAL_ENUM, 0, 0, 0);
    /* 8: PRINT "---" */
    kprog_add_ins(p, OP_PRINT, -1, c_sep, 0);
    /* 9: PRINT "=== CPU 信息 ===" */
    kprog_add_ins(p, OP_PRINT, -1, c_title_cpu, 0);
    /* 10: HW_CPU_INFO */
    kprog_add_ins(p, OP_HW_CPU_INFO, 0, 0, 0);
    /* 11: PRINT "---" */
    kprog_add_ins(p, OP_PRINT, -1, c_sep, 0);
    /* 12: PRINT "=== 系统内存 ===" */
    kprog_add_ins(p, OP_PRINT, -1, c_title_mem, 0);
    /* 13: HW_SYS_INFO → R0=total, R1=free */
    kprog_add_ins(p, OP_HW_SYS_INFO, 0, 1, 0);
    /* 14: PRINT "Total: " */
    kprog_add_ins(p, OP_PRINT, -1, c_fmt_total, 1);
    /* 15: PRINT R0 (total bytes) */
    kprog_add_ins(p, OP_PRINT, 0, -1, 0);
    /* 16: 计算 GB: R0 = R0 / 1024/1024/1024 — 简化: 直接打印原始值 */
    /* 17: PRINT " GB" */
    kprog_add_ins(p, OP_PRINT, -1, c_fmt_gb, 0);
    /* 18: PRINT "Free: " */
    kprog_add_ins(p, OP_PRINT, -1, c_fmt_free, 1);
    /* 19: PRINT R1 (free bytes) */
    kprog_add_ins(p, OP_PRINT, 1, -1, 0);
    /* 20: PRINT " MB" */
    kprog_add_ins(p, OP_PRINT, -1, c_fmt_mb, 0);
    /* 21: PRINT "---" */
    kprog_add_ins(p, OP_PRINT, -1, c_sep, 0);
    /* 22: PRINT "=== GPU 探测 (PCI 00:02.0) ===" */
    kprog_add_ins(p, OP_PRINT, -1, c_title_gpu, 0);
    /* 23: HW_PCI_RD bus=0, dev=2, func=0, offset=0x00 (Vendor+Device) → R2
     *     imm = (dev<<16 | func<<8 | offset) = (2<<16 | 0<<8 | 0) = 0x00020000 */
    kprog_add_ins(p, OP_HW_PCI_RD, 2, 0, 0x00020000);
    /* 24: PRINT "Vendor+Device: " */
    kprog_add_ins(p, OP_PRINT, -1, c_fmt_vendor, 1);
    /* 25: PRINT R2 (vendor+device) */
    kprog_add_ins(p, OP_PRINT, 2, -1, 0);
    /* 26: HW_PCI_RD bus=0, dev=2, func=0, offset=0x08 (Class+Revision) → R3
     *     imm = (2<<16 | 0<<8 | 8) = 0x00020008 */
    kprog_add_ins(p, OP_HW_PCI_RD, 3, 0, 0x00020008);
    /* 27: PRINT "Class: " */
    kprog_add_ins(p, OP_PRINT, -1, c_fmt_class, 1);
    /* 28: PRINT R3 (class+revision) */
    kprog_add_ins(p, OP_PRINT, 3, -1, 0);
    /* 29: HALT */
    kprog_add_ins(p, OP_HALT, 0, 0, 0);

    return p;
}

/*
 * 运行硬件探测演示
 * 返回 0 成功, 非 0 失败
 */
int hw_demo_run(void) {
    KillsProgram* p = hw_demo_build();
    if (!p) {
        printf("构建硬件探测程序失败\n");
        return 1;
    }

    printf("=== xiaomo 硬件探测演示 ===\n\n");

    /* 反汇编 */
    char dis[4096];
    kvm_disassemble(p, dis, sizeof(dis));
    printf("--- 反汇编 ---\n%s\n", dis);

    /* 执行 */
    KillsVM vm;
    kvm_init(&vm);
    int rc = kvm_run(&vm, p);
    if (rc != 0 && vm.error_count > 0) {
        printf("硬件探测错误: %s\n", vm.error_msg);
    }

    printf("\n--- 结果 ---\n");
    for (int i = 0; i < kvm_output_count(&vm); i++) {
        printf("%s\n", kvm_output(&vm, i));
    }

    kvm_free(&vm);
    kprog_free(p);
    return rc;
}

#ifdef __cplusplus
}
#endif