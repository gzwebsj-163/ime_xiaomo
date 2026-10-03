/*
 * xiaomo - Kills 字节码 VM 内核实现
 *
 * 对应 vm_core.h 设计:
 *   - 64 个 64 位通用寄存器 (R0..R63)
 *   - 操作数栈 (VmStack): 表达式/传参/返回
 *   - 调用栈   (VmStack): 函数调用现场 (保存返回 PC + 全部寄存器快照)
 *   - 线性内存 (数据段): LOAD/STORE 访问
 *
 * 字节码格式 (Kills binary):
 *   [魔数 "KILLS" 5B] [版本 u8] [flags u8]
 *   [寄存器数 u32] [数据段大小 u32] [指令数 u32]
 *   [常量池...] [数据段...] [指令表...]
 *
 * 指令编码: opcode u8 | a i32 | b i32 | imm i64  (内部表示为 KillsIns)
 */
#include "vm_core.h"
#include "hw_direct.h"
#include "hw_oem.h"
#include "hw_dev.h"
#include "hw_wdbg.h"
#include "hw_flash.h"
#include "hw_pin.h"
#include "hw_dc.h"
#include "hw_dmc.h"
#include "hw_fault.h"
#include "hw_core.h"
#include "hw_main.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ================= 嵌入式 Linux 内核核心 (阶段六): 弱符号桥 =================
 * 主构建(不链接 TinyEMU)时, 这里提供弱定义空实现 → LINUX opcode 返回
 * "not supported", 程序不崩、其余功能不受影响。
 * 用 Makefile 链接 linux_embed.o + TinyEMU 核心时, linux_embed.c 的强符号
 * 自动覆盖这些弱符号 → 真 Linux 内核可被 .mo 字节码直接驱动。
 * 注意: 与 linux_embed.h 的声明保持完全一致(签名/语义)。
 */
typedef struct LinuxVM LinuxVM;
/* 注意: 本文件被主构建以 C++ 编译(g++ -x c++), 若不加 extern "C" 弱符号会
 * 变成 C++ 修饰名(__Z...), 与 linux_embed.o(gcc 编译) 的 C 名 _linux_init
 * 对不上 → 强符号永远无法覆盖弱符号, 真内核不被调用(只走空实现)。
 * 统一用 extern "C" 让两边符号名一致。 */
#ifdef __cplusplus
extern "C" {
#endif
__attribute__((weak)) LinuxVM *linux_init(const char *cfg_path) { (void)cfg_path; return NULL; }
__attribute__((weak)) int linux_exec(LinuxVM *vm, const char *cmd, const char *expect, int timeout_ms) { (void)vm;(void)cmd;(void)expect;(void)timeout_ms; return -1; }
__attribute__((weak)) int linux_read_output(LinuxVM *vm, char *buf, int buf_size) { (void)vm;(void)buf;(void)buf_size; return 0; }
__attribute__((weak)) int linux_has_output(LinuxVM *vm) { (void)vm; return 0; }
__attribute__((weak)) void linux_end(LinuxVM *vm) { (void)vm; }
#ifdef __cplusplus
}
#endif

/* ================= 程序构建 ================= */
KillsProgram* kprog_new(void) {
    KillsProgram* p = (KillsProgram*)calloc(1, sizeof(KillsProgram));
    return p;
}

void kprog_add_ins(KillsProgram* p, uint8_t op, int32_t a, int32_t b, int64_t imm) {
    if (!p) return;
    p->code = (KillsIns*)realloc(p->code, sizeof(KillsIns) * (p->code_count + 1));
    KillsIns* ins = &p->code[p->code_count++];
    ins->op = op; ins->a = a; ins->b = b; ins->imm = imm;
}

int kprog_add_const(KillsProgram* p, uint8_t type, int64_t iv, double fv, const char* sv) {
    if (!p) return -1;
    /* 去重: 字符串按文本、数值按 type+iv 精确匹配。
     * 关键: 同文本字符串复用同一常量索引 → 变量负常量标记 -(ci+1) 一致 →
     * 编译路径字符串 == 比较直接 JE 寄存器即语义正确 (probe_cap T2 根因)。 */
    for (uint32_t i = 0; i < p->const_count; i++) {
        KillsConst* c = &p->consts[i];
        if (type == 1) {
            if (c->type == 1 && c->sv && sv && strcmp(c->sv, sv) == 0) return (int)i;
        } else {
            if (c->type == type && c->iv == iv && c->fv == fv) return (int)i;
        }
    }
    p->consts = (KillsConst*)realloc(p->consts, sizeof(KillsConst) * (p->const_count + 1));
    KillsConst* c = &p->consts[p->const_count++];
    c->type = type; c->iv = iv; c->fv = fv;
    c->sv = sv ? strdup(sv) : NULL;
    return p->const_count - 1;
}

int kprog_add_func(KillsProgram* p, const char* name, uint32_t pc, uint32_t nparams) {
    if (!p) return -1;
    p->funcs = (KillsFunc*)realloc(p->funcs, sizeof(KillsFunc) * (p->func_count + 1));
    KillsFunc* f = &p->funcs[p->func_count++];
    f->name = strdup(name);
    f->pc = pc; f->nparams = nparams;
    return p->func_count - 1;
}

/* 在 data 段末尾追加 size 字节(返回起始地址), 用于数组/字符串运行时存储 */
uint32_t kprog_alloc_data(KillsProgram* p, uint32_t size) {
    if (!p || size == 0) return 0;
    uint32_t start = p->data_size;
    p->data = (uint8_t*)realloc(p->data, p->data_size + size);
    memset(p->data + start, 0, size);
    p->data_size += size;
    return start;
}

void kprog_free(KillsProgram* p) {
    if (!p) return;
    free(p->code); p->code = NULL; p->code_count = 0;
    if (p->consts) { for (uint32_t i = 0; i < p->const_count; i++) if (p->consts[i].sv) free(p->consts[i].sv); free(p->consts); }
    p->consts = NULL; p->const_count = 0;
    free(p->data); p->data = NULL; p->data_size = 0;
    if (p->funcs) { for (uint32_t i = 0; i < p->func_count; i++) if (p->funcs[i].name) free(p->funcs[i].name); free(p->funcs); }
    p->funcs = NULL; p->func_count = 0;
    free(p);
}

/* ================= 值工具 ================= */
static void kvm_add_output_ex(KillsVM* vm, const char* s, int append) {
    if (append && vm->output_count > 0) {
        /* 追加到最后一行 */
        char* old = vm->output[vm->output_count - 1];
        size_t newlen = strlen(old) + strlen(s) + 1;
        char* buf = (char*)malloc(newlen);
        snprintf(buf, newlen, "%s%s", old, s);
        free(old);
        vm->output[vm->output_count - 1] = buf;
        return;
    }
    if (vm->output_count >= vm->output_cap) {
        vm->output_cap = vm->output_cap ? vm->output_cap * 2 : 16;
        vm->output = (char**)realloc(vm->output, sizeof(char*) * vm->output_cap);
    }
    vm->output[vm->output_count++] = strdup(s);
}
static void kvm_add_output(KillsVM* vm, const char* s) { kvm_add_output_ex(vm, s, 0); }

/* ── output flush: 只在 input_wait 等待时调用 (对齐 ESP32 版) ──
 * 链式 print/LLM append 的拼接机制依赖 output[last] 持续累积;
 * 每轮回复完整缓冲后, 回到 input_wait 阻塞等待下一轮输入 → 此刻 flush
 * 恰好整行输出一轮回复。⚠️ 不能在每条指令后 flush(拆碎拼接)。 */
/* ── flush 重定向 (xdebugd 调试软件): input_wait 清空输出前回调, 行搬进调试会话 ── */
static kvm_flush_fn g_flush_redirect = NULL;
void kvm_set_flush_redirect(kvm_flush_fn fn) { g_flush_redirect = fn; }

static void kvm_flush_output(KillsVM* vm) {
    if (vm->output_count > 0) {
        /* xdebugd 重定向: 清空前把行搬进调试会话缓冲 (默认 NULL = 原行为) */
        if (g_flush_redirect) g_flush_redirect(vm);
        for (int i = 0; i < vm->output_count; i++) {
            if (vm->output[i]) {
                if (!g_flush_redirect) printf("%s\n", vm->output[i]);
                free(vm->output[i]);
                vm->output[i] = NULL;
            }
        }
        vm->output_count = 0;
        fflush(stdout);
    }
}

/* ================= FFI 表 (内置外部函数) ================= */
/* FFI idx: 0=print_int, 1=print_str, 2=sqrt,
            3=input_pending (0/1), 4=input_read(→ hash cid) */
typedef struct { int64_t result; const char* str; } FFIRet;

static long ffi_int_arg(KillsVM* vm, int reg) { return (long)vm->regs[reg]; }

/* ── 交互输入桥 ──────────────────────────────────────────────
 * openclaw 交互系统: VM 从"输入源"逐行读用户消息。
 * 宿主(host): 从 stdin 读; ESP32: 从 TCP 读。
 * ⚠️ FIFO 队列(16 槽)而不是单缓冲: 批量注入(nc 粘贴/管道多发)时
 *   单缓冲会被最后一条覆盖丢消息; 队列保证逐条消费不丢。
 *   且 llm_query 阻塞期间新消息只入队, 不会覆盖当前正在处理的 g_input_line
 *   (llm_query_host 依赖它做 cid 分类, 单缓冲下会被新消息污染)。 */
#define IO_Q_CAP 16
#define IO_Q_LEN 2048
static char g_io_q[IO_Q_CAP][IO_Q_LEN];
static int  g_io_head = 0, g_io_tail = 0, g_io_count = 0;
static char g_input_line[IO_Q_LEN] = "";   /* VM 正在处理的那一行 */
/* 供外部(宿主 main / ESP32 任务)写入待处理输入; 满则丢弃最老并提示 */
void kvm_io_push(const char* line) {
    if (g_io_count >= IO_Q_CAP) { fprintf(stderr, "[io] queue full, dropping oldest\n"); g_io_head = (g_io_head + 1) % IO_Q_CAP; g_io_count--; }
    snprintf(g_io_q[g_io_tail], IO_Q_LEN, "%s", line);
    g_io_tail = (g_io_tail + 1) % IO_Q_CAP; g_io_count++;
}
void kvm_io_pushf(const char* fmt, ...) {
    char tmp[IO_Q_LEN];
    va_list ap; va_start(ap, fmt); vsnprintf(tmp, IO_Q_LEN, fmt, ap); va_end(ap);
    kvm_io_push(tmp);
}
void kvm_io_set_pending(int v){ if (!v && g_io_count == 0) { } }  /* 兼容旧调用: 清空队列可选 */
int  kvm_io_pending(void){ return g_io_count > 0; }
const char* kvm_io_line(void){ return g_input_line; }
/* kbc 自动推理 (FFI 7 feat) 特征缓存 — 声明在 kvm_io_pop 之前供其标脏 */
#define INFER_DIM   64
#define INFER_Q16   65536
static uint64_t g_feats[INFER_DIM];
static int      g_feats_dirty = 1;
/* 弹出队首到 g_input_line (VM 消费); 空则返回 0 */
static int kvm_io_pop(void) {
    if (g_io_count <= 0) return 0;
    snprintf(g_input_line, IO_Q_LEN, "%s", g_io_q[g_io_head]);
    g_io_head = (g_io_head + 1) % IO_Q_CAP; g_io_count--;
    g_feats_dirty = 1;   /* 新行消费 → feat(i) 特征缓存失效 (kbc 自动推理) */
    return 1;
}

/* ============================================================
 * 特征提取 (kbc 自动推理 FFI 7: feat(i))
 * 与 examples/intent/train_intent.py 的 featurize 逐字节同款:
 *   - ASCII 字母数字连续段 = 词单元 → hash("w:"+word)
 *   - 连续 >=0x80 字节段(中文) → 逐字节滑窗 2-gram hash + 整段 unigram hash
 *   - hash: h=5381; h = h*33 + byte (mod 2^64); 桶 = h % 64
 *   - presence 特征: 值 ∈ {0, 65536} (Q16 的 1.0)
 * 惰性计算: 每消费一行输入只提取一次, 64 次 feat(i) 复用缓存。
 * ============================================================ */
static uint64_t infer_hash_n(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = h * 33 + b[i];
    return h;
}

static void infer_featurize(const char* s) {
    /* 双状态扫描: ASCII 词单元(小写化) / 多字节中文段(2-gram+unigram) */
    size_t n = strlen(s);
    memset(g_feats, 0, sizeof(g_feats));
    char word[256]; size_t wn = 0;
    uint8_t cjk[512]; size_t cn = 0;
    size_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) {
            if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
                if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
                if (wn < sizeof(word) - 1) word[wn++] = (char)c;
                i++;
            } else {
                /* 分隔符: 冲刷词 + 中文段 */
                if (wn) { uint64_t h = infer_hash_n(5381, "w:", 2); h = infer_hash_n(h, word, wn); g_feats[h % INFER_DIM] = INFER_Q16; wn = 0; }
                for (size_t k = 0; cn >= 2 && k + 1 < cn; k++)
                    g_feats[infer_hash_n(infer_hash_n(5381, &cjk[k], 1), &cjk[k+1], 1) % INFER_DIM] = INFER_Q16;
                if (cn) g_feats[infer_hash_n(5381, cjk, cn) % INFER_DIM] = INFER_Q16;
                cn = 0;
                i++;
            }
        } else {
            if (wn) { uint64_t h = infer_hash_n(5381, "w:", 2); h = infer_hash_n(h, word, wn); g_feats[h % INFER_DIM] = INFER_Q16; wn = 0; }
            if (cn < sizeof(cjk)) cjk[cn++] = c;
            i++;
        }
    }
    if (wn) { uint64_t h = infer_hash_n(5381, "w:", 2); h = infer_hash_n(h, word, wn); g_feats[h % INFER_DIM] = INFER_Q16; }
    for (size_t k = 0; cn >= 2 && k + 1 < cn; k++)
        g_feats[infer_hash_n(infer_hash_n(5381, &cjk[k], 1), &cjk[k+1], 1) % INFER_DIM] = INFER_Q16;
    if (cn) g_feats[infer_hash_n(5381, cjk, cn) % INFER_DIM] = INFER_Q16;
    g_feats_dirty = 0;
}

/* 供宿主侧读取当前行特征 (调试/对拍; 未消费新行时返回 NULL) */
const uint64_t* infer_feats_current(int* dim) {
    if (g_feats_dirty) return NULL;
    if (dim) *dim = INFER_DIM;
    return g_feats;
}

/* 简单中文/英文关键词分类 → cid (供 .mo 路由):
 *   1 打招呼(hi/hello/你好/在吗)  2 跑工具/计算(算/工具/tool/计算/weather)
 *   3 查状态(status/状态/几点/时间)  4 其它/闲聊
 * 输入行同时回显到 output, 便于双端对拍与 TCP 转发。 */
static int input_classify(const char* s) {
    const struct { const char* kw; int cid; } tab[] = {
        {"你好",1},{"在吗",1},{"hi",1},{"hi ",1},{"hello",1},{"hello ",1},
        {"工具",2},{"tool",2},{"算",2},{"计算",2},{"weather",2},{"天气",2},
        {"状态",3},{"status",3},{"几点",3},{"时间",3},{"现在",3},
    };
    for (size_t i = 0; i < sizeof(tab)/sizeof(tab[0]); i++)
        if (strstr(s, tab[i].kw)) return tab[i].cid;
    return 4;
}

/* ── FFI 6: llm_query_host() 宿主版真实 LLM 回复 ──
 * 链路: VM → TCP → 本地网关 127.0.0.1:9101 (llm_gate.py) → SiliconFlow
 * 协议: [4B 大端 len][payload] 请求; [4B 大端 len][reply] 回复 (可含换行) */
#define LLM_GATE_HOST   "127.0.0.1"
#define LLM_GATE_PORT_H 9101

static int llm_query_host(KillsVM* vm) {
    char payload[2300];
    int cid = input_classify(g_input_line);
    char esc[4600]; int e = 0;
    for (int i = 0; g_input_line[i] && e < (int)sizeof(esc)-2; i++) {
        char c = g_input_line[i];
        if (c == '"')  { esc[e++]='\\'; esc[e++]='"'; }
        else if (c == '\\') { esc[e++]='\\'; esc[e++]='\\'; }
        else if (c == '\n' || c == '\r') { esc[e++]=' '; }
        else esc[e++] = c;
    }
    esc[e] = '\0';
    snprintf(payload, sizeof(payload), "{\"cid\":%d,\"msg\":\"%s\"}", cid, esc);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fprintf(stderr, "[llm] socket fail\n"); return 0; }
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(LLM_GATE_PORT_H);
    sa.sin_addr.s_addr = inet_addr(LLM_GATE_HOST);
    struct timeval tv = { .tv_sec = 35, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        fprintf(stderr, "[llm] connect %s:%d fail\n", LLM_GATE_HOST, LLM_GATE_PORT_H);
        close(fd); return 0;
    }
    uint32_t nlen = htonl((uint32_t)strlen(payload));
    if (send(fd, &nlen, 4, 0) != 4 || send(fd, payload, strlen(payload), 0) <= 0) {
        fprintf(stderr, "[llm] send fail\n"); close(fd); return 0;
    }
    uint32_t rbe = 0; int got = 0;
    while (got < 4) { int n = recv(fd, ((char*)&rbe)+got, 4-got, 0); if (n <= 0) break; got += n; }
    if (got < 4) { fprintf(stderr, "[llm] recv hdr fail got=%d\n", got); close(fd); return 0; }
    uint32_t rlen = ntohl(rbe);
    if (rlen == 0 || rlen > 4096) { fprintf(stderr, "[llm] bad rlen=%u\n", rlen); close(fd); return 0; }
    char* reply = (char*)malloc(rlen+1);
    if (!reply) { close(fd); return 0; }
    got = 0;
    while (got < (int)rlen) { int n = recv(fd, reply+got, rlen-got, 0); if (n <= 0) break; got += n; }
    close(fd);
    if (got < (int)rlen) { fprintf(stderr, "[llm] recv body fail got=%d/%u\n", got, rlen); free(reply); return 0; }
    reply[got] = '\0';
    for (int i = 0; reply[i]; i++) if (reply[i]=='\n' || reply[i]=='\r') reply[i] = ' ';
    kvm_add_output_ex(vm, reply, 1);   /* append 到当前行 */
    fprintf(stderr, "[llm] reply (%u B): %.100s\n", rlen, reply);
    free(reply);
    return 1;
}

static void kvm_ffi(KillsVM* vm, int idx, int result_reg, int64_t imm_arg) {
    (void)imm_arg;  /* 仅 FFI 7 使用 */
    switch (idx) {
    case 0: { /* print_int from reg a */
        char buf[64]; snprintf(buf, sizeof(buf), "%lld", (long long)ffi_int_arg(vm, vm->regs[63] & 63));
        kvm_add_output(vm, buf); break;
    }
    case 1: { /* print str from const via reg a */
        int ci = (int)(vm->regs[63] & 63);
        if (ci >= 0 && ci < (int)vm->prog->const_count && vm->prog->consts[ci].type == 1 && vm->prog->consts[ci].sv)
            kvm_add_output(vm, vm->prog->consts[ci].sv);
        break;
    }
    case 2: { /* sqrt from reg a into result_reg */
        double v = (double)ffi_int_arg(vm, vm->regs[63] & 63);
        vm->regs[result_reg] = (int64_t)(v * v); break; /* 简单平方 */
    }
    case 3: { /* input_pending() → result_reg = 0/1 */
        vm->regs[result_reg] = kvm_io_pending() ? 1 : 0; break;
    }
    case 4: { /* input_read() → 消费一行, 回显, result_reg = 分类 cid */
        if (!kvm_io_pop()) { vm->regs[result_reg] = 0; break; }
        /* 回显到 output (TCP 侧据此可见用户原话) */
        char echo[2100]; snprintf(echo, sizeof(echo), "> %s", g_input_line);
        kvm_add_output(vm, echo);
        int cid = input_classify(g_input_line);
        vm->regs[result_reg] = cid;
        break;
    }
    case 5: { /* input_wait() → 阻塞直到有输入, 消费, result_reg = 分类 cid
                  (宿主: 轮询+usleep 让步 + flush 对齐 ESP32 版;
                   ⚠️ 轮询等待期间 flush: LLM 回复 append 到已有行, count 不变,
                   交互主线程按 count 增量轮询看不到 → 必须靠这里整行 flush) */
        while (!kvm_io_pop()) {
            kvm_flush_output(vm);
            if (vm->dbg_cancel) {      /* xdebugd 取消: 交换程序/复位时强制退出等待 */
                vm->halted = 1;
                vm->regs[result_reg] = 0;
                break;
            }
            usleep(20000);
        }
        if (vm->dbg_cancel) break;    /* 取消时不回显不分类, 直接落回主循环退出 */
        char echo[2100]; snprintf(echo, sizeof(echo), "> %s", g_input_line);
        kvm_add_output(vm, echo);
        int cid = input_classify(g_input_line);
        vm->regs[result_reg] = cid;
        break;
    }
    case 6: { /* llm_query() → 真实 LLM 回复 (宿主 POSIX 版: 连本地网关 127.0.0.1:9101)
                  llm_gate.py 转发 SiliconFlow → 回复 append 到当前行 */
        int ok = llm_query_host(vm);
        vm->regs[result_reg] = ok ? 1 : 0;
        break;
    }
    case 7: { /* feat(i) → 最近消费输入行的第 i 维特征 (Q16 presence, 0/65536)
                  imm 编码: imm-1 = 实参所在寄存器 (与 hw_dev 双参同款约定)
                  kbc 自动推理特征通道 (examples/intent/) */
        int arg_reg = (int)imm_arg - 1;
        long fi = (arg_reg >= 0 && arg_reg < 64) ? (long)vm->regs[arg_reg & 63] : 0;
        if (g_feats_dirty) infer_featurize(g_input_line);
        vm->regs[result_reg] = (fi >= 0 && fi < INFER_DIM)
            ? (int64_t)g_feats[fi] : 0;
        break;
    }
    default: break;
    }
}

/* ================= 执行 ================= */
void kvm_init(KillsVM* vm) {
    memset(vm, 0, sizeof(KillsVM));
    vstack_init(&vm->operand, 4096);
    vstack_init(&vm->callstack, 65536);
    vm->step_limit = 100000000;
}

void kvm_free(KillsVM* vm) {
    vstack_destroy(&vm->operand);
    vstack_destroy(&vm->callstack);
    for (int i = 0; i < vm->output_count; i++) free(vm->output[i]);
    if (vm->output) free(vm->output);
    vm->output = NULL; vm->output_count = vm->output_cap = 0;
}

int kvm_output_count(const KillsVM* vm) { return vm->output_count; }
const char* kvm_output(KillsVM* vm, int idx) {
    if (idx < 0 || idx >= vm->output_count) return NULL;
    return vm->output[idx];
}

/* 读取第二个操作数值: b>=0 为寄存器, b<0 用 imm 全无符号(简化: 用 flag 区分由调用侧决定) */
/* 本内核统一: 当 ins->b >= 0 视为寄存器下标; 否则用 ins->imm */
static int is_reg_operand(int32_t b) { return b >= 0; }

/* 执行加载立即数或寄存器到 dst */
int kvm_run(KillsVM* vm, const KillsProgram* prog) {
    if (!vm || !prog) return 1;
    vm->prog = prog;
    if (vm->dbg_resume) {
        /* 续跑 (xdebugd 调试会话): 跳过上电复位, 从当前现场继续 */
        vm->dbg_resume = 0;
    } else {
    vm->pc = 0;
    vm->halted = 0;
    vm->steps = 0;
    vm->error_count = 0;
    vstack_reset(&vm->operand);
    vstack_reset(&vm->callstack);
    memset(vm->regs, 0, sizeof(vm->regs));

    /* OEM 设备签名上电烧录: 熔丝值固化进 R127, 全程写保护见循环末尾 */
    hw_oem_protect(vm);

    /* hw_dev 命令表上电初始化: 清动态注册表 (旧表不跨运行残留) */
    hw_dev_init(NULL);

    /* hw_fault 故障注入表/BSP 上电复位: 健康态起步 (旧注入不跨运行残留) */
    hw_fault_init(NULL);

    /* hw_core 内核 DNA 层上电: 黄金自证 + 槽位复位 (旧槽不跨运行残留) */
    hw_core_init(NULL);

    /* hw_main 家族总调度上电: 黄金自证 + 探针缓存复位 (幂等) */
    hw_main_init(NULL);

    /* hw_wdbg 无线调试器信号层上电: 桥接/监控/线序复位 (旧状态不跨运行残留) */
    hw_wdbg_init(NULL);

    /* hw_flash ROM 烧录层上电: 会话/计数/BSP/静态 flash 模型复位 (旧状态不跨运行残留) */
    hw_flash_init(NULL);

    /* hw_pin 引脚档案层上电: 活动档案/GPIO 电平/器件模型复位 (旧状态不跨运行残留) */
    hw_pin_init(NULL);

    /* hw_dc DC 电源信号层上电: 黄金自证 + 信号/极值/BSP 复位 (旧状态不跨运行残留) */
    hw_dc_init(NULL);
    }
    vm->stop_request = 0;

    /* 每帧保存: 返回 PC + 快照区寄存器 = 1+KILLS_SNAP_NREG 个 u64 */
    while (!vm->halted) {
        if (vm->steps++ > vm->step_limit) {
            snprintf(vm->error_msg, sizeof(vm->error_msg), "step limit exceeded (%u)", vm->step_limit);
            vm->error_count = 1;
            return 1;
        }
        if (vm->pc >= prog->code_count) break;
        KillsIns* ins = &prog->code[vm->pc];
        uint32_t next_pc = vm->pc + 1;

        /* 调试钩子 (debug 工具): 每步执行前回调; 返回非 0 -> 提前停机。
         * 停机语义: vm->pc 停在当前指令, halted 不置位, stop_request 置 1。 */
        if (vm->dbg_hook &&
            vm->dbg_hook(vm->dbg_ud, vm, ins, next_pc, (uint64_t)vm->steps) != 0) {
            vm->stop_request = 1;
            return 0;
        }

        switch (ins->op) {
        case OP_NOP: break;
        case OP_MOV:
            vm->regs[ins->a] = is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm;
            break;
        case OP_ADD: vm->regs[ins->a] = vm->regs[ins->a] + (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_SUB: vm->regs[ins->a] = vm->regs[ins->a] - (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_MUL: vm->regs[ins->a] = vm->regs[ins->a] * (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_DIV:
            { int64_t d = is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm;
              if (d == 0) { snprintf(vm->error_msg,sizeof(vm->error_msg),"divide by zero at pc %u",vm->pc); vm->error_count=1; return 1; }
              vm->regs[ins->a] = vm->regs[ins->a] / d; break; }
        case OP_MOD:
            { int64_t d = is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm;
              if (d == 0) { snprintf(vm->error_msg,sizeof(vm->error_msg),"mod by zero at pc %u",vm->pc); vm->error_count=1; return 1; }
              vm->regs[ins->a] = vm->regs[ins->a] % d; break; }
        case OP_AND: vm->regs[ins->a] = vm->regs[ins->a] & (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        /* ---- 浮点扩展: 寄存器 64 位值 = double 位模式 ---- */
        case OP_FADD: { double x, y; memcpy(&x, &vm->regs[ins->a], 8); memcpy(&y, &vm->regs[ins->b], 8); double z = x + y; memcpy(&vm->regs[ins->a], &z, 8); break; }
        case OP_FSUB: { double x, y; memcpy(&x, &vm->regs[ins->a], 8); memcpy(&y, &vm->regs[ins->b], 8); double z = x - y; memcpy(&vm->regs[ins->a], &z, 8); break; }
        case OP_FMUL: { double x, y; memcpy(&x, &vm->regs[ins->a], 8); memcpy(&y, &vm->regs[ins->b], 8); double z = x * y; memcpy(&vm->regs[ins->a], &z, 8); break; }
        case OP_FDIV: { double x, y; memcpy(&x, &vm->regs[ins->a], 8); memcpy(&y, &vm->regs[ins->b], 8); double z = (y == 0.0) ? (x / 0.0) : (x / y); memcpy(&vm->regs[ins->a], &z, 8); break; }
        case OP_F2I:  { double x; memcpy(&x, &vm->regs[ins->a], 8); vm->regs[ins->a] = (int64_t)x; break; }  /* 截断, 对齐 val_float 的 ival */
        case OP_I2F:  { double z = (double)vm->regs[ins->a]; memcpy(&vm->regs[ins->a], &z, 8); break; }
        case OP_OR:  vm->regs[ins->a] = vm->regs[ins->a] | (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_XOR: vm->regs[ins->a] = vm->regs[ins->a] ^ (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_NOT: vm->regs[ins->a] = ~vm->regs[ins->a]; break;
        case OP_SHL: vm->regs[ins->a] = vm->regs[ins->a] << (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm); break;
        case OP_SHR: vm->regs[ins->a] = (int64_t)((uint64_t)vm->regs[ins->a] >> (is_reg_operand(ins->b) ? vm->regs[ins->b] : ins->imm)); break;
        case OP_PUSH:
            { int64_t v = is_reg_operand(ins->a) ? vm->regs[ins->a] : ins->imm;
              if (vstack_push_u64(&vm->operand, (uint64_t)v) != 0) { snprintf(vm->error_msg,sizeof(vm->error_msg),"operand stack overflow at pc %u",vm->pc); vm->error_count=1; return 1; } break; }
        case OP_POP:
            { uint64_t v; if (vstack_pop_u64(&vm->operand, &v) == 0) vm->regs[ins->a] = (int64_t)v; break; }
        case OP_LOAD:
            { uint64_t addr = (uint64_t)vm->regs[ins->b];
              if (addr >= prog->data_size) { snprintf(vm->error_msg,sizeof(vm->error_msg),"LOAD out of bounds %llu at pc %u",(unsigned long long)addr,vm->pc); vm->error_count=1; return 1; }
              vm->regs[ins->a] = (int8_t)prog->data[addr]; break; }
        case OP_STORE:
            { uint64_t addr = (uint64_t)vm->regs[ins->a];
              if (addr >= prog->data_size) { snprintf(vm->error_msg,sizeof(vm->error_msg),"STORE out of bounds %llu at pc %u",(unsigned long long)addr,vm->pc); vm->error_count=1; return 1; }
              prog->data[addr] = (uint8_t)(vm->regs[ins->b] & 0xFF); break; }
        case OP_LOAD64:
            { uint64_t addr = (uint64_t)vm->regs[ins->b];
              if (addr + 8 > prog->data_size) { snprintf(vm->error_msg,sizeof(vm->error_msg),"LOAD64 out of bounds %llu at pc %u",(unsigned long long)addr,vm->pc); vm->error_count=1; return 1; }
              int64_t v; memcpy(&v, prog->data + addr, 8); vm->regs[ins->a] = v;
              if (getenv("KVM_TRACE")) fprintf(stderr, "[TRACE] LOAD64 pc=%u addr=%llu -> %lld (R%d)\n", vm->pc, (unsigned long long)addr, (long long)v, ins->a); break; }
        case OP_STORE64:
            { uint64_t addr = (uint64_t)vm->regs[ins->a];
              if (addr + 8 > prog->data_size) { snprintf(vm->error_msg,sizeof(vm->error_msg),"STORE64 out of bounds %llu at pc %u",(unsigned long long)addr,vm->pc); vm->error_count=1; return 1; }
              int64_t v = vm->regs[ins->b]; memcpy(prog->data + addr, &v, 8);
              if (getenv("KVM_TRACE")) fprintf(stderr, "[TRACE] STORE64 pc=%u addr=%llu <- %lld (R%d)\n", vm->pc, (unsigned long long)addr, (long long)v, ins->b); break; }
        case OP_JMP: next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JZ:  if (vm->regs[ins->a] == 0) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JNZ: if (vm->regs[ins->a] != 0) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JE:  if (vm->regs[ins->a] == (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JNE: if (getenv("KVM_TRACE")) fprintf(stderr, "[TRACE] JNE pc=%u R%d=%lld vs R%d=%lld -> %s\n", vm->pc, ins->a, (long long)vm->regs[ins->a], ins->b, ins->b >= 0 ? (long long)vm->regs[ins->b] : 0LL, vm->regs[ins->a] != (ins->b >= 0 ? vm->regs[ins->b] : 0) ? "JUMP" : "fall"); if (vm->regs[ins->a] != (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JG:  if (vm->regs[ins->a] >  (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JGE: if (vm->regs[ins->a] >= (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JL:  if (vm->regs[ins->a] <  (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_JLE: if (vm->regs[ins->a] <= (ins->b >= 0 ? vm->regs[ins->b] : 0)) next_pc = vm->pc + (uint32_t)(int32_t)ins->imm; break;
        case OP_CALL:
            { int fi = ins->a;
              if (fi < 0 || fi >= (int)prog->func_count) { snprintf(vm->error_msg,sizeof(vm->error_msg),"CALL bad func idx %d",fi); vm->error_count=1; return 1; }
              KillsFunc* fn = &prog->funcs[fi];
              /* 从操作数栈弹 nparams 个实参 (逆序压入参数寄存器 R0..) */
              for (int k = fn->nparams - 1; k >= 0; k--) { uint64_t a; if (vstack_pop_u64(&vm->operand, &a) == 0) vm->regs[k] = (int64_t)a; }
              if (getenv("KVM_TRACE")) { fprintf(stderr, "[TRACE] CALL fn=%d entry=%u nparams=%d", fi, fn->pc, fn->nparams); for (int k = 0; k < fn->nparams && k < 4; k++) fprintf(stderr, " R%d=%lld", k, (long long)vm->regs[k]); fprintf(stderr, "\n"); }
              /* 保存返回现场: 返回 PC + 快照区寄存器。
               * ⚠️ push 失败不可静默忽略: 否则 RET 会弹到垃圾现场,
               * next_pc 野生跳转 → 寄存器堆越界写 → 堆腐蚀 (ch340_dock 段错误链路)。 */
              if (vstack_push_u64(&vm->callstack, (uint64_t)next_pc) != 0) { snprintf(vm->error_msg,sizeof(vm->error_msg),"call stack overflow at pc %u",vm->pc); vm->error_count=1; return 1; }
              for (int r = 0; r < KILLS_SNAP_NREG; r++) { if (vstack_push_u64(&vm->callstack, (uint64_t)vm->regs[r]) != 0) { snprintf(vm->error_msg,sizeof(vm->error_msg),"call stack overflow at pc %u",vm->pc); vm->error_count=1; return 1; } }
              next_pc = fn->pc;
              break; }
        case OP_RET:
            { /* 恢复现场但不覆盖 R0 (R0 约定为返回值寄存器) */
              if (getenv("KVM_TRACE")) fprintf(stderr, "[TRACE] RET at pc=%u R0=%lld\n", vm->pc, (long long)vm->regs[0]);
              for (int r = KILLS_SNAP_NREG - 1; r >= 1; r--) { uint64_t v; if (vstack_pop_u64(&vm->callstack, &v) == 0) vm->regs[r] = (int64_t)v; }
              uint64_t r0; if (vstack_pop_u64(&vm->callstack, &r0) == 0) { /* 丢弃保存的 R0, 保留当前 R0=返回值 */ }
              uint64_t retpc; if (vstack_pop_u64(&vm->callstack, &retpc) == 0) next_pc = (uint32_t)retpc; break; }
        case OP_PRINT:
            { int append = (ins->imm & 1);
              int raw = (ins->imm & 2);   /* raw: 直接打印整数值(允许负数) */
              int ffmt = (ins->imm & 4);  /* ffmt: 按 double 位模式 %g 打印 (浮点扩展) */
              if (ins->b >= 0) {
                  /* PRINT -1, const_idx : 打印常量池字符串 */
                  int ci = ins->b;
                  if (ci < (int)prog->const_count && prog->consts[ci].type == 1 && prog->consts[ci].sv) {
                      kvm_add_output_ex(vm, prog->consts[ci].sv, append);
                  } else {
                      char buf[64]; snprintf(buf, sizeof(buf), "<bad const %d>", ci); kvm_add_output_ex(vm, buf, append);
                  }
              } else {
                  int64_t v = vm->regs[ins->a];
                  if (ffmt) {
                      /* 浮点打印: 寄存器 64 位值 = double 位模式, %g 对齐解释器 */
                      double d; memcpy(&d, &v, 8);
                      char buf[64]; snprintf(buf, sizeof(buf), "%g", d); kvm_add_output_ex(vm, buf, append);
                  } else if (raw) {
                      /* raw 裸整数打印: 允许负值 (imm bit1) */
                      char buf[64]; snprintf(buf, sizeof(buf), "%lld", (long long)v); kvm_add_output_ex(vm, buf, append);
                  } else if (v < 0) {
                      /* R_TMP < 0 = 常量索引标记 (值 -(ci+1)) */
                      int ci = (int)(-v - 1);
                      if (ci < (int)prog->const_count && prog->consts[ci].type == 1 && prog->consts[ci].sv) {
                          kvm_add_output_ex(vm, prog->consts[ci].sv, append);
                      } else {
                          char buf[64]; snprintf(buf, sizeof(buf), "<bad const %d>", ci); kvm_add_output_ex(vm, buf, append);
                      }
                  } else {
                      char buf[64]; snprintf(buf, sizeof(buf), "%lld", (long long)v); kvm_add_output_ex(vm, buf, append);
                  }
              }
              break; }
        case OP_FFI:
            kvm_ffi(vm, ins->a, ins->b >= 0 ? ins->b : 0, ins->imm);
            break;
        case OP_HALT:
            vm->halted = 1; break;
        /* ---- 硬件直访指令 ----
         * ⚠️ 陷阱修复(2026-08-27): 枚举缓冲必须 malloc 不能放栈!
         *    devbuf[65536] 在栈上 → GCC 给整个 kvm_run 分配超大栈帧
         *    (反汇编: lui t0,0xfff00; add sp,sp,t0 = sp-=1MB) → 在栈小的
         *    嵌入式平台(ESP32-C6 3584B) sp 直接掉出 RAM → 硬栈保护异常
         *    静默卡死(宿主 x86 栈 8MB 从未暴露)。统一 heap 后栈帧回归正常。 */
        case OP_HW_PCI_ENUM: {
            char* devbuf = (char*)malloc(65536);
            if (!devbuf) break;
            int n = hw_pci_enumerate(devbuf, 65536);
            if (n > 0) {
                /* 按行分割输出 */
                char* line = strtok(devbuf, "\n");
                while (line) {
                    kvm_add_output(vm, line);
                    line = strtok(NULL, "\n");
                }
            }
            free(devbuf);
            break;
        }
        case OP_HW_USB_ENUM: {
            char* devbuf = (char*)malloc(65536);
            if (!devbuf) break;
            int n = hw_usb_enumerate(devbuf, 65536);
            if (n > 0) {
                char* line = strtok(devbuf, "\n");
                while (line) {
                    kvm_add_output(vm, line);
                    line = strtok(NULL, "\n");
                }
            }
            free(devbuf);
            break;
        }
        case OP_HW_SERIAL_ENUM: {
            char* devbuf = (char*)malloc(65536);
            if (!devbuf) break;
            int n = hw_serial_enumerate(devbuf, 65536);
            if (n > 0) {
                char* line = strtok(devbuf, "\n");
                while (line) {
                    kvm_add_output(vm, line);
                    line = strtok(NULL, "\n");
                }
            }
            free(devbuf);
            break;
        }
        case OP_HW_CPU_INFO: {
            char infobuf[4096];
            int n = hw_cpu_info(infobuf, sizeof(infobuf));
            if (n > 0) {
                kvm_add_output(vm, infobuf);
            }
            break;
        }
        case OP_HW_PCI_RD: {
            /* imm = (dev<<16 | func<<8 | offset) */
            uint8_t bus = (uint8_t)(ins->b & 0xFF);
            uint8_t dev = (uint8_t)((ins->imm >> 16) & 0xFF);
            uint8_t func = (uint8_t)((ins->imm >> 8) & 0xFF);
            uint32_t offset = (uint32_t)(ins->imm & 0xFF);
            uint32_t val = 0xFFFFFFFF;
            int rc = hw_pci_read(bus, dev, func, offset, &val, 4);
            vm->regs[ins->a] = rc == 0 ? (int64_t)val : -1;
            break;
        }
        case OP_HW_UART_OPEN: {
            /* b = const_idx (路径字符串), a = baud_reg */
            int ci = ins->b;
            int baud = (int)vm->regs[ins->a];
            if (ci >= 0 && ci < (int)prog->const_count &&
                prog->consts[ci].type == 1 && prog->consts[ci].sv) {
                int handle = hw_uart_open(prog->consts[ci].sv, baud > 0 ? baud : 9600);
                vm->regs[ins->a] = handle;
            } else {
                vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_UART_CLOSE: {
            int handle = (int)vm->regs[ins->a];
            int rc = hw_uart_close(handle);
            vm->regs[ins->a] = rc;
            break;
        }
        case OP_HW_UART_RD: {
            /* a=dst_reg, b=handle_reg, imm=maxlen */
            int handle = (int)vm->regs[ins->b];
            int maxlen = (int)ins->imm;
            if (maxlen <= 0 || maxlen > 4096) maxlen = 256;
            uint8_t* rbuf = (uint8_t*)malloc(maxlen + 1);
            int n = hw_uart_read(handle, rbuf, maxlen);
            if (n > 0) {
                rbuf[n] = '\0';
                /* 把读取的数据写入 data 段，返回地址 */
                uint32_t addr = kprog_alloc_data((KillsProgram*)prog, n + 1);
                memcpy(prog->data + addr, rbuf, n + 1);
                vm->regs[ins->a] = (int64_t)addr;
            } else {
                vm->regs[ins->a] = n < 0 ? -1 : 0;
            }
            free(rbuf);
            break;
        }
        case OP_HW_UART_WR: {
            /* a=handle_reg, b=data_reg, imm=len
             * data_reg 指向 data 段中的地址 */
            int handle = (int)vm->regs[ins->a];
            uint64_t addr = (uint64_t)vm->regs[ins->b];
            int len = (int)ins->imm;
            if (addr + len <= prog->data_size) {
                int n = hw_uart_write(handle, prog->data + addr, len);
                vm->regs[ins->a] = n;
            } else {
                vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_SYS_INFO: {
            uint64_t total = 0, free_bytes = 0;
            hw_phys_mem_info(&total, &free_bytes);
            /* 返回: R[ins->a] = total, R[ins->b] = free */
            vm->regs[ins->a] = (int64_t)total;
            if (ins->b >= 0) vm->regs[ins->b] = (int64_t)free_bytes;
            break;
        }
        /* ---- 嵌入式 Linux 内核核心 (阶段六) ---- */
        case OP_LINUX_INIT: {
            /* b=cfg_path_const_idx, a=dst_handle_reg */
            int ci = ins->b;
            if (ci >= 0 && ci < (int)prog->const_count &&
                prog->consts[ci].type == 1 && prog->consts[ci].sv) {
                LinuxVM *lv = linux_init(prog->consts[ci].sv);
                vm->regs[ins->a] = lv ? (int64_t)(intptr_t)lv : -1;
            } else {
                vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_LINUX_EXEC: {
            /* a=handle_reg, b=cmd_const_idx, imm=expect_const_idx(<0 不等待) */
            LinuxVM *lv = (LinuxVM *)(intptr_t)vm->regs[ins->a];
            int rc = -1;
            if (lv && lv != (LinuxVM *)(intptr_t)-1) {
                const char *cmd = "", *expect = NULL;
                if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                    prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv)
                    cmd = prog->consts[ins->b].sv;
                if (ins->imm >= 0 && ins->imm < (int)prog->const_count &&
                    prog->consts[ins->imm].type == 1 && prog->consts[ins->imm].sv)
                    expect = prog->consts[ins->imm].sv;
                rc = linux_exec(lv, cmd, expect, 30000);
            }
            vm->regs[ins->a] = rc;
            break;
        }
        case OP_LINUX_READ: {
            /* a=handle_reg, b=count_reg; 读走 guest 全部输出→data 段+追加到 KVM 输出 */
            LinuxVM *lv = (LinuxVM *)(intptr_t)vm->regs[ins->a];
            int count = 0;
            if (lv && lv != (LinuxVM *)(intptr_t)-1 && linux_has_output(lv)) {
                char* buf = (char*)malloc(1 << 20);
                if (!buf) break;
                int n = linux_read_output(lv, buf, 1 << 20);
                if (n > 0) {
                    uint32_t addr = kprog_alloc_data((KillsProgram *)prog, (uint32_t)n + 1);
                    memcpy(prog->data + addr, buf, (size_t)n + 1);
                    vm->regs[ins->a] = (int64_t)addr;
                    /* guest 输出按行追加到 KVM 输出 */
                    char *cpy = strdup(buf);
                    for (char *line = strtok(cpy, "\n"); line; line = strtok(NULL, "\n")) {
                        kvm_add_output(vm, line);
                        count++;
                    }
                    free(cpy);
                }
                free(buf);
            }
            if (ins->b >= 0) vm->regs[ins->b] = count;
            break;
        }
        case OP_LINUX_END: {
            LinuxVM *lv = (LinuxVM *)(intptr_t)vm->regs[ins->a];
            if (lv && lv != (LinuxVM *)(intptr_t)-1) linux_end(lv);
            vm->regs[ins->a] = 0;
            break;
        }
        case OP_HW_SIG_RD:
            /* OEM 设备签名读: dst = 熔丝签名寄存器 R127 (只读透传) */
            if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)vm->sig_fuse;
            break;
        case OP_HW_DEV_CALL: {
            /* hw_dev 命令分发 (2026-09-07): dst = hw_dev_dispatch(命令串)
             * b = 命令串常量索引 (同 LINUX_* 的传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_dev("fmt", 数值) 的编译产物; 旧 .kbc imm=0 不追加);
             * rc: 0x00=命中执行 / 0xFF(255)=未找到命令; arg 传 vm 供 handler 联动内核。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char devbuf[512];
                    snprintf(devbuf, sizeof(devbuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_dev_dispatch(devbuf, vm);
                } else {
                    rc = hw_dev_dispatch(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = 0xFF;
            }
            break;
        }
        case OP_HW_FAULT_CALL: {
            /* TFT 模组故障诊断 (2026-09-24): dst = hw_fault_cmd(命令串)
             * b = 命令串常量索引 (同 OP_HW_DEV_CALL 传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_fault("fmt", 数值) 的编译产物);
             * 返回: >=0 诊断结果 (scan=故障数/0 健康; pin N=故障码) / -1 未识别
             *   (uint8 回绕 → 255); arg 传 vm 供未来真机联动内核。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char fltbuf[512];
                    snprintf(fltbuf, sizeof(fltbuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_fault_cmd(fltbuf, vm);
                } else {
                    rc = hw_fault_cmd(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_CORE_CALL: {
            /* 内核 DNA 编码层 (2026-09-28): dst = hw_core_cmd(命令串)
             * b = 命令串常量索引 (同 OP_HW_DEV_CALL 传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_core("fmt", 数值) 的编译产物);
             * 返回: >=0 结果 (count/ok/idx N/slot/free/mode) / -1 未识别
             *   (uint8 回绕 → 255); arg 传 vm 供未来真机联动内核。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char corebuf[512];
                    snprintf(corebuf, sizeof(corebuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_core_cmd(corebuf, vm);
                } else {
                    rc = hw_core_cmd(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_MAIN_CALL: {
            /* hw 家族总调度 (2026-09-28): dst = hw_main_cmd(命令串)
             * b = 命令串常量索引 (同 OP_HW_DEV/CORE_CALL 传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_main("fmt", 数值) 的编译产物);
             * 命令: count=9 / sum=(int)黄金(≥2^31 时为负) / ok=1 / idx N=类ID /
             *        find ID=表序 / probe ID=探针结果 / probeall / selftest;
             * 返回: >=0 结果 / -1 未识别 (uint8 回绕 → 255)。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char mainbuf[512];
                    snprintf(mainbuf, sizeof(mainbuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_main_cmd(mainbuf, vm);
                } else {
                    rc = hw_main_cmd(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_WDBG_CALL: {
            /* 无线调试器信号层 (2026-09-29): dst = hw_wdbg_cmd(命令串)
             * b = 命令串常量索引 (同 OP_HW_DEV/CORE/MAIN_CALL 传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_wdbg("fmt", 数值) 的编译产物);
             * 命令: bridge open|close|baud|wr|rd|stat / pwm out|off|inject|meas|stat /
             *       spi mode|xfer|list|clr / i2c wr|rd|list|clr /
             *       pin def|get|set|reset / status / count / mode / help;
             * 返回: >=0 结果码 (0x00 OK / 0x01 NOARGS / 0x02 BADARG /
             *       0x03 IOERR / 0x04 STATE) / -1 未识别 / -2 help。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char wdbuf[512];
                    snprintf(wdbuf, sizeof(wdbuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_wdbg_cmd(wdbuf, vm);
                } else {
                    rc = hw_wdbg_cmd(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_FLASH_CALL: {
            /* ESP32 ROM 下载协议烧录层 (2026-09-30): dst = hw_flash_cmd(命令串)
             * b = 命令串常量索引 (同 OP_HW_DEV/CORE/MAIN/WDBG_CALL 传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_flash("fmt", 数值) 的编译产物);
             * 命令: card|mode|slip|md5|sync|chip|run N|verify|stat|selftest
             * 返回: >=0 结果码 (0x00 OK / 0x01 NOARGS / 0x02 BADARG /
             *       0x03 IOERR / 0x04 PROTO / 0x05 CHECKSUM / 0x06 BADSIZE) /
             *       -1 未识别 / -2 help。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char flbuf[512];
                    snprintf(flbuf, sizeof(flbuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_flash_cmd(flbuf, vm);
                } else {
                    rc = hw_flash_cmd(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_PIN_CALL: {
            /* 引脚档案/双模驱动/编程电压层 (2026-09-30): dst = hw_pin_cmd(命令串)
             * b = 命令串常量索引 (同 OP_HW_DEV/CORE/MAIN/WDBG/FLASH_CALL 传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_pin("fmt", 数值) 的编译产物);
             * 命令: card|mode|profiles|load NAME|valid|drv hw|bb|id|read A|
             *       erase A|wtest|vpp N|vppread|stat|selftest
             * 返回: >=0 结果码 (0x00 OK / 0x01 NOARGS / 0x02 BADARG / 0x03 IOERR /
             *       0x04 NOFLASH / 0x05 VERIFY / 0x06 NODEV / 0x07 RANGE) /
             *       -1 未识别 / -2 help。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char pnbuf[512];
                    snprintf(pnbuf, sizeof(pnbuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_pin_cmd(pnbuf, vm);
                } else {
                    rc = hw_pin_cmd(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_DC_CALL: {
            /* DC 电源信号层 (2026-10-01): dst = hw_dc_cmd(命令串)
             * b = 命令串常量索引 (同 OP_HW_DEV/CORE/MAIN/WDBG/FLASH/PIN_CALL 传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_dc("fmt", 数值) 的编译产物);
             * 命令: count|datacount|ok|mode|sig N|name N|set N V|base N|
             *       range N|data N|help
             * 返回: >=0 结果 (count=8 / ok=1 / sig N=读数 / base=参考预值 /
             *       range=1|0 / data=组合帧) / -1 未识别或参数非法 / -2 help。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char dcbuf[512];
                    snprintf(dcbuf, sizeof(dcbuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_dc_cmd(dcbuf, vm);
                } else {
                    rc = hw_dc_cmd(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = -1;
            }
            break;
        }
        case OP_HW_DMC_CALL: {
            /* DMC 设备管理层 (2026-10-01): dst = hw_dmc_cmd(命令串)
             * b = 命令串常量索引 (同 OP_HW_DC_CALL 传参约定);
             * imm > 0 → 寄存器 (imm-1) 的十进制值动态追加到命令串尾
             *   (mo2kbc 双参内置 hw_dmc("fmt", 数值) 的编译产物);
             * 命令: count|cmds|states|errs|golden|frame|ok|crcvec|hello|selftest|help
             * 返回: >=0 结果 (count=9 / states=7 / errs=8 / golden=0x169A603E /
             *       ok=1 / crcvec=1 / frame=258 / hello=1) / -1 未识别 / -2 help。 */
            if (ins->b >= 0 && ins->b < (int)prog->const_count &&
                prog->consts[ins->b].type == 1 && prog->consts[ins->b].sv) {
                int rc;
                if (ins->imm > 0 && ins->imm - 1 < KILLS_NREG) {
                    char dmcbuf[512];
                    snprintf(dmcbuf, sizeof(dmcbuf), "%s%lld",
                             prog->consts[ins->b].sv,
                             (long long)vm->regs[ins->imm - 1]);
                    rc = hw_dmc_cmd(dmcbuf, vm);
                } else {
                    rc = hw_dmc_cmd(prog->consts[ins->b].sv, vm);
                }
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = (int64_t)rc;
            } else {
                if (ins->a >= 0 && ins->a < KILLS_NREG) vm->regs[ins->a] = -1;
            }
            break;
        }
        default:
            snprintf(vm->error_msg, sizeof(vm->error_msg), "unknown opcode %u at pc %u", ins->op, vm->pc);
            vm->error_count = 1; return 1;
        }
        /* ---- OEM 签名寄存器写屏蔽 (硬件 RO 语义) ----
         * 任何指令若在本步越权写入 R127, 此处立即回滚为熔丝值并计数。
         * 程序视角: R127 永远是设备标志, 写不进去。 */
        if (vm->regs[KVM_REG_SIG] != (int64_t)vm->sig_fuse) {
            vm->sig_violations++;
            vm->regs[KVM_REG_SIG] = (int64_t)vm->sig_fuse;
        }
        vm->pc = next_pc;
    }
    vm->halted = 1;
    return 0;
}

/* ================= 反汇编 (调试) ================= */
static const char* kvm_opname(uint8_t op) {
    switch (op) {
    case OP_NOP: return "NOP"; case OP_MOV: return "MOV";
    case OP_ADD: return "ADD"; case OP_SUB: return "SUB"; case OP_MUL: return "MUL";
    case OP_DIV: return "DIV"; case OP_MOD: return "MOD"; case OP_AND: return "AND";
    case OP_OR: return "OR"; case OP_XOR: return "XOR"; case OP_NOT: return "NOT";
    case OP_SHL: return "SHL"; case OP_SHR: return "SHR"; case OP_PUSH: return "PUSH";
    case OP_POP: return "POP"; case OP_LOAD: return "LOAD"; case OP_STORE: return "STORE";
    case OP_LOAD64: return "LOAD64"; case OP_STORE64: return "STORE64";
    case OP_JMP: return "JMP"; case OP_JZ: return "JZ"; case OP_JNZ: return "JNZ";
    case OP_JE: return "JE"; case OP_JNE: return "JNE"; case OP_JG: return "JG";
    case OP_JGE: return "JGE"; case OP_JL: return "JL"; case OP_JLE: return "JLE";
    case OP_CALL: return "CALL"; case OP_RET: return "RET"; case OP_FFI: return "FFI";
    case OP_PRINT: return "PRINT"; case OP_HALT: return "HALT";
    case OP_HW_PCI_ENUM: return "HW_PCI_ENUM";
    case OP_HW_USB_ENUM: return "HW_USB_ENUM";
    case OP_HW_SERIAL_ENUM: return "HW_SERIAL_ENUM";
    case OP_HW_CPU_INFO: return "HW_CPU_INFO";
    case OP_HW_PCI_RD: return "HW_PCI_RD";
    case OP_HW_UART_OPEN: return "HW_UART_OPEN";
    case OP_HW_UART_CLOSE: return "HW_UART_CLOSE";
    case OP_HW_UART_RD: return "HW_UART_RD";
    case OP_HW_UART_WR: return "HW_UART_WR";
    case OP_HW_SYS_INFO: return "HW_SYS_INFO";
    case OP_LINUX_INIT: return "LINUX_INIT";
    case OP_LINUX_EXEC: return "LINUX_EXEC";
    case OP_LINUX_READ: return "LINUX_READ";
    case OP_LINUX_END: return "LINUX_END";
    case OP_HW_SIG_RD: return "HW_SIG_RD";
    case OP_HW_DEV_CALL: return "HW_DEV_CALL";
    case OP_HW_FAULT_CALL: return "HW_FAULT_CALL";
    case OP_HW_CORE_CALL: return "HW_CORE_CALL";
    case OP_HW_MAIN_CALL: return "HW_MAIN_CALL";
    case OP_HW_WDBG_CALL: return "HW_WDBG_CALL";
    case OP_HW_FLASH_CALL: return "HW_FLASH_CALL";
    case OP_HW_PIN_CALL: return "HW_PIN_CALL";
    case OP_HW_DC_CALL: return "HW_DC_CALL";
    case OP_HW_DMC_CALL: return "HW_DMC_CALL";
    case OP_FADD: return "FADD"; case OP_FSUB: return "FSUB"; case OP_FMUL: return "FMUL";
    case OP_FDIV: return "FDIV"; case OP_F2I: return "F2I"; case OP_I2F: return "I2F";
    default: return "?";
    }
}

void kvm_disassemble(const KillsProgram* p, char* buf, int buflen) {
    if (!p || !buf || buflen <= 0) return;
    int off = 0;
    off += snprintf(buf + off, buflen - off, "; Kills program: %u ins, %u consts, %u funcs, data=%u\n",
                    p->code_count, p->const_count, p->func_count, p->data_size);
    for (uint32_t i = 0; i < p->code_count && off < buflen - 2; i++) {
        KillsIns* ins = &p->code[i];
        off += snprintf(buf + off, buflen - off, "  %4u: %-4s a=%d b=%d imm=%lld\n",
                        i, kvm_opname(ins->op), ins->a, ins->b, (long long)ins->imm);
    }
}

/* pub: 指令名 (debug 工具单行 trace 用) */
const char* kvm_op_name(uint8_t op) { return kvm_opname(op); }

/* ================= 序列化 ================= */
/* 布局: [KILLS(5)][ver u8][flags u8][nreg u32][data_size u32][code_count u32]
        [const_count u32][func_count u32]
        常量池: 每项 [type u8][iv i64] (str 另含 sv)  -- 简化: int/float 内联, str 存 sv
        数据段: data_size 字节
        指令表: 每项 [op u8][a i32][b i32][imm i64]
        函数表: 每项 [name C-string][pc u32][nparams u32] */
long kprog_serialize(const KillsProgram* p, uint8_t** out) {
    if (!p || !out) return -1;
    size_t cap = 512;
    uint8_t* buf = (uint8_t*)malloc(cap);
    if (!buf) return -1;
    size_t len = 0;

#define PUT(d, n) do { if (len + (n) > cap) { cap = cap*2 + (n); buf = (uint8_t*)realloc(buf, cap); } memcpy(buf+len, (d), (n)); len += (n); } while(0)

    memcpy(buf, KILLS_MAGIC, 5); len = 5;
    uint8_t ver = KILLS_VERSION, flags = 0;
    PUT(&ver, 1); PUT(&flags, 1);
    uint32_t nreg = KILLS_NREG;
    PUT(&nreg, 4); PUT(&p->data_size, 4); PUT(&p->code_count, 4);
    PUT(&p->const_count, 4); PUT(&p->func_count, 4);

    /* constants */
    for (uint32_t i = 0; i < p->const_count; i++) {
        KillsConst* c = &p->consts[i];
        PUT(&c->type, 1);
        PUT(&c->iv, 8);
        if (c->type == 1) { /* str */
            uint32_t sl = c->sv ? (uint32_t)strlen(c->sv) : 0;
            PUT(&sl, 4); if (sl) PUT(c->sv, sl);
        } else {
            PUT(&c->fv, 8);
        }
    }
    /* data */
    if (p->data_size) PUT(p->data, p->data_size);
    /* code */
    for (uint32_t i = 0; i < p->code_count; i++) {
        KillsIns* ins = &p->code[i];
        PUT(&ins->op, 1); PUT(&ins->a, 4); PUT(&ins->b, 4); PUT(&ins->imm, 8);
    }
    /* funcs */
    for (uint32_t i = 0; i < p->func_count; i++) {
        KillsFunc* f = &p->funcs[i];
        uint32_t nl = f->name ? (uint32_t)strlen(f->name) : 0;
        PUT(&nl, 4); if (nl) PUT(f->name, nl);
        PUT(&f->pc, 4); PUT(&f->nparams, 4);
    }
#undef PUT
    *out = buf;
    return (long)len;
}

KillsProgram* kprog_deserialize(const uint8_t* buf, long len, char* err, int errlen) {
    if (!buf || len < 21) { if (err) snprintf(err, errlen, "bad header"); return NULL; }
    if (memcmp(buf, KILLS_MAGIC, 5) != 0) { if (err) snprintf(err, errlen, "bad magic"); return NULL; }
    long o = 5;
    uint8_t ver = buf[o++];
    uint8_t flags = buf[o++];
    (void)flags;
    if (ver != KILLS_VERSION) { if (err) snprintf(err, errlen, "version mismatch %d", ver); return NULL; }
    uint32_t nreg, dsz, ccode, cconst, cfunc;
    memcpy(&nreg, buf + o, 4); o += 4;
    memcpy(&dsz, buf + o, 4); o += 4;
    memcpy(&ccode, buf + o, 4); o += 4;
    memcpy(&cconst, buf + o, 4); o += 4;
    memcpy(&cfunc, buf + o, 4); o += 4;
    (void)nreg;
    KillsProgram* p = kprog_new();
    if (!p) { if (err) snprintf(err, errlen, "alloc fail"); return NULL; }
    p->data_size = dsz;

    /* constants */
    for (uint32_t i = 0; i < cconst; i++) {
        uint8_t type = buf[o++];
        int64_t iv; memcpy(&iv, buf + o, 8); o += 8;
        double fv = 0;
        char* sv = NULL;
        if (type == 1) { uint32_t sl; memcpy(&sl, buf + o, 4); o += 4; sv = (char*)malloc(sl + 1); if (sl) memcpy(sv, buf + o, sl); sv[sl] = 0; o += sl; }
        else { memcpy(&fv, buf + o, 8); o += 8; }
        kprog_add_const(p, type, iv, fv, sv);
        free(sv); sv = NULL;
    }
    /* data */
    if (dsz) { p->data = (uint8_t*)malloc(dsz); memcpy(p->data, buf + o, dsz); o += dsz; p->data_size = dsz; }
    /* code */
    for (uint32_t i = 0; i < ccode; i++) {
        uint8_t op = buf[o++];
        int32_t a, b; int64_t imm;
        memcpy(&a, buf + o, 4); o += 4;
        memcpy(&b, buf + o, 4); o += 4;
        memcpy(&imm, buf + o, 8); o += 8;
        kprog_add_ins(p, op, a, b, imm);
    }
    /* funcs */
    for (uint32_t i = 0; i < cfunc; i++) {
        uint32_t nl; memcpy(&nl, buf + o, 4); o += 4;
        char name[256] = ""; if (nl < sizeof(name)) { memcpy(name, buf + o, nl); name[nl] = 0; }
        o += nl;
        uint32_t pc, nparams; memcpy(&pc, buf + o, 4); o += 4; memcpy(&nparams, buf + o, 4); o += 4;
        kprog_add_func(p, name, pc, nparams);
    }
    return p;
}
