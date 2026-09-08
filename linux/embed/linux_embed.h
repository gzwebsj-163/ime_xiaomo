/*
 * linux_embed.h — xiaomo 嵌入式 Linux 内核核心 API
 *
 * 把 TinyEMU（Fabrice Bellard 的 RISC-V 全系统模拟器）封装成
 * 一个干净的 C 模块：linux_init / linux_exec / linux_read_output / linux_end。
 *
 * 与 primitive/ 中 interrupt/mmu/pipeline/bus/freertos 的模块范式一致，
 * 这是 primitive 路线的「阶段六」：真 Linux 内核核心。
 *
 * 设计要点：
 *  - 自研 CharacterDevice（环形缓冲控制台），替代 temu.c 的 termios 终端
 *  - 无 SDL / 无网络 / 无 FS_NET，纯内核模拟核心
 *  - linux_exec 支持「注入命令 + 等预期输出」的交互驱动
 *
 * (c) xiaomo project, MIT
 */
#ifndef LINUX_EMBED_H
#define LINUX_EMBED_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LinuxVM LinuxVM;

/*
 * linux_init — 创建并启动一个 Linux 内核 VM。
 *   cfg_path: root-riscv64-embed.cfg 的绝对路径（JSON VM 配置）
 *   返回: 非 NULL 成功；NULL 失败
 */
LinuxVM *linux_init(const char *cfg_path);

/*
 * linux_exec — 注入一行命令并等待预期输出。
 *   cmd:     注入 guest shell 的命令（不含换行；传 "" 表示只跑不动手）
 *   expect:  期望在 guest 新输出中出现的子串；NULL = 不等待匹配（跑满超时）
 *   timeout_ms: 超时毫秒（0 = 默认 30000）
 *   返回: 0=出现 expect；1=超时；-1=VM 错误
 *
 * 注意：注入前会清空历史输出缓冲，expect 只匹配本次命令之后的内容。
 */
int linux_exec(LinuxVM *vm, const char *cmd, const char *expect, int timeout_ms);

/*
 * linux_read_output — 取出 guest 的全部输出（读走即消费，缓冲被清空）。
 *   返回: 写入 buf 的字节数（不含 \0）
 */
int linux_read_output(LinuxVM *vm, char *buf, int buf_size);

/* linux_has_output — 查询是否有待读输出（非阻塞） */
int linux_has_output(LinuxVM *vm);

/* 打印 guest 全部输出（调试用） */
void linux_dump_output(LinuxVM *vm);

/* linux_end — 关闭并销毁 VM */
void linux_end(LinuxVM *vm);

#ifdef __cplusplus
}
#endif

#endif /* LINUX_EMBED_H */
