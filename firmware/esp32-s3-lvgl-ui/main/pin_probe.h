/**
 * pin_probe.h — 按键引脚可用性真机探针（仅 -DPIN_PROBE=1 时编译）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 跑一遍探针（打印 Q1~Q4 全部结论）。产品构建不编译，调用点也被 #if 包住。 */
void pin_probe_run(void);

#ifdef __cplusplus
}
#endif
