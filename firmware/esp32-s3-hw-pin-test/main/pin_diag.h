#ifndef PIN_DIAG_H
#define PIN_DIAG_H

/* hw_pin 真机验证桥: 启动后 A/B/C/L0 四段自检并退出任务
 *   Phase A  = 默认模拟 BSP 的确定性路径 (真机 == 宿主逐位一致)
 *   Phase B  = 真实硅引脚面 (GPIO 内部上下拉自证 / 模块==硬件对拍 / 诚实失败)
 *   Phase C  = 真实异步链路 (UART1 内部回环 + UART1↔UART2 完整 ISP 烧录)
 *   Phase L0 = 编程电压层 (LEDC + ADC 回读闭环)
 */
void pin_diag_start(void);

#endif
