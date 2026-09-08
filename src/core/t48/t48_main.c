/*
 * t48_main.c - XGecu T48 烧录器·自研烧录工具 CLI
 *
 * 用法：
 *   t48 probe              # 探测 T48 是否在线
 *   t48 list               # 列出支持的芯片
 *   t48 -c W25Q128 read    # 读取整片到 stdout
 *   t48 -c W25Q128 read file.bin    # 读取到文件
 *   t48 -c W25Q128 write file.bin   # 写入文件到芯片
 *   t48 -c W25Q128 erase            # 全片擦除
 *   t48 -c W25Q128 verify file.bin  # 校验
 *   t48 -c W25Q128 id               # 读芯片 ID
 *   t48 detect                      # SPI 自动检测
 *   t48 -c W25Q128 info             # 显示芯片信息
 *
 * 集成到 xiaomo VM：通过 .mo 字节码调用 T48 指令
 *   t48 --mo-exec read file.bin     # 生成 .mo 可执行烧录脚本
 */

#include "t48_proto.h"
#include "t48_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static void progress_cb(int percent, const char *msg)
{
    if (msg) {
        printf("\r%s", msg);
        if (percent >= 0) printf("... %d%%", percent);
        printf("   \n");
        fflush(stdout);
        return;
    }
    if (percent >= 0) {
        printf("\r  进度: %d%%", percent);
        fflush(stdout);
    }
}

static void print_banner(void)
{
    printf("╔══════════════════════════════════════╗\n");
    printf("║   XGecu T48 · 自研烧录工具 v1.0     ║\n");
    printf("║   基于 xiaomo VM 架构                ║\n");
    printf("╚══════════════════════════════════════╝\n");
}

static int cmd_probe(int verbose)
{
    int n = t48_probe(verbose);
    if (n > 0) {
        printf("✅ 发现 %d 个 T48 烧录器\n", n);
        return 0;
    } else {
        printf("❌ 未发现 T48 烧录器\n");
        return 1;
    }
}

static int cmd_list(void)
{
    t48_db_list();
    return 0;
}

static int cmd_info(const char *chip)
{
    const t48_device_t *d = t48_db_find(chip);
    if (!d) {
        fprintf(stderr, "❌ 未知芯片: %s\n", chip);
        return 1;
    }
    printf("芯片信息: %s\n", d->name);
    printf("  类型:     %s\n", d->type == T48_CHIP_EEPROM ? "EEPROM" : "MCU");
    printf("  协议:     %s\n", d->protocol_id == 0x03 ? "SPI" :
                               d->protocol_id == 0x01 ? "I2C" : "其它");
    printf("  主区大小: %u bytes (%.1f KB)\n",
           d->code_memory_size,
           d->code_memory_size / 1024.0);
    printf("  页大小:   %u\n", d->page_size);
    printf("  读缓冲:   %u\n", d->read_buffer_size);
    printf("  写缓冲:   %u\n", d->write_buffer_size);
    printf("  芯片 ID:  0x%06x\n", d->chip_id);
    printf("  SPI 时钟: %s\n", d->spi_clock ? "8MHz" : "默认");
    return 0;
}

static int cmd_id(const char *chip, int verbose)
{
    t48_handle_t *h = t48_open(chip, verbose);
    if (!h) return 1;

    int ret = t48_begin_transaction(h);
    if (ret == 0) {
        uint32_t id = 0;
        ret = t48_read_chip_id(h, &id);
        if (ret == 0) {
            uint32_t expected = t48_chip_size(h) ? 0 : 0;
            (void)expected;
            printf("芯片 ID: 0x%06x\n", id & 0xffffff);
        } else {
            fprintf(stderr, "❌ 读 ID 失败: %s\n", t48_strerror(ret));
        }
        t48_end_transaction(h);
    } else {
        fprintf(stderr, "❌ 事务开始失败: %s\n", t48_strerror(ret));
    }

    t48_close(h);
    return ret;
}

static int cmd_detect(int verbose)
{
    t48_handle_t *h = t48_open(NULL, verbose);
    if (!h) return 1;

    int ret = t48_begin_transaction(h);
    if (ret == 0) {
        uint32_t id = 0;
        ret = t48_spi_autodetect(h, &id);
        if (ret == 0) {
            const char *name = t48_chip_name(h);
            printf("✅ 检测到: JEDEC ID=0x%06x", id & 0xffffff);
            if (name)
                printf(" → %s (%u bytes)", name, t48_chip_size(h));
            printf("\n");
        } else {
            fprintf(stderr, "❌ 自动检测失败: %s\n", t48_strerror(ret));
        }
        t48_end_transaction(h);
    } else {
        fprintf(stderr, "❌ 事务开始失败: %s\n", t48_strerror(ret));
    }

    t48_close(h);
    return ret;
}

static int cmd_read(const char *chip, const char *filename, int verbose)
{
    t48_handle_t *h = t48_open(chip, verbose);
    if (!h) return 1;

    uint32_t size = t48_chip_size(h);
    uint8_t *buf = (uint8_t *)malloc(size);
    if (!buf) {
        fprintf(stderr, "❌ 内存不足\n");
        t48_close(h);
        return 1;
    }

    printf("📖 读取 %s (%u bytes)...\n", chip, size);

    int ret = t48_begin_transaction(h);
    if (ret == 0) {
        ret = t48_read_all(h, buf, progress_cb);
        t48_end_transaction(h);
    }

    if (ret > 0) {
        printf("\n✅ 读取完成: %d bytes\n", ret);

        if (filename) {
            FILE *fp = fopen(filename, "wb");
            if (!fp) {
                fprintf(stderr, "❌ 无法写入文件 %s: %s\n",
                        filename, strerror(errno));
                free(buf);
                t48_close(h);
                return 1;
            }
            fwrite(buf, 1, ret, fp);
            fclose(fp);
            printf("💾 已保存到: %s\n", filename);
        } else {
            /* 写到 stdout（可用于管道） */
            fwrite(buf, 1, ret, stdout);
        }
        ret = 0;
    } else {
        fprintf(stderr, "❌ 读取失败: %s\n", t48_strerror(ret));
    }

    free(buf);
    t48_close(h);
    return ret;
}

static int cmd_write(const char *chip, const char *filename, int verbose)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "❌ 无法打开文件 %s: %s\n", filename, strerror(errno));
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_size <= 0) {
        fprintf(stderr, "❌ 文件为空\n");
        fclose(fp);
        return 1;
    }

    uint8_t *buf = (uint8_t *)malloc(file_size);
    if (!buf) {
        fprintf(stderr, "❌ 内存不足\n");
        fclose(fp);
        return 1;
    }
    fread(buf, 1, file_size, fp);
    fclose(fp);

    printf("📝 写入 %s (%ld bytes 到 %s)...\n", filename, file_size, chip);

    t48_handle_t *h = t48_open(chip, verbose);
    if (!h) {
        free(buf);
        return 1;
    }

    int ret = t48_begin_transaction(h);
    if (ret == 0) {
        /* 先关保护 */
        t48_protect_off(h);
        ret = t48_write_all(h, buf, file_size, progress_cb);
        t48_end_transaction(h);
    }

    if (ret == 0) {
        printf("\n✅ 写入完成\n");
    } else {
        fprintf(stderr, "\n❌ 写入失败: %s\n", t48_strerror(ret));
    }

    free(buf);
    t48_close(h);
    return ret;
}

static int cmd_erase(const char *chip, int verbose)
{
    t48_handle_t *h = t48_open(chip, verbose);
    if (!h) return 1;

    printf("🧹 擦除 %s...\n", chip);

    int ret = t48_begin_transaction(h);
    if (ret == 0) {
        t48_protect_off(h);
        ret = t48_erase_chip(h);
        t48_end_transaction(h);
    }

    printf("%s 擦除 %s\n",
           ret == 0 ? "✅" : "❌",
           ret == 0 ? "完成" : t48_strerror(ret));

    t48_close(h);
    return ret;
}

static int cmd_verify(const char *chip, const char *filename, int verbose)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "❌ 无法打开文件 %s: %s\n", filename, strerror(errno));
        return 1;
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *buf = (uint8_t *)malloc(file_size);
    if (!buf) {
        fprintf(stderr, "❌ 内存不足\n");
        fclose(fp);
        return 1;
    }
    fread(buf, 1, file_size, fp);
    fclose(fp);

    printf("🔍 校验 %s 与 %s...\n", chip, filename);

    t48_handle_t *h = t48_open(chip, verbose);
    if (!h) {
        free(buf);
        return 1;
    }

    int ret = t48_begin_transaction(h);
    if (ret == 0) {
        ret = t48_verify(h, buf, file_size, progress_cb);
        t48_end_transaction(h);
    }

    if (ret == 0) {
        printf("\n✅ 校验通过: 完全一致\n");
    } else if (ret > 0) {
        printf("\n❌ 校验失败: 偏移 0x%x 不一致\n", ret - 1);
    } else {
        printf("\n❌ 校验错误: %s\n", t48_strerror(ret));
    }

    free(buf);
    t48_close(h);
    return ret;
}

static void usage(const char *prog)
{
    printf("用法: %s [选项] <命令> [参数]\n\n", prog);
    printf("选项:\n");
    printf("  -c <芯片名>   指定芯片型号（如 W25Q128）\n");
    printf("  -v            详细输出\n");
    printf("  -q            静默模式\n\n");
    printf("命令:\n");
    printf("  probe         探测 T48 烧录器\n");
    printf("  list          列出支持的芯片\n");
    printf("  info          显示芯片信息\n");
    printf("  id            读取芯片 ID\n");
    printf("  detect        SPI 自动检测\n");
    printf("  read [文件]   读取芯片到文件（无文件=stdout）\n");
    printf("  write <文件>  写入文件到芯片\n");
    printf("  erase         全片擦除\n");
    printf("  verify <文件> 校验芯片内容\n\n");
    printf("示例:\n");
    printf("  %s probe\n", prog);
    printf("  %s -c W25Q128 info\n", prog);
    printf("  %s -c W25Q128 read backup.bin\n", prog);
    printf("  %s -c W25Q128 write firmware.bin\n", prog);
    printf("  %s -c W25Q128 verify firmware.bin\n", prog);
    printf("  %s -c W25Q128 erase\n", prog);
    printf("  %s detect\n", prog);
}

int main(int argc, char *argv[])
{
    const char *chip = NULL;
    int verbose = 1;
    int i = 1;

    /* 解析选项 */
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            chip = argv[++i];
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 2;
        } else if (strcmp(argv[i], "-q") == 0) {
            verbose = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知选项: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
        i++;
    }

    if (i >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[i];
    const char *arg = (i + 1 < argc) ? argv[i + 1] : NULL;

    if (verbose)
        print_banner();

    if (strcmp(cmd, "probe") == 0) {
        return cmd_probe(verbose);
    } else if (strcmp(cmd, "list") == 0) {
        return cmd_list();
    } else if (strcmp(cmd, "info") == 0) {
        if (!chip) { fprintf(stderr, "❌ 请用 -c 指定芯片\n"); return 1; }
        return cmd_info(chip);
    } else if (strcmp(cmd, "id") == 0) {
        if (!chip) { fprintf(stderr, "❌ 请用 -c 指定芯片\n"); return 1; }
        return cmd_id(chip, verbose);
    } else if (strcmp(cmd, "detect") == 0) {
        return cmd_detect(verbose);
    } else if (strcmp(cmd, "read") == 0) {
        if (!chip) { fprintf(stderr, "❌ 请用 -c 指定芯片\n"); return 1; }
        return cmd_read(chip, arg, verbose);
    } else if (strcmp(cmd, "write") == 0) {
        if (!chip) { fprintf(stderr, "❌ 请用 -c 指定芯片\n"); return 1; }
        if (!arg) { fprintf(stderr, "❌ 请指定文件名\n"); return 1; }
        return cmd_write(chip, arg, verbose);
    } else if (strcmp(cmd, "erase") == 0) {
        if (!chip) { fprintf(stderr, "❌ 请用 -c 指定芯片\n"); return 1; }
        return cmd_erase(chip, verbose);
    } else if (strcmp(cmd, "verify") == 0) {
        if (!chip) { fprintf(stderr, "❌ 请用 -c 指定芯片\n"); return 1; }
        if (!arg) { fprintf(stderr, "❌ 请指定文件名\n"); return 1; }
        return cmd_verify(chip, arg, verbose);
    } else {
        fprintf(stderr, "❌ 未知命令: %s\n", cmd);
        usage(argv[0]);
        return 1;
    }
}