/* ============================================================================
 * xiaomo - 麦克风采集层 (hw_mic) 【第 14 位 hw 家族成员】
 * ============================================================================
 * 定位: 【只做本地采样与本地信号处理】。本模块不联网、不上传、不调云端。
 *       它是「本地麦克风 → 板上转文字 → 板上 AI 推理」这条全本��链路的第一环,
 *       产出统一规格的 PCM, 交给 hw_asr (板上转文字) 与 infer (板上推理)。
 *
 * 【为什么单独立一个模块, 而不在 hw_asr 里加几个函数】
 *   hw_asr 现状是【桌面形态】: 数据来源 = fopen("*.wav") + 模板库 = malloc
 *   二级指针 + 文件系统。ESP32-C3 实测约束: SRAM 400KB / flash 2MB / 无 PSRAM,
 *   且无文件系统 => 桌面形态在 C3 上结构性跑不起来(详见头文件下的数字表)。
 *   而"采集"与"识别"是两种关注点: 采集要的是【实时性 + 定点 + 零分配】,
 *   识别要的是【模型精度 + 浮点 + 可注册词表】。混在一起两边都做不好。
 *   ⇒ 采集独立成层(L1 信号 + L2 档案), 识别另立 C3 驻留版。
 *
 * 【本模块只保证"可自证", 不假装知道的部分 —— 明确声明】
 *   1. 【位宽归一未实现】。档案里记了 bits(24/32/16), 但本版只处理 16bit PCM。
 *      INMP441 常见驱动出 32bit(左移 14 位对齐)或 24bit, 真机上由 BSP 负责
 *      先右移到 16bit 再交给本层。模块【不去编造】没验证过的移位量。
 *   2. 【声道抽取未实现】。档案记了 channels, 但本版要求 BSP 已给单声道。
 *      立体声下混同样留给 BSP。理由同上: 没有手册核对过偏移, 不猜。
 *   3. 【不自带 AGC/降噪】。MAX9814 的 AGC 由芯片硬件做, 档案只做标记。
 *
 * 【为什么重采样必须是定点的】
 *   freestanding 下 libm 不可用(坑 2 家族: -ffreestanding 不带浮点数学),
 *   而浮点重采样在 xtensa/riscv 与 x86 上的舍入可能差 1 LSB => 六模式
 *   逐位一致直接归零。⇒ 全链路纯 int32: 位置用整数除, 小数用 16.16 定点。
 *
 * 【为什么是流式而不是批式】
 *   批式重采样要求整个源音频在内存里: 1 秒 48kHz int16 = 96KB, C3 装不下。
 *   且批式按块调用会在每块边界丢 1 个样本(取不到 s[pos+1]) —— 这是静默的
 *   音频损坏, 越界越少越不容易发现。⇒ 本模块做【流式重采样器】, 内存 O(1),
 *   跨块逐位等于一次性喂入(selftest [3] 专门锁这条等价性)。
 *
 * 【档案锁: 换麦子 = 换一条档案, 别改判据代码】(同 panel_lcd / hw_pin 范式)
 *   内置 4 条档案覆盖: INMP441 原生 48k / INMP441 直出 16k / MAX9814 走 ADC /
 *   模拟表驱动 SIM。脚号/时钟/通道选择由真机 BSP 自己配, 本模块只管格式。
 * ==========================================================================*/
#ifndef XIAOMO_HW_MIC_H
#define XIAOMO_HW_MIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 模式 (六模式, 家族规约) ============================ */
#define HW_MIC_MODE_HOST     0
#define HW_MIC_MODE_LINUX    1
#define HW_MIC_MODE_KELL     2
#define HW_MIC_MODE_ESP32    3
#define HW_MIC_MODE_ESP8266  4
#define HW_MIC_MODE_TEST     5

#if defined(HW_MIC_MODE_OVERRIDE)
  #define HW_MIC_MODE ((int)HW_MIC_MODE_OVERRIDE)
#elif defined(HW_MIC_KELL)
  #define HW_MIC_MODE HW_MIC_MODE_KELL
#elif defined(ESP8266)
  #define HW_MIC_MODE HW_MIC_MODE_ESP8266
#elif defined(ESP_PLATFORM) || defined(CONFIG_IDF_TARGET_ESP32) || \
      defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32S3)
  #define HW_MIC_MODE HW_MIC_MODE_ESP32
#elif defined(__linux__)
  #define HW_MIC_MODE HW_MIC_MODE_LINUX
#elif defined(__APPLE__) || defined(_WIN32) || defined(__unix__)
  #define HW_MIC_MODE HW_MIC_MODE_HOST
#else
  #define HW_MIC_MODE HW_MIC_MODE_TEST
#endif

int         hw_mic_mode(void);
const char* hw_mic_mode_name(void);

/* ============================ 引擎规格 (与 hw_asr 对齐) ============================ */
/* ⚠️ 这两个常量与 hw_asr.h 的 HW_ASR_SR / HW_ASR_FRAME 是【同一份规格的两处声明】。
 *   改这里必须同步改那里, 否则采集端和识别端会各说各话(家族铁律: 一份规格
 *   一处真相源)。selftest [7] 只在本模块内断言, 跨模块一致性由
 *   tools/mic_matrix.sh 的向拍项单独验。 */
#define HW_MIC_ENGINE_SR     16000   /* 引擎采样率 (与 HW_ASR_SR 同源) */
#define HW_MIC_VAD_FRAME     400     /* VAD 帧长 25ms (与 HW_ASR_FRAME 同源) */

/* ============================ L2 档案层 ============================ */
#define HW_MIC_PROFILE_MAX   4

typedef struct {
    const char* name;
    uint32_t    sr_hz;      /* 源采样率 */
    uint16_t    bits;       /* 源位宽 (仅记录; 归一由 BSP 负责) */
    uint8_t     channels;   /* 声道数 (仅记录; 下混由 BSP 负责) */
    uint8_t     left_sel;   /* INMP441 L/R 脚选择: 1=左声道 0=右声道 */
    uint8_t     agc;        /* 芯片自带 AGC: MAX9814=1 */
} hw_mic_profile_t;

extern const hw_mic_profile_t hw_mic_profiles[HW_MIC_PROFILE_MAX];
int hw_mic_profile_count(void);
const hw_mic_profile_t* hw_mic_profile_get(void);
/* 载入档案 (换麦子入口)。成功 0, 失败 -1。未载入时默认第 0 条。 */
int hw_mic_profile_load(const char* name);
int hw_mic_profile_load_idx(int idx);

/* ============================ L1 BSP 层 ============================
 * 结构性 NULL 分水岭 (家族坑 21): 装了 BSP = REAL(真 I2S), 没装 = SIM(时间域
 * 波形表)。判据是【单个指针】, 不是缓存状态, 不是上层自觉。
 * ⚠️ init() 绝不得抹掉已装 BSP (家族坑 25, hw_dc/hw_pin 两次亲历的头号假成功)。 */
typedef struct {
    /* 读 n 个源样本。返回实际读到的样本数; <0 = 硬件失败(诚实失败, 不返回 0 冒充成功) */
    int (*read)(int16_t* buf, int n, uint32_t sr_hz);
    /* 采集使能。返回 0 = 成功。NULL = 不需要(麦克风常开) */
    int (*enable)(int on);
} hw_mic_bsp_t;

void hw_mic_bsp_install(const hw_mic_bsp_t* bsp);
int  hw_mic_is_sim(void);          /* 1 = SIM, 0 = REAL */

/* ============================ 信号处理 ============================ */

/* 定点重采样器 (流式, 内存 O(1)) */
typedef struct {
    uint32_t sr_in;
    uint32_t sr_out;
    int32_t  prev;        /* 上一个源样本 s[j-1] */
    int32_t  cur;         /* 当前源样本 s[j] */
    int      have_prev;
    int64_t  next_out;    /* 下一个要产出的输出下标 */
    int64_t  total_in;    /* 累计读入的源样本数 (flush 需要它定右边界) */
    int64_t  dropped;     /* 因输出满而【被丢弃】的源样本数 (背压时诚实记账) */
    int      overflow;    /* 1 = 曾经 sr_in < sr_out (超出本版支持范围) */
} hw_mic_rs_t;

int hw_mic_rs_init(hw_mic_rs_t* st, uint32_t sr_in, uint32_t sr_out);
/* 喂入 n 个源样本, 最多产出 cap 个输出。*out_n = 实际产出数。0 = 成功。
 * ⚠️ 背压契约(初版在这里报 -2, 结果"恰好填满"也被当溢出, 采集直接失败):
 *   输出填满时【干净返回 0】, *out_n == cap, 并把本块剩余源样本记进 st->dropped。
 *   调用方必须先把 out 排空再继续喂, 否则会丢音频。
 *   -2 只在 cap == 0(根本没地方放输出) 时返回。 */
int hw_mic_rs_feed(hw_mic_rs_t* st, const int16_t* in, int n,
                   int16_t* out, int cap, int* out_n);
/* 收尾: 产出最后几个(取不到 s[pos+1] 的)。0 = 成功 */
int hw_mic_rs_flush(hw_mic_rs_t* st, int16_t* out, int cap, int* out_n);

/* 统计: 峰值/直流/均方根/过零率。全整数(整数 sqrt), 无浮点 */
typedef struct {
    int n;
    int peak;
    int dc;
    int rms;
    int zcr;
} hw_mic_stats_t;

int hw_mic_stats(const int16_t* s, int n, hw_mic_stats_t* out);
/* 两个样本序列的最大绝对差 (重采样保真度判据用) */
int hw_mic_maxdiff(const int16_t* a, const int16_t* b, int n);

/* VAD: 帧级能量判决 + 连续帧起判。返回语音帧总数, <0 = 参数错 */
int hw_mic_vad_run(const int16_t* s16, int n, int thr, int run_n,
                   int* out_first_frame, int* out_speech_frames);

/* ============================ 采集状态机 ============================ */
#define HW_MIC_ST_IDLE    0
#define HW_MIC_ST_ARMED   1
#define HW_MIC_ST_CAPTURE 2
#define HW_MIC_ST_DONE    3
#define HW_MIC_ST_ERR     4

typedef struct {
    int      state;
    uint32_t sample_rate;    /* 当前档案源采样率 */
    uint32_t engine_sr;      /* 引擎采样率 */
    int64_t  captured;       /* 已产出引擎样本数 */
    int64_t  src_read;       /* 已读源样本数 */
    int      last_err;       /* 首个错误码, 0 = 无 */
    int      vad_first;      /* 首个【确认】语音帧 */
    int      vad_frames;     /* 语音帧总数 */
    int      level;          /* 归一电平 0..100 (由 rms 映射, 供 UI 画麦) */
} hw_mic_status_t;

/* 只复位模块自身状态, 【不碰 BSP】。0 = 成功 */
int hw_mic_init(void);
int hw_mic_status(hw_mic_status_t* out);
int hw_mic_state(void);

/* 采一段语音: 从 BSP 读源样本, 流式重采样到 engine_sr, 填 out。
 * max_out = out 容量(引擎样本数)。返回产出数, <0 = 错误。
 * ⚠️ 本函数是【非阻塞单次】语义: 它读满 max_out 或 BSP 返回 <n 即停。
 *   VAD 驱动的"说完自动停"由 hw_mic_capture_vad() 提供。
 * ⚠️ timeout_ms 不是墙上时钟(模块不依赖任何时钟, 保证 freestanding 与六模式
 *   一致), 它被换算成【读取预算】= timeout_ms*sr/1000 个源样本, 于是
 *   "BSP 一直返回满块"这一种挂死形态也能被确定性截断。 */
int hw_mic_capture(int16_t* out, int max_out, int timeout_ms);

/* VAD 驱动的采集: 边采边判, 连续 run_n 帧语音后, 再留 tail 帧就停。
 * 返回产出数, <0 = 错误。out_first = 语音起点(引擎样本下标, -1 = 没检出)。 */
int hw_mic_capture_vad(int16_t* out, int max_out, int thr, int run_n,
                       int tail_frames, int timeout_ms, int* out_first);

/* ============================ 黄金值 (由 tools/mic_golden_calc.py 独立算出) ===
 * ⚠️ 录这些值的那一次不许再改源码(要改就重算重录)。算法口径:
 *   波形定义在【时间域】: phase = i*48000/sr_in, 750Hz 三角波, 语音段
 *   phase∈[14400,38400) (0.3s~0.8s), 幅度 ±6000。
 *   因为定义在时间域, 48k 与 32k 采到的是【同一段物理声音】, 重采样到 16k
 *   后应当逐位相同 —— 这给了重采样器一个已知的标准答案, 而非自洽数字。
 *   (v1 把波形定义在样本序号域, 两条路径本就应当不同, 对拍无从判对错;
 *    且 FNV 只取前 256 样本落在静音区 => 两条路径 FNV 相同 =
 *    不具区分力的信号, 差点被当成"路径没被走到"。) */
#define HW_MIC_GOLDEN_RS_FNV     0x06A985A5UL   /* 16k 输出 16000 样本低 8 位的 FNV-1a32 */
#define HW_MIC_GOLDEN_RS_PEAK    6000
#define HW_MIC_GOLDEN_RS_DC      0
#define HW_MIC_GOLDEN_RS_RMS     2451
#define HW_MIC_GOLDEN_RS_ZCR     752
#define HW_MIC_GOLDEN_VAD_FIRST  12            /* 首个确认帧 (0.3s = 帧 12) */
#define HW_MIC_GOLDEN_VAD_FRAMES 18
#define HW_MIC_GOLDEN_VAD_TOTAL  40
#define HW_MIC_GOLDEN_VAD_FNV    0x3AB449D9UL  /* 40 个判决位的 FNV-1a32 */
#define HW_MIC_GOLDEN_PROF_FNV   0x8ECFC926UL  /* 4 条档案表的 FNV-1a32 */
#define HW_MIC_GOLDEN_STAT_FNV   0x77010BE0UL  /* 统计字段合成锚 */

/* ============================ 命令 / 自检 / CLI ============================ */
int hw_mic_cmd(int argc, char** argv);

typedef int (*hw_mic_puts_fn)(const char* s);
int hw_mic_selftest(hw_mic_puts_fn putf);

int hw_mic_cli(int argc, char** argv);

#ifdef __cplusplus
}
#endif
#endif /* XIAOMO_HW_MIC_H */
