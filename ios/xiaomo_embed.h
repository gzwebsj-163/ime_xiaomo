#ifndef XIAOMO_EMBED_H
#define XIAOMO_EMBED_H
/* xiaomo 嵌入式接口：把 .mo 脚本跑在宿主 App 内（iOS / macOS / Linux 通用）。
 * 只依赖 VM 核心（lexer/parser/ast/vm/tensor/nd_tensor/weights），不含 hw_* 硬件绑定。 */
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* output;        /* 所有 >> 输出行，以 '\n' 结尾；可能为 NULL */
    int error;           /* 0 = 成功；1 = 读文件失败；2 = 解析失败；3 = 执行错误 */
    char error_msg[1024];
} XmoResult;

/* 执行 .mo 源码 / 文件；返回 res->error */
int xmo_run_source(const char* src, XmoResult* res);
int xmo_run_file(const char* path, XmoResult* res);
void xmo_result_free(XmoResult* res);

/* 扁平版接口，便于 Swift / Objective-C 直接调用：
 * 返回 0 表示成功；out 收到输出（用 xmo_free 释放）；失败时 errbuf 写入原因。 */
int xmo_exec(const char* path, char** out, char* errbuf, int errlen);
void xmo_free(char* p);

#ifdef __cplusplus
}
#endif
#endif
