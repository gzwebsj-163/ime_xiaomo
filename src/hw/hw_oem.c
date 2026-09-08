/*
 * xiaomo - OEM 设备身份层 (hw_oem)
 *
 * 1. 设备签名熔丝: HW_OEM_SIG_HEX (ASCII "XIAOMO01") 上电烧录进
 *    VM 签名寄存器 KVM_REG_SIG (R127), 四重固化永不变化:
 *      ① 熔丝 write-once  ② 内核每指令步写屏蔽  ③ 编译器全局区封顶
 *      ④ 字节码层只有读指令 (OP_HW_SIG_RD), 无写指令存在
 * 2. ESP 目标识别 (遗留保留): 仅真机编译激活。
 */
#include "hw_oem.h"
#include "hw_hex.h"
#include "vm_core.h"
#include <stdint.h>

/* ---- ESP OEM 寄存器窗 (遗留保留, 真机编译激活) ---- */
#if ESP_TARGET_C3
#define HW_OEM_REG_BASE     0x60000000U
#elif ESP_TARGET_C6
#define HW_OEM_REG_BASE     0x60000000U
#elif ESP_TARGET_S3
#define HW_OEM_REG_BASE     0x60000000U
#else
#define HW_OEM_REG_BASE     0U
#endif
#define HW_OEM_REG_OFFSET    (0x10U + 0x08U)

/* ============================================================
 * 1. 设备签名熔丝层
 * ============================================================ */

static HwOemBurnState g_oem_burn_state = HW_OEM_UNBURNED;

uint64_t hw_oem_sig(void)
{
    return HW_OEM_SIG_HEX;   /* 编译期常量: 出厂即定, 永不变化 */
}

int hw_oem_burn(uint64_t v)
{
    if (g_oem_burn_state == HW_OEM_BURNED) {
        /* 熔丝写一次语义: 已经烧过, 一律拒绝 */
        g_oem_burn_state = HW_OEM_REJECTED;
        return -1;
    }
    if (v != HW_OEM_SIG_HEX) {
        /* 只允许烧录出厂签名值 */
        return -1;
    }
    g_oem_burn_state = HW_OEM_BURNED;
    return 0;
}

const char* hw_oem_burn_state_str(void)
{
    switch (g_oem_burn_state) {
    case HW_OEM_BURNED:   return "BURNED";
    case HW_OEM_REJECTED: return "REJECTED";
    default:              return "UNBURNED";
    }
}

void hw_oem_protect(struct KillsVM* vm)
{
    if (!vm) return;
    /* 上电断言: 熔丝常量固化进签名寄存器 + VM 熔丝槽。
     * 注意: 这里不走 write-once 状态机 —— kvm_run 每次都是一次"上电",
     * 硬件每次上电重新断言同一个出厂常量; write-once 属于出厂数程
     * (hw_oem_burn)。值本身永不变化, 两层语义不冲突。 */
    vm->sig_fuse   = hw_oem_sig();
    vm->sig_burned = 1;
    vm->regs[KVM_REG_SIG] = (int64_t)vm->sig_fuse;
}

/* ---- 字节码演示: sigprobe ----
 *   0: PRINT "=== OEM 设备签名探针 ==="
 *   1: PRINT "SIG[R127] ="
 *   2: HW_SIG_RD R0          ; 读熔丝签名寄存器
 *   3: PRINT R0 (raw)
 *   4: PRINT "attacker: MOV R127, 0xDEAD..."
 *   5: MOV R127, 0xDEADBEEF  ; 越权改写 (将被步末写屏蔽回滚)
 *   6: HW_SIG_RD R1          ; 再读
 *   7: PRINT R1 (raw)
 *   8: PRINT "violations ="
 *   9: MOV R2, sig_violations 由宿主回填...
 */
int hw_sigprobe_run(void)
{
    KillsProgram* p = kprog_new();
    if (!p) return 1;

    int c_title = kprog_add_const(p, 1, 0, 0, "=== OEM sig probe ===");
    int c_lbl   = kprog_add_const(p, 1, 0, 0, "SIG[R127] (raw) =");
    int c_atk   = kprog_add_const(p, 1, 0, 0, "attacker writes R127 = 0xDEADBEEF ...");
    int c_lbl2  = kprog_add_const(p, 1, 0, 0, "after: SIG[R127] (raw) =");

    /* 0 */ kprog_add_ins(p, OP_PRINT, -1, c_title, 0);
    /* 1 */ kprog_add_ins(p, OP_PRINT, -1, c_lbl, 1);
    /* 2 */ kprog_add_ins(p, OP_HW_SIG_RD, 0, -1, 0);      /* R0 = R127 熔丝值 */
    /* 3 */ kprog_add_ins(p, OP_PRINT, 0, -1, 2);          /* raw 打印 R0 */
    /* 4 */ kprog_add_ins(p, OP_PRINT, -1, c_atk, 0);
    /* 5 */ kprog_add_ins(p, OP_MOV, KVM_REG_SIG, -1, (int64_t)0xDEADBEEF); /* 越权写 */
    /* 6 */ kprog_add_ins(p, OP_HW_SIG_RD, 1, -1, 0);      /* R1 = 再读 */
    /* 7 */ kprog_add_ins(p, OP_PRINT, -1, c_lbl2, 1);
    /* 8 */ kprog_add_ins(p, OP_PRINT, 1, -1, 2);

    printf("=== xiaomo OEM 设备签名 (eFuse -> R127) ===\n\n");
    char dis[1024];
    kvm_disassemble(p, dis, sizeof(dis));
    printf("--- 反汇编 ---\n%s\n", dis);

    KillsVM vm;
    kvm_init(&vm);
    int rc = kvm_run(&vm, p);
    if (rc != 0 && vm.error_count > 0) {
        printf("签名探针错误: %s\n", vm.error_msg);
    }

    printf("\n--- 结果 ---\n");
    for (int i = 0; i < kvm_output_count(&vm); i++) {
        printf("%s\n", kvm_output(&vm, i));
    }
    /* 步末写屏蔽审计: 越权写 R127 应被回滚且计数 */
    printf("violations = %u (期望 >= 1)\n", vm.sig_violations);
    char hex[24];
    printf("sig = %s, R127 = %s\n",
           hw_hex_fmt64(hw_oem_sig(), hex, sizeof(hex)),
           vm.regs[KVM_REG_SIG] == (int64_t)HW_OEM_SIG_HEX ? "UNCHANGED" : "CORRUPTED!");

    int ok = (vm.regs[KVM_REG_SIG] == (int64_t)HW_OEM_SIG_HEX) && vm.sig_violations >= 1;
    kvm_free(&vm);
    kprog_free(p);
    return ok ? 0 : 1;
}

/* ---- CLI 身份卡: ./xiaomo sig ---- */
int hw_oem_sig_cli(void)
{
    char hex[24];
    uint64_t sig = hw_oem_sig();
    printf("=== xiaomo OEM 设备签名 (eFuse) ===\n");
    printf("sig hex    : %s\n", hw_hex_fmt64(sig, hex, sizeof(hex)));
    printf("ascii      : \"XIAOMO01\"\n");
    printf("register   : R%d (KVM_REG_SIG, 寄存器堆顶端)\n", KVM_REG_SIG);

    /* 出厂数程演示: 熔丝 write-once */
    hw_oem_burn(sig);
    printf("fuse       : %s", hw_oem_burn_state_str());
    hw_oem_burn(0xDEADBEEF);   /* 重复烧录 → 必须被拒 */
    printf(" → 重复烧录: %s\n", hw_oem_burn_state_str());

    /* 真 VM 上电验证: R127 即设备标志 */
    KillsProgram* p = kprog_new();
    if (p) {
        kprog_add_ins(p, OP_HALT, 0, 0, 0);
        KillsVM vm;
        kvm_init(&vm);
        int rc = kvm_run(&vm, p);
        printf("live vm    : R127 = %s %s (violations=%u)\n",
               hw_hex_fmt64((uint64_t)vm.regs[KVM_REG_SIG], hex, sizeof(hex)),
               (rc == 0 && vm.regs[KVM_REG_SIG] == (int64_t)sig) ? "OK" : "MISMATCH",
               vm.sig_violations);
        kvm_free(&vm);
        kprog_free(p);
    }
    printf("protection : 熔丝 write-once + 内核步末写屏蔽 + 编译器 R_GLOBAL_MAX 封顶 + opcode 只读\n");
    return 0;
}

/* ============================================================
 * 2. ESP 目标识别 (遗留保留)
 * ============================================================ */
int g_hw_runtime = 0;
int g_hw_outtime = 0;

void hw_target_init(void *arg)
{
#if ESP_TARGET_C6
    hw_espc6_init(arg);
#elif ESP_TARGET_C3
    hw_espc3_init(arg);
#elif ESP_TARGET_S3
    hw_esps3_init(arg);
#else
    (void)arg;
#endif
}

void hw_target_deinit(void *arg)
{
#if ESP_TARGET_C6
    hw_espc6_deinit(arg);
#elif ESP_TARGET_C3
    hw_espc3_deinit(arg);
#elif ESP_TARGET_S3
    hw_esps3_deinit(arg);
#else
    (void)arg;
#endif
}

int *hw_target_runtime(void)
{
    return &g_hw_runtime;
}

int *hw_target_outtime(void)
{
    return &g_hw_outtime;
}

int oem_info(open_oem_info_cb open_info,input_cb input){
    int ret = open_info(ESP_C3_RUN,ESP_C6_RUN,ESP_S3_RUN);
    int *p = input(NULL);
    (void)p;
    return ret;
}

int hw_oem(int c3, int c6, int s3)
{
    int ret = 0;
    (void)c3;
    (void)c6;
    (void)s3;

#if (ESP_TARGET_C3 || ESP_TARGET_C6 || ESP_TARGET_S3)
    volatile uint16_t * const reg_oem = (volatile uint16_t *)(HW_OEM_REG_BASE + HW_OEM_REG_OFFSET);
    uint16_t val = *reg_oem;
    (void)val;
    ret = 0;
#else
    ret = 0;
#endif
    return ret;
}
