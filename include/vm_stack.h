/*
 * xiaomo - 轻量 C 栈 (VM 底层执行栈)
 *
 * 纯 C 实现，无 C++/STL 依赖，便于跨平台(含 ESP32 等嵌入式)。
 * 采用字节栈模型: 可 push/pop 任意长度的数据块(对齐到 8 字节)。
 * 提供三种视图:
 *   - 操作数栈 (operand stack): 表达式求值用
 *   - 调用栈   (call stack):    函数调用现场
 *   - 数据内存  (linear memory): 数据段
 */
#ifndef XIAOMO_VM_STACK_H
#define XIAOMO_VM_STACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 轻量字节栈 */
typedef struct {
    uint8_t* buf;      /* 栈底 */
    size_t cap;        /* 容量(字节) */
    size_t top;        /* 栈顶偏移(已用字节数) */
} VmStack;

/* 初始化栈 (分配 stack_size 字节), 返回 0 成功 */
int vstack_init(VmStack* s, size_t stack_size);
/* 释放栈 */
void vstack_destroy(VmStack* s);
/* 压入 data 的 len 字节, 返回 0 成功 */
int vstack_push(VmStack* s, const void* data, size_t len);
/* 弹出 len 字节到 out (out 可为 NULL 表示丢弃) */
int vstack_pop(VmStack* s, void* out, size_t len);
/* 查看栈顶 len 字节 (不弹出) */
int vstack_peek(VmStack* s, void* out, size_t len);
/* 弹出一个 64 位值 */
int vstack_pop_u64(VmStack* s, uint64_t* out);
/* 压入一个 64 位值 */
int vstack_push_u64(VmStack* s, uint64_t v);
/* 当前已用字节数 */
size_t vstack_used(const VmStack* s);
/* 当前剩余容量 */
size_t vstack_remain(const VmStack* s);
/* 清空 */
void vstack_reset(VmStack* s);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_VM_STACK_H */
