/*
 * xiaomo - 权重加载器 (Weight Loader)
 *
 * "造模型 → 接真实权重跑推理" 的内核地基:
 * 从外部 JSON 权重文件读取一层参数 (矩阵/向量) 并转成 MoValue(VAL_ARRAY),
 * 使 .mo 计算图能直接使用训练好/导出的真实权重, 而不是硬编码在源码里。
 *
 * JSON 权重文件格式 (简洁子集, 只支持嵌套数值数组):
 *   {
 *     "W1": [[0.5, 0.5], [0.3, -0.1]],
 *     "b1": [0.1, 0.2, 0.0, -0.1]
 *   }
 *   - 值为嵌套数组时 -> 矩阵 (VAL_ARRAY of rows)
 *   - 值为一维数组时 -> 向量 (VAL_ARRAY of float)
 *   - 支持负数 / 小数 / 科学计数法, 忽略空白与换行
 *
 * 这是纯 C 实现, 不依赖外部 JSON 库, 也不依赖 vm.c 的 static helper,
 * 直接基于公开 MoValue/MoArray 结构自建结果值。
 */
#ifndef XIAOMO_WEIGHTS_H
#define XIAOMO_WEIGHTS_H

#include "vm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 从 JSON 权重文件加载指定 key 的权重为 MoValue。
 * 成功返回 0, 并把结果写入 out (调用方负责 value 释放);
 * 失败返回非 0, 错误信息写入 errbuf (若非 NULL)。 */
int xm_load_weight(const char* path, const char* key, MoValue* out,
                   char* errbuf, int errbuflen);

/* 从内存字符串解析 JSON 权重并取指定 key (供测试/内嵌使用) */
int xm_load_weight_from_json(const char* json, const char* key, MoValue* out,
                             char* errbuf, int errbuflen);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_WEIGHTS_H */
