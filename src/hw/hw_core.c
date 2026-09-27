#include "hw_core.h"
#include <string.h>

// 全局内核DNA常量数组，存放全部编码
static const core_dna core_dna_table[HW_CORE_MAX] = {
    CORE_DNA_DEF,
    CORE_DNA_DFF,
    CORE_DNA_SIN,
    CORE_DNA_SNG,
    CORE_DNA_SID,
    CORE_DNA_DFT,
    CORE_DNA_DDT,
    CORE_DNA_ODT,
    CORE_DNA_FFQ,
    CORE_DNA_TTU,
};

const core_dna* core_dna_get_table(void)
{
    return core_dna_table;
}

/**
 * @brief 硬件内核初始化，加载handler句柄数组
 * @param handler int类型句柄数组
 */
void core_init(int handler[])
{
    if(handler == NULL)
    {
        return;
    }
    // 可在此挂载DNA译码回调、硬件状态机复位
    CORE_DNA_FOREACH(i)
    {
        handler[i] = (int)core_dna_table[i];
    }
}

/**
 * @brief 内核反初始化，清理handler资源
 * @param handler uint16_t句柄数组
 */
void core_deinit(uint16_t handler[])
{
    if(handler == NULL)
    {
        return;
    }
    memset(handler, 0, sizeof(uint16_t)*HW_CORE_MAX);
}

#ifdef __cplusplus
// C++ auto返回实现
auto core_ject(int point[])
{
    if(!point) return (core_dna)0;
    return (core_dna)point[0];
}

auto core_deject(uint16_t point[])
{
    if(!point) return (core_dna)0;
    return (core_dna)point[0];
}
#endif
