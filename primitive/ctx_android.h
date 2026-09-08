/**
 * ctx_android.h — Android/bionic 版 ucontext 函数声明（结构用系统 <ucontext.h>）
 *
 * bionic 的 <sys/ucontext.h> 只定义 ucontext_t 结构，不提供
 * getcontext/makecontext/swapcontext/setcontext（链接期符号缺失）。
 * 这里自建 aarch64 汇编实现（见 ctx_android.c）。结构布局直接用系统定义，
 * 汇编按 bionic aarch64 真实偏移存取（uc_mcontext 固定 +176，探针实测）。
 *
 * 仅在 __ANDROID__/__BIONIC__ 且 __aarch64__ 下生效。
 */
#ifndef CTX_ANDROID_H
#define CTX_ANDROID_H

#include <ucontext.h>

#if defined(__aarch64__)

#ifdef __cplusplus
extern "C" {
#endif

int  getcontext(ucontext_t *ucp);
int  setcontext(const ucontext_t *ucp);
void makecontext(ucontext_t *ucp, void (*func)(void), int argc, ...);
int  swapcontext(ucontext_t *oucp, const ucontext_t *ucp);

#ifdef __cplusplus
}
#endif

#endif /* __aarch64__ */
#endif /* CTX_ANDROID_H */
