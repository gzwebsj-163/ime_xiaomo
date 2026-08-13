/*
 * xiaomo - 主程序入口
 *
 * 用法:
 *   xiaomo run <file.mo>    执行 .mo 文件
 *   xiaomo parse <file.mo>  解析并打印 AST (调试)
 *   xiaomo tokens <file.mo> 打印 Token 序列 (调试)
 *   xiaomo repl              交互式
 *
 * 架构: 纯 C 实现 .mo 解析 + VM 执行
 *   .mo 源码 -> Lexer -> Token -> Parser -> AST -> VM 执行
 *   VM 复用 linux-xiaomo 的 stack_memory 作为执行栈
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include "vm_core.h"

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(size + 1);
    size_t rd = fread(buf, 1, size, f);
    buf[rd] = '\0';
    fclose(f);
    return buf;
}

static int cmd_tokens(const char* path) {
    char* src = read_file(path);
    if (!src) { fprintf(stderr, "无法读取文件: %s\n", path); return 1; }
    Lexer lx;
    lexer_init(&lx, src);
    TokenList tl = lexer_tokenize(&lx);
    if (lx.error_count > 0) {
        fprintf(stderr, "词法错误: %s\n", lx.error_msg);
        token_list_free(&tl);
        free(src);
        return 1;
    }
    token_list_dump(&tl);
    token_list_free(&tl);
    free(src);
    return 0;
}

static int cmd_parse(const char* path) {
    char* src = read_file(path);
    if (!src) { fprintf(stderr, "无法读取文件: %s\n", path); return 1; }
    char errbuf[1024] = "";
    AstNode* program = xiaomo_parse_source(src, errbuf, sizeof(errbuf));
    if (!program) {
        fprintf(stderr, "解析失败: %s\n", errbuf);
        free(src);
        return 1;
    }
    ast_dump(program, 0);
    ast_free(program);
    free(src);
    return 0;
}

static int cmd_run(const char* path) {
    char* src = read_file(path);
    if (!src) { fprintf(stderr, "无法读取文件: %s\n", path); return 1; }
    char errbuf[1024] = "";
    AstNode* program = xiaomo_parse_source(src, errbuf, sizeof(errbuf));
    if (!program) {
        fprintf(stderr, "解析失败: %s\n", errbuf);
        free(src);
        return 1;
    }
    VM vm;
    vm_init(&vm);
    int rc = vm_run(&vm, program);
    if (rc != 0 && vm.error_count > 0) {
        fprintf(stderr, "执行错误: %s\n", vm.error_msg);
    }
    /* 输出 */
    for (int i = 0; i < vm_output_count(&vm); i++) {
        printf("%s\n", vm_output(&vm, i));
    }
    vm_free(&vm);
    ast_free(program);
    free(src);
    return rc;
}

static void kvm_test_callret(void);
static void kvm_test_recursion(void);
static int cmd_kvm_demo(void) {
    /* 内嵌演示: 计算 6! 用字节码 (MOV/算术/JNZ 循环/PRINT/HALT) */
    KillsProgram* p = kprog_new();
    /* pc: 0 MOV R0, 6 | 1 MOV R1, 1 | 2 MUL R1, R0 | 3 SUB R0, 1 | 4 JNZ R0, loop(2) | 5 PRINT R1 | 6 HALT */
    kprog_add_ins(p, OP_MOV, 0, -1, 6);         /* 0: R0 = 6 */
    kprog_add_ins(p, OP_MOV, 1, -1, 1);         /* 1: R1 = 1 */
    kprog_add_ins(p, OP_MUL, 1, 0, 0);          /* 2: R1 *= R0 */
    kprog_add_ins(p, OP_SUB, 0, -1, 1);         /* 3: R0 -= 1 */
    kprog_add_ins(p, OP_JNZ, 0, -1, -2);        /* 4: if R0 != 0 loop -> pc+(-2)=2 */
    kprog_add_ins(p, OP_PRINT, 1, 0, 0);        /* 5: print R1 */
    kprog_add_ins(p, OP_HALT, 0, 0, 0);         /* 6: halt */

    char dis[2048];
    kvm_disassemble(p, dis, sizeof(dis));
    printf("=== 反汇编 ===\n%s\n", dis);

    KillsVM vm;
    kvm_init(&vm);
    int rc = kvm_run(&vm, p);
    printf("=== 执行结果 (6! = 720) ===\n");
    if (rc != 0 && vm.error_count > 0) printf("错误: %s\n", vm.error_msg);
    for (int i = 0; i < kvm_output_count(&vm); i++) printf("  %s\n", kvm_output(&vm, i));

    /* 序列化往返测试 */
    uint8_t* blob = NULL;
    long blen = kprog_serialize(p, &blob);
    char err[256] = "";
    KillsProgram* p2 = (blen > 0 && blob) ? kprog_deserialize(blob, blen, err, sizeof(err)) : NULL;
    if (p2) {
        KillsVM vm2; kvm_init(&vm2);
        int rc2 = kvm_run(&vm2, p2);
        printf("=== 序列化往返执行 ===\n");
        if (rc2 != 0 && vm2.error_count > 0) printf("错误: %s\n", vm2.error_msg);
        for (int i = 0; i < kvm_output_count(&vm2); i++) printf("  %s\n", kvm_output(&vm2, i));
        kvm_free(&vm2); kprog_free(p2);
    } else {
        printf("  反序列化失败: %s\n", err);
    }
    free(blob);
    kvm_free(&vm);
    kprog_free(p);
    kvm_test_callret();
    kvm_test_recursion();
    return 0;
}

static void kvm_test_callret(void) {
    /* CALL/RET 测试: add(2,3) -> R0 = 2+3 = 5 */
    KillsProgram* p = kprog_new();
    kprog_add_ins(p, OP_ADD, 0, 1, 0);    /* 0 add: R0 = R0 + R1 */
    kprog_add_ins(p, OP_RET, 0, 0, 0);    /* 1 ret */
    kprog_add_ins(p, OP_PUSH, -1, 0, 2);  /* 2 push a=2 */
    kprog_add_ins(p, OP_PUSH, -1, 0, 3);  /* 3 push b=3 */
    kprog_add_ins(p, OP_CALL, 0, 0, 0);   /* 4 call add */
    kprog_add_ins(p, OP_PRINT, 0, 0, 0);  /* 5 print R0 */
    kprog_add_ins(p, OP_HALT, 0, 0, 0);   /* 6 halt */
    kprog_add_func(p, "add", 0, 2);
    KillsVM vm; kvm_init(&vm);
    int rc = kvm_run(&vm, p);
    printf("=== CALL/RET 函数调用 (add(2,3) 应 = 5) ===\n");
    if (rc != 0 && vm.error_count) printf("  错误: %s\n", vm.error_msg);
    for (int i = 0; i < kvm_output_count(&vm); i++) printf("  -> %s\n", kvm_output(&vm, i));
    kvm_free(&vm);
    kprog_free(p);
}

static void kvm_test_recursion(void) {
    /* 递归阶乘 fact(5)=120, 验证多帧调用栈保存/恢复
       约定: CALL 实参经操作数栈弹入 R0; R0 为返回值寄存器; RET 恢复 R1..R63 现场.
       R2 保存本层 n (因 R0 会被递归返回值覆盖, R1 作临时量) */
    KillsProgram* p = kprog_new();
    /* 函数 fact: 入口 R0=n */
    kprog_add_ins(p, OP_MOV, 2, 0, 0);  /*  0: R2 = n (保存) */
    kprog_add_ins(p, OP_MOV, 1, 0, 0);  /*  1: R1 = n */
    kprog_add_ins(p, OP_SUB, 1, -1, 1); /*  2: R1 = n-1 */
    kprog_add_ins(p, OP_JG,  1, -1, 3); /*  3: if (n-1)>0 goto 6 (rec) */
    kprog_add_ins(p, OP_MOV, 0, -1, 1); /*  4: R0 = 1 (base) */
    kprog_add_ins(p, OP_RET, 0, 0, 0);  /*  5: ret */
    kprog_add_ins(p, OP_MOV, 1, 0, 0);  /*  6 rec: R1 = n */
    kprog_add_ins(p, OP_SUB, 1, -1, 1); /*  7: R1 = n-1 */
    kprog_add_ins(p, OP_PUSH, 1, 0, 0); /*  8: push n-1 */
    kprog_add_ins(p, OP_CALL, 0, 0, 0); /*  9: R0 = fact(n-1), 现场恢复 R1/R2 */
    kprog_add_ins(p, OP_MUL,  0, 2, 0); /* 10: R0 = fact(n-1) * R2(=n) */
    kprog_add_ins(p, OP_RET, 0, 0, 0);  /* 11: ret */
    /* main */
    kprog_add_ins(p, OP_PUSH, -1, 0, 5); /* 12: push 5 */
    kprog_add_ins(p, OP_CALL, 0, 0, 0);  /* 13: call fact */
    kprog_add_ins(p, OP_PRINT, 0, 0, 0); /* 14: print R0 */
    kprog_add_ins(p, OP_HALT, 0, 0, 0);  /* 15: halt */
    kprog_add_func(p, "fact", 0, 1);
    KillsVM vm; kvm_init(&vm);
    int rc = kvm_run(&vm, p);
    printf("=== 递归阶乘 fact(5) 应 = 120 ===\n");
    if (rc != 0 && vm.error_count) printf("  错误: %s\n", vm.error_msg);
    for (int i = 0; i < kvm_output_count(&vm); i++) printf("  -> %s\n", kvm_output(&vm, i));
    kvm_free(&vm);
    kprog_free(p);
}

static int cmd_kvm(const char* path) {
    if (!path || strcmp(path, "-") == 0) return cmd_kvm_demo();
    /* 加载 .kbc 二进制执行 */
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "无法读取: %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(sz ? sz : 1);
    fread(buf, 1, sz, f); fclose(f);
    char err[256] = "";
    KillsProgram* p = kprog_deserialize(buf, sz, err, sizeof(err));
    free(buf);
    if (!p) { fprintf(stderr, "反序列化失败: %s\n", err); return 1; }
    KillsVM vm; kvm_init(&vm);
    int rc = kvm_run(&vm, p);
    if (rc != 0 && vm.error_count > 0) fprintf(stderr, "错误: %s\n", vm.error_msg);
    for (int i = 0; i < kvm_output_count(&vm); i++) printf("%s\n", kvm_output(&vm, i));
    kvm_free(&vm); kprog_free(p);
    return rc;
}

static void print_usage(const char* prog) {
    printf("xiaomo - Mo 语言轻量级虚拟机 (纯 C)\n");
    printf("用法:\n");
    printf("  %s run <file.mo>       执行 .mo 文件\n", prog);
    printf("  %s parse <file.mo>     解析并打印 AST\n", prog);
    printf("  %s tokens <file.mo>    打印 Token 序列\n", prog);
    printf("  %s kvm [-] [file.kbc]  Kills 字节码内核: 内嵌演示(-)或执行二进制\n", prog);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    const char* cmd = argv[1];
    if (strcmp(cmd, "tokens") == 0 && argc >= 3) {
        return cmd_tokens(argv[2]);
    } else if (strcmp(cmd, "parse") == 0 && argc >= 3) {
        return cmd_parse(argv[2]);
    } else if (strcmp(cmd, "run") == 0 && argc >= 3) {
        return cmd_run(argv[2]);
    } else if (strcmp(cmd, "kvm") == 0) {
        return cmd_kvm(argc >= 3 ? argv[2] : "-");
    } else {
        print_usage(argv[0]);
        return 1;
    }
}
