/* usbpd_hw.h —— ESP32-S3 真机 BSP + 验证桥 的共用声明
 *
 * ⚠️ 本工程的"真机"到底真在哪 (先说清边界, 免得把桩当实据):
 *   本板【没有】USB-C 母座 + 分压电阻 + FUSB302。所以真机验的不是
 *   "D+/D- 电压判读正确"(那需要接一个真实充电器), 而是:
 *     1. 模块能在 xtensa 真指令集上跑 (freestanding 只证"能编", 不证"能跑")
 *     2. BSP 装上后 is_sim() 真的翻成 REAL
 *     3. ADC 读到的是【真悬空脚】的噪声 => 悬空闸门必须在真硬件上触发
 *        (这一条是本工程最有价值的真机项: 宿主桩喂的是人工构造的 9999mV,
 *         真机上没有构造, 只有物理拾波)
 *     4. 未装 BSP 仍诚实失败, 装上后失败原因从 NODEV 变 HW/ERR_FLOAT
 */
#ifndef USBPD_HW_H
#define USBPD_HW_H

#include "hw_usbpd.h"

/* 真机 BSP 装/卸 */
int  usbpd_hw_install(void);
void usbpd_hw_uninstall(void);

/* ADC 引脚体检 (仅换板排查, 默认关) */
int  usbpd_hw_pin_scan(void);

/* 验证桥: 返回 fails 计数 (0 = 全过) */
int  usbpd_diag_run(void);

#endif
