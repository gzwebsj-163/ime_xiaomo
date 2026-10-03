/**
 * pin_spi_ab.h — SPI 侧 GPIO Matrix A/B 实测（裁决「S3 FSPI 能否任意路由」）
 *
 * 用法：int v; int rc = pin_spi_ab_run(&v);
 *   v ==  1  → SPI matrix 出向通   （FSPI 可路由到任意 GPIO）
 *   v ==  2  → SPI matrix 出向不通 （第一刀必须用 IOMUX 专属脚）
 *   v == -1  → 无效（阳性对照失效，本轮不产出结论）
 *
 * 详见 pin_spi_ab.c 顶部的实验设计与盲点说明。
 */
#ifndef PIN_SPI_AB_H
#define PIN_SPI_AB_H

#ifdef __cplusplus
extern "C" {
#endif

int pin_spi_ab_run(int* verdict);

#ifdef __cplusplus
}
#endif

#endif /* PIN_SPI_AB_H */
