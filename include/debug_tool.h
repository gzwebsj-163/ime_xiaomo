/*
 * xiaomo debug 工具 —— 排障三层证据链统一入口
 */
#ifndef XIAOMO_DEBUG_TOOL_H
#define XIAOMO_DEBUG_TOOL_H

#ifdef __cplusplus
extern "C" {
#endif

/* CLI: xiaomo debug <sub> ...
 * 调用约定: main.c 传 (argc-1, argv+1), 即 argv[0]="debug"。 */
int debug_tool_main(int argc, char** argv);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_DEBUG_TOOL_H */
