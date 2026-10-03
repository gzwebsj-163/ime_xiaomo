#ifndef HW_WDBG_DIAG_H
#define HW_WDBG_DIAG_H

/* hw_wdbg 真机验证桥: 启动后 A/B 两段自检并退出任务
 *   Phase A = 默认模拟器 BSP 的确定性路径 (全跨逐位一致)
 *   Phase B = 注入真机 BSP (UART1 内部回环 + LEDC PWM) 验真实硅
 */
void hw_wdbg_diag_start(void);

#endif
