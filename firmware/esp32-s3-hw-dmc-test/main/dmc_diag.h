#ifndef DMC_DIAG_H
#define DMC_DIAG_H

/* hw_dmc 真机验证桥: 启动后 A/B 两段自检并退出任务
 *   Phase A = 确定性路径 (真机 == 宿主逐位一致: 黄金/CRC 向量/表/自检/往返)
 *   Phase B = 真机 UART1 内部回环: 帧真的能走线 (不需任何外部器件)
 */
void dmc_diag_start(void);

#endif
