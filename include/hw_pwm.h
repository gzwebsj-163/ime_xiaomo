/* ============================================================================
 * xiaomo - PWM 脉冲输出层 (hw_pwm)
 * ============================================================================
 * 来源: 用户 2026-10-04 给出的寄存器档案 (include/hw_pwm.h 初版) + 一行空壳
 *       src/hw/hw_pwm.c。地址一位未改, 本文件在其上补齐家族契约。
 *
 * 【命名分层: 为什么本模块用 pwm_ 前缀而不是 hw_pwm_ 前缀】   ← 2026-10-04 查证
 *   仓库里 hw_direct.c 早已占用【且已在 include/hw_direct.h 公开声明】7 个
 *   hw_pwm_* 符号 (sysfs PWM 通道):
 *       hw_pwm_export / hw_pwm_unexport / hw_pwm_set_period /
 *       hw_pwm_set_duty   / hw_pwm_set_enable / hw_pwm_read / hw_pwm_enumerate
 *   那是【Linux sysfs /sys/class/pwm 那一路】, 与本模块的【芯片 PWM 寄存器
 *   那一路】是两个不同的东西 (家族坑: 同一个名字两套真相源)。本模块若也叫
 *   hw_pwm_* 会在链接期直接撞车 (duplicate symbol), 且两层语义会互相污染。
 *   ⇒ 本模块统一用 pwm_ 前缀。头文件初版里的两个 static inline
 *   (hw_pwm_read_oep_reg / hw_pwm_emc_map_to_ddc) 一并改名为
 *   pwm_reg_read_oep_scaled / pwm_reg_map_emc_to_ddc —— 它们是本模块的函数,
 *   挂在 sysfs 那个命名空间下属于误放。
 *
 * 【寄存器访问: 为什么裸解引用必须走 BSP】
 *   头文件初版把 pwm_crw/pwm_cww 定义成 *(volatile uint32_t*)(addr) 裸解引用。
 *   这套地址是【某个具体芯片】的寄存器窗口, 在宿主 macOS/Linux 上解引用
 *   0x103000 = SIGSEGV, 六模式一致性直接归零。家族既有同款修法:
 *   hw_dmc_crw.c 明确写着"无 BSP 时返回 0 而不是解引用猜测地址"。
 *   ⇒ 契约:
 *       pwm_crw/pwm_cww  保留, 但【只供真机 BSP 自己用】(它就是真机写法)
 *       pwm_reg_read/write  一律走 BSP 注入; 未装 BSP = 诚实 0 / 拒绝写
 *       分水岭是结构性 NULL (g_read == NULL), 不是上层自觉 (家族坑 #21)
 *
 * 【档案锁: 保留初版的 #error 意图, 但给出显式解锁口】
 *   初版用 `#if X != 默认 #error` 把头文件钉死在这颗芯片上。这个意图要保留
 *   (它拦住"换芯片忘了换地址"这种最难查的错), 但它不能让本头在别的芯片上
 *   连编译都过不了 (家族坑 #26: 内置档案脚号写死某型号 = 上真机的必要动作
 *   而不是死锁)。⇒ 加 HW_PWM_PROFILE_UNLOCK 显式解锁, 默认仍锁。
 *   锁的判据同时由 selftest 断言 (运行期), 预处理器只是第一道。
 *
 * 【本轮未验证的边界 —— 明确声明, 不假装知道】
 *   七个寄存器的【位域含义】(DC/DDC/OEP/EMC 各哪些 bit 管什么)与
 *   PUT/POST/OPEN 那组写选通码的时序语义, 目前只有地址, 没有手册核对。
 *   本模块因此【不】去编造位域布局与时序, 只做可自证的纯粹:
 *       地址表 (数据, 黄金值钉死) / 频率与占空比换算 (纯算术, 黄金值钉死)
 *       / 通道状态机 / 寄存器读写通道
 *   任何"按位域配置 PWM"的真机功能, 必须等拿到手册后另立一条并补对拍。
 *   拿不到手册就调这些寄存器 = 往未知地址写未知值 (家族铁律: 诚实失败,
 *   不假装成功)。见 pwm_cmd() 里 REAL 档的 NOBSP 分支。
 * ==========================================================================*/
#ifndef XIAOMO_HW_PWM_H
#define XIAOMO_HW_PWM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 1. 寄存器地址表 —— 用户初版真值, 一位未改
 * ============================================================ */
#define HW_PWM_FREQUENCY  100000U     /* 100 kHz 中心频点            */
#define HW_PWM_DC         0x030000U   /* 占空比控制字                */
#define HW_PWM_DDC        0x052800U   /* DDC                        */
#define HW_PWM_OEP        0x063800U   /* 输出使能/极性              */
#define HW_PWM_EMC        0x103000U   /* EMC 源                     */
#define HW_PWM_OPEN       0x010358U   /* 通道开窗选通               */
#define HW_PWM_PUT        0x0EFAFFU   /* 写选通                     */
#define HW_PWM_POST       0x0DEEFFU   /* 写后选通                   */
#define HW_PWM_VPP        0x01U       /* VPP 脚号                   */
#define HW_PWM_GND        0x00U       /* GND 脚号                   */

/* 初版自带的三个上界/一致性检查, 保留但可显式解锁 (见文件头说明) */
#if !defined(HW_PWM_PROFILE_UNLOCK)
#  if HW_PWM_FREQUENCY > 0x200000U
#    error "HW_PWM_FREQUENCY exceeds maximum limit"
#  endif
#  if HW_PWM_DC > 0x033000U
#    error "HW_PWM_DC address out of range"
#  endif
#  if HW_PWM_DDC != 0x052800U
#    error "HW_PWM_DDC must be equal to 0x052800U"
#  endif
#endif

/* ---- 真机直访宏 (仅供真机 BSP 内部使用) --------------------
 * ⚠️ 这两个宏是【裸解引用】。在宿主上解引用 0x103000 = SIGSEGV。
 *    模块本体一律不得直接用它们, 必须走 pwm_reg_read/write (走 BSP)。
 *    保留它们的唯一目的: 真机固件注册 BSP 时, 里面写的正是这个。 */
#define pwm_crw(addr)      (*(volatile uint32_t *)((uintptr_t)(addr)))
#define pwm_cww(addr, val) do { pwm_crw(addr) = (val); } while (0)

/* ============================================================
 * 2. 六模式探测 (家族规范: OVERRIDE > KELL > ESP8266 > ESP32 > LINUX > HOST > TEST)
 * ============================================================ */
#define HW_PWM_MODE_HOST     0u
#define HW_PWM_MODE_LINUX    1u
#define HW_PWM_MODE_KELL     2u
#define HW_PWM_MODE_ESP32    3u
#define HW_PWM_MODE_ESP8266  4u
#define HW_PWM_MODE_TEST     5u
#define HW_PWM_MODE_MAX      6u

/* ============================================================
 * 3. 返回码 (家族约定: 0 = 成功/就绪; 0 不能既是"空"又是"就绪"以外的东西)
 *    ERR_NOBSP 是本模块的招牌: 无硬件时诚实失败, 而不是读一个猜测地址
 * ============================================================ */
#define PWM_OK             0
#define PWM_ERR_PARAM     (-1)   /* 参数越界 (pct>100 / freq 越界 / ch 越界) */
#define PWM_ERR_NOBSP     (-2)   /* 未装 BSP = 无硬件 = 诚实失败, 非崩溃    */
#define PWM_ERR_STATE     (-3)   /* 通道状态不允许该操作                    */
#define PWM_ERR_BOUND     (-4)   /* 取值先校验后索引: 上界越界             */
#define PWM_ERR_REJECT    (-5)   /* 命令语义被拒 (非法命令)                 */

/* ============================================================
 * 4. 常量
 * ============================================================ */
#define PWM_REG_MAX        7u    /* DC/DDC/OEP/EMC/OPEN/PUT/POST 七字        */
#define PWM_PIN_MAX        2u    /* VPP/GND 两脚                            */
#define PWM_CH_MAX         4u    /* 通道数 (本模块的建模上限)                */
#define PWM_DUTY_MAX       100u  /* 占空比百分比上界, 不是 65535             */
#define PWM_DUTY_SCALE     65535u/* 占空比编码满量程                        */
#define PWM_FREQ_MAX       0x200000u  /* 初版上界, 频率不得超过此值          */

/* ---- 黄金值 (2026-10-04 由独立 Python 实现 tools 侧口径锁定) ----
 * 口径: FNV-1a-32, 按声明顺序把各 uint32 的【小端 4 字节】逐字节喂入。
 *   G_REG: 7 个寄存器地址的 LE32 拼接
 *   G_PIN: 2 个脚号的 LE32 拼接
 *   G_ALL: G_REG 原料 + G_PIN 原料 + 频率上限 LE32
 *   G_VEC: 占空比编码向量 {0,25,50,75,100} 的 LE32 拼接
 * 复算工具见 knowledge 页与本轮交付说明; 换算法必须连口径一起重录 (坑 #34) */
#define PWM_GOLDEN_REG     0xBAC4EE86u
#define PWM_GOLDEN_PIN     0x3E801244u
#define PWM_GOLDEN_ALL     0x7F0222B8u
#define PWM_GOLDEN_VEC     0xB279AFC1u

/* 公共出口掩码 (家族同族口径, 坑 #48)。
 * ⚠️ 实况: PWM_GOLDEN_ALL = 0x7F0222B8 < 2^31 ⇒ 这个掩码对它【是空操作】;
 *    而 PWM_GOLDEN_REG = 0xBAC4EE86 与 PWM_GOLDEN_VEC = 0xB279AFC1 都跨过
 *    2^31 ⇒ 掩码在它们上面【真的生效】。selftest 必须逐条钉住两种情形,
 *    否则将来改黄金值时没人知道出口到底该给什么 (家族坑 #48 原例)。 */
#define PWM_OUT_MASK       0x7FFFFFFFu

/* ============================================================
 * 5. BSP 注入 (平台差异唯一出口)
 * ============================================================ */
typedef uint32_t (*pwm_reg_read_fn)(uint32_t addr, void* user);
typedef int      (*pwm_reg_write_fn)(uint32_t addr, uint32_t val, void* user);

void pwm_bsp_install(pwm_reg_read_fn rd, pwm_reg_write_fn wr, void* user);
void pwm_bsp_uninstall(void);
int  pwm_bsp_installed(void);   /* 1 = 装了 BSP (REAL), 0 = 表驱动 (SIM) */

/* ---- 寄存器读写: 唯一的模块内访问口 ----
 * 未装 BSP 时 read 诚实返回 0, write 返回 PWM_ERR_NOBSP。
 * 【绝不】在这里解引用 pwm_crw 的地址。 */
uint32_t pwm_reg_read(uint32_t addr);
int      pwm_reg_write(uint32_t addr, uint32_t val);

/* ============================================================
 * 6. 档案 (地址表当数据, 黄金值钉死)
 * ============================================================ */
const char* pwm_reg_name(uint32_t idx);          /* 越界返回 "?" */
int         pwm_reg_addr(uint32_t idx, uint32_t* out);  /* PWM_OK / ERR_BOUND */
int         pwm_reg_index(uint32_t addr);        /* 反查, 命中返回 0..6, 否则 -1 */
int         pwm_pin_addr(uint32_t idx, uint32_t* out);
const char* pwm_pin_name(uint32_t idx);

/* 口径全公开, 供外部独立复算 (家族纪律: 黄金值的【算法口径】必须和值一起
 * 交付, 否则明天连"算的是什么"都不知道 —— 坑 #34) */
uint32_t    pwm_fnv32(const void* data, uint32_t n);
uint32_t    pwm_fnv32_feed_byte(uint32_t h, uint8_t b);
/* 四个黄金值的【未掩码】复算出口: selftest 拿它们与硬编码宏比 (坑 #28) */
uint32_t    pwm_fnv32_reg_raw(void);
uint32_t    pwm_fnv32_pin_raw(void);
uint32_t    pwm_fnv32_vec_raw(void);
uint32_t    pwm_golden_raw(void);
/* 公共出口: 已带 PWM_OUT_MASK */
uint32_t    pwm_profile_golden(void);            /* GOLDEN_ALL & MASK */
uint32_t    pwm_golden_reg(void);                /* GOLDEN_REG & MASK (跨 2^31) */
uint32_t    pwm_golden_vec(void);                /* GOLDEN_VEC & MASK (跨 2^31) */

/* ---- 初版两个 inline 的正式实现 (语义照搬, 访问改走 BSP) ---- */
/* 初版: uint32_t v = pwm_crw(0x063800U); return v << 5U;  —— 左移 5 位后返回 */
uint32_t pwm_reg_read_oep_scaled(void);
/* 初版: uint32_t e = pwm_crw(HW_PWM_EMC); pwm_cww(HW_PWM_DDC, e); —— EMC 映射到 DDC */
int     pwm_reg_map_emc_to_ddc(void);

/* ============================================================
 * 5. 模式查询
 * ============================================================ */
uint8_t     pwm_mode(void);
const char* pwm_mode_str(uint8_t mode);   /* 越界返回 "??" */

/* ============================================================
 * 7. 频率与占空比 (纯算术, 无硬件依赖, 六模式逐位一致)
 * ============================================================ */
/* 占空比百分比 -> 16 位计数值, 四舍五入 (pct*65535+50)/100 */
int      pwm_duty_encode(uint32_t pct, uint32_t* out);
/* 16 位计数值 -> 占空比百分比, 反算回 0..100 (用于回读显示) */
int      pwm_duty_decode(uint32_t code, uint32_t* pct);
/* 频率合法性: 0 或 > PWM_FREQ_MAX 判非法 (家族坑 #31: 先校验后使用) */
int      pwm_freq_valid(uint32_t hz);

/* ============================================================
 * 8. 通道状态机
 * ============================================================ */
typedef struct {
    uint32_t ch;        /* 通道号 0..PWM_CH_MAX-1                        */
    uint32_t hz;        /* 频率 (Hz)                                     */
    uint32_t duty_pct;  /* 占空比百分比 0..100                           */
    uint32_t duty_code; /* 占空比计数值 0..65535 (由 duty_pct 派生)      */
    uint32_t running;   /* 0 = 停, 1 = 开                                 */
} pwm_ch_t;

int pwm_ch_init(void);                      /* 复位全部通道; 【不碰 BSP】(坑 #25) */
int pwm_ch_config(uint32_t ch, uint32_t hz, uint32_t pct);
int pwm_ch_start(uint32_t ch);
int pwm_ch_stop(uint32_t ch);
int pwm_ch_get(uint32_t ch, pwm_ch_t* out);
int pwm_ch_count(void);

/* ============================================================
 * 9. 命令通道 (VM OP_HW_PWM_CALL / CLI 共用 —— 不新增 opcode 之外的口子)
 * ============================================================ */
int pwm_cmd(const char* cmd);   /* 返回码, 0 = 成功/就绪 */

/* ============================================================
 * 10. 自检与 CLI
 * ============================================================ */
typedef int (*pwm_puts_fn)(const char* s);
int pwm_selftest(pwm_puts_fn putf);   /* 返回失败计数; putf=NULL 静默 (坑 #4) */
int pwm_cli(int argc, char** argv);   /* 读 argv[2] (坑 #9/#50) */

#ifdef __cplusplus
}
#endif

#endif /* XIAOMO_HW_PWM_H */
