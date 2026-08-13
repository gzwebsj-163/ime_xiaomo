#include "./include/stack_memory.h"

void trigger_listener(CustomStack* stack, StackEventType event) {
    if (!stack || !stack->enable_listener || !stack->callback) {
        return;
    }
    std::lock_guard<std::mutex> guard(stack->lock);
    size_t used = stack->top;
    size_t total = stack->stack_size;
    size_t threshold = stack->threshold;
    std::thread([=]() {
        stack->callback(event, total, used, threshold);
    }).detach();
}

StackStatus stack_init(CustomStack* stack, size_t stack_size) {
    if (stack == nullptr) {
        std::cerr << "[Stack Error] Init failed: stack pointer is null!" << std::endl;
        return STACK_MEMORY_ERROR;
    }
    std::lock_guard<std::mutex> guard(stack->lock);
    #ifdef ENABLE_STACK_CHECK
    #if ENABLE_STACK_CHECK == 1
        if (IS_STACK_INVALID_OR_FULL(stack, stack_size)) {
            std::cerr << "[Stack Error] Init failed: invalid params (null or full or size 0)!" << std::endl;
            return STACK_MEMORY_ERROR;
        }
        stack->buffer = (uint8_t*)malloc(stack_size);
        if (stack->buffer == nullptr) {
            std::cerr << "Stack memory allocation failed!" << std::endl;
            return STACK_MEMORY_ERROR;
        }
        memset(stack->buffer, 0, stack_size);
    #else
        stack->buffer = nullptr;
    #endif
    #endif
    stack->stack_size = stack_size;
    stack->top = 0;
    stack->callback = nullptr;
    stack->threshold = 0;
    stack->enable_listener = false;
    return STACK_OK;
}

StackStatus stack_push(CustomStack* stack, const void* data, size_t data_len) {
    if (stack == nullptr || data == nullptr || data_len == 0) {
        std::cerr << "[Stack Error] Push failed: invalid params!" << std::endl;
        return STACK_OUT_OF_BOUNDS;
    }
    std::lock_guard<std::mutex> guard(stack->lock);
    if (stack->top + data_len > stack->stack_size) {
        std::cerr << "[Stack Error] Push failed: stack full (used: " << stack->top << ", need: " << data_len << ")!" << std::endl;
        trigger_listener(stack, STACK_EVENT_FULL);
        return STACK_FULL;
    }
    memcpy(stack->buffer + stack->top, data, data_len);
    stack->top += data_len;
    trigger_listener(stack, STACK_EVENT_PUSH);
    if (stack->threshold > 0 && stack->top >= stack->threshold) {
        trigger_listener(stack, STACK_EVENT_USED_THRESHOLD);
    }
    return STACK_OK;
}

StackStatus stack_pop(CustomStack* stack, void* out_data, size_t data_len) {
    if (stack == nullptr || out_data == nullptr || data_len == 0) {
        std::cerr << "[Stack Error] Pop failed: invalid params!" << std::endl;
        return STACK_OUT_OF_BOUNDS;
    }
    std::lock_guard<std::mutex> guard(stack->lock);
    if (stack->top == 0 || stack->top < data_len) {
        std::cerr << "[Stack Error] Pop failed: stack empty or data len too big (used: " << stack->top << ")!" << std::endl;
        trigger_listener(stack, STACK_EVENT_EMPTY);
        return STACK_EMPTY;
    }
    stack->top -= data_len;
    memcpy(out_data, stack->buffer + stack->top, data_len);
    memset(stack->buffer + stack->top, 0, data_len);
    trigger_listener(stack, STACK_EVENT_POP);
    return STACK_OK;
}

StackStatus stack_peek(CustomStack* stack, void* out_data, size_t data_len) {
    if (stack == nullptr || out_data == nullptr || data_len == 0) {
        return STACK_OUT_OF_BOUNDS;
    }
    std::lock_guard<std::mutex> guard(stack->lock);

    if (stack->top == 0 || stack->top < data_len) {
        return STACK_EMPTY;
    }
    memcpy(out_data, stack->buffer + (stack->top - data_len), data_len);
    return STACK_OK;
}

bool stack_is_empty(CustomStack* stack) {
    if (stack == nullptr) return true;
    std::lock_guard<std::mutex> guard(stack->lock);
    return stack->top == 0;
}

bool stack_is_full(CustomStack* stack) {
    if (stack == nullptr) return false;
    std::lock_guard<std::mutex> guard(stack->lock);
    return stack->top == stack->stack_size;
}

size_t stack_used_size(CustomStack* stack) {
    if (stack == nullptr) return 0;
    std::lock_guard<std::mutex> guard(stack->lock);
    return stack->top;
}

void stack_register_listener(CustomStack* stack, StackCallback cb, size_t threshold) {
    if (!stack) return;
    std::lock_guard<std::mutex> guard(stack->lock);
    stack->callback = cb;
    stack->threshold = threshold;
}

void stack_enable_listener(CustomStack* stack, bool enable) {
    if (!stack) return;
    std::lock_guard<std::mutex> guard(stack->lock);
    stack->enable_listener = enable;
}
StackStatus stack_resize(CustomStack* stack, size_t new_size) {
    if (!stack || new_size == 0) {
        std::cerr << "[Stack Error] Resize failed: invalid params!" << std::endl;
        return STACK_MEMORY_ERROR;
    }
    std::lock_guard<std::mutex> guard(stack->lock);
    uint8_t* new_buffer = (uint8_t*)realloc(stack->buffer, new_size);
    if (!new_buffer) {
        std::cerr << "[Stack Error] Resize failed: memory allocation failed!" << std::endl;
        return STACK_MEMORY_ERROR;
    }
    if (new_size < stack->top) {
        stack->top = new_size;
        memset(new_buffer + stack->top, 0, new_size - stack->top);
    }
    stack->buffer = new_buffer;
    stack->stack_size = new_size;
    trigger_listener(stack, STACK_EVENT_SIZE_CHANGED);
    return STACK_OK;
}

void stack_destroy(CustomStack* stack) {
    if (!stack) return;
    std::lock_guard<std::mutex> guard(stack->lock);
    if (stack->buffer != nullptr) {
        free(stack->buffer);
        stack->buffer = nullptr;
    }
    stack->stack_size = 0;
    stack->top = 0;
    stack->callback = nullptr;
    stack->threshold = 0;
    stack->enable_listener = false;
}