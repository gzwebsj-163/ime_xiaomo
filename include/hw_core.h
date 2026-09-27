#ifndef HW_CORE_H
#define HW_CORE_H
#include <stdint.h>

#define HW_CORE_BASE_SIGNAL 0x0000U
#define HW_CORE_BASE_DATA   0x0D00U
#define HW_CORE_BASE_FLOW   0x0E00U
#define HW_CORE_BASE_TERM   0x0F00U

#define HW_CORE_SUB_EF    0x00efU
#define HW_CORE_SUB_DD    0x00ddU
#define HW_CORE_SUB_EE    0x00eeU
#define HW_CORE_SUB_59    0x0059U
#define HW_CORE_SUB_80    0x0080U
#define HW_CORE_SUB_100   0x0100U
#define HW_CORE_SUB_FF    0x00ffU
#define HW_CORE_SUB_FD    0x00fdU

#define HW_CORE_DEF    (HW_CORE_BASE_DATA | HW_CORE_SUB_EF)
#define HW_CORE_DFF    (HW_CORE_BASE_DATA | HW_CORE_SUB_FF)
#define HW_CORE_SIN    (HW_CORE_BASE_SIGNAL | HW_CORE_SUB_EF)
#define HW_CORE_SNG    (HW_CORE_BASE_SIGNAL | HW_CORE_SUB_DD)
#define HW_CORE_SID    (HW_CORE_BASE_SIGNAL | HW_CORE_SUB_EE)
#define HW_CORE_DFT    (HW_CORE_BASE_SIGNAL | 0x0059U)
#define HW_CORE_DDT    (HW_CORE_BASE_SIGNAL | 0x0080U)
#define HW_CORE_ODT    (HW_CORE_BASE_SIGNAL | 0x0100U)
#define HW_CORE_FFQ    (HW_CORE_BASE_FLOW | HW_CORE_SUB_FF)
#define HW_CORE_TTU    (HW_CORE_BASE_TERM | HW_CORE_SUB_FD)

#ifndef HW_CORE_MAX
#define HW_CORE_MAX 10U
#endif

#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
    CORE_DNA_DEF = HW_CORE_DEF,
    CORE_DNA_DFF = HW_CORE_DFF,
    CORE_DNA_SIN = HW_CORE_SIN,
    CORE_DNA_SNG = HW_CORE_SNG,
    CORE_DNA_SID = HW_CORE_SID,
    CORE_DNA_DFT = HW_CORE_DFT,
    CORE_DNA_DDT = HW_CORE_DDT,
    CORE_DNA_ODT = HW_CORE_ODT,
    CORE_DNA_FFQ = HW_CORE_FFQ,
    CORE_DNA_TTU = HW_CORE_TTU,
} core_dna;

void core_init(int handler[]);
void core_deinit(uint16_t handler[]);

#ifdef __cplusplus
}

auto core_ject(int point[]);
auto core_deject(uint16_t point[]);
#endif

#define AUTO(var, expr) __auto_type var = (expr)
#define AUTO_PTR(var, expr) __auto_type *var = &(expr)

#define CORE_DNA_FOREACH(it) \
    for (AUTO(it, 0U); (it) < HW_CORE_MAX; ++(it))

#endif
