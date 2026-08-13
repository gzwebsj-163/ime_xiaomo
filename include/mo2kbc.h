/*
 * xiaomo - .mo → Kills 字节码 编译器
 *
 * 打通两层: .mo 源码 → AST → KillsProgram(字节码) → kvm_run 执行
 * 调用约定依赖 Kills 内核的 CALL/RET 全量寄存器保存恢复 (帧隔离).
 *
 * 寄存器约定:
 *   R0..R3     函数形参 (CALL 自动从操作数栈弹出填入)
 *   R4..R23    全局变量
 *   R24..R49   函数局部变量
 *   R50 = TMP  表达式结果寄存器
 *   R51 = R2   表达式辅助(右操作数)
 *   R63 = R0   返回值载体 (CALL 结果)
 */
#ifndef XIAOMO_MO2KBC_H
#define XIAOMO_MO2KBC_H

#include "ast.h"
#include "vm_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 编译 .mo AST 到 KillsProgram.
 * 失败返回 NULL 并写入 err. 成功后调用方负责 kprog_free. */
KillsProgram* mo2kbc_compile(AstNode* program, char* err, int errlen);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_MO2KBC_H */
