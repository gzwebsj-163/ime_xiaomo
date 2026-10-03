#ifndef HW_FAULT_DIAG_H
#define HW_FAULT_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 hw_fault 真机诊断任务 (自动模式探测 + BSP 扫描 + selftest) */
void hw_fault_diag_start(void);

/* 诊断任务本体 (FreeRTOS 任务函数, 仅供 xTaskCreate 使用) */
void hw_fault_diag_task(void *arg);

#ifdef __cplusplus
}
#endif
#endif /* HW_FAULT_DIAG_H */
