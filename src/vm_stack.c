/*
 * xiaomo - 轻量 C 栈实现
 */
#include "vm_stack.h"
#include <stdlib.h>
#include <string.h>

int vstack_init(VmStack* s, size_t stack_size) {
    if (!s) return -1;
    s->buf = (uint8_t*)malloc(stack_size ? stack_size : 1);
    if (!s->buf) return -1;
    s->cap = stack_size;
    s->top = 0;
    return 0;
}

void vstack_destroy(VmStack* s) {
    if (!s) return;
    if (s->buf) free(s->buf);
    s->buf = NULL;
    s->cap = 0;
    s->top = 0;
}

int vstack_push(VmStack* s, const void* data, size_t len) {
    if (!s || !s->buf) return -1;
    size_t pad = (len + 7) & ~(size_t)7;  /* 对齐到 8 */
    if (s->top + pad > s->cap) return -1; /* 溢出 */
    if (data && len) memcpy(s->buf + s->top, data, len);
    else if (len) memset(s->buf + s->top, 0, len);
    s->top += pad;
    return 0;
}

int vstack_pop(VmStack* s, void* out, size_t len) {
    if (!s || !s->buf) return -1;
    size_t pad = (len + 7) & ~(size_t)7;
    if (s->top < pad) return -1; /* 下溢 */
    s->top -= pad;
    if (out && len) memcpy(out, s->buf + s->top, len);
    return 0;
}

int vstack_peek(VmStack* s, void* out, size_t len) {
    if (!s || !s->buf) return -1;
    size_t pad = (len + 7) & ~(size_t)7;
    if (s->top < pad) return -1;
    if (out && len) memcpy(out, s->buf + s->top - pad, len);
    return 0;
}

int vstack_pop_u64(VmStack* s, uint64_t* out) {
    return vstack_pop(s, out, sizeof(uint64_t));
}

int vstack_push_u64(VmStack* s, uint64_t v) {
    return vstack_push(s, &v, sizeof(uint64_t));
}

size_t vstack_used(const VmStack* s) {
    return s ? s->top : 0;
}

size_t vstack_remain(const VmStack* s) {
    return s ? (s->cap - s->top) : 0;
}

void vstack_reset(VmStack* s) {
    if (s) s->top = 0;
}
