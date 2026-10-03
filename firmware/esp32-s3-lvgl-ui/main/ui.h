/**
 * ui.h — 10 页 UI（9 页复刻 + 第 9 页 PROG FLASH 烧录器，几何 ×2 适配 320x240）
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 构建全部页面并启动动画/轮播定时器。需在 lcd_hw_init() 之后调用。 */
void ui_init(void);

/** 手动切到第 n 页（0..8）；自动轮播会被重置计时。 */
void ui_goto(int n);

/** 整棵 UI 重建（切语言后用：所有文案都要按新语言重取）。 */
void ui_rebuild(void);

/** 直接进入首启向导（首次开机 / 用户在菜单里选「设置向导」时用）。 */
void ui_start_wizard(void);

#ifdef __cplusplus
}
#endif
