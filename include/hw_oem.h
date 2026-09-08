/*
 * xiaomo - OEM 设备身份层 (hw_oem)
 *
 * 两层职责:
 *   1. OEM 设备签名 (eFuse 熔丝): 一枚永不变化的 64 位 hex 标志,
 *      上电由 VM (kvm_run) 自动烧录进签名寄存器 KVM_REG_SIG (R127),
 *      运行期硬件级写保护 —— 作为「这是哪台设备」的身份标志。
 *   2. ESP 目标识别 (遗留保留): C3/C6/S3 目标宏与 init/deinit 分派,
 *      仅真机编译 (CONFIG_IDF_TARGET_*) 时激活。
 */
#ifndef XIAOMO_HW_OEM_H
#define XIAOMO_HW_OEM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef __linux__
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <dirent.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 1. OEM 设备签名熔丝层
 * ============================================================ */

/* 设备标志符号: 永不变化的 64 位 hex (ASCII "XIAOMO01")。
 * 出厂即定, 全生命周期只读 —— 改它 = 换一台设备。 */
#define HW_OEM_SIG_HEX   0x5849414F4D4F3031ULL

/* 熔丝烧录状态 */
typedef enum {
    HW_OEM_UNBURNED = 0,   /* 出厂态: 尚未烧录 */
    HW_OEM_BURNED   = 1,   /* 已烧录: 熔丝值 = HW_OEM_SIG_HEX */
    HW_OEM_REJECTED = 2    /* 重复烧录被拒 (写一次熔丝) */
} HwOemBurnState;

/* 读熔丝值 (编译期常量, 永不变化) */
uint64_t        hw_oem_sig(void);
/* 烧录: 仅第一次生效写入 *out, 重复烧录返回 -1 (REJECTED) */
int             hw_oem_burn(uint64_t v);
/* 当前熔丝状态字符串 ("UNBURNED"/"BURNED"/"REJECTED") */
const char*     hw_oem_burn_state_str(void);

struct KillsVM;
/* 上电断言: 熔丝值固化进 vm->regs[KVM_REG_SIG] (kvm_run 起点自动调用) */
void            hw_oem_protect(struct KillsVM* vm);
/* CLI 身份卡: 打印签名 hex/寄存器/熔丝状态/真 VM 验证 (./xiaomo sig) */
int             hw_oem_sig_cli(void);
/* 字节码演示: 读签名寄存器 → 越权改写 R127 → 验证熔丝未动。
 * 返回 0 成功 (输出含 UNCHANGED)。 */
int             hw_sigprobe_run(void);

/* ============================================================
 * 2. ESP 目标识别 (遗留保留, 真机编译激活)
 * ============================================================ */
#define ESP_C3_RUN 0x03901
#define ESP_C6_RUN 0x06080
#define ESP_S3_RUN 0x09063

typedef uint16_t (*open_oem_info_cb)(uint16_t,uint16_t,uint16_t);
typedef int* (*input_cb)(void *arg);

typedef struct{
    uint16_t point;
    uint16_t address;
    uint8_t *buffer;
}HwOew;

#if CONFIG_IDF_TARGET_ESP32C6
    #define ESP_TARGET_RUN ESP_C6_RUN
    #define ESP_TARGET_C6  1
    #define ESP_TARGET_C3  0
    #define ESP_TARGET_S3  0
    void hw_espc6_init(void *arg);
    void hw_espc6_deinit(void *arg);
#elif CONFIG_IDF_TARGET_ESP32C3
    #define ESP_TARGET_RUN ESP_C3_RUN
    #define ESP_TARGET_C6  0
    #define ESP_TARGET_C3  1
    #define ESP_TARGET_S3  0
    void hw_espc3_init(void *arg);
    void hw_espc3_deinit(void *arg);
#elif CONFIG_IDF_TARGET_ESP32S3
    #define ESP_TARGET_RUN ESP_S3_RUN
    #define ESP_TARGET_C6  0
    #define ESP_TARGET_C3  0
    #define ESP_TARGET_S3  1
    void hw_esps3_init(void *arg);
    void hw_esps3_deinit(void *arg);
#elif defined(__APPLE__) || defined(__linux__) || defined(_WIN32)
    #define ESP_TARGET_RUN 0
    #define ESP_TARGET_C6  0
    #define ESP_TARGET_C3  0
    #define ESP_TARGET_S3  0
#else
    #error "Unsupported ESP32 target"
#endif

void hw_target_init(void *arg);
void hw_target_deinit(void *arg);
int *hw_target_runtime(void);
int *hw_target_outtime(void);
int hw_oem(int c6,int c3,int s3);

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_HW_OEM_H */
