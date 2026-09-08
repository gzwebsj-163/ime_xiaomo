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
#include <unistd.h>

#include "lexer.h"
#include "parser.h"
#include "vm.h"
#include "vm_core.h"
#include "mo2kbc.h"
#include "hw_demo.h"
#include "hw_oem.h"
#include "hw_hex.h"
#include "hw_dev.h"

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

/* .mo → Kills 字节码 → kvm_run 执行 (打通两层); 可选 -o out.kbc 序列化落盘 */
static int cmd_mo2kbc(const char* path, const char* outpath) {
    char* src = read_file(path);
    if (!src) { fprintf(stderr, "无法读取文件: %s\n", path); return 1; }
    char errbuf[1024] = "";
    AstNode* program = xiaomo_parse_source(src, errbuf, sizeof(errbuf));
    if (!program) {
        fprintf(stderr, "解析失败: %s\n", errbuf);
        free(src);
        return 1;
    }
    KillsProgram* kp = mo2kbc_compile(program, errbuf, sizeof(errbuf));
    if (!kp) {
        fprintf(stderr, "编译失败: %s\n", errbuf);
        ast_free(program); free(src);
        return 1;
    }
    /* MO2KBC_DUMP=1: 反汇编打印编译产物 (排障用) */
    if (getenv("MO2KBC_DUMP")) {
        static char dbuf[1 << 20];
        kvm_disassemble(kp, dbuf, (int)sizeof(dbuf));
        fputs(dbuf, stderr);
    }
    if (outpath) {
        uint8_t* blob = NULL;
        long blen = kprog_serialize(kp, &blob);
        if (blen > 0 && blob) {
            FILE* f = fopen(outpath, "wb");
            if (f) {
                fwrite(blob, 1, (size_t)blen, f);
                fclose(f);
                fprintf(stderr, "[mo2kbc] 已序列化字节码: %s (%ld bytes)\n", outpath, blen);
            } else {
                fprintf(stderr, "[mo2kbc] 无法写入: %s\n", outpath);
            }
            free(blob);
        } else {
            fprintf(stderr, "[mo2kbc] 序列化失败\n");
        }
        /* 纯编译模式: 落盘后直接返回, 不执行 (交互 .mo 是无限循环) */
        kprog_free(kp);
        ast_free(program);
        free(src);
        return 0;
    }

    KillsVM vm;
    kvm_init(&vm);
    int rc = kvm_run(&vm, kp);
    if (rc != 0 && vm.error_count > 0) {
        fprintf(stderr, "Kills 执行错误: %s\n", vm.error_msg);
    }
    for (int i = 0; i < kvm_output_count(&vm); i++) {
        printf("%s\n", kvm_output(&vm, i));
    }
    kvm_free(&vm);
    kprog_free(kp);
    ast_free(program);
    free(src);
    return rc;
}

static void kvm_test_callret(void);
static void kvm_test_recursion(void);
/* openclaw 交互系统宿主验证: ./xiaomo interact <file.mo>
 * VM 在独立线程跑 kvm_run(无限事件循环, 靠 input_wait 阻塞等输入);
 * 主线程读 stdin 每行 → kvm_io_push 注入 VM; 每注入一行后刷出 VM 的新输出。
 * 验证通过后, 同一 .mo + FFI 逻辑下沉到 ESP32(TCP 通道)。 */
#include <pthread.h>
static void* interact_vm_thread(void* arg) {
    KillsVM* vm = (KillsVM*)arg;
    int rc = kvm_run(vm, vm->prog);
    (void)rc;
    return NULL;
}
static int cmd_interact(const char* path) {
    char* src = read_file(path);
    if (!src) { fprintf(stderr, "无法读取文件: %s\n", path); return 1; }
    char errbuf[1024] = "";
    AstNode* program = xiaomo_parse_source(src, errbuf, sizeof(errbuf));
    if (!program) { fprintf(stderr, "解析失败: %s\n", errbuf); free(src); return 1; }
    KillsProgram* kp = mo2kbc_compile(program, errbuf, sizeof(errbuf));
    if (!kp) { fprintf(stderr, "编译失败: %s\n", errbuf); ast_free(program); free(src); return 1; }

    KillsVM vm;
    kvm_init(&vm);
    vm.prog = kp;
    pthread_t tid;
    pthread_create(&tid, NULL, interact_vm_thread, &vm);

    printf("[interact] VM loop started. Type a message and press Enter (Ctrl-D to quit):\n");
    fflush(stdout);
    /* 输出统一由 VM 线程在 input_wait 阻塞边界 flush 打印:
       LLM 回复是 append 到已有行(output_count 不变), 主线程按 count 增量轮询
       永远看不到 → 这里只负责注入输入, 不再轮询打印。 */
    char line[2048];
    while (fgets(line, sizeof(line), stdin)) {
        /* 去掉换行 */
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r')) { line[--ln] = 0; }
        if (ln == 0) continue;
        kvm_io_push(line);
    }
    /* stdin EOF: 等 VM 真正回到 input_wait 空闲再 cancel。
       ⚠️ pending==0 不等于线程空闲: VM 消费输入后可能正在 llm_query 阻塞(3~8s);
       判定条件 = pending 已空 且 output 已 flush 清空(count==0) 连续 3 次(1.5s),
       此时线程确实停在 input_wait 等下一轮输入 → 安全取消。上限 100s。 */
    fprintf(stderr, "[interact] EOF, waiting for VM to finish...\n");
    int idle_rounds = 0;
    for (int w = 0; w < 200; w++) {
        usleep(500000);
        if (!kvm_io_pending() && kvm_output_count(&vm) == 0) { idle_rounds++; if (idle_rounds >= 3) break; }
        else idle_rounds = 0;
    }
    pthread_cancel(tid);
    pthread_join(tid, NULL);
    kvm_free(&vm);
    kprog_free(kp);
    ast_free(program);
    free(src);
    return 0;
}

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

/* ================= hw_dev CLI (2026-09-07) ================= */

/* 动态注册演示用 LED 桩: 返回 0x2A 便于与静态命令(0x00)区分 */
static uint16_t dev_led_stub(void *arg) {
    (void)arg;
    return 0x2AU;
}

/* hw_dev 能力卡: init → ctrl 回调两态 → 静态表分发 → 动态注册/撞名/分发 → hook 释放 */
static int hwdev_show(void) {
    printf("=== xiaomo hw_dev 设备命令分发层 ===\n\n");

    /* 1) ctrl 两态 */
    printf("ctrl(NULL)          = 0x%02X  (无回调默认 0x01)\n", hw_dev_ctrl(HW_DEV_OPTIONS, NULL, NULL));
    printf("ctrl(DBG, echo桩)   = 0x%02X  (回调透传)\n", hw_dev_ctrl(HW_DEV_DEBUG, dev_led_stub, NULL));

    /* 2) 静态命令表分发 (前缀匹配, "\n\r " 尾缀) + 结果通道 (hw_dev_result) */
    char rbuf[256];
    uint16_t r = hw_dev_dispatch("echo\n\r hello", NULL);
    hw_dev_result(rbuf, sizeof(rbuf));
    printf("dispatch echo       = 0x%02X  result=[%s]\n", r, rbuf);
    r = hw_dev_dispatch("pwm\n\r 50", NULL);
    printf("dispatch pwm        = 0x%02X\n", r);
    r = hw_dev_dispatch("bogus\n\r ", NULL);
    printf("dispatch bogus      = 0x%02X  (未命中)\n", r);

    /* 2b) i2c/pwm 真实外设演示 (Linux 实收发; macOS 无节点→0x03 IO错) */
    r = hw_dev_dispatch("i2c\n\r list", NULL);
    hw_dev_result(rbuf, sizeof(rbuf));
    printf("dispatch i2c list   = 0x%02X  result=[%s]\n", r, rbuf);
    r = hw_dev_dispatch("pwm\n\r list", NULL);
    hw_dev_result(rbuf, sizeof(rbuf));
    printf("dispatch pwm list   = 0x%02X  result=[%s]\n", r, rbuf);

    /* 3) 动态注册 → 分发 → 撞名拒绝 → 二次注册拒绝 */
    int rc = hw_dev_register("led\n\r ", 0xABCD00U, dev_led_stub);
    printf("register led        = %d   (0=成功)\n", rc);
    rc = hw_dev_register("led\n\r ", 0x1U, dev_led_stub);
    printf("register led again  = %d   (-2=撞名拒绝)\n", rc);
    rc = hw_dev_register("echo\n\r ", 0x2U, dev_led_stub);
    printf("register dup echo   = %d   (-2=静态表撞名拦截)\n", rc);
    r = hw_dev_dispatch("led\n\r on", NULL);
    printf("dispatch led        = 0x%02X  (动态命令透传)\n", r);
    printf("动态表条数           = %u\n", (unsigned)hw_dev_registered());

    /* 4) hook 释放后动态命令失效 */
    hw_dev_hook(NULL);
    r = hw_dev_dispatch("led\n\r on", NULL);
    printf("hook 后 dispatch led= 0x%02X  (0xFF=动态表已清)\n", r);
    printf("VM 内核联动: kvm_run 上电自动 hw_dev_init; .mo 端 hw_dev(\"...\") 见 examples/hwdev_test.mo\n");
    return 0;
}

/* 一次性分发 CLI: ./xiaomo hwdev "echo\n\r hello" */
static int cmd_hwdev(const char* cmd) {
    if (cmd == NULL) return hwdev_show();
    uint16_t r = hw_dev_dispatch(cmd, NULL);
    char rbuf[256];
    hw_dev_result(rbuf, sizeof(rbuf));
    printf("hwdev dispatch = 0x%02X%s\n", r,
           (r == 0xFFU) ? " (未命中: 命令须含 \"\\n\\r \" 尾缀, 如 'echo\\n\\r hello')" : "");
    if (rbuf[0] != '\0') printf("hwdev result   = [%s]\n", rbuf);
    return (r == 0xFFU) ? 1 : 0;
}

static void print_usage(const char* prog) {
    printf("xiaomo - Mo 语言轻量级虚拟机 (纯 C)\n");
    printf("用法:\n");
    printf("  %s run <file.mo>       执行 .mo 文件\n", prog);
    printf("  %s parse <file.mo>     解析并打印 AST\n", prog);
    printf("  %s tokens <file.mo>    打印 Token 序列\n", prog);
    printf("  %s kvm [-] [file.kbc]  Kills 字节码内核: 内嵌演示(-)或执行二进制\n", prog);
    printf("  %s mo2kbc <file.mo>    .mo 编译到 Kills 字节码并执行 (打通两层)\n", prog);
    printf("  %s hwprobe             硬件探测演示 (PCI/USB/串口/CPU/内存/GPU)\n", prog);
    printf("  %s sig                 OEM 设备签名 (熔丝 hex -> R127 只读标志)\n", prog);
    printf("  %s sigprobe            签名寄存器攻防演示 (越权写被屏蔽)\n", prog);
    printf("  %s hwdev [cmd]         hw_dev 命令分发 (无参=演示; 命令串须含 \"\\n\\r \" 尾缀)\n", prog);
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
    } else if (strcmp(cmd, "mo2kbc") == 0 && argc >= 3) {
        const char* outpath = NULL;
        const char* srcpath = argv[2];
        if (strcmp(argv[2], "-o") == 0 && argc >= 5) { outpath = argv[3]; srcpath = argv[4]; }
        return cmd_mo2kbc(srcpath, outpath);
    } else if (strcmp(cmd, "interact") == 0 && argc >= 3) {
        return cmd_interact(argv[2]);
    } else if (strcmp(cmd, "hwprobe") == 0) {
        return hw_demo_run();
    } else if (strcmp(cmd, "sig") == 0) {
        return hw_oem_sig_cli();
    } else if (strcmp(cmd, "sigprobe") == 0) {
        return hw_sigprobe_run();
    } else if (strcmp(cmd, "hwdev") == 0) {
        return cmd_hwdev(argc >= 3 ? argv[2] : NULL);
    } else {
        print_usage(argv[0]);
        return 1;
    }
}
