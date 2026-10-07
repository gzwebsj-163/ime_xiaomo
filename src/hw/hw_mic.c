/* ============================================================================
 * xiaomo - 麦克风采集层 (hw_mic) 实现
 * ----------------------------------------------------------------------------
 * 纪律:
 *   - 零 malloc / 零浮点 / 零文件 IO / 零 pthread  => freestanding 可编译,
 *     六模式行为一致 (freestanding 下 libm 不可用, 一切用定点)
 *   - 平台差异全走 BSP 注入, 分水岭是【结构性 NULL】而非上层自觉
 *   - init() 只复位模块自身状态, 【绝不动 BSP】(家族坑 25)
 *   - >512B 的局部数组一律 static(家族坑 12: 真机栈溢出宿主永远测不出)
 *   - 局部变量名避开 C++ 关键字(家族坑 19: C11 过了不等于 C++ 过了)
 * ==========================================================================*/
#include "hw_mic.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================ 0. 全局状态 (前置声明, 各节共用) ============================
 * ⚠️ >512B 的缓冲一律 static(家族坑 12: 真机栈溢出, 宿主 8MB 栈永远测不出) */
static int      g_prof_idx = 0;
static hw_mic_bsp_t   g_bsp;
static int            g_bsp_set = 0;
static hw_mic_status_t g_st;
static hw_mic_rs_t    g_rs;

#define HW_MIC_SRC_CHUNK     256
#define HW_MIC_CAPTURE_MAX   16000      /* 1.0s @16k = 32KB */
static int16_t g_src[HW_MIC_SRC_CHUNK];
static int16_t g_cap[HW_MIC_CAPTURE_MAX];

/* ============================ 1. 模式 ============================ */
int hw_mic_mode(void) { return HW_MIC_MODE; }

const char* hw_mic_mode_name(void)
{
    switch (HW_MIC_MODE) {
    case HW_MIC_MODE_HOST:    return "HOST";
    case HW_MIC_MODE_LINUX:   return "LINUX";
    case HW_MIC_MODE_KELL:    return "KELL";
    case HW_MIC_MODE_ESP32:   return "ESP32";
    case HW_MIC_MODE_ESP8266: return "ESP8266";
    default:                  return "TEST";
    }
}

/* ============================ 2. L2 档案表 ============================ */
/* ⚠️ 这 4 条是【对拍锚点】, 不是"本板档案"。真机 BSP 用
 *   hw_mic_profile_load() 注入现场档案, 别改这张表
 *   (家族坑 21/26: 内置档案写死某型号 = 上真机的必要动作, 不是死锁)。 */
const hw_mic_profile_t hw_mic_profiles[HW_MIC_PROFILE_MAX] = {
    /* name           sr_hz  bits ch left agc */
    { "inmp441-48k", 48000, 32, 2, 1, 0 },  /* INMP441 原生: 左声道, 48k */
    { "inmp441-16k", 16000, 32, 1, 1, 0 },  /* INMP441 直出 16k(免重采样) */
    { "max9814-adc", 16000, 12, 1, 0, 1 },  /* MAX9814 走 ADC, 芯片自带 AGC */
    { "sim-table",   16000, 16, 1, 0, 0 }   /* 模拟表驱动 SIM */
};

int hw_mic_profile_count(void) { return HW_MIC_PROFILE_MAX; }

const hw_mic_profile_t* hw_mic_profile_get(void)
{
    if (g_prof_idx < 0 || g_prof_idx >= HW_MIC_PROFILE_MAX) g_prof_idx = 0;
    return &hw_mic_profiles[g_prof_idx];
}

int hw_mic_profile_load(const char* name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < HW_MIC_PROFILE_MAX; i++) {
        if (strcmp(hw_mic_profiles[i].name, name) == 0) return hw_mic_profile_load_idx(i);
    }
    return -1;
}

int hw_mic_profile_load_idx(int idx)
{
    if (idx < 0 || idx >= HW_MIC_PROFILE_MAX) return -1;
    g_prof_idx = idx;
    return 0;
}

/* ============================ 3. BSP 层 ============================ */
void hw_mic_bsp_install(const hw_mic_bsp_t* bsp)
{
    if (bsp && bsp->read) {
        memcpy(&g_bsp, bsp, sizeof(g_bsp));   /* 拷进静态体, 不存指针(悬垂) */
        g_bsp_set = 1;
    } else {
        memset(&g_bsp, 0, sizeof(g_bsp));
        g_bsp_set = 0;
    }
}

/* 结构性分水岭: 判【单个指针】, 不用缓存状态(同 hw_pin.c:615 形态) */
int hw_mic_is_sim(void) { return (g_bsp.read == 0) ? 1 : 0; }

/* ============================ 4. 全局状态 ============================ */

/* ============================ 5. SIM 源 (确定性, 全整数) ============================
 * 波形定义在【时间域】: phase = i*48000/sr_in
 *   ⇒ 同一段物理声音, 48k 采和 32k 采, 重采样到 16k 后应当逐位相同。
 * 这给了重采样器一个【已知的标准答案】, 而不是一个只能自证的自洽数字。
 * 幅度 ±6000, 语音段 phase ∈ [14400, 38400) = 0.3s ~ 0.8s, 750Hz 三角波。
 * (第一版曾把波形定义在样本序号域 ⇒ 48k/32k 的三角波频率不同 ⇒ 两条路径
 *  本就应当不同, 对拍无从判对错。教训: 测试向量必须让"正确答案"存在。) */
#define HW_MIC_SIM_AMP   12000
#define HW_MIC_SIM_PH_LO 14400
#define HW_MIC_SIM_PH_HI 38400

static int32_t sim_sample(int64_t i, uint32_t sr_in)
{
    int32_t phase = (int32_t)(((int64_t)i * 48000) / (int64_t)sr_in);
    int32_t tri   = phase % 64;
    int32_t env;
    if (tri >= 32) tri = 64 - tri;
    env = (phase >= HW_MIC_SIM_PH_LO && phase < HW_MIC_SIM_PH_HI) ? HW_MIC_SIM_AMP : 0;
    /* 两项均非负 => 整数除法无负数歧义, 与 Python 的 // 逐位一致 */
    return (tri * env) / 32 - env / 2;
}

/* 填 n 个 SIM 源样本 (i 从 base 开始, 便于分块喂) */
static void sim_fill(int16_t* buf, int n, int64_t base, uint32_t sr_in)
{
    int i;
    for (i = 0; i < n; i++) buf[i] = (int16_t)sim_sample(base + i, sr_in);
}

/* ============================ 6. 定点重采样器 (流式, 内存 O(1)) ============================
 * 口径与 tools/mic_golden_calc.py 完全一致:
 *   num  = next_out * sr_in
 *   pos  = num / sr_out
 *   frac = (num % sr_out) << 16 / sr_out
 *   y    = s[pos] + ((s[pos+1] - s[pos]) * frac >> 16)
 * 负数右移: C 的算术右移 = floor 语义, 与 Python 的 >> 一致 (已实测)。
 * 只支持 sr_in >= sr_out (降采样/等采样), 这覆盖全部真实场景
 * (48k/32k/16k -> 16k)。上采样需要保存 >2 个历史样本, 本版【不做】,
 * 强行传入只置 overflow 标志并在 feed 时报 -3 (诚实失败, 不静默产出垃圾)。 */
int hw_mic_rs_init(hw_mic_rs_t* st, uint32_t sr_in, uint32_t sr_out)
{
    if (!st || sr_in == 0 || sr_out == 0) return -1;
    memset(st, 0, sizeof(*st));
    st->sr_in  = sr_in;
    st->sr_out = sr_out;
    if (sr_in < sr_out) { st->overflow = 1; return -3; }
    return 0;
}

int hw_mic_rs_feed(hw_mic_rs_t* st, const int16_t* in, int n,
                   int16_t* out, int cap, int* out_n)
{
    int j, cnt = 0;
    if (!st || !in || !out || n < 0 || cap < 0) return -1;
    if (st->overflow) return -3;
    if (cap == 0) return -2;          /* 没有任何地方可放输出 = 调用方的错 */
    for (j = 0; j < n; j++) {
        /* ⚠️⚠️ 必须用【累计输入下标】, 不是本次调用内的 j。
         *   next_out 是跨调用累计的, 若拿 per-call 的 j 去比 pos, 第二个分块
         *   起来 pos 已经跑远, 永远等不到 s[pos+1] => 重采样器停摆 =>
         *   产出数不增长 => 上层 while 死等。症状是"挂死"而不是"数据错",
         *   批式测试永远发现不了(坑 30: 先把超时变成可解释信号)。 */
        int64_t jabs = st->total_in + (int64_t)j;
        st->cur = in[j];
        if (st->have_prev) {
            for (;;) {
                int64_t num = st->next_out * (int64_t)st->sr_in;
                int64_t pos = num / (int64_t)st->sr_out;
                int32_t frac, d;
                if (pos + 1 > jabs) break;            /* 还取不到 s[pos+1] */
                if (cnt >= cap) {
                    /* 背压: 输出满了是【正常流控】, 不是错误 —— 干净返回,
                     * 并诚实记下这一块里被丢弃的源样本数。调用方必须先把
                     * out 排空再继续喂, 否则丢音频(初版在这里报 -2, 结果
                     * "恰好填满"也被当溢出 => 采集整体失败, 见坑 31/32)。 */
                    st->dropped += (n - j);
                    if (out_n) *out_n = cnt;
                    return 0;
                }
                frac = (int32_t)((((num % (int64_t)st->sr_out)) << 16) / (int64_t)st->sr_out);
                d = st->cur - st->prev;
                out[cnt++] = (int16_t)(st->prev + (int32_t)(((int64_t)d * (int64_t)frac) >> 16));
                st->next_out++;
            }
        }
        st->prev = st->cur;
        st->have_prev = 1;
    }
    st->total_in += n;
    if (out_n) *out_n = cnt;
    return 0;
}

int hw_mic_rs_flush(hw_mic_rs_t* st, int16_t* out, int cap, int* out_n)
{
    int cnt = 0;
    int64_t j = st ? st->total_in : 0;
    if (!st || !out || cap < 0) return -1;
    if (st->overflow) return -3;
    if (cap == 0) return -2;
    for (;;) {
        int64_t num = st->next_out * (int64_t)st->sr_in;
        int64_t pos = num / (int64_t)st->sr_out;
        int32_t frac, d;
        if (pos + 1 >= j) break;                    /* 右边取不到 s[pos+1] */
        if (cnt >= cap) { if (out_n) *out_n = cnt; return 0; }  /* 背压 */
        frac = (int32_t)((((num % (int64_t)st->sr_out)) << 16) / (int64_t)st->sr_out);
        d = st->cur - st->prev;
        out[cnt++] = (int16_t)(st->prev + (int32_t)(((int64_t)d * (int64_t)frac) >> 16));
        st->next_out++;
    }
    if (out_n) *out_n = cnt;
    return 0;
}

/* ============================ 7. 统计 (整数 sqrt, 无浮点) ============================ */
static uint64_t isqrt_floor_u64(uint64_t v)
{
    uint64_t r = 0, bit = 1;
    if (v == 0) return 0;
    while ((bit << 2) <= v) bit <<= 2;   /* 起点: 不超过 v 的最大 4 的幂 */
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else              { r >>= 1; }
        bit >>= 2;
    }
    return r;
}

int hw_mic_stats(const int16_t* s, int n, hw_mic_stats_t* out)
{
    int i, peak = 0;
    int64_t acc = 0;
    uint64_t sq = 0;
    int zcr = 0;
    int16_t prev;
    if (!s || !out || n <= 0) { if (out) memset(out, 0, sizeof(*out)); return -1; }
    for (i = 0; i < n; i++) {
        int32_t v = s[i];
        int32_t a = v < 0 ? -v : v;
        if (a > peak) peak = a;
        acc += v;
        sq  += (uint64_t)((int64_t)v * (int64_t)v);
    }
    prev = s[0];
    for (i = 1; i < n; i++) {
        if ((s[i] >= 0) != (prev >= 0)) zcr++;
        prev = s[i];
    }
    out->n    = n;
    out->peak = peak;
    out->dc   = (int)(acc >= 0 ? (acc / n) : -((-acc) / n));   /* 向零取整 */
    out->rms  = (int)isqrt_floor_u64((uint64_t)(sq / (uint64_t)n));
    out->zcr  = zcr;
    return 0;
}

int hw_mic_maxdiff(const int16_t* a, const int16_t* b, int n)
{
    int i, m = 0;
    if (!a || !b || n <= 0) return -1;
    for (i = 0; i < n; i++) {
        int d = (int)a[i] - (int)b[i];
        if (d < 0) d = -d;
        if (d > m) m = d;
    }
    return m;
}

/* ============================ 8. VAD ============================
 * 帧级 RMS 判决 + 连续帧起判。first = 首个【确认】帧 (= 连续 run_n 帧的
 * 第一帧), 不是首个命中帧 —— 这两个语义不同, 黄金值锁的是【确认帧】。 */
int hw_mic_vad_run(const int16_t* s16, int n, int thr, int run_n,
                   int* out_first, int* out_speech)
{
    int k, nframes, run = 0, first = -1, speech = 0;
    if (!s16 || n <= 0 || run_n <= 0) return -1;
    nframes = n / HW_MIC_VAD_FRAME;
    for (k = 0; k < nframes; k++) {
        hw_mic_stats_t st;
        int hit;
        if (hw_mic_stats(s16 + k * HW_MIC_VAD_FRAME, HW_MIC_VAD_FRAME, &st) != 0) return -1;
        hit = (st.rms >= thr) ? 1 : 0;
        if (hit) {
            run++;
            if (run >= run_n) {
                speech++;
                if (first < 0) first = k - run_n + 1;
            }
        } else {
            run = 0;
        }
    }
    if (out_first)  *out_first  = first;
    if (out_speech) *out_speech = speech;
    return nframes;
}

/* ============================ 9. 状态机 / 采集 ============================
 * 契约(家族坑 25): init() 只复位模块自身状态, 【不碰 BSP】。 */
int hw_mic_init(void)
{
    memset(&g_st, 0, sizeof(g_st));
    g_st.state       = HW_MIC_ST_IDLE;
    g_st.sample_rate = hw_mic_profile_get()->sr_hz;
    g_st.engine_sr   = HW_MIC_ENGINE_SR;
    g_st.vad_first   = -1;
    g_st.last_err    = 0;
    /* g_bsp 故意不动 —— 装好的真机 BSP 必须活过 init */
    return 0;
}

int hw_mic_state(void) { return g_st.state; }

int hw_mic_status(hw_mic_status_t* out)
{
    if (!out) return -1;
    *out = g_st;
    out->sample_rate = hw_mic_profile_get()->sr_hz;
    return 0;
}

/* 由 rms 映射到 0..100 的电平, 供 UI 画麦 */
static int level_of(int rms)
{
    if (rms <= 0) return 0;
    if (rms >= 6000) return 100;
    return rms * 100 / 6000;
}

int hw_mic_capture(int16_t* out, int max_out, int timeout_ms)
{
    const hw_mic_profile_t* p;
    uint32_t sr_in;
    int64_t budget, produced = 0;
    int rc;

    if (!out || max_out <= 0) { g_st.last_err = -1; g_st.state = HW_MIC_ST_ERR; return -1; }
    p    = hw_mic_profile_get();
    sr_in = p->sr_hz;
    g_st.state = HW_MIC_ST_ARMED;

    if (hw_mic_rs_init(&g_rs, sr_in, HW_MIC_ENGINE_SR) != 0) {
        g_st.last_err = -3; g_st.state = HW_MIC_ST_ERR; return -3;
    }
    if (g_bsp.enable && g_bsp.enable(1) != 0) {
        g_st.last_err = -4; g_st.state = HW_MIC_ST_ERR; return -4;
    }

    /* timeout_ms -> 读取预算(源样本数), 不依赖墙上时钟 => 六模式一致 */
    budget = ((int64_t)(timeout_ms > 0 ? timeout_ms : 0) * (int64_t)sr_in) / 1000;
    if (budget < (int64_t)max_out * 3) budget = (int64_t)max_out * 3;

    {
    int64_t ratio = (int64_t)sr_in / (int64_t)HW_MIC_ENGINE_SR;   /* 48k->16k = 3 */
    while (produced < (int64_t)max_out && g_rs.total_in < budget) {
        int got, made = 0, need = HW_MIC_SRC_CHUNK;
        /* 按剩余容量反算该读多少源样本 (+2 留 s[pos+1] 的前瞻),
         * 否则块大小与剩余容量不匹配 => feed 撞容量报 -2 => 采集整体失败。
         * 契约: -2 只应作为"调用方把 cap 算小了"的守卫, 正常操作不该触发。 */
        {
            int64_t room = (int64_t)max_out - produced;
            int64_t want = room * ratio + 2;
            if (want < HW_MIC_SRC_CHUNK) need = (int)want;
        }
        if (need < 1) need = 1;
        if (g_bsp.read) {
            got = g_bsp.read(g_src, need, sr_in);
            if (got < 0) { g_st.last_err = got; g_st.state = HW_MIC_ST_ERR; return got; }
            if (got == 0) break;             /* BSP 说没有更多了 */
        } else {
            sim_fill(g_src, need, g_rs.total_in, sr_in);
            got = need;
        }
        g_st.src_read += got;
        rc = hw_mic_rs_feed(&g_rs, g_src, got, out + produced,
                            max_out - (int)produced, &made);
        /* 先累加再判错 (坑 31): -2 时 made 仍有效, 先 return 会丢产出 */
        produced += made;
        if (rc != 0) { g_st.last_err = rc; g_st.state = HW_MIC_ST_ERR; return rc; }
        if (g_bsp.read && got < need) break;   /* BSP 供不满 = 采不动了 */
    }
    /* 只在还剩容量时收尾: 已经采满时 max_out-produced == 0, 而 flush 在
     * cap==0 时按契约返回 -2 —— 会让"已采满"这个成功状态被报成失败。 */
    if (produced < (int64_t)max_out) {
        int made = 0;
        rc = hw_mic_rs_flush(&g_rs, out + produced, max_out - (int)produced, &made);
        if (rc != 0) { g_st.last_err = rc; g_st.state = HW_MIC_ST_ERR; return rc; }
        produced += made;
    }
    }
    g_st.captured = produced;
    g_st.state    = (produced > 0) ? HW_MIC_ST_DONE : HW_MIC_ST_ERR;
    return (int)produced;
}

int hw_mic_capture_vad(int16_t* out, int max_out, int thr, int run_n,
                       int tail_frames, int timeout_ms, int* out_first)
{
    /* 边采边判: 分块推进, 逐帧判决, 确认语音后再留 tail 帧就停。
     * 停止条件 = (连续 run_n 帧语音) 且 (其后连续 tail_frames 帧静音)。
     * 这样"说完自动停"不依赖任何时钟, 也不需要把整段音频留在内存里。 */
    const hw_mic_profile_t* p = hw_mic_profile_get();
    uint32_t sr_in = p->sr_hz;
    int64_t budget, produced = 0;
    int run = 0, seen = 0, quiet = 0, first = -1;
    static int16_t pending[HW_MIC_VAD_FRAME];   /* 静态: 局部 800B 会踩真机栈 (坑 12) */
    int have_pending = 0, n_pending = 0;
    int base, rc;

    if (!out || max_out <= 0 || run_n <= 0) return -1;
    g_st.state = HW_MIC_ST_ARMED;
    if (hw_mic_rs_init(&g_rs, sr_in, HW_MIC_ENGINE_SR) != 0) {
        g_st.last_err = -3; g_st.state = HW_MIC_ST_ERR; return -3;
    }
    if (g_bsp.enable && g_bsp.enable(1) != 0) {
        g_st.last_err = -4; g_st.state = HW_MIC_ST_ERR; return -4;
    }
    budget = ((int64_t)(timeout_ms > 0 ? timeout_ms : 0) * (int64_t)sr_in) / 1000;
    if (budget < (int64_t)max_out * 3) budget = (int64_t)max_out * 3;

    {
    int64_t ratio = (int64_t)sr_in / (int64_t)HW_MIC_ENGINE_SR;
    while (produced < (int64_t)max_out && g_rs.total_in < budget) {
        int got, made = 0, need = HW_MIC_SRC_CHUNK;
        int i;
        {
            int64_t room = (int64_t)max_out - produced;
            int64_t want = room * ratio + 2;
            if (want < HW_MIC_SRC_CHUNK) need = (int)want;
        }
        if (need < 1) need = 1;
        if (g_bsp.read) {
            got = g_bsp.read(g_src, need, sr_in);
            if (got < 0) { g_st.last_err = got; g_st.state = HW_MIC_ST_ERR; return got; }
            if (got == 0) break;
        } else {
            sim_fill(g_src, need, g_rs.total_in, sr_in);
            got = need;
        }
        g_st.src_read += got;
        rc = hw_mic_rs_feed(&g_rs, g_src, got, out + produced,
                            max_out - (int)produced, &made);
        base = (int)produced;                 /* 内层逐帧判决要用【本块起点】 */
        produced += made;                     /* 先累加再判错 (坑 31) */
        if (rc != 0) { g_st.last_err = rc; g_st.state = HW_MIC_ST_ERR; return rc; }
        /* 逐帧判决: made 个新样本, 凑满一帧就判 */
        for (i = 0; i < made; i++) {
            pending[n_pending++] = out[base + i];
            have_pending = 1;
            if (n_pending < HW_MIC_VAD_FRAME) continue;
            {
                hw_mic_stats_t st;
                int hit;
                hw_mic_stats((const int16_t*)pending, HW_MIC_VAD_FRAME, &st);
                hit = (st.rms >= thr) ? 1 : 0;
                g_st.level = level_of(st.rms);
                if (hit) {
                    run++;
                    quiet = 0;
                    if (run >= run_n) {
                        seen++;
                        if (first < 0) first = (int)((base + i + 1) / HW_MIC_VAD_FRAME) - run_n;
                    }
                } else {
                    run = 0;
                    if (seen > 0) quiet++;
                }
                n_pending = 0;
                have_pending = 0;
            }
        }
        produced += made;
        if (seen > 0 && quiet >= tail_frames) break;
        if (g_bsp.read && got < need) break;
    }
    }
    g_st.captured   = produced;
    g_st.vad_first  = first;
    g_st.vad_frames = seen;
    g_st.state      = (produced > 0) ? HW_MIC_ST_DONE : HW_MIC_ST_ERR;
    if (out_first) *out_first = first;
    (void)have_pending;
    return (int)produced;
}

/* ============================ 10. 命令 / 自检 / CLI ============================ */
static int g_fail = 0;
static hw_mic_puts_fn g_putf = 0;

static uint32_t fnv1a32_bytes(const uint8_t* d, int n)
{
    uint32_t h = 0x811C9DC5u;
    int i;
    for (i = 0; i < n; i++) { h ^= d[i]; h *= 0x01000193u; }
    return h;
}

static uint32_t fnv1a32_i32(int32_t v)
{
    uint8_t b[4];
    uint32_t u = (uint32_t)v;
    b[0] = (uint8_t)(u & 0xFF);       b[1] = (uint8_t)((u >> 8) & 0xFF);
    b[2] = (uint8_t)((u >> 16) & 0xFF); b[3] = (uint8_t)((u >> 24) & 0xFF);
    return fnv1a32_bytes(b, 4);
}

static void mic_log(const char* msg)
{
    if (g_putf) g_putf(msg);
}

static void mic_fail(const char* tag, const char* detail)
{
    char b[224];
    g_fail++;
    if (!g_putf) return;
    snprintf(b, sizeof(b), "  [FAIL] %-6s %s\n", tag, detail);
    mic_log(b);
}

/* 独立批量重采样参考实现 —— 【不复用流式状态机】。
 * 直接按公式向 sim_sample() 要 s[pos]/s[pos+1], 因此它与流式路径是两条
 * 独立代码路径, 互相对拍才有意义(家族纪律: 判据与被测物同源 = 对拍是复印机)。 */
static int run_rs_batch(uint32_t sr_in, int16_t* out, int n_out)
{
    int i;
    for (i = 0; i < n_out; i++) {
        int64_t num  = (int64_t)i * (int64_t)sr_in;
        int64_t pos  = num / (int64_t)HW_MIC_ENGINE_SR;
        int32_t frac = (int32_t)(((num % (int64_t)HW_MIC_ENGINE_SR) << 16)
                                 / (int64_t)HW_MIC_ENGINE_SR);
        int32_t a = sim_sample(pos, sr_in);
        int32_t b = sim_sample(pos + 1, sr_in);
        out[i] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)frac) >> 16));
    }
    return 0;
}

/* 流式: 源样本分块喂, 产出 n_out 个引擎样本。
 * ⚠️ 刻意用【不整除 n_out 的块大小】调用: 若块大小恰好整除, 分块边界与
 *   样本边界重合, 就会漏掉"跨块丢样本"这个 bug(家族坑 35 同族:
 *   边界值要单独当一条用例, 别让它混在通用用例里)。 */
static int run_rs_stream(uint32_t sr_in, int16_t* out, int n_out, int chunk)
{
    hw_mic_rs_t st;
    int produced = 0;
    int64_t read = 0;
    if (hw_mic_rs_init(&st, sr_in, HW_MIC_ENGINE_SR) != 0) return -3;
    while (produced < n_out) {
        int made = 0;
        int rc;
        sim_fill(g_src, chunk, read, sr_in);
        read += chunk;
        rc = hw_mic_rs_feed(&st, g_src, chunk, out + produced,
                            n_out - produced, &made);
        /* ⚠️ 必须【先累加再判错】: feed 撞容量时返回 -2, 但它已经写出了 made 个
         *   样本且 *out_n 有效。先 break 再累加会静默丢掉这部分产出
         *   (坑 31: 上报长度必须与实际拷出的内容一致)。 */
        produced += made;
        if (rc != 0) break;
    }
    {
        int made = 0;
        hw_mic_rs_flush(&st, out + produced, n_out - produced, &made);
        produced += made;
    }
    return produced;
}

/* 整段 FNV (黄金值口径: 全部 16000 个样本的低 8 位) */
static uint32_t rs_fnv_all(const int16_t* s, int n)
{
    int i;
    uint32_t h = 0x811C9DC5u;
    for (i = 0; i < n; i++) { h ^= (uint8_t)(s[i] & 0xFF); h *= 0x01000193u; }
    return h;
}

static uint32_t prof_fnv(void)
{
    uint8_t blob[256];
    int n = 0, i, j;
    for (i = 0; i < HW_MIC_PROFILE_MAX; i++) {
        const hw_mic_profile_t* p = &hw_mic_profiles[i];
        const char* s = p->name;
        while (*s && n < (int)sizeof(blob)) blob[n++] = (uint8_t)*s++;
        blob[n++] = (uint8_t)(p->sr_hz & 0xFF);
        blob[n++] = (uint8_t)((p->sr_hz >> 8) & 0xFF);
        blob[n++] = (uint8_t)(p->bits & 0xFF);
        blob[n++] = (uint8_t)(p->channels & 0xFF);
        blob[n++] = p->left_sel;
        blob[n++] = p->agc;
    }
    (void)j;
    return fnv1a32_bytes(blob, n);
}

/* ---- 桩 BSP: 用于测 SIM/REAL 分水岭 ---- */
static int stub_read_calls = 0;
static int stub_read(int16_t* buf, int n, uint32_t sr_hz)
{
    stub_read_calls++;
    sim_fill(buf, n, 0, sr_hz);
    return n;
}
static int stub_enable(int on) { return on ? 0 : 0; }

int hw_mic_selftest(hw_mic_puts_fn putf)
{
    char b[224];
    hw_mic_stats_t stt;
    int first = -1, speech = -1, nframes;

    g_putf = putf;
    g_fail = 0;
    hw_mic_init();

    /* [1] 引擎规格与识别端对齐 (同一份规格两处声明) */
    if (HW_MIC_ENGINE_SR != 16000) mic_fail("[1]", "engine_sr != 16000");
    if (HW_MIC_VAD_FRAME != 400)  mic_fail("[1]", "vad_frame != 400");

    /* [2] 档案表黄金值 */
    {
        uint32_t g = prof_fnv();
        if (g != HW_MIC_GOLDEN_PROF_FNV) {
            snprintf(b, sizeof(b), "profile fnv 0x%08X != 0x%08X",
                     g, (uint32_t)HW_MIC_GOLDEN_PROF_FNV);
            mic_fail("[2]", b);
        }
    }

    /* [3] 档案载入契约 */
    if (hw_mic_profile_count() != HW_MIC_PROFILE_MAX) mic_fail("[3]", "count");
    if (hw_mic_profile_load("inmp441-16k") != 0)     mic_fail("[3]", "load known");
    if (hw_mic_profile_load("no-such-mic") != -1)     mic_fail("[3]", "load unknown");
    if (hw_mic_profile_load_idx(-1) != -1)            mic_fail("[3]", "idx -1");
    if (hw_mic_profile_load_idx(99) != -1)            mic_fail("[3]", "idx 99");
    hw_mic_profile_load_idx(0);

    /* [4] 流式 == 独立批量 (O(1) 内存的核心承诺) */
    {
        int n1 = run_rs_stream(48000, g_cap, 16000, 256);
        static int16_t ref[16000];
        int n2 = run_rs_batch(48000, ref, 16000);
        int d;
        if (n1 != 16000) { snprintf(b, sizeof(b), "stream n=%d", n1); mic_fail("[4]", b); }
        d = hw_mic_maxdiff(g_cap, ref, 16000);
        if (d != 0) { snprintf(b, sizeof(b), "stream vs batch maxdiff=%d", d); mic_fail("[4]", b); }
        (void)n2;
    }

    /* [5] 两条降采样路径必须还原出【同一段物理声音】+ 黄金值 */
    {
        static int16_t b32[16000];
        int n32 = run_rs_stream(32000, b32, 16000, 256);
        int d = hw_mic_maxdiff(g_cap, b32, 16000);
        uint32_t f48 = rs_fnv_all(g_cap, 16000);
        uint32_t f32 = rs_fnv_all(b32, 16000);
        if (n32 != 16000) { snprintf(b, sizeof(b), "32k n=%d", n32); mic_fail("[5]", b); }
        if (d != 0) { snprintf(b, sizeof(b), "48k vs 32k maxdiff=%d", d); mic_fail("[5]", b); }
        if (f48 != HW_MIC_GOLDEN_RS_FNV || f32 != HW_MIC_GOLDEN_RS_FNV) {
            snprintf(b, sizeof(b), "fnv 48k=0x%08X 32k=0x%08X", f48, f32);
            mic_fail("[5]", b);
        }
        if (hw_mic_stats(g_cap, 16000, &stt) != 0) { mic_fail("[5]", "stats"); }
        else {
            if (stt.peak != HW_MIC_GOLDEN_RS_PEAK) { snprintf(b, sizeof(b), "peak=%d", stt.peak); mic_fail("[5]", b); }
            if (stt.dc   != HW_MIC_GOLDEN_RS_DC)   { snprintf(b, sizeof(b), "dc=%d", stt.dc);     mic_fail("[5]", b); }
            if (stt.rms  != HW_MIC_GOLDEN_RS_RMS)  { snprintf(b, sizeof(b), "rms=%d", stt.rms);   mic_fail("[5]", b); }
            if (stt.zcr  != HW_MIC_GOLDEN_RS_ZCR)  { snprintf(b, sizeof(b), "zcr=%d", stt.zcr);   mic_fail("[5]", b); }
        }
    }

    /* [6] VAD 判决 */
    {
        uint8_t bits[HW_MIC_GOLDEN_VAD_TOTAL];
        uint32_t fv;
        int k;
        nframes = hw_mic_vad_run(g_cap, 16000, 500, 3, &first, &speech);
        if (nframes != HW_MIC_GOLDEN_VAD_TOTAL) { snprintf(b, sizeof(b), "frames=%d", nframes); mic_fail("[6]", b); }
        if (first  != HW_MIC_GOLDEN_VAD_FIRST)  { snprintf(b, sizeof(b), "first=%d", first);     mic_fail("[6]", b); }
        if (speech != HW_MIC_GOLDEN_VAD_FRAMES) { snprintf(b, sizeof(b), "speech=%d", speech);   mic_fail("[6]", b); }
        for (k = 0; k < nframes && k < HW_MIC_GOLDEN_VAD_TOTAL; k++) {
            hw_mic_stats_t fs;
            hw_mic_stats(g_cap + k * HW_MIC_VAD_FRAME, HW_MIC_VAD_FRAME, &fs);
            bits[k] = (uint8_t)((fs.rms >= 500) ? 1 : 0);
        }
        fv = fnv1a32_bytes(bits, nframes);
        if (fv != HW_MIC_GOLDEN_VAD_FNV) { snprintf(b, sizeof(b), "vad fnv 0x%08X", fv); mic_fail("[6]", b); }
    }

    /* [7] 统计边界: 全零 / 负直流 / 常量 */
    {
        static int16_t z[800], c[800];
        int i;
        for (i = 0; i < 800; i++) { z[i] = 0; c[i] = -1000; }
        if (hw_mic_stats(z, 800, &stt) != 0 || stt.peak != 0 || stt.rms != 0 || stt.zcr != 0) {
            mic_fail("[7]", "all-zero stats");
        }
        if (hw_mic_stats(c, 800, &stt) != 0 || stt.peak != 1000 || stt.dc != -1000 ||
            stt.rms != 1000 || stt.zcr != 0) {
            snprintf(b, sizeof(b), "const: peak=%d dc=%d rms=%d zcr=%d",
                     stt.peak, stt.dc, stt.rms, stt.zcr);
            mic_fail("[7]", b);
        }
    }

    /* [8] BSP 分水岭 (结构性 NULL) */
    if (hw_mic_is_sim() != 1) mic_fail("[8]", "default should be SIM");
    {
        hw_mic_bsp_t bsp;
        memset(&bsp, 0, sizeof(bsp));
        bsp.read = stub_read; bsp.enable = stub_enable;
        hw_mic_bsp_install(&bsp);
        if (hw_mic_is_sim() != 0) mic_fail("[8]", "after install should be REAL");
        /* [9] init 不得抹掉已装 BSP (家族坑 25 护栏) */
        hw_mic_init();
        if (hw_mic_is_sim() != 0) mic_fail("[9]", "init wiped BSP!");
        hw_mic_bsp_install(NULL);
        if (hw_mic_is_sim() != 1) mic_fail("[8]", "uninstall should be SIM");
    }

    /* [10] 负向用例: 每个错误码各一条 */
    {
        hw_mic_rs_t rs;
        static int16_t tmp[64];
        int made = 0;
        if (hw_mic_rs_init(&rs, 8000, 16000) != -3)  mic_fail("[10]", "upsample should be -3");
        if (hw_mic_rs_init(&rs, 0, 16000) != -1)      mic_fail("[10]", "sr_in=0 should be -1");
        if (hw_mic_rs_init(NULL, 48000, 16000) != -1) mic_fail("[10]", "null st");
        if (hw_mic_rs_init(&rs, 48000, 16000) != 0)   mic_fail("[10]", "init ok");
        if (hw_mic_rs_feed(&rs, g_src, 64, tmp, 0, &made) != -2) mic_fail("[10]", "cap=0 should be -2");
        if (hw_mic_rs_feed(&rs, g_src, 64, tmp, -1, &made) != -1) mic_fail("[10]", "cap<0 should be -1");
        {   /* 背压契约: 恰好填满必须干净返回 0 且 *out_n == cap, 不是 -2 */
            int m2 = 0;
            if (hw_mic_rs_feed(&rs, g_src, 64, tmp, 4, &m2) != 0 || m2 != 4) {
                snprintf(b, sizeof(b), "backpressure: rc!=0 or m2=%d", m2);
                mic_fail("[10]", b);
            }
        }
        if (hw_mic_stats(NULL, 10, &stt) != -1)       mic_fail("[10]", "stats null");
        if (hw_mic_stats(g_cap, 0, &stt) != -1)       mic_fail("[10]", "stats n=0");
        if (hw_mic_maxdiff(g_cap, g_cap, 0) != -1)    mic_fail("[10]", "maxdiff n=0");
        if (hw_mic_vad_run(g_cap, 16000, 500, 0, &first, &speech) != -1) mic_fail("[10]", "vad run_n=0");
        if (hw_mic_capture(NULL, 100, 100) != -1)     mic_fail("[10]", "capture null");
        if (hw_mic_status(NULL) != -1)                 mic_fail("[10]", "status null");
    }

    /* [11] SIM 采集全链路 (无 BSP 时走时间域表)
     * ⚠️ 判据按【已知波形结构】写, 不是笼统的"peak != 0": SIM 语音段从
     *   0.3s(样本 4800) 才开始, 采 0.1s 拿到的整段都是静音, peak==0 是
     *   正确结果。首版就这样误报了 1 个失败(坑 53: 先怀疑用例)。 */
    {
        hw_mic_stats_t s_pre, s_voice;
        int n = hw_mic_capture(g_cap, 6000, 500);
        if (n != 6000) { snprintf(b, sizeof(b), "capture n=%d", n); mic_fail("[11]", b); }
        if (hw_mic_state() != HW_MIC_ST_DONE) mic_fail("[11]", "state not DONE");
        if (hw_mic_stats(g_cap, 4000, &s_pre) != 0 || s_pre.peak != 0) {
            snprintf(b, sizeof(b), "前 0.25s 应静音, peak=%d", s_pre.peak);
            mic_fail("[11]", b);
        }
        if (hw_mic_stats(g_cap + 4800, 1200, &s_voice) != 0 || s_voice.peak == 0) {
            snprintf(b, sizeof(b), "语音段应有信号, peak=%d", s_voice.peak);
            mic_fail("[11]", b);
        }
    }

    /* [12] VAD 驱动采集: 必须检出语音起点 */
    {
        int f2 = -1;
        int n = hw_mic_capture_vad(g_cap, 16000, 500, 3, 5, 2000, &f2);
        if (n <= 0) { snprintf(b, sizeof(b), "vad capture n=%d", n); mic_fail("[12]", b); }
        if (f2 < 0)  { snprintf(b, sizeof(b), "vad first=%d", f2);   mic_fail("[12]", b); }
    }

    /* [13] 未知命令必须被拒 (usage 承诺的命令表逐条可数) */
    {
        char* av_unknown[2];
        char  s_unknown[] = "no-such-cmd";
        av_unknown[0] = s_unknown;
        av_unknown[1] = 0;
        if (hw_mic_cmd(1, 0) == 0)           mic_fail("[13]", "argc=1 should fail");
        if (hw_mic_cmd(2, av_unknown) == 0)  mic_fail("[13]", "unknown cmd accepted");
    }

    /* [14] 统计字段合成锚 (跨模块一致性: 一份统计, 一个指纹) */
    {
        int32_t  v;
        uint32_t fv;
        /* ⚠️ 必须自己重建缓冲: [12] 的 capture_vad 覆盖过 g_cap,
         *   沿用会统计到 VAD 采到的数据而不是黄金数据(测试间状态污染)。 */
        run_rs_stream(48000, g_cap, 16000, 256);
        hw_mic_stats(g_cap, 16000, &stt);
        v  = (int32_t)(((uint32_t)stt.peak << 16) ^ ((uint32_t)stt.dc & 0xFFFFu)
                       ^ ((uint32_t)stt.rms << 8)  ^ ((uint32_t)stt.zcr & 0xFFu));
        fv = fnv1a32_i32(v);
        if (fv != HW_MIC_GOLDEN_STAT_FNV) {
            snprintf(b, sizeof(b), "stat fnv 0x%08X != 0x%08X", fv, (uint32_t)HW_MIC_GOLDEN_STAT_FNV);
            mic_fail("[14]", b);
        }
    }

    /* [15] 装了 BSP 就必须【真的被调用】—— 否则 [8]/[9] 只是查了个标志位
     * (家族坑 17: 桩函数从未被调用 = 自检空转, 比崩溃更险因为它表现为全绿)。
     * 判别法: 故意把桩拆掉, 这条必须由 PASS 变 FAIL。 */
    {
        int before;
        hw_mic_bsp_t bsp;
        memset(&bsp, 0, sizeof(bsp));
        bsp.read = stub_read;
        hw_mic_bsp_install(&bsp);
        before = stub_read_calls;
        hw_mic_capture(g_cap, 800, 200);
        if (stub_read_calls == before) mic_fail("[15]", "BSP read 从未被调用 (空转?)");
        hw_mic_bsp_install(NULL);
    }

    hw_mic_init();
    g_putf = 0;
    if (putf) {
        snprintf(b, sizeof(b), "  mic selftest: %s (%d 失败)\n",
                 g_fail ? "FAIL" : "ALL PASS", g_fail);
        putf(b);
    }
    return g_fail;
}

/* ------------------------- 命令分发 (VM 与 CLI 共用, 零新增 opcode) ------------------------- */
static int mic_puts_wrap(const char* s) { return printf("%s", s); }

int hw_mic_cli(int argc, char** argv)
{
    /* 家族约定: 读 argv[2], 判 argc >= 3 (坑 9) */
    if (argc < 3) { hw_mic_cmd(1, 0); return 1; }
    if (strcmp(argv[2], "selftest") == 0) return hw_mic_selftest(mic_puts_wrap);
    return (hw_mic_cmd(argc - 2, argv + 2) == 0) ? 0 : 1;
}

int hw_mic_cmd(int argc, char** argv)
{
    const char* c;
    if (argc < 1 || !argv) { printf("缺命令\n"); return -1; }
    c = argv[0];
    if (!c) c = "help";

    if (strcmp(c, "help") == 0 || strcmp(c, "-h") == 0) {
        printf("用法: xiaomo mic <命令>\n");
        printf("  mode            打印模式\n");
        printf("  prof            列出档案\n");
        printf("  prof <name>     载入档案\n");
        printf("  stat <n>        SIM 源 n 个引擎样本并打统计\n");
        printf("  vad <n>         SIM 源 n 个引擎样本并打 VAD 判决\n");
        printf("  golden          核对全部黄金值\n");
        printf("  capture <n>     走 BSP 采一段 (未装 BSP = SIM)\n");
        printf("  selftest        全量自检\n");
        return 0;
    }
    if (strcmp(c, "mode") == 0) {
        printf("mode = %d (%s) sim = %d\n", hw_mic_mode(), hw_mic_mode_name(), hw_mic_is_sim());
        return 0;
    }
    if (strcmp(c, "prof") == 0) {
        if (argc >= 2 && argv[1]) {
            if (hw_mic_profile_load(argv[1]) != 0) { printf("未知档案: %s\n", argv[1]); return -1; }
            printf("已载入 %s\n", argv[1]);
            return 0;
        }
        {
            const hw_mic_profile_t* p = hw_mic_profile_get();
            int i;
            for (i = 0; i < HW_MIC_PROFILE_MAX; i++) {
                printf("  [%d] %-14s sr=%-6u bits=%-2u ch=%u L=%u agc=%u\n", i,
                       hw_mic_profiles[i].name, hw_mic_profiles[i].sr_hz,
                       hw_mic_profiles[i].bits, hw_mic_profiles[i].channels,
                       hw_mic_profiles[i].left_sel, hw_mic_profiles[i].agc);
            }
            printf("当前 = %s (sr=%u)\n", p->name, p->sr_hz);
        }
        return 0;
    }
    if (strcmp(c, "stat") == 0) {
        int n = (argc >= 2) ? atoi(argv[1]) : 16000;
        hw_mic_stats_t s;
        if (n < 1 || n > HW_MIC_CAPTURE_MAX) n = 16000;
        run_rs_stream(48000, g_cap, n, 256);
        if (hw_mic_stats(g_cap, n, &s) != 0) { printf("stats 失败\n"); return -1; }
        printf("n=%d peak=%d dc=%d rms=%d zcr=%d fnv=0x%08X\n",
               s.n, s.peak, s.dc, s.rms, s.zcr, rs_fnv_all(g_cap, n));
        return 0;
    }
    if (strcmp(c, "vad") == 0) {
        int n = (argc >= 2) ? atoi(argv[1]) : 16000;
        int first = -1, speech = -1, nf;
        if (n < 1 || n > HW_MIC_CAPTURE_MAX) n = 16000;
        run_rs_stream(48000, g_cap, n, 256);
        nf = hw_mic_vad_run(g_cap, n, 500, 3, &first, &speech);
        printf("frames=%d first=%d speech=%d\n", nf, first, speech);
        return 0;
    }
    if (strcmp(c, "golden") == 0) {
        hw_mic_stats_t s;
        uint32_t f;
        int first = -1, speech = -1, nf;
        run_rs_stream(48000, g_cap, 16000, 256);
        f  = rs_fnv_all(g_cap, 16000);
        nf = hw_mic_vad_run(g_cap, 16000, 500, 3, &first, &speech);
        hw_mic_stats(g_cap, 16000, &s);
        printf("rs_fnv   = 0x%08X  期望 0x%08X  %s\n", f, (uint32_t)HW_MIC_GOLDEN_RS_FNV,
               f == HW_MIC_GOLDEN_RS_FNV ? "OK" : "MISMATCH");
        printf("peak=%d dc=%d rms=%d zcr=%d\n", s.peak, s.dc, s.rms, s.zcr);
        printf("vad      = frames=%d first=%d speech=%d  期望 %d/%d/%d  %s\n",
               nf, first, speech, HW_MIC_GOLDEN_VAD_TOTAL, HW_MIC_GOLDEN_VAD_FIRST,
               HW_MIC_GOLDEN_VAD_FRAMES,
               (nf == HW_MIC_GOLDEN_VAD_TOTAL && first == HW_MIC_GOLDEN_VAD_FIRST &&
                speech == HW_MIC_GOLDEN_VAD_FRAMES) ? "OK" : "MISMATCH");
        printf("prof_fnv = 0x%08X  期望 0x%08X  %s\n", prof_fnv(),
               (uint32_t)HW_MIC_GOLDEN_PROF_FNV,
               prof_fnv() == HW_MIC_GOLDEN_PROF_FNV ? "OK" : "MISMATCH");
        return 0;
    }
    if (strcmp(c, "capture") == 0) {
        int n = (argc >= 2) ? atoi(argv[1]) : 1600;
        hw_mic_stats_t s;
        int got;
        if (n < 1 || n > HW_MIC_CAPTURE_MAX) n = 1600;
        got = hw_mic_capture(g_cap, n, 500);
        if (got < 0) { printf("capture 失败 rc=%d (bsp=%s)\n", got, hw_mic_is_sim() ? "SIM" : "REAL"); return got; }
        hw_mic_stats(g_cap, got, &s);
        printf("capture n=%d peak=%d rms=%d zcr=%d\n", got, s.peak, s.rms, s.zcr);
        return 0;
    }
    printf("未知命令: %s\n", c);
    return -1;
}
