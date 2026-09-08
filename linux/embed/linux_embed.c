/*
 * linux_embed.c — xiaomo 嵌入式 Linux 内核核心实现
 *
 * 把 TinyEMU 封装成 xiaomo 的一个 C 模块。
 * 不再使用 temu.c 的 main/console_init（那些依赖 termios 终端 + select 事件循环），
 * 这里自研一个环形缓冲 CharacterDevice，让 guest 的 virtio-console 输出
 * 落到内存缓冲，输入从命令队列注入 —— 从而可以被任意宿主程序驱动。
 *
 * (c) xiaomo project, MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <time.h>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"

#include "linux_embed.h"

/* ================================================================== */
/* 环形字节缓冲（保留最近 N 字节）                                     */
/* ================================================================== */
#define RING_SIZE (4 << 20)  /* 4MB 输出缓冲 */

typedef struct {
    uint8_t *buf;
    int size;
    int head;   /* 下一个写位置 */
    int count;  /* 有效字节数 */
} RingBuf;

static void ring_init(RingBuf *r, int size)
{
    r->buf = malloc(size);
    r->size = size;
    r->head = 0;
    r->count = 0;
}

static void ring_reset(RingBuf *r)
{
    r->head = 0;
    r->count = 0;
}

static void ring_free(RingBuf *r)
{
    free(r->buf);
    r->buf = NULL;
    r->size = 0;
    r->head = 0;
    r->count = 0;
}

/* 追加 n 字节（覆盖最旧数据） */
static void ring_write(RingBuf *r, const uint8_t *buf, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        r->buf[r->head] = buf[i];
        r->head = (r->head + 1) % r->size;
        if (r->count < r->size)
            r->count++;
    }
}

/* 顺序读出全部有效字节，最多 n 字节；返回实际读出数 */
static int ring_read_all(RingBuf *r, uint8_t *out, int n)
{
    int i, pos, m;
    m = (r->count < n) ? r->count : n;
    pos = (r->head - r->count + r->size) % r->size;
    for (i = 0; i < m; i++) {
        out[i] = r->buf[(pos + i) % r->size];
    }
    r->count -= m;
    return m;
}

/* 在环形缓冲的有效内容里找子串 */
static int ring_find(RingBuf *r, const char *needle)
{
    int nlen = strlen(needle);
    int i, j, pos;
    if (nlen == 0 || r->count < nlen)
        return 0;
    pos = (r->head - r->count + r->size) % r->size;
    for (i = 0; i <= r->count - nlen; i++) {
        for (j = 0; j < nlen; j++) {
            if (r->buf[(pos + i + j) % r->size] != (uint8_t)needle[j])
                break;
        }
        if (j == nlen)
            return 1;
    }
    return 0;
}

/* ================================================================== */
/* 嵌入式控制台 CharacterDevice                                       */
/*                                                                     */
/*   write_data ← guest 输出（virtio-console）→ 写入输出 ring           */
/*   read_data  ← 宿主输入队列（命令注入）→ 喂给 guest                  */
/* ================================================================== */
#define IN_SIZE (1 << 16)  /* 64KB 输入队列 */

typedef struct {
    RingBuf out;        /* guest 输出 */
    uint8_t *in_buf;    /* 宿主输入队列 */
    int in_pos, in_len;
} EmbedConsole;

static void ec_write(void *opaque, const uint8_t *buf, int len)
{
    EmbedConsole *ec = opaque;
    ring_write(&ec->out, buf, len);
}

static int ec_read(void *opaque, uint8_t *buf, int len)
{
    EmbedConsole *ec = opaque;
    int n;
    if (ec->in_len <= 0)
        return 0;
    n = (ec->in_len < len) ? ec->in_len : len;
    memcpy(buf, ec->in_buf + ec->in_pos, n);
    ec->in_pos += n;
    ec->in_len -= n;
    if (ec->in_len == 0)
        ec->in_pos = 0;
    return n;
}

/* ================================================================== */
/* 块设备（从 temu.c 移植的纯文件 IO，去掉终端/网络依赖）              */
/* ================================================================== */
typedef enum {
    BF_MODE_RO,
    BF_MODE_RW,
    BF_MODE_SNAPSHOT,
} BlockDeviceModeEnum;

#define SECTOR_SIZE 512

typedef struct {
    FILE *f;
    int64_t nb_sectors;
    BlockDeviceModeEnum mode;
    uint8_t **sector_table;
} BlockDeviceFile;

static int64_t bf_get_sector_count(BlockDevice *bs)
{
    BlockDeviceFile *bf = bs->opaque;
    return bf->nb_sectors;
}

static int bf_read_async(BlockDevice *bs,
                         uint64_t sector_num, uint8_t *buf, int n,
                         BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceFile *bf = bs->opaque;
    if (!bf->f)
        return -1;
    if (bf->mode == BF_MODE_SNAPSHOT) {
        int i;
        for (i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num]) {
                fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
                fread(buf, 1, SECTOR_SIZE, bf->f);
            } else {
                memcpy(buf, bf->sector_table[sector_num], SECTOR_SIZE);
            }
            sector_num++;
            buf += SECTOR_SIZE;
        }
    } else {
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fread(buf, 1, (size_t)n * SECTOR_SIZE, bf->f);
    }
    return 0; /* synchronous read */
}

static int bf_write_async(BlockDevice *bs,
                          uint64_t sector_num, const uint8_t *buf, int n,
                          BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceFile *bf = bs->opaque;
    int ret;

    switch (bf->mode) {
    case BF_MODE_RO:
        ret = -1;
        break;
    case BF_MODE_RW:
        fseek(bf->f, sector_num * SECTOR_SIZE, SEEK_SET);
        fwrite(buf, 1, (size_t)n * SECTOR_SIZE, bf->f);
        ret = 0;
        break;
    case BF_MODE_SNAPSHOT:
        {
            int i;
            if ((sector_num + n) > (uint64_t)bf->nb_sectors)
                return -1;
            for (i = 0; i < n; i++) {
                if (!bf->sector_table[sector_num])
                    bf->sector_table[sector_num] = malloc(SECTOR_SIZE);
                memcpy(bf->sector_table[sector_num], buf, SECTOR_SIZE);
                sector_num++;
                buf += SECTOR_SIZE;
            }
            ret = 0;
        }
        break;
    default:
        abort();
    }
    return ret;
}

static BlockDevice *block_device_init(const char *filename,
                                      BlockDeviceModeEnum mode)
{
    BlockDevice *bs;
    BlockDeviceFile *bf;
    int64_t file_size;
    FILE *f;

    f = fopen(filename, "rb");
    if (!f) {
        perror(filename);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    file_size = ftello(f);

    bs = mallocz(sizeof(*bs));
    bf = mallocz(sizeof(*bf));

    bf->mode = mode;
    bf->nb_sectors = file_size / SECTOR_SIZE;
    bf->f = f;

    if (mode == BF_MODE_SNAPSHOT) {
        bf->sector_table = mallocz(sizeof(bf->sector_table[0]) * bf->nb_sectors);
    }

    bs->opaque = bf;
    bs->get_sector_count = bf_get_sector_count;
    bs->read_async = bf_read_async;
    bs->write_async = bf_write_async;
    return bs;
}

/* ================================================================== */
/* LinuxVM 主体                                                       */
/* ================================================================== */
#define MAX_EXEC_CYCLE 200000

struct LinuxVM {
    VirtMachine *m;
    VirtMachineParams p;
    EmbedConsole ec;
    int booted;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ================================================================== */
/* 执行循环：喂输入 → 执行指令 → 睡一小段推进虚拟时钟                  */
/* ================================================================== */
static void vm_pump(LinuxVM *vm, int sleep_ms)
{
    VirtMachine *m = vm->m;
    CharacterDevice *cs = m->console;

    /* 把宿主输入队列送入 guest 的 virtio-console */
    if (cs && m->console_dev && vm->ec.in_len > 0 &&
        virtio_console_can_write_data(m->console_dev)) {
        int len = virtio_console_get_write_len(m->console_dev);
        uint8_t tmp[512];
        int n;
        len = min_int(len, (int)sizeof(tmp));
        n = min_int(len, vm->ec.in_len);
        if (n > 0) {
            memcpy(tmp, vm->ec.in_buf + vm->ec.in_pos, n);
            vm->ec.in_pos += n;
            vm->ec.in_len -= n;
            if (vm->ec.in_len == 0)
                vm->ec.in_pos = 0;
            virtio_console_write_data(m->console_dev, tmp, n);
        }
    }

    virt_machine_interp(m, MAX_EXEC_CYCLE);

    if (sleep_ms > 0) {
        int delay = virt_machine_get_sleep_duration(m, sleep_ms);
        if (delay > 0) {
            struct timespec ts;
            ts.tv_sec = delay / 1000;
            ts.tv_nsec = (int64_t)(delay % 1000) * 1000000L;
            nanosleep(&ts, NULL);
        }
    }
}

static void vm_push_stdin(LinuxVM *vm, const char *s)
{
    int n = strlen(s);
    if (vm->ec.in_len + n > IN_SIZE)
        n = IN_SIZE - vm->ec.in_len;
    if (n > 0) {
        memcpy(vm->ec.in_buf + vm->ec.in_pos + vm->ec.in_len, s, n);
        vm->ec.in_len += n;
    }
}

/* ================================================================== */
/* 公开 API                                                           */
/* ================================================================== */
LinuxVM *linux_init(const char *cfg_path)
{
    LinuxVM *vm;
    int i;

    vm = mallocz(sizeof(*vm));

    /* 打开 drive 文件（相对 cfg 目录解析） */
    virt_machine_set_defaults(&vm->p);
    virt_machine_load_config_file(&vm->p, cfg_path, NULL, NULL);

    for (i = 0; i < vm->p.drive_count; i++) {
        char *fname = get_file_path(vm->p.cfg_filename,
                                    vm->p.tab_drive[i].filename);
        vm->p.tab_drive[i].block_dev = block_device_init(fname, BF_MODE_SNAPSHOT);
        free(fname);
        if (!vm->p.tab_drive[i].block_dev) {
            fprintf(stderr, "[linux_embed] 打开 drive %d 失败\n", i);
            free(vm);
            return NULL;
        }
    }

    /* 自研嵌入式控制台（环形缓冲，替代 termios 终端） */
    ring_init(&vm->ec.out, RING_SIZE);
    vm->ec.in_buf = malloc(IN_SIZE);
    vm->ec.in_pos = vm->ec.in_len = 0;

    vm->p.console = mallocz(sizeof(*vm->p.console));
    vm->p.console->opaque = &vm->ec;
    vm->p.console->write_data = ec_write;
    vm->p.console->read_data = ec_read;

    /* RTC 用真实时间，加速 boot 时钟 */
    vm->p.rtc_real_time = TRUE;

    vm->m = virt_machine_init(&vm->p);
    if (!vm->m) {
        fprintf(stderr, "[linux_embed] virt_machine_init 失败\n");
        linux_end(vm);
        return NULL;
    }
    virt_machine_free_config(&vm->p);

    vm->booted = 0;
    return vm;
}

int linux_exec(LinuxVM *vm, const char *cmd, const char *expect, int timeout_ms)
{
    uint64_t start, deadline;
    if (!vm || !vm->m)
        return -1;
    if (timeout_ms <= 0)
        timeout_ms = 30000;

    /* 注入命令前清空历史输出，expect 只匹配本次之后的输出 */
    ring_reset(&vm->ec.out);

    if (cmd && cmd[0]) {
        vm_push_stdin(vm, cmd);
        vm_push_stdin(vm, "\n");
    }

    start = now_ms();
    deadline = start + (uint64_t)timeout_ms;
    for (;;) {
        vm_pump(vm, 10);
        if (expect && ring_find(&vm->ec.out, expect))
            return 0;  /* 出现预期输出 */
        if (now_ms() > deadline)
            return expect ? 1 : 0;  /* 超时；无 expect 时视为完成 */
    }
}

int linux_read_output(LinuxVM *vm, char *buf, int buf_size)
{
    int n;
    if (!vm || !vm->m || buf_size <= 0)
        return 0;
    n = ring_read_all(&vm->ec.out, (uint8_t *)buf, buf_size - 1);
    buf[n] = '\0';
    return n;
}

int linux_has_output(LinuxVM *vm)
{
    if (!vm || !vm->m)
        return 0;
    return vm->ec.out.count > 0;
}

void linux_dump_output(LinuxVM *vm)
{
    char buf[RING_SIZE > (1 << 20) ? (1 << 20) : RING_SIZE];
    int n;
    if (!vm || !vm->m)
        return;
    n = linux_read_output(vm, buf, (int)sizeof(buf));
    if (n > 0)
        fwrite(buf, 1, n, stdout);
}

void linux_end(LinuxVM *vm)
{
    if (!vm)
        return;
    if (vm->m)
        virt_machine_end(vm->m);
    ring_free(&vm->ec.out);
    free(vm->ec.in_buf);
    free(vm->p.console);
    free(vm);
}
