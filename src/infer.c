/*
 * infer.c - kbc 自动推理运行时 (见 include/infer.h)
 *
 * 运行模型: examples/intent/intent_kbc.kbc (Q16 MLP 64→16→6, numpy 训练导出)
 * 数据流:   输入行 → kvm_io_push → VM input_wait 消费 → 宿主 infer_featurize
 *           (vm_core.c) → feat(i) FFI 7 → .kbc 内 MLP 前向 → VM 打印结果
 *
 * ⚠️ 单次推理 = 全新 KillsVM: .kbc 主循环是 input_wait 无限循环,
 *    推理完成判定 = output 先变为非零(echo/logits)再归零(input_wait 边界
 *    已 flush), 连续 3 轮稳定 → pthread_cancel 安全回收。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "vm_core.h"
#include "hw_asr.h"
#include "infer.h"

/* 与 cmd_kvm 同款: 读文件 → 反序列化 */
static KillsProgram* infer_load_kbc(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[infer] 无法读取模型: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t* buf = (uint8_t*)malloc(sz ? (size_t)sz : 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "[infer] 模型读取失败: %s\n", path);
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    char err[256] = "";
    KillsProgram* p = kprog_deserialize(buf, sz, err, sizeof(err));
    free(buf);
    if (!p) { fprintf(stderr, "[infer] 反序列化失败: %s\n", err); return NULL; }
    return p;
}

static volatile int g_thread_done;   /* VM 线程结束标志 (单条顺序使用, volatile 足够) */

static void* infer_vm_thread(void* arg) {
    KillsVM* vm = (KillsVM*)arg;
    int rc = kvm_run(vm, vm->prog);
    (void)rc;
    g_thread_done = 1;
    return NULL;
}

/* 跑一行输入 → VM 自动推理 (阻塞到完成或超时)。
 * 单次版模型: 推理完自然 HALT, kvm_run 返回 → join 零竞态收尾;
 * 循环版模型(input_wait): 超时后 cancel 兜底。 */
static void infer_run_once(KillsProgram* kp, const char* line) {
    KillsVM vm;
    kvm_init(&vm);
    vm.prog = kp;
    g_thread_done = 0;
    pthread_t tid;
    pthread_create(&tid, NULL, infer_vm_thread, &vm);

    kvm_io_push(line);   /* FIFO: VM 起跑先后不影响消费 */

    for (int w = 0; w < 400; w++) {          /* 上限 20s */
        usleep(50000);
        if (g_thread_done) break;
    }
    if (!g_thread_done) pthread_cancel(tid);
    pthread_join(tid, NULL);

    /* 打印输出 (单次版全程未 flush, 由宿主统一打印, 顺序保持) */
    for (int i = 0; i < kvm_output_count(&vm); i++) printf("%s\n", kvm_output(&vm, i));
    fflush(stdout);
    kvm_free(&vm);
}

static int infer_usage(void) {
    fprintf(stderr,
        "用法: ./xiaomo infer <model.kbc> text  \"文本\"\n"
        "      ./xiaomo infer <model.kbc> voice <wav>\n"
        "      ./xiaomo infer <model.kbc> loop\n");
    return 1;
}

int infer_cmd(int argc, char** argv) {
    if (argc < 3) return infer_usage();
    KillsProgram* kp = infer_load_kbc(argv[2]);
    if (!kp) return 1;

    const char* mode = argv[3];
    if (argc >= 5 && strcmp(mode, "text") == 0) {
        infer_run_once(kp, argv[4]);
    } else if (argc >= 5 && strcmp(mode, "voice") == 0) {
        hw_asr_result_t res;
        int widx = hw_asr_match(argv[4], &res);
        if (widx == -2) { fprintf(stderr, "[infer] 音频非法: %s\n", argv[4]); kprog_free(kp); return 2; }
        if (widx < 0)   { fprintf(stderr, "[infer] 未识别出命令词 (rc=%d)\n", widx); kprog_free(kp); return 2; }
        printf("[asr] word=\"%s\" dist=%.4f\n", res.word, res.score);
        infer_run_once(kp, res.word);   /* 识别词作为文本 → 特征 → kbc 推理 */
    } else if (strcmp(mode, "loop") == 0) {
        char line[2048];
        while (fgets(line, sizeof(line), stdin)) {
            size_t ln = strlen(line);
            while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r')) line[--ln] = 0;
            if (ln == 0) continue;
            infer_run_once(kp, line);
        }
    } else {
        kprog_free(kp);
        return infer_usage();
    }
    kprog_free(kp);
    return 0;
}
