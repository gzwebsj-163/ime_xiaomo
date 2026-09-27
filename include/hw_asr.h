/*
 * xiaomo - 语音识别层 (hw_asr)
 *
 * 双引擎设计:
 *   A. 本地引擎 (hw_asr_local_*): 纯 C 离线命令词识别。
 *      WAV(PCM16, 任意采样率自动重采样到16k单声道) → 分帧加窗 →
 *      FFT 功率谱 → Mel 滤波26带 → DCT-II → 13维MFCC+能量 = 14维帧向量
 *      → 全句均值归一 (CMVN) → 与注册模板做 DTW (弦距离, 滚动数组)。
 *      零外部库依赖, freestanding 友好 (FFT/窗表 static 区)。
 *      词条由 enroll 注入真实音频; 内置拼音命令表用于语义占位。
 *   B. 线上引擎 (hw_asr_online_*): OpenAI 兼容转写协议客户端。
 *      POST {HW_ASR_URL}/audio/transcriptions (multipart: file+model),
 *      解析 {"text": "..."}。传输优先 fork curl(支持https), 无 curl 时
 *      POSIX socket 裸 HTTP POST (仅 http)。后端可指:
 *        SiliconFlow(SenseVoice) / Groq(whisper) / OpenAI(whisper) /
 *        火山方舟(若开通转写) / 自建 whisper.cpp / 本地语音桥 :8800。
 *
 * 安全 (对应 2026-09-13 IDE 审计教训):
 *   - fork curl 的所有参数经 shell 单引号强转义, 词名走白名单校验;
 *   - 音频一律落 /tmp 固定名, 不从外部拼路径。
 *
 * 跨模式: 本地引擎纯无符号/浮点运算, C11/C++17 双编译一致;
 *         在线引擎宿主可用 (ESP 端编译仅本地引擎, 网络由固件层自理)。
 */
#ifndef XIAOMO_HW_ASR_H
#define XIAOMO_HW_ASR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 1. 常量与限制
 * ============================================================ */

#define HW_ASR_SR          16000   /* 引擎内部采样率 */
#define HW_ASR_FRAME       400     /* 帧长 25ms @16k */
#define HW_ASR_HOP         160     /* 帧移 10ms @16k */
#define HW_ASR_FFT         512     /* FFT 点数 (>= FRAME) */
#define HW_ASR_MEL_BANDS   26      /* Mel 三角滤波器数 */
#define HW_ASR_NCEP        13      /* MFCC 维数 */
#define HW_ASR_DIM         14      /* 帧向量维数 (13 MFCC + log能量) */
#define HW_ASR_MAX_WORDS   32      /* 最多词条数 */
#define HW_ASR_MAX_TPL     4       /* 每词最多模板数 */
#define HW_ASR_MAX_FRAMES  600     /* 单模板最多帧数 (~6s) */
#define HW_ASR_MAX_SEC     30      /* 在线引擎音频上限 (秒) */
#define HW_ASR_TEXT_CAP    512     /* 识别文本缓冲 */

/* 识别结果结构 */
typedef struct hw_asr_result {
    char     word[64];     /* 命中的词条 (本地) */
    double   score;        /* DTW 距离 (越小越好) */
    double   second;       /* 次佳距离 (供阈值参考) */
    int      tpl_used;     /* 命中的模板序号 */
} hw_asr_result_t;

/* 在线识别结果 */
typedef struct hw_asr_online_result {
    int      http_code;    /* 0 = 传输层失败; 200 = 成功 */
    char     text[HW_ASR_TEXT_CAP];   /* 转写文本 */
    char     via[16];      /* "curl" / "socket" */
    int      ms;           /* 耗时毫秒 */
} hw_asr_online_result_t;

/* ============================================================
 * 2. WAV 读取 (PCM16, 自动重采样到 16k 单声道)
 * ============================================================ */

/* 读 wav → 内部格式。成功返回帧数, 失败返回负数
 * (-1 打不开/-2 非RIFF/-3 非PCM/-4 空音频)。*out 由内部分配,
 * 调用方 free(*out)。 */
int hw_asr_load_wav(const char* path, float** out, int* out_n);

/* ============================================================
 * 3. 本地引擎
 * ============================================================ */

/* 帧向量化: 音频 → MFCC 帧序列 (返回帧数; frames 由内部分配,
 * 每帧 HW_ASR_DIM 个 float, 调用方 free(*frames)) */
int hw_asr_mfcc(const float* pcm, int n, float*** frames, int* out_n);

/* DTW 距离 (弦距离, 滚动数组, Sakoe-Chiba 带 ±60) */
double hw_asr_dtw(float** a, int na, float** b, int nb);

/* 注册: 词 → 追加一条模板 (.asr/ 持久化)。
 * 已有 4 条时挤掉最旧一条。返回该词当前模板数, 失败 -1。 */
int hw_asr_enroll(const char* word, const char* wav_path);

/* 本地识别: 返回命中词在表中的序号 (>=0), 无词注册返回 -1,
 * 音频非法 -2。*res 填词名/距离。 */
int hw_asr_match(const char* wav_path, hw_asr_result_t* res);

/* 当前词表: 填 words[i] (词名) 与 counts[i] (模板数), 返回词数 */
int hw_asr_list(char words[][64], int counts[], int max);

/* 清空全部模板 (删 .asr/) */
void hw_asr_reset(void);

/* ============================================================
 * 4. 线上引擎
 * ============================================================ */

/* 配置 (env 优先, 兜底内置):
 *   HW_ASR_URL   默认 $OPENAI_API_BASE (方舟/兼容网关), 无则
 *                https://api.siliconflow.cn/v1
 *   HW_ASR_KEY   默认 $OPENAI_API_KEY
 *   HW_ASR_MODEL 默认 FunAudioLLM/SenseVoiceSmall (方舟协议填
 *                doubao-speech 等, 按后端定)
 * 返回 0 后 *url、*key、*model 指向静态串 (勿 free)。 */
int hw_asr_online_config(const char** url, const char** key,
                         const char** model);

/* 在线识别: wav → 云转写。*res 填 http/文本/通道/耗时。
 * 返回 0 = 拿到 200 响应 (文本可能为空), -1 = 传输/HTTP 失败。 */
int hw_asr_online(const char* wav_path, hw_asr_online_result_t* res);

/* 从 {"text":"..."} 响应体提取 text (处理 \" \\\\ 等转义)。
 * 成功回填 out 返回 0。 */
int hw_asr_parse_text(const char* json, char* out, int cap);

/* ============================================================
 * 5. 自检与 CLI
 * ============================================================ */

/* 自检输出回调 (NULL = 静默) */
typedef int (*hw_asr_puts_fn)(const char* s);

/* 自检: FFT能量/MFCC形状/DTW自距离/合成chirp注册识别互斥/
 * WAV往返/在线协议 mock 回环 (起本地 /v1/audio/transcriptions)。
 * 返回失败项数 (0 = 全绿)。 */
int hw_asr_selftest(hw_asr_puts_fn putf);

/* CLI: ./xiaomo asr demo|list|enroll <word> <wav>|match <wav>|
 *       online <wav>|reset */
int hw_asr_cli(int argc, char** argv);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* XIAOMO_HW_ASR_H */
