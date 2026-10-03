#ifndef DC_DIAG_H
#define DC_DIAG_H

/* hw_dc 真机验证桥: 启动后 A/B/C 三段自检并退出任务
 *   Phase A = 默认模拟 BSP 的确定性路径 (真机 == 宿主逐位一致)
 *   Phase B = 注入真机 BSP (片上 ADC + GPIO), 且不依赖任何外部器件自证
 *   Phase C = DCPP 帧层真机传输 (UART1 内部回环): 帧真的能走线
 */
void dc_diag_start(void);

#endif
