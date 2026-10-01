/*
 * xdebugd —— xiaomo Debug Workbench 守护进程 (2026-09-28)
 *
 * 单二进制调试软件: 持久 VM 调试会话 + REST API + 内嵌 Web 调试工作台 + ESP32 真机串口桥。
 *
 *   make xdebugd && ./xdebugd          # 打开 http://127.0.0.1:9210
 *
 * 与 CLI 版 (xiaomo debug, 每次整跑) 的本质区别: 会话常驻 ——
 *   load 一次, 断点/单步(step)/单步跳过(over)/单步跳出(out)/继续(continue)/暂停 随便切,
 *   VM 现场 (pc/regs/栈/输出) 全程保留, 靠内核 dbg_resume 续跑标记驱动 kvm_run。
 *
 * REST API (均返回 JSON, 见 api_* 函数):
 *   GET  /api/state?out_since=N&trace_since=N   全量状态 (增量拉输出/trace/watch 日志)
 *   GET  /api/disasm?from=0&limit=200           反汇编窗口+常量池+函数表
 *   GET  /api/programs                          可加载程序列表
 *   POST /api/load    {"path":...}              加载 .mo/.kbc (.mo 现场编译)
 *   POST /api/run     {"bps":"10,21","watch":"r3"} 上电整跑 (从头)
 *   POST /api/step    {"n":1}                   单步 n 条 (跳过当前指令上的断点防锁死)
 *   POST /api/over                              单步跳过 (CALL 当整体)
 *   POST /api/out                               单步跳出当前函数
 *   POST /api/cont    {"budget":N}              继续到断点/HALT/预算/暂停
 *   POST /api/pause                             请求暂停 (引擎安全点生效)
 *   POST /api/restart                           程序复位 (VM 回上电态, 断点/观察保留)
 *   POST /api/bp      {"pc":N,"on":1}           断点开关
 *   POST /api/watch   {"reg":N,"on":1}           观察寄存器开关 (值变化进日志)
 *   POST /api/input   {"line":...}              交互输入 (kvm_io_push, FFI4/5 消费)
 *   GET  /api/ports                             真机串口列表
 *   POST /api/board/open   {"path":..,"baud":N} 打开真机串口 (开始后台读)
 *   POST /api/board/close                      关闭串口
 *   POST /api/board/reset  {"secs":12}          硬复位抓 boot (DTR=False, RTS 1→0.2s→0)
 *   POST /api/board/send    {"line":...}        发一行 (自动补 \n)
 *   GET  /api/state?...&board_since=N          增量拉真机日志 (真机日志随 state 一起下发)
 *
 * 真机复位序列照抄 esptool HardReset (USB-Serial-JTAG 板坑: DTR 拉高会进
 * 下载模式 → boot:0x0 DOWNLOAD 卡死假象, 必须 DTR 全程 False)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "vm_core.h"
#include "vm_stack.h"
#include "parser.h"
#include "ast.h"
#include "mo2kbc.h"

/* ================= 配置 ================= */
#define XDBG_PORT      9210
#define OUT_MAX        4000     /* 程序输出 ring 行数 */
#define TRACE_MAX      800      /* 指令 trace ring 条数 */
#define WLOG_MAX       400      /* watch 变化 ring 条数 */
#define BP_MAX         64
#define WATCH_MAX      32
#define BOARD_MAX      6000     /* 真机日志 ring 行数 */
#define READBUF_MAX    (64 * 1024)
#define LINE_MAX       2100     /* 单行最长 (vm 输出行 2048+安全) */
#define CONT_BUDGET    200000000ULL   /* continue 默认预算≈跑到断点/HALT */
#define TR_MAX         160

/* ================= 会话 ================= */
typedef struct {
    char    path[512];
    char    err[512];
    KillsProgram* prog;
    int     loaded;

    KillsVM vm;
    int     started;        /* 至少跑过 (现场有效) */

    pthread_mutex_t lock;   /* 引擎启停与状态读的短临界区 */
    pthread_t th;
    volatile int running;   /* 引擎线程在 kvm_run 中 */
    volatile int pause_req;
    int     mode;           /* 0=step 1=over 2=out 3=cont */
    uint64_t budget;
    uint64_t executed;      /* 本轮已执行条数 */
    uint32_t start_depth, start_pc;
    int     skip_bp_first;  /* 从断点续跑: 首条跳过 bp 检查防原地连停 */
    char    stop_reason[40];
    char    last_run_ms[24];

    uint32_t bps[BP_MAX]; int nbp;
    int     wreg[WATCH_MAX]; int nwatch;
    int64_t wbase[WATCH_MAX];

    int     vm_out_base;    /* VM output 数组搬运游标 (防重复 drain) */
} Session;

static Session* S = NULL;

/* ================= ring 缓冲 (文件级, 单写多读, 均在 S->lock 内) ================ */
static char out_buf[OUT_MAX][LINE_MAX];   static uint64_t out_seq = 0;
static char trace_buf[TRACE_MAX][TR_MAX]; static uint64_t trace_seq = 0;
static char wlog_buf[WLOG_MAX][128];       static uint64_t wlog_seq = 0;
static char board_buf[BOARD_MAX][420];    static uint64_t board_seq = 0;
static pthread_mutex_t board_lock = PTHREAD_MUTEX_INITIALIZER;

static void out_push(const char* line) {
    uint64_t i = out_seq % OUT_MAX;
    snprintf(out_buf[i], LINE_MAX, "%s", line);
    out_seq++;
}
static void trace_push(const char* line) {
    uint64_t i = trace_seq % TRACE_MAX;
    snprintf(trace_buf[i], TR_MAX, "%s", line);
    trace_seq++;
}
static void wlog_push(const char* line) {
    uint64_t i = wlog_seq % WLOG_MAX;
    snprintf(wlog_buf[i], 128, "%s", line);
    wlog_seq++;
}
static void board_push(const char* line) {
    pthread_mutex_lock(&board_lock);
    uint64_t i = board_seq % BOARD_MAX;
    snprintf(board_buf[i], 420, "%s", line);
    board_seq++;
    pthread_mutex_unlock(&board_lock);
}

/* ================= 工具 ================= */
static uint64_t now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}
static void json_escape(char* dst, int cap, const char* s) {
    int o = 0;
    for (const char* p = s ? s : ""; *p && o < cap - 8; p++) {
        unsigned char c = (unsigned char)*p;
        if      (c == '"')  { dst[o++] = '\\'; dst[o++] = '"'; }
        else if (c == '\\') { dst[o++] = '\\'; dst[o++] = '\\'; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c == '\r') { }
        else if (c < 0x20)  { o += snprintf(dst + o, 7, "\\u%04x", c); }   /* \u00XX = 6 字符 + NUL,
                                   原先写 5 会被截成 "\u00" 且游标仍跳 6 留空洞 → 整份 JSON 非法
                                   (真机日志含控制字节时 /api/state 直接解析失败, 面板全挂) */
        else                { dst[o++] = (char)c; }
    }
    dst[o] = 0;
}
/* 解析 JSON 字符串字段 (双引号内, 处理 \\ \" 简单转义); 找不到返回 0 */
static int json_get_str(const char* body, const char* key, char* out, int cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    p++; while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    int o = 0;
    while (*p && *p != '"' && o < cap - 1) {
        if (*p == '\\' && p[1]) { out[o++] = p[1]; p += 2; }
        else out[o++] = *p++;
    }
    out[o] = 0;
    return 1;
}
static int json_get_int(const char* body, const char* key, long long* out) {
    char tmp[64];
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    p++; while (*p == ' ' || *p == '\t') p++;
    int o = 0;
    while ((*p >= '0' && *p <= '9') || *p == '-' || (o == 0 && *p == '+')) tmp[o++] = *p++;
    tmp[o] = 0;
    if (!o) return 0;
    *out = strtoll(tmp, NULL, 10);
    return 1;
}

/* ================= 调试引擎 ================= */
static int is_ctrl_op(uint8_t op) {
    switch (op) {
    case OP_JMP: case OP_JZ: case OP_JNZ: case OP_JE: case OP_JNE:
    case OP_JG: case OP_JGE: case OP_JL: case OP_JLE:
    case OP_CALL: case OP_RET: case OP_HALT:
        return 1;
    default: return 0;
    }
}
static uint32_t vm_depth(const KillsVM* vm) {
    return (uint32_t)(vstack_used((VmStack*)&vm->callstack) / 8);
}

/* 引擎钩子: 每条指令执行前回调 (此路径必在引擎线程, 与 HTTP 线程以 lock 隔离) */
static int engine_hook(void* ud, struct KillsVM* vm, const KillsIns* ins,
                       uint32_t next_pc, uint64_t steps) {
    Session* s = (Session*)ud;
    (void)steps;

    /* 暂停: 任意安全点生效 (当前指令未执行 = "断下" 语义) */
    if (s->pause_req) { snprintf(s->stop_reason, sizeof(s->stop_reason), "paused"); return 1; }

    uint32_t pc = vm->pc;
    uint32_t depth = vm_depth(vm);
    uint64_t done = s->executed;

    /* watch: 值变化打点 (基线=撤 watch 重加时初始化) */
    for (int i = 0; i < s->nwatch; i++) {
        int r = s->wreg[i];
        int64_t v = vm->regs[r];
        if (v != s->wbase[i]) {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "r%d: %lld -> %lld @pc=%u",
                     r, (long long)s->wbase[i], (long long)v, pc);
            wlog_push(tmp);
            s->wbase[i] = v;
        }
    }

    /* trace ring */
    {
        char tr[TR_MAX];
        if (is_ctrl_op(ins->op))
            snprintf(tr, TR_MAX, "[%llu] %4u  %-14s a=%d b=%d imm=%lld -> %u",
                     (unsigned long long)done, pc, kvm_op_name(ins->op),
                     ins->a, ins->b, (long long)ins->imm, next_pc);
        else
            snprintf(tr, TR_MAX, "[%llu] %4u  %-14s a=%d b=%d imm=%lld",
                     (unsigned long long)done, pc, kvm_op_name(ins->op),
                     ins->a, ins->b, (long long)ins->imm);
        trace_push(tr);
    }

    /* step 模式: 预算满即停 (断点不参与 step —— 单步永远前进, 防断点原点锁死) */
    if (s->mode == 0) {
        if (done >= s->budget) { snprintf(s->stop_reason, sizeof(s->stop_reason), "step"); return 1; }
        s->executed++;
        return 0;
    }
    /* over: 执行≥1条后回到本层非起始 pc (CALL 当整体越过) */
    if (s->mode == 1) {
        if (done >= 1 && depth <= s->start_depth && pc != s->start_pc) {
            snprintf(s->stop_reason, sizeof(s->stop_reason), "over"); return 1;
        }
    }
    /* out: 回到更浅层 (函数返回) */
    if (s->mode == 2) {
        if (done >= 1 && depth < s->start_depth) {
            snprintf(s->stop_reason, sizeof(s->stop_reason), "out"); return 1;
        }
    }
    /* 断点: over/out/cont 模式均尊重 (skip_bp_first 防续跑原地连停) */
    if (s->skip_bp_first) s->skip_bp_first = 0;
    else {
        for (int i = 0; i < s->nbp; i++) {
            if (s->bps[i] == pc) {
                snprintf(s->stop_reason, sizeof(s->stop_reason), "breakpoint"); return 1;
            }
        }
    }
    /* cont 预算 */
    if (s->mode == 3 && done >= s->budget) {
        snprintf(s->stop_reason, sizeof(s->stop_reason), "budget"); return 1;
    }
    s->executed++;
    return 0;
}

/* 搬运 VM 输出到会话 ring (复制不 steal, VM 自有内存) */
static void drain_outputs(Session* s) {
    int cnt = kvm_output_count(&s->vm);
    for (int i = s->vm_out_base; i < cnt; i++) {
        const char* ln = kvm_output(&s->vm, i);
        out_push(ln ? ln : "");
    }
    s->vm_out_base = cnt;
}

/* flush 重定向: FFI5 等待时内核清空 output → 先搬进会话 ring 再放行 */
static int session_flush_redirect(struct KillsVM* vm) {
    (void)vm;
    if (!S) return 0;
    drain_outputs(S);
    S->vm_out_base = 0;   /* 数组即将被内核清空, 游标归零 */
    return 1;
}

/* 引擎线程: 一轮控制完成后退出 (现场保留在 S->vm) */
static void* engine_thread(void* arg) {
    Session* s = (Session*)arg;
    uint64_t t0 = now_ms();
    int rc = kvm_run(&s->vm, s->prog);
    uint64_t t1 = now_ms();

    pthread_mutex_lock(&s->lock);
    if (rc != 0)                 snprintf(s->stop_reason, sizeof(s->stop_reason), "error");
    else if (s->vm.halted)       snprintf(s->stop_reason, sizeof(s->stop_reason), "halt");
    drain_outputs(s);
    if (s->vm.halted) s->vm_out_base = 0;   /* HALT 后 VM 不会再吐, 防复跑重复搬运 */
    snprintf(s->last_run_ms, sizeof(s->last_run_ms), "%llu", (unsigned long long)(t1 - t0));
    s->pause_req = 0;
    s->running = 0;
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

/* 启动一轮 (mode: 0=step 1=over 2=out 3=cont) */
static int engine_start(Session* s, int mode, uint64_t budget) {
    if (!s->loaded) return -1;
    if (s->running) return -2;
    if (!s->started) return -5;                    /* 尚未上电: 请用 run/restart */
    if (s->vm.halted) return -3;                   /* 已跑完: 需 restart */
    s->mode = mode;
    s->budget = budget;
    s->executed = 0;
    s->start_depth = vm_depth(&s->vm);
    s->start_pc = s->vm.pc;
    if (strcmp(s->stop_reason, "breakpoint") == 0 || strcmp(s->stop_reason, "paused") == 0)
        s->skip_bp_first = 1;   /* 从断点/暂停续跑: 首条跳过 bp 检查 */
    else s->skip_bp_first = 0;
    s->pause_req = 0;
    s->vm.dbg_cancel = 0;
    for (int i = 0; i < s->nwatch; i++) s->wbase[i] = s->vm.regs[s->wreg[i]];
    s->vm.dbg_hook = engine_hook;
    s->vm.dbg_ud = s;
    s->vm.dbg_resume = 1;         /* 关键: 续跑标记, kvm_run 跳过上电复位 */
    s->running = 1;
    strcpy(s->stop_reason, "");
    if (pthread_create(&s->th, NULL, engine_thread, s) != 0) { s->running = 0; return -4; }
    pthread_detach(s->th);
    return 0;
}

/* 强制停引擎线程: 暂停钩子(指令边界) + FFI5 取消(等待中) 双管齐下。
 * 交互程序卡在 input_wait 时, 仅靠 pause 钩子永远等不到下一条指令。 */
static int force_stop_engine(Session* s) {
    if (!s->running) return 0;
    s->pause_req = 1;
    if (s->started) s->vm.dbg_cancel = 1;   /* FFI5 等待循环 20ms 内退出 */
    for (int i = 0; i < 100 && s->running; i++) usleep(20000);
    return s->running ? -1 : 0;
}

/* ================= 程序加载 (双格式) ================= */
static char* read_file_all(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    return buf;
}

static int load_program(Session* s, const char* path) {
    if (force_stop_engine(s) != 0) return -10;   /* 引擎无法停: 拒绝换程序 */
    size_t n = strlen(path);
    int is_kbc = (n >= 4 && strcmp(path + n - 4, ".kbc") == 0);
    char err[512] = "";
    KillsProgram* prog = NULL;

    if (is_kbc) {
        FILE* f = fopen(path, "rb");
        if (!f) { snprintf(s->err, sizeof(s->err), "无法读取文件: %s", path); return -1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t* buf = (uint8_t*)malloc((size_t)sz + 1);
        if (!buf) { fclose(f); return -2; }
        size_t rd = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        prog = kprog_deserialize(buf, (long)rd, err, (int)sizeof(err));
        free(buf);
        if (!prog) { snprintf(s->err, sizeof(s->err), "kbc 反序列化失败: %s", err); return -3; }
    } else {
        char* src = read_file_all(path);
        if (!src) { snprintf(s->err, sizeof(s->err), "无法读取文件: %s", path); return -1; }
        AstNode* program = xiaomo_parse_source(src, err, (size_t)sizeof(err));
        free(src);
        if (!program) { snprintf(s->err, sizeof(s->err), "解析失败: %s", err); return -3; }
        prog = mo2kbc_compile(program, err, (size_t)sizeof(err));
        ast_free(program);
        if (!prog) { snprintf(s->err, sizeof(s->err), "编译失败: %s", err); return -3; }
    }

    /* 替换旧程序 (kvm_free 对零值结构安全) */
    if (s->prog) kprog_free(s->prog);
    kvm_free(&s->vm);
    memset(&s->vm, 0, sizeof(s->vm));
    kvm_init(&s->vm);
    s->prog = prog;
    s->loaded = 1;
    s->started = 0;
    s->vm_out_base = 0;
    snprintf(s->path, sizeof(s->path), "%s", path);
    s->err[0] = 0;
    out_seq = 0; trace_seq = 0; wlog_seq = 0;   /* 新程序新 ring */
    return 0;
}

/* 上电整跑 (run/restart 共用): 复位 VM 现场从头执行, 断点/watch 配置保留 */
static int engine_fresh_run(Session* s, uint64_t budget) {
    if (!s->loaded) return -1;
    if (force_stop_engine(s) != 0) return -2;
    kvm_free(&s->vm);           /* load_program 已 kvm_init, 必为有效实例 */
    memset(&s->vm, 0, sizeof(s->vm));
    kvm_init(&s->vm);
    s->started = 1;
    s->vm_out_base = 0;
    out_seq = 0; trace_seq = 0; wlog_seq = 0;
    s->mode = 3;
    s->budget = budget;
    s->executed = 0;
    s->start_depth = 0;
    s->start_pc = 0;
    s->skip_bp_first = 0;
    s->pause_req = 0;
    for (int i = 0; i < s->nwatch; i++) s->wbase[i] = 0;
    s->vm.dbg_hook = engine_hook;
    s->vm.dbg_ud = s;
    s->vm.dbg_resume = 0;         /* 全新上电: 走 kvm_run 原始复位路径 */
    s->running = 1;
    strcpy(s->stop_reason, "");
    if (pthread_create(&s->th, NULL, engine_thread, s) != 0) { s->running = 0; return -4; }
    pthread_detach(s->th);
    return 0;
}

/* ================= 真机串口桥 ================= */
typedef struct {
    int     fd;
    char    path[256];
    int     open;
    pthread_t rd;
    volatile int rd_run;
} Board;

static Board B = { -1, "", 0, 0, 0 };

static int serial_open_raw(const char* path, int baud) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios tio;
    if (tcgetattr(fd, &tio) != 0) { close(fd); return -2; }
    speed_t sp = B115200;
    if      (baud == 9600)   sp = B9600;
    else if (baud == 57600)  sp = B57600;
    else if (baud == 115200) sp = B115200;
    else if (baud == 230400) sp = B230400;   /* macOS termios 上限; 更高速须 IOSSIOSPEED, 暂不展开 */
    cfmakeraw(&tio);
    cfsetispeed(&tio, sp);
    cfsetospeed(&tio, sp);
    tio.c_cc[VMIN] = 0; tio.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) { close(fd); return -3; }
    return fd;
}

/* 线控: set=1 拉高(set bit), 0 拉低 (compound literal 取址在 C++ 不合法, 用局部变量) */
static void line_ctl(int fd, int sig, int set) {
    int v = sig;
    ioctl(fd, set ? TIOCMBIS : TIOCMBIC, &v);
}

/* 关串口 (含读线程停等) */
static void board_close(void) {
    if (!B.open) return;
    B.rd_run = 0;
    void* ret = NULL;
    pthread_join(B.rd, &ret);
    close(B.fd);
    B.fd = -1; B.open = 0; B.path[0] = 0;
}

/* 后台读线程: 行切分进 ring (\n 分割, \r 剥离) */
static void* board_reader(void* arg) {
    Board* b = (Board*)arg;
    char acc[8192];
    int acc_len = 0;
    while (b->rd_run) {
        char chunk[1024];
        ssize_t n = read(b->fd, chunk, sizeof(chunk));
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                char c = chunk[i];
                if (c == '\n') {
                    acc[acc_len] = 0;
                    board_push(acc);
                    acc_len = 0;
                } else if (c != '\r') {
                    if (acc_len < (int)sizeof(acc) - 1) acc[acc_len++] = c;
                }
            }
        } else {
            usleep(20000);
        }
    }
    return NULL;
}

static int board_open_port(const char* path, int baud) {
    board_close();
    int fd = serial_open_raw(path, baud);
    if (fd < 0) return (int)fd;
    B.fd = fd;
    snprintf(B.path, sizeof(B.path), "%s", path);
    B.open = 1;
    B.rd_run = 1;
    if (pthread_create(&B.rd, NULL, board_reader, &B) != 0) {
        close(fd); B.fd = -1; B.open = 0; return -10;
    }
    return 0;
}

/* esptool HardReset 序列 (USB-Serial-JTAG 板坑: DTR 必须全程 False) */
static void board_hard_reset(void) {
    int fd = B.fd;
    if (fd < 0 || !B.open) return;
    line_ctl(fd, TIOCM_DTR, 0);   /* DTR 全程 False */
    line_ctl(fd, TIOCM_RTS, 1);   /* RTS 1 → 拉复位 */
    tcflush(fd, TCIFLUSH);
    usleep(200000);
    line_ctl(fd, TIOCM_RTS, 0);   /* RTS 0 → 松开, 冷启动; 后台读线程抓 boot */
}

/* 枚举 /dev/cu.* (排除蓝牙; macOS PTY 是 ttys*, cu.* 天然安全) */
static int list_ports(char* dst, int cap) {
    int n = 0;
    DIR* d = opendir("/dev");
    if (!d) { n += snprintf(dst + n, (size_t)(cap - n), "[]"); return n; }
    n += snprintf(dst + n, (size_t)(cap - n), "[");   /* 数组开括号: 端口为 0 时收尾即 "[]" */
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "cu.", 3) != 0) continue;
        if (strstr(e->d_name, "Bluetooth") || strstr(e->d_name, "ttys")) continue;
        const char* tag = "";
        if (strstr(e->d_name, "usbmodem") || strstr(e->d_name, "modem")) tag = "ESP32/USB";
        else if (strstr(e->d_name, "usbserial") || strstr(e->d_name, "SLAB")) tag = "CP210x";
        else if (strstr(e->d_name, "wch") || strstr(e->d_name, "ch34")) tag = "WCH/CH34x";
        int is_usb = tag[0] != 0;
        if (!is_usb) continue;                 /* 只列 USB 芯片系 (蓝牙/其他杂项过滤) */
        char path[256], pesc[300], tesc[64];
        snprintf(path, sizeof(path), "/dev/%s", e->d_name);
        json_escape(pesc, (int)sizeof(pesc), path);
        json_escape(tesc, (int)sizeof(tesc), tag);
        n += snprintf(dst + n, (size_t)(cap - n),
                      "%s{\"path\":\"%s\",\"desc\":\"%s\"}", n > 1 ? "," : "", pesc, tesc);
        if (n >= cap - 128) break;
    }
    closedir(d);
    n += snprintf(dst + n, (size_t)(cap - n), "]");   /* 数组闭括号 */
    return n;
}

/* ================= HTTP 服务器 ================= */
typedef struct {
    char method[8];
    char path[1024];
    char query[1024];
    char body[READBUF_MAX];
} HttpReq;

static void http_send(int fd, const char* status, const char* ctype, const char* body, long blen) {
    char hdr[256];
    int h = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, ctype, blen);
    (void)write(fd, hdr, (size_t)h);
    long off = 0;
    while (off < blen) {
        ssize_t w = write(fd, body + off, (size_t)(blen - off));
        if (w <= 0) break;
        off += w;
    }
}
static void http_json(int fd, const char* json) {
    http_send(fd, "200 OK", "application/json; charset=utf-8", json, (long)strlen(json));
}

/* 查询参数取整数 (?key=N) */
static long long query_int(const char* q, const char* key, long long dflt) {
    char pat[48];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char* p = strstr(q, pat);
    if (!p) return dflt;
    return strtoll(p + strlen(pat), NULL, 10);
}

static int ring_json(char* dst, int cap, const char* key,
                     const char* ring, int stride, uint64_t rcap,
                     uint64_t seq, uint64_t since, uint64_t* out_from) {
    /* since 超前(换程序后 seq 归零) → 从头; ring 溢出 → 从最老存活行起 */
    uint64_t from = (since > seq) ? 0 : since;
    if (seq > rcap && from < seq - rcap) from = seq - rcap;
    int n = 0;
    n += snprintf(dst + n, (size_t)(cap - n), "\"%s\":[", key);
    uint64_t cnt = 0;
    for (uint64_t i = from; i < seq; i++) {
        char esc[2 * LINE_MAX + 32];
        json_escape(esc, (int)sizeof(esc), ring + (i % rcap) * (uint64_t)stride);
        n += snprintf(dst + n, (size_t)(cap - n), "%s\"%s\"", cnt ? "," : "", esc);
        cnt++;
        /* 截断保护: out_from 推进到已推条数之后, 前端据此续拉不死循环 */
        if (n >= cap - 300) break;
        if (cnt >= 200) break;
    }
    *out_from = from + cnt;   /* 续拉游标 = 本次已交付之后 */
    n += snprintf(dst + n, (size_t)(cap - n), "]");
    return n;
}
/* 真机日志行宽 420, 单独版 */
static int board_json(char* dst, int cap, uint64_t since, uint64_t* out_from) {
    uint64_t seq = board_seq;
    uint64_t from = (since > seq) ? 0 : since;
    if (seq > BOARD_MAX && from < seq - BOARD_MAX) from = seq - BOARD_MAX;
    int n = 0;
    n += snprintf(dst + n, (size_t)(cap - n), "\"board\":[");
    uint64_t cnt = 0;
    for (uint64_t i = from; i < seq; i++) {
        char esc[880];
        pthread_mutex_lock(&board_lock);
        json_escape(esc, (int)sizeof(esc), board_buf[i % BOARD_MAX]);
        pthread_mutex_unlock(&board_lock);
        n += snprintf(dst + n, (size_t)(cap - n), "%s\"%s\"", cnt ? "," : "", esc);
        cnt++;
        if (n >= cap - 950) break;
        if (cnt >= 120) break;
    }
    *out_from = from + cnt;
    n += snprintf(dst + n, (size_t)(cap - n), "]");
    return n;
}

/* ---- GET /api/state ---- */
static void api_state(int fd, const char* q) {
    char* buf = (char*)malloc(READBUF_MAX);
    if (!buf) return;
    int n = 0;
    uint64_t os_ = (uint64_t)query_int(q, "out_since", 0);
    uint64_t ts_ = (uint64_t)query_int(q, "trace_since", 0);
    uint64_t ws_ = (uint64_t)query_int(q, "wlog_since", 0);
    uint64_t bs_ = (uint64_t)query_int(q, "board_since", 0);

    Session* s = S;
    char pesc[600], resc[1100], sresc[80], eesc[1100];
    json_escape(pesc, (int)sizeof(pesc), s->path);
    json_escape(resc, (int)sizeof(resc), s->err);
    json_escape(sresc, (int)sizeof(sresc), s->stop_reason);
    json_escape(eesc, (int)sizeof(eesc), s->vm.error_msg);

    n += snprintf(buf + n, (size_t)(READBUF_MAX - n),
        "{\"loaded\":%d,\"path\":\"%s\",\"err\":\"%s\","
        "\"code_count\":%u,\"const_count\":%u,\"func_count\":%u,"
        "\"running\":%d,\"started\":%d,\"halted\":%d,\"pc\":%u,\"steps\":%llu,"
        "\"stop_reason\":\"%s\",\"last_run_ms\":\"%s\","
        "\"error_count\":%d,\"error_msg\":\"%s\",\"sig_violations\":%u,"
        "\"op_depth\":%d,\"call_depth\":%d,",
        s->loaded ? 1 : 0, pesc, resc,
        s->prog ? s->prog->code_count : 0,
        s->prog ? s->prog->const_count : 0,
        s->prog ? s->prog->func_count : 0,
        s->running ? 1 : 0, s->started ? 1 : 0, s->vm.halted ? 1 : 0,
        s->vm.pc, (unsigned long long)s->vm.steps,
        sresc, s->last_run_ms,
        s->vm.error_count, eesc, s->vm.sig_violations,
        (int)(s->started ? vstack_used(&s->vm.operand) / 8 : 0),
        (int)(s->started ? vstack_used(&s->vm.callstack) / 8 : 0));

    /* 寄存器: 128 个全量 */
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "\"regs\":[");
    for (int i = 0; i < (int)KILLS_NREG; i++)
        n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "%s%lld",
                      i ? "," : "", (long long)s->vm.regs[i]);
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "],");

    /* 断点/观察 */
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "\"bps\":[");
    for (int i = 0; i < s->nbp; i++)
        n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "%s%u", i ? "," : "", s->bps[i]);
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "],\"watches\":[");
    for (int i = 0; i < s->nwatch; i++)
        n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "%s%d", i ? "," : "", s->wreg[i]);
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "],");

    n += snprintf(buf + n, (size_t)(READBUF_MAX - n),
                  "\"out_seq\":%llu,\"trace_seq\":%llu,\"wlog_seq\":%llu,",
                  (unsigned long long)out_seq, (unsigned long long)trace_seq,
                  (unsigned long long)wlog_seq);

    uint64_t f1, f2, f3;
    n += ring_json(buf + n, (int)(READBUF_MAX - n), "out", (const char*)out_buf, LINE_MAX, OUT_MAX, out_seq, os_, &f1);
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), ",\"out_from\":%llu,", (unsigned long long)f1);
    n += ring_json(buf + n, (int)(READBUF_MAX - n), "trace", (const char*)trace_buf, TR_MAX, TRACE_MAX, trace_seq, ts_, &f2);
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), ",\"trace_from\":%llu,", (unsigned long long)f2);
    n += ring_json(buf + n, (int)(READBUF_MAX - n), "wlog", (const char*)wlog_buf, 128, WLOG_MAX, wlog_seq, ws_, &f3);
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), ",\"wlog_from\":%llu,", (unsigned long long)f3);

    n += snprintf(buf + n, (size_t)(READBUF_MAX - n),
                  "\"board_open\":%d,\"board_path\":\"%s\",\"board_seq\":%llu,",
                  B.open ? 1 : 0, B.path, (unsigned long long)board_seq);
    uint64_t f4;
    n += board_json(buf + n, (int)(READBUF_MAX - n), bs_, &f4);
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), ",\"board_from\":%llu}", (unsigned long long)f4);

    http_json(fd, buf);
    free(buf);
}

/* ---- GET /api/disasm ---- */
static void api_disasm(int fd, const char* q) {
    Session* s = S;
    char* buf = (char*)malloc(READBUF_MAX);
    if (!buf) return;
    if (!s->loaded) { snprintf(buf, 64, "{\"err\":\"no program\"}"); http_json(fd, buf); free(buf); return; }
    long long from  = query_int(q, "from", 0);
    long long limit = query_int(q, "limit", 200);
    if (from < 0) from = 0;
    if (limit <= 0 || limit > 600) limit = 200;

    int n = 0;
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n),
                  "{\"code_count\":%u,\"from\":%lld,\"limit\":%lld,\"ins\":[",
                  s->prog->code_count, from, limit);
    for (long long i = from; i < (long long)s->prog->code_count && i < from + limit; i++) {
        KillsIns* ins = &s->prog->code[i];
        char line[TR_MAX];
        const char* bk = "";
        for (int k = 0; k < s->nbp; k++) if (s->bps[k] == (uint32_t)i) { bk = "◆"; break; }
        const char* cur = (!s->running && s->started && s->vm.pc == (uint32_t)i) ? "▶" : " ";
        if (is_ctrl_op(ins->op))
            snprintf(line, sizeof(line), "%s%s%4lld: %-14s a=%d b=%d imm=%lld",
                     cur, bk, i, kvm_op_name(ins->op),
                     ins->a, ins->b, (long long)ins->imm);
        else
            snprintf(line, sizeof(line), "%s%s%4lld: %-14s a=%d b=%d imm=%lld",
                     cur, bk, i, kvm_op_name(ins->op),
                     ins->a, ins->b, (long long)ins->imm);
        char esc[2 * TR_MAX + 16];
        json_escape(esc, (int)sizeof(esc), line);
        n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "%s\"%s\"", (i > from) ? "," : "", esc);
        if (n >= READBUF_MAX - 300) break;
    }
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "],\"consts\":[");
    for (uint32_t i = 0; i < s->prog->const_count && i < 64; i++) {
        KillsConst* c = &s->prog->consts[i];
        char item[340], esc[360];
        if (c->type == 1)      snprintf(item, sizeof(item), "[%u] str \"%s\"", i, c->sv ? c->sv : "");
        else if (c->type == 2) snprintf(item, sizeof(item), "[%u] float %f", i, c->fv);
        else                   snprintf(item, sizeof(item), "[%u] int %lld", i, (long long)c->iv);
        json_escape(esc, (int)sizeof(esc), item);
        n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "%s\"%s\"", i ? "," : "", esc);
    }
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "],\"funcs\":[");
    for (uint32_t i = 0; i < s->prog->func_count && i < 64; i++) {
        char item[340], esc[360];
        snprintf(item, sizeof(item), "[%u] %s pc=%u nparams=%u",
                 i, s->prog->funcs[i].name, s->prog->funcs[i].pc, s->prog->funcs[i].nparams);
        json_escape(esc, (int)sizeof(esc), item);
        n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "%s\"%s\"", i ? "," : "", esc);
    }
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "]}");
    http_json(fd, buf);
    free(buf);
}

/* ---- GET /api/programs ---- */
static void api_programs(int fd) {
    char* buf = (char*)malloc(READBUF_MAX);
    if (!buf) return;
    int n = 0;
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "{\"programs\":[");
    const char* dirs[2] = { "examples", "." };
    for (int d = 0; d < 2; d++) {
        DIR* dp = opendir(dirs[d]);
        if (!dp) continue;
        struct dirent* e;
        while ((e = readdir(dp)) != NULL) {
            size_t l = strlen(e->d_name);
            int is_mo = (l >= 3 && strcmp(e->d_name + l - 3, ".mo") == 0);
            int is_kbc = (l >= 4 && strcmp(e->d_name + l - 4, ".kbc") == 0);
            if (!is_mo && !is_kbc) continue;
            if (!strcmp(e->d_name, "xiaomo.mo")) continue;
            char rel[512], esc[1100];
            snprintf(rel, sizeof(rel), "%s/%s", dirs[d], e->d_name);
            if (!strcmp(dirs[d], ".")) snprintf(rel, sizeof(rel), "%s", e->d_name);
            json_escape(esc, (int)sizeof(esc), rel);
            n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "%s\"%s\"", (n > 14) ? "," : "", esc);
            if (n >= READBUF_MAX - 300) break;
        }
        closedir(dp);
        if (n >= READBUF_MAX - 300) break;
    }
    n += snprintf(buf + n, (size_t)(READBUF_MAX - n), "]}");
    http_json(fd, buf);
    free(buf);
}

/* ---- POST 控制器 ---- */
static void api_ok(int fd, int rc, const char* extra) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"rc\":%d%s}", rc, extra ? extra : "");
    http_json(fd, buf);
}

/* bps/watch 逗号列表解析 (兼容 r 前缀) */
static int parse_list(const char* s, int64_t* out, int maxn) {
    int n = 0;
    while (s && *s && n < maxn) {
        if (*s == 'r' || *s == 'R' || *s == ' ') s++;
        char* end = NULL;
        long long v = strtoll(s, &end, 0);
        if (end == s) break;
        out[n++] = v;
        s = (*end == ',') ? end + 1 : end;
    }
    return n;
}

static void post_handler(int fd, const char* path, const char* body) {
    Session* s = S;
    char arg[2048] = "";
    long long num = 0;

    if (strcmp(path, "/api/load") == 0) {
        if (!json_get_str(body, "path", arg, (int)sizeof(arg))) {
            api_ok(fd, -1, ",\"msg\":\"missing path\""); return;
        }
        int rc = load_program(s, arg);
        char esc[1100];
        json_escape(esc, (int)sizeof(esc), s->err);
        char extra[1200];
        snprintf(extra, sizeof(extra), ",\"msg\":\"%s\"", esc);
        api_ok(fd, rc, extra);
        return;
    }
    if (strcmp(path, "/api/run") == 0 || strcmp(path, "/api/restart") == 0) {
        if (strcmp(path, "/api/restart") == 0 && !s->started) { api_ok(fd, -5, ",\"msg\":\"尚未运行\""); return; }
        /* run 可带 bps/watch 一次性配置 */
        if (json_get_str(body, "bps", arg, (int)sizeof(arg))) {
            int64_t tmp[BP_MAX];
            int n = parse_list(arg, tmp, BP_MAX);
            s->nbp = 0;
            for (int i = 0; i < n; i++) s->bps[s->nbp++] = (uint32_t)tmp[i];
        }
        if (json_get_str(body, "watch", arg, (int)sizeof(arg))) {
            int64_t tmp[WATCH_MAX];
            int n = parse_list(arg, tmp, WATCH_MAX);
            s->nwatch = 0;
            for (int i = 0; i < n && i < (int)KILLS_NREG; i++) s->wreg[s->nwatch++] = (int)tmp[i];
        }
        int rc = engine_fresh_run(s, CONT_BUDGET);
        api_ok(fd, rc, NULL);
        return;
    }
    if (strcmp(path, "/api/cont") == 0) {
        uint64_t budget = CONT_BUDGET;
        if (json_get_int(body, "budget", &num) && num > 0) budget = (uint64_t)num;
        int rc = engine_start(s, 3, budget);
        api_ok(fd, rc, NULL);
        return;
    }
    if (strcmp(path, "/api/step") == 0) {
        uint64_t n = 1;
        if (json_get_int(body, "n", &num) && num > 0 && num < 1000000) n = (uint64_t)num;
        int rc = engine_start(s, 0, n);
        api_ok(fd, rc, NULL);
        return;
    }
    if (strcmp(path, "/api/over") == 0) { api_ok(fd, engine_start(s, 1, CONT_BUDGET), NULL); return; }
    if (strcmp(path, "/api/out")  == 0) { api_ok(fd, engine_start(s, 2, CONT_BUDGET), NULL); return; }
    if (strcmp(path, "/api/pause") == 0) { s->pause_req = 1; api_ok(fd, 0, NULL); return; }

    if (strcmp(path, "/api/bp") == 0) {
        if (!json_get_int(body, "pc", &num) || num < 0) { api_ok(fd, -1, NULL); return; }
        uint32_t pc = (uint32_t)num;
        long long on = 1;
        json_get_int(body, "on", &on);
        int found = -1;
        for (int i = 0; i < s->nbp; i++) if (s->bps[i] == pc) found = i;
        if (on) { if (found < 0 && s->nbp < BP_MAX) s->bps[s->nbp++] = pc; }
        else    { if (found >= 0) { s->bps[found] = s->bps[s->nbp - 1]; s->nbp--; } }
        api_ok(fd, 0, NULL);
        return;
    }
    if (strcmp(path, "/api/watch") == 0) {
        if (!json_get_int(body, "reg", &num) || num < 0 || num >= (int)KILLS_NREG) { api_ok(fd, -1, NULL); return; }
        int r = (int)num;
        long long on = 1;
        json_get_int(body, "on", &on);
        int found = -1;
        for (int i = 0; i < s->nwatch; i++) if (s->wreg[i] == r) found = i;
        if (on) {
            if (found < 0 && s->nwatch < WATCH_MAX) s->wreg[s->nwatch++] = r;
            s->wbase[s->nwatch - 1] = s->started ? s->vm.regs[r] : 0;
        } else {
            if (found >= 0) { s->wreg[found] = s->wreg[s->nwatch - 1]; s->nwatch--; }
        }
        api_ok(fd, 0, NULL);
        return;
    }
    if (strcmp(path, "/api/input") == 0) {
        if (!json_get_str(body, "line", arg, (int)sizeof(arg))) { api_ok(fd, -1, NULL); return; }
        kvm_io_push(arg);
        api_ok(fd, 0, NULL);
        return;
    }
    if (strcmp(path, "/api/board/open") == 0) {
        int baud = 115200;
        if (!json_get_str(body, "path", arg, (int)sizeof(arg))) { api_ok(fd, -1, NULL); return; }
        if (json_get_int(body, "baud", &num) && num > 0) baud = (int)num;
        int rc = board_open_port(arg, baud);
        api_ok(fd, rc, NULL);
        return;
    }
    if (strcmp(path, "/api/board/close") == 0) { board_close(); api_ok(fd, 0, NULL); return; }
    if (strcmp(path, "/api/board/reset") == 0) {
        if (!B.open) { api_ok(fd, -1, ",\"msg\":\"串口未打开\""); return; }
        board_hard_reset();
        api_ok(fd, 0, NULL);
        return;
    }
    if (strcmp(path, "/api/board/send") == 0) {
        if (!B.open) { api_ok(fd, -1, ",\"msg\":\"串口未打开\""); return; }
        if (!json_get_str(body, "line", arg, (int)sizeof(arg))) { api_ok(fd, -1, NULL); return; }
        size_t len = strlen(arg);
        ssize_t w;
        if (len == 0) {
            w = write(B.fd, "\n", 1);          /* 空行 = 只发一个换行 (文档: 自动补 \n) */
        } else {
            w = write(B.fd, arg, len);
            (void)write(B.fd, "\n", 1);
        }
        /* rc 必须遵守 api_ok 契约(0=成功): 早期直接回 write() 字节数,
           前端 act() 判定 rc!=0 即报错 → 每次发送都弹假错误提示 */
        if (w < 0) { api_ok(fd, -1, ",\"msg\":\"串口写失败\""); return; }
        api_ok(fd, 0, NULL);
        return;
    }
    api_ok(fd, -99, ",\"msg\":\"unknown POST\"");
}

/* ---- 静态文件 (xdebug/web) ---- */
static char web_root[512] = "xdebug/web";

static void serve_static(int fd, const char* path) {
    const char* rel = (strcmp(path, "/") == 0) ? "/index.html" : path;
    if (strstr(rel, "..")) { http_send(fd, "403 Forbidden", "text/plain", "no", 2); return; }
    char full[1024];
    snprintf(full, sizeof(full), "%s%s", web_root, rel);
    FILE* f = fopen(full, "rb");
    if (!f) {
        snprintf(full, sizeof(full), "%s/index.html", web_root);
        f = fopen(full, "rb");
        if (!f) { http_send(fd, "404 Not Found", "text/plain", "?", 1); return; }
        rel = "/index.html";
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* body = (char*)malloc((size_t)sz + 1);
    size_t rd = fread(body, 1, (size_t)sz, f);
    fclose(f);
    const char* ct = "text/html; charset=utf-8";
    size_t l = strlen(rel);
    if (l >= 3 && strcmp(rel + l - 3, ".js") == 0)  ct = "application/javascript; charset=utf-8";
    if (l >= 4 && strcmp(rel + l - 4, ".css") == 0) ct = "text/css; charset=utf-8";
    http_send(fd, "200 OK", ct, body, (long)rd);
    free(body);
}

/* ---- 连接处理 ---- */
static void* conn_thread(void* arg) {
    int fd = *(int*)arg;
    free(arg);
    char* req = (char*)malloc(READBUF_MAX + 4096);
    if (!req) { close(fd); return NULL; }
    ssize_t total = 0;
    char* hdr_end = NULL;
    long clen = 0;
    while (total < READBUF_MAX + 4095) {
        ssize_t n = read(fd, req + total, (size_t)(READBUF_MAX + 4096 - 1 - total));
        if (n <= 0) break;
        total += n;
        req[total] = 0;
        if (!hdr_end) {
            hdr_end = strstr(req, "\r\n\r\n");
            if (hdr_end && !clen) {
                const char* cl = strstr(req, "Content-Length:");
                if (cl) clen = strtoll(cl + 15, NULL, 10);
            }
        }
        if (hdr_end && total >= (hdr_end - req) + 4 + clen) break;   /* body 收齐 */
    }
    if (total <= 0) { free(req); close(fd); return NULL; }
    req[total] = 0;

    char method[8] = "", url[1200] = "";
    sscanf(req, "%7s %1100s", method, url);
    char* qmark = strchr(url, '?');
    char query[1024] = "";
    if (qmark) { snprintf(query, sizeof(query), "%s", qmark + 1); *qmark = 0; }

    char* body = hdr_end ? hdr_end + 4 : (char*)"";

    if (strcmp(method, "GET") == 0) {
        if      (strcmp(url, "/api/state") == 0)    api_state(fd, query);
        else if (strcmp(url, "/api/disasm") == 0)   api_disasm(fd, query);
        else if (strcmp(url, "/api/programs") == 0) api_programs(fd);
        else if (strcmp(url, "/api/ports") == 0) {
            char* buf = (char*)malloc(4096);
            int n = 0;
            n += snprintf(buf + n, 4096 - (size_t)n, "{\"ports\":");
            n += list_ports(buf + n, 4096 - (size_t)n);
            snprintf(buf + n, 4096 - (size_t)n, "}");
            http_json(fd, buf);
            free(buf);
        }
        else serve_static(fd, url);
    } else if (strcmp(method, "POST") == 0) {
        post_handler(fd, url, body);
    } else {
        http_send(fd, "405 Method Not Allowed", "text/plain", "?", 1);
    }
    free(req);
    close(fd);
    return NULL;
}

/* ================= main ================= */
int main(int argc, char** argv) {
    int port = XDBG_PORT;
    if (argc > 1) port = atoi(argv[1]);
    signal(SIGPIPE, SIG_IGN);

    /* web 根: cwd 优先, 回退可执行文件同目录 */
    struct stat st;
    if (stat(web_root, &st) != 0) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s", argv[0]);
        char* slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        snprintf(web_root, sizeof(web_root), "%s/xdebug/web", dir);
    }

    S = (Session*)calloc(1, sizeof(Session));
    pthread_mutex_init(&S->lock, NULL);
    kvm_set_flush_redirect(session_flush_redirect);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "端口 %d 绑定失败: %s\n", port, strerror(errno));
        return 1;
    }
    if (listen(lfd, 16) != 0) { perror("listen"); return 1; }

    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║   xdebugd —— xiaomo Debug Workbench  v1.0          ║\n");
    printf("║   持久 VM 调试会话 + 真机串口桥 + Web 工作台        ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    printf("  ➜  打开  http://127.0.0.1:%d\n", port);
    printf("  ➜  退出   Ctrl-C\n");
    fflush(stdout);

    while (1) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;
        int* pf = (int*)malloc(sizeof(int));
        *pf = cfd;
        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread, pf) == 0) pthread_detach(t);
        else { close(cfd); free(pf); }
    }
    return 0;
}




