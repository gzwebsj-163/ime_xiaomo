#ifndef HW_FLASH_DIAG_H
#define HW_FLASH_DIAG_H

/* hw_flash 真机验证桥: 启动后 A/B 两段自检并退出任务
 *   Phase A = 默认 ROM 模拟器 BSP 的确定性路径 (真机 == 宿主逐位一致)
 *   Phase B = 注入真机 BSP (UART1 内部回环) 验真实硅字节通道
 */
void hw_flash_diag_start(void);

#endif
