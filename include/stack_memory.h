#ifndef STACK_MEMORY_H
#define STACK_MEMORY_H
#include <cstdlib>
#include <cstring>
#include <stdint.h>
#include <iostream>
#include <mutex>
#include <functional>
#include <thread>
#ifdef __cplusplus
extern "C"
{
#endif
typedef enum {
    STACK_OK = 0,
    STACK_MEMORY_ERROR = -1,
    STACK_FULL = -2,
    STACK_EMPTY = -3,
    STACK_OUT_OF_BOUNDS = -4
} StackStatus;

typedef enum {
    STACK_EVENT_USED_THRESHOLD,
    STACK_EVENT_FULL,
    STACK_EVENT_EMPTY,
    STACK_EVENT_SIZE_CHANGED,
    STACK_EVENT_PUSH,
    STACK_EVENT_POP
} StackEventType;

typedef std::function<void(StackEventType event, size_t total_size, size_t used_size, size_t threshold)> StackCallback;
typedef struct {
    uint8_t* buffer;
    size_t stack_size;
    size_t top; 
    std::mutex lock;
    StackCallback callback;
    size_t threshold;
    bool enable_listener;
    int* address;
    int size;
} CustomStack;

#define IS_STACK_INVALID_OR_FULL(stack, stack_size) \
    (((stack) == nullptr) || ((stack_size) == 0) || stack_is_full(stack))
#define IS_STACK_INIT_INVALID(stack, stack_size) \
    ((stack) == nullptr || (stack_size) == 0)
#ifndef ENABLE_STACK_CHECK
#define ENABLE_STACK_CHECK 1
#endif

StackStatus stack_init(CustomStack* stack, size_t stack_size);
StackStatus stack_push(CustomStack* stack, const void* data, size_t data_len);
StackStatus stack_pop(CustomStack* stack, void* out_data, size_t data_len);
StackStatus stack_peek(CustomStack* stack, void* out_data, size_t data_len);
bool stack_is_empty(CustomStack* stack);
bool stack_is_full(CustomStack* stack);
size_t stack_used_size(CustomStack* stack);
void stack_register_listener(CustomStack* stack, StackCallback cb, size_t threshold = 0);
void stack_enable_listener(CustomStack* stack, bool enable);
StackStatus stack_resize(CustomStack* stack, size_t new_size);
void stack_destroy(CustomStack* stack);
void trigger_listener(CustomStack* stack, StackEventType event);
#ifdef __cplusplus
}
#endif
#endif // STACK_MEMORY_H
