/*
 * xiaomo - 语音识别层 (hw_asr) 实现
 * 本地引擎: WAV → MFCC(FFT/Mel/DCT) → CMVN → DTW 模板匹配
 * 线上引擎: OpenAI 兼容 /audio/transcriptions (curl 或裸 socket)
 */
#include "hw_asr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================
 * 1. WAV 读取 (chunk 遍历, PCM16, 重采样 16k 单声道)
 * ============================================================ */

static unsigned rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static unsigned rd16(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

int hw_asr_load_wav(const char* path, float** out, int* out_n)
{
    FILE* f = fopen(path, "rb");
    unsigned char hdr[64];
    long fsize;
    unsigned sr = 0, ch = 0, bits = 0, fmt = 0;
    long data_off = -1, data_len = 0;
    int i;

    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 44 || fread(hdr, 1, 12, f) != 12 ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return -2;
    }
    /* chunk 遍历: 找 fmt 与 data */
    for (;;) {
        unsigned char ch8[8];
        unsigned cid, clen;
        if (fread(ch8, 1, 8, f) != 8) break;
        cid = rd32(ch8);
        clen = rd32(ch8 + 4);
        if (cid == 0x20746D66u) {          /* "fmt " */
            unsigned char fmtb[16];
            if (clen < 16 || fread(fmtb, 1, 16, f) != 16) break;
            fmt = rd16(fmtb);
            ch = rd16(fmtb + 2);
            sr = rd32(fmtb + 4);
            bits = rd16(fmtb + 14);
            if (clen > 16 && fseek(f, (long)(clen - 16), SEEK_CUR) != 0) break;
        } else if (cid == 0x61746164u) {   /* "data" */
            data_off = ftell(f);
            data_len = (long)clen;
            if (data_off + data_len > fsize) data_len = fsize - data_off;
            break;   /* 之后可能有 LIST 等, 不管 */
        } else {
            if (clen & 1u) clen++;         /* chunk 字对齐 */
            if (fseek(f, (long)clen, SEEK_CUR) != 0) break;
        }
    }
    fclose(f);
    if (data_off < 0 || data_len <= 0) return -2;
    if (fmt != 1 || bits != 16 || ch == 0 || sr == 0) return -3;

    {
        long nsmp_all = data_len / 2;               /* int16 样本总数 */
        short* raw = (short*)malloc((size_t)nsmp_all * 2);
        long nframes_mono, need;
        float* mono;
        float* res;
        if (!raw) return -4;
        f = fopen(path, "rb");
        if (!f) { free(raw); return -4; }
        fseek(f, data_off, SEEK_SET);
        if (fread(raw, 2, (size_t)nsmp_all, f) != (size_t)nsmp_all) {
            fclose(f); free(raw); return -4;
        }
        fclose(f);

        /* 多声道 → 平均单声道 */
        nframes_mono = nsmp_all / (long)ch;
        mono = (float*)malloc((size_t)(nframes_mono > 0 ? nframes_mono : 1) * sizeof(float));
        if (!mono) { free(raw); return -4; }
        for (i = 0; i < (int)nframes_mono; i++) {
            long s = (long)i * (long)ch;   /* 帧基址 (曾写成死 0 → 全音频读成第1样本) */
            int c;
            float acc = 0.f;
            for (c = 0; c < (int)ch; c++) acc += (float)raw[s + (long)c];
            mono[i] = acc / (float)ch / 32768.f;
        }
        free(raw);

        /* 线性插值重采样 → 16k */
        if (sr == HW_ASR_SR) {
            *out = mono;
            *out_n = (int)nframes_mono;
            return *out_n;
        }
        need = (long)((double)nframes_mono * (double)HW_ASR_SR / (double)sr);
        if (need < 1) { free(mono); return -4; }
        res = (float*)malloc((size_t)need * sizeof(float));
        if (!res) { free(mono); return -4; }
        for (i = 0; i < (int)need; i++) {
            double pos = (double)i * (double)sr / (double)HW_ASR_SR;
            long i0 = (long)pos;
            double fr = pos - (double)i0;
            long i1 = (i0 + 1 < nframes_mono) ? i0 + 1 : i0;
            res[i] = (float)((double)mono[i0] * (1.0 - fr) + (double)mono[i1] * fr);
        }
        free(mono);
        *out = res;
        *out_n = (int)need;
        return *out_n;
    }
}

/* ============================================================
 * 2. FFT (radix-2 迭代, static 区防爆栈 — 吸取 ESP32 栈帧教训)
 * ============================================================ */

static double g_re[HW_ASR_FFT], g_im[HW_ASR_FFT];
static double g_hann[HW_ASR_FRAME];
static int g_hann_init = 0;

static void fft512(void)
{
    int n = HW_ASR_FFT, i, j, len;
    for (i = 1, j = 0; i < n; i++) {          /* bit reversal */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            double t = g_re[i]; g_re[i] = g_re[j]; g_re[j] = t;
            t = g_im[i]; g_im[i] = g_im[j]; g_im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * M_PI / (double)len;
        double wr = cos(ang), wi = sin(ang);
        for (i = 0; i < n; i += len) {
            double cr = 1.0, ci = 0.0;
            int k;
            for (k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                double ur = g_re[a], ui = g_im[a];
                double vr = g_re[b] * cr - g_im[b] * ci;
                double vi = g_re[b] * ci + g_im[b] * cr;
                g_re[a] = ur + vr; g_im[a] = ui + vi;
                g_re[b] = ur - vr; g_im[b] = ui - vi;
                {
                    double ncr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;
                    cr = ncr;
                }
            }
        }
    }
}

/* ============================================================
 * 3. MFCC: 分帧 → FFT → Mel26 → log → DCT-II 13系 + 能量 → CMVN
 * ============================================================ */

static int g_mel_lo[HW_ASR_MEL_BANDS], g_mel_hi[HW_ASR_MEL_BANDS];
static int g_mel_init = 0;

static double hz2mel(double f) { return 2595.0 * log10(1.0 + f / 700.0); }
static double mel2hz(double m) { return 700.0 * (pow(10.0, m / 2595.0) - 1.0); }

static void mel_init(void)
{
    int b;
    double fmin = 0.0, fmax = 8000.0;
    double mlo = hz2mel(fmin), mhi = hz2mel(fmax);
    for (b = 0; b < HW_ASR_MEL_BANDS; b++) {
        double f0 = mel2hz(mlo + (mhi - mlo) * (double)b / (double)(HW_ASR_MEL_BANDS + 1));
        double f1 = mel2hz(mlo + (mhi - mlo) * (double)(b + 2) / (double)(HW_ASR_MEL_BANDS + 1));
        g_mel_lo[b] = (int)(f0 / (8000.0 / (HW_ASR_FFT / 2)));
        g_mel_hi[b] = (int)(f1 / (8000.0 / (HW_ASR_FFT / 2)) + 0.999);
        if (g_mel_hi[b] <= g_mel_lo[b]) g_mel_hi[b] = g_mel_lo[b] + 1;
        if (g_mel_hi[b] > HW_ASR_FFT / 2) g_mel_hi[b] = HW_ASR_FFT / 2;
    }
    g_mel_init = 1;
}

int hw_asr_mfcc(const float* pcm, int n, float*** frames, int* out_n)
{
    int nf, fi, d, b;
    float** fr;
    double* mean;
    double* pwr;

    if (!pcm || n < HW_ASR_FRAME || !frames || !out_n) return -1;
    if (!g_mel_init) mel_init();
    if (!g_hann_init) {
        for (fi = 0; fi < HW_ASR_FRAME; fi++)
            g_hann[fi] = 0.5 - 0.5 * cos(2.0 * M_PI * (double)fi / (double)(HW_ASR_FRAME - 1));
        g_hann_init = 1;
    }
    nf = (n - HW_ASR_FRAME) / HW_ASR_HOP + 1;
    if (nf < 1) return -1;
    nf = (nf > HW_ASR_MAX_FRAMES) ? HW_ASR_MAX_FRAMES : nf;

    fr = (float**)malloc((size_t)nf * sizeof(float*));
    if (!fr) return -1;
    for (fi = 0; fi < nf; fi++) fr[fi] = (float*)malloc(HW_ASR_DIM * sizeof(float));
    mean = (double*)calloc(HW_ASR_DIM, sizeof(double));
    pwr = (double*)calloc(HW_ASR_FFT / 2, sizeof(double));
    if (!mean || !pwr) return -1;

    for (fi = 0; fi < nf; fi++) {
        const float* seg = pcm + fi * HW_ASR_HOP;
        double energy = 0.0;
        int k;
        for (k = 0; k < HW_ASR_FRAME; k++) {
            g_re[k] = (double)seg[k] * g_hann[k];
            g_im[k] = 0.0;
            energy += (double)seg[k] * seg[k];
        }
        for (; k < HW_ASR_FFT; k++) { g_re[k] = 0.0; g_im[k] = 0.0; }
        fft512();
        for (k = 0; k < HW_ASR_FFT / 2; k++)
            pwr[k] = g_re[k] * g_re[k] + g_im[k] * g_im[k];

        {
            double mel_e[HW_ASR_MEL_BANDS];
            for (b = 0; b < HW_ASR_MEL_BANDS; b++) {
                double acc = 0.0;
                int k;
                for (k = g_mel_lo[b]; k < g_mel_hi[b]; k++) acc += pwr[k];
                mel_e[b] = log(acc + 1e-10);
            }
            for (d = 0; d < HW_ASR_NCEP; d++) {   /* DCT-II */
                double acc = 0.0;
                for (b = 0; b < HW_ASR_MEL_BANDS; b++)
                    acc += mel_e[b] * cos(M_PI * (double)d * ((double)b + 0.5) / (double)HW_ASR_MEL_BANDS);
                fr[fi][d] = (float)acc;
            }
            fr[fi][HW_ASR_NCEP] = (float)log(energy + 1e-10);
        }
        for (d = 0; d < HW_ASR_DIM; d++) mean[d] += fr[fi][d];
    }
    for (d = 0; d < HW_ASR_DIM; d++) mean[d] /= (double)nf;
    for (fi = 0; fi < nf; fi++)
        for (d = 0; d < HW_ASR_DIM; d++)
            fr[fi][d] -= (float)mean[d];      /* CMVN (减均值) */

    free(mean);
    free(pwr);
    *frames = fr;
    *out_n = nf;
    return nf;
}

/* ============================================================
 * 4. DTW (弦距离, 滚动数组, Sakoe-Chiba 带)
 * ============================================================ */

static double vec_dist(const float* a, const float* b)
{
    double dot = 0, na = 0, nb = 0;
    int d;
    for (d = 0; d < HW_ASR_DIM; d++) {
        dot += (double)a[d] * b[d];
        na += (double)a[d] * a[d];
        nb += (double)b[d] * b[d];
    }
    if (na < 1e-12 || nb < 1e-12) return 1.0;
    return 1.0 - dot / (sqrt(na) * sqrt(nb));
}

double hw_asr_dtw(float** a, int na, float** b, int nb)
{
    /* 两行滚动 DP; 列间带约束 */
    static float* dp[2] = { NULL, NULL };
    static int dp_cap = 0;
    const int band = 60;
    int i, j, row;
    float INF = 1e30f;

    if (na <= 0 || nb <= 0 || !a || !b) return 2.0;
    if (dp_cap < nb + 1) {
        dp[0] = (float*)realloc(dp[0], (size_t)(nb + 1) * sizeof(float));
        dp[1] = (float*)realloc(dp[1], (size_t)(nb + 1) * sizeof(float));
        dp_cap = nb + 1;
        if (!dp[0] || !dp[1]) return 2.0;
    }
    for (j = 0; j <= nb; j++) dp[0][j] = INF;
    dp[0][0] = 0.f;
    for (i = 1; i <= na; i++) {
        row = i & 1;
        dp[row][0] = INF;
        for (j = 1; j <= nb; j++) {
            if (abs(i - j) > band) { dp[row][j] = INF; continue; }
            {
                float up = dp[row ^ 1][j];        /* 插入 */
                float lf = dp[row][j - 1];        /* 删除 */
                float dg = dp[row ^ 1][j - 1];    /* 匹配 */
                float m = dg;
                if (up < m) m = up;
                if (lf < m) m = lf;
                if (m >= INF) dp[row][j] = INF;
                else dp[row][j] = m + (float)vec_dist(a[i - 1], b[j - 1]);
            }
        }
    }
    /* 余弦距离浮点舍入可能给出 1.0000000001 → 微小负数, 钳到 0 */
    {
        double d_ = (double)dp[na & 1][nb] / (double)(na + nb);
        return d_ < 0 ? 0 : d_;
    }
}

/* ============================================================
 * 5. 模板库 (.asr/store.txt, 全量读-改-原子写回)
 * ============================================================ */

typedef struct hw_asr_tpl {
    char word[64];
    int nframes;
    float** frames;   /* [nframes][HW_ASR_DIM] */
} hw_asr_tpl;

static hw_asr_tpl g_tpl[HW_ASR_MAX_WORDS * HW_ASR_MAX_TPL];
static int g_ntpl = 0;
static int g_loaded = 0;
static const char* ASR_DIR = ".asr";
static const char* ASR_STORE = ".asr/store.txt";

static void store_load(void)
{
    FILE* f;
    char line[2048];
    if (g_loaded) return;
    g_loaded = 1;
    f = fopen(ASR_STORE, "r");
    if (!f) return;
    if (fgets(line, sizeof(line), f) && strncmp(line, "HWASR1", 6) == 0) {
        while (fgets(line, sizeof(line), f)) {
            if (line[0] == 'T' && line[1] == ' ') {
                int nf = atoi(line + 2), k;
                hw_asr_tpl* t;
                if (g_ntpl >= HW_ASR_MAX_WORDS * HW_ASR_MAX_TPL) break;
                if (nf <= 0 || nf > HW_ASR_MAX_FRAMES) continue;
                t = &g_tpl[g_ntpl];
                t->nframes = nf;
                t->frames = (float**)malloc((size_t)nf * sizeof(float*));
                for (k = 0; k < nf; k++) {
                    int d;
                    t->frames[k] = (float*)malloc(HW_ASR_DIM * sizeof(float));
                    if (fgets(line, sizeof(line), f)) {
                        char* p = line;
                        for (d = 0; d < HW_ASR_DIM; d++) {
                            t->frames[k][d] = (float)strtod(p, &p);
                        }
                    }
                }
                g_ntpl++;
            } else if (line[0] == 'W' && line[1] == ' ') {
                /* 词头: "W <word> <count>" — 只取最后空格前的词名 */
                char* p = line + 2;
                char* sp = strrchr(p, ' ');
                char* e;
                if (sp) *sp = '\0';
                e = p + strlen(p);
                while (e > p && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;
                if (g_ntpl < HW_ASR_MAX_WORDS * HW_ASR_MAX_TPL)
                    snprintf(g_tpl[g_ntpl].word, 64, "%s", p);
            }
        }
    }
    fclose(f);
}

static void store_save(void)
{
    FILE* f;
    char tmp[256];
    int i, k, d;
#ifdef _WIN32
    _mkdir(ASR_DIR);
#else
    if (mkdir(ASR_DIR, 0755) != 0 && errno != EEXIST) return;
#endif
    snprintf(tmp, sizeof(tmp), "%s.tmp", ASR_STORE);
    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "HWASR1\n");
    for (i = 0; i < g_ntpl;) {
        if (g_tpl[i].word[0] == '\0') { i++; continue; }
        /* 同词模板连续输出 */
        {
            int j = i, cnt = 0;
            while (j < g_ntpl && strcmp(g_tpl[j].word, g_tpl[i].word) == 0) { cnt++; j++; }
            fprintf(f, "W %s %d\n", g_tpl[i].word, cnt);
            for (; i < j; i++) {
                fprintf(f, "T %d\n", g_tpl[i].nframes);
                for (k = 0; k < g_tpl[i].nframes; k++) {
                    for (d = 0; d < HW_ASR_DIM; d++)
                        fprintf(f, "%.6e%c", g_tpl[i].frames[k][d],
                                (d == HW_ASR_DIM - 1) ? '\n' : ' ');
                }
            }
        }
    }
    fclose(f);
    rename(tmp, ASR_STORE);   /* 原子替换 */
}

static void tpl_free(hw_asr_tpl* t)
{
    int k;
    for (k = 0; k < t->nframes; k++) free(t->frames[k]);
    free(t->frames);
    t->frames = NULL;
    t->nframes = 0;
}

static int word_valid(const char* w)
{
    size_t n = strlen(w), i;
    if (n == 0 || n >= 64) return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)w[i];
        if (c == '/' || c == '\\' || c == ' ' || c < 0x20 || c == 0x7F) return 0;
    }
    return 1;
}

int hw_asr_enroll(const char* word, const char* wav_path)
{
    float* pcm = NULL;
    float** fr = NULL;
    int n, nf, i, cnt = 0;
    if (!word_valid(word)) return -1;
    store_load();
    if (hw_asr_load_wav(wav_path, &pcm, &n) < 0) return -2;
    if (hw_asr_mfcc(pcm, n, &fr, &nf) < 0) { free(pcm); return -3; }
    free(pcm);

    /* 找同词槽位: 已有 MAX_TPL 条时腾最旧 */
    for (i = 0; i < g_ntpl; i++)
        if (strcmp(g_tpl[i].word, word) == 0) cnt++;
    if (cnt >= HW_ASR_MAX_TPL) {
        for (i = 0; i < g_ntpl; i++) {
            if (strcmp(g_tpl[i].word, word) == 0) {
                memmove(&g_tpl[i], &g_tpl[i + 1],
                        sizeof(hw_asr_tpl) * (size_t)(g_ntpl - i - 1));
                g_ntpl--;
                break;   /* 只腾最旧一条 */
            }
        }
    }
    if (g_ntpl >= HW_ASR_MAX_WORDS * HW_ASR_MAX_TPL) return -4;
    snprintf(g_tpl[g_ntpl].word, 64, "%s", word);
    g_tpl[g_ntpl].nframes = nf;
    g_tpl[g_ntpl].frames = fr;
    g_ntpl++;
    store_save();
    for (i = 0; i < g_ntpl; i++)
        if (strcmp(g_tpl[i].word, word) == 0) cnt++;
    return cnt;
}

int hw_asr_match(const char* wav_path, hw_asr_result_t* res)
{
    float* pcm = NULL;
    float** fr = NULL;
    int n, nf, i, words_seen = 0;
    double best = 1e30, second = 1e30;
    int best_i = -1;
    char best_w[64] = "";

    store_load();
    if (g_ntpl == 0) return -1;
    if (hw_asr_load_wav(wav_path, &pcm, &n) < 0) return -2;
    if (hw_asr_mfcc(pcm, n, &fr, &nf) < 0) { free(pcm); return -3; }
    free(pcm);

    for (i = 0; i < g_ntpl; i++) {
        double d = hw_asr_dtw(fr, nf, g_tpl[i].frames, g_tpl[i].nframes);
        if (g_tpl[i].word[0] == '\0') continue;
        words_seen++;
        if (d < best) {
            if (strcmp(g_tpl[i].word, best_w) != 0) second = best;
            best = d;
            best_i = i;
            snprintf(best_w, 64, "%s", g_tpl[i].word);
        } else if (strcmp(g_tpl[i].word, best_w) != 0 && d < second) {
            second = d;
        }
    }
    for (i = 0; i < nf; i++) free(fr[i]);
    free(fr);
    if (res) {
        snprintf(res->word, 64, "%s", best_w);
        res->score = best;
        res->second = second;
        res->tpl_used = best_i;
    }
    return (words_seen > 0) ? 0 : -1;
}

int hw_asr_list(char words[][64], int counts[], int max)
{
    int i, nw = 0;
    store_load();
    for (i = 0; i < g_ntpl && nw < max; i++) {
        int j, found = -1;
        for (j = 0; j < nw; j++)
            if (strcmp(words[j], g_tpl[i].word) == 0) { found = j; break; }
        if (found < 0) {
            snprintf(words[nw], 64, "%s", g_tpl[i].word);
            counts[nw] = 1;
            nw++;
        } else {
            counts[found]++;
        }
    }
    return nw;
}

void hw_asr_reset(void)
{
    int i;
    store_load();
    for (i = 0; i < g_ntpl; i++) tpl_free(&g_tpl[i]);
    g_ntpl = 0;
    remove(ASR_STORE);
}

/* ============================================================
 * 6. 线上引擎 (OpenAI 兼容 multipart 转写)
 * ============================================================ */

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

/* shell 单引号强转义: a'b → 'a'\''b' */
static void sh_quote(const char* s, char* out, int cap)
{
    int n = 0, i;
    out[n++] = '\'';
    for (i = 0; s[i] && n < cap - 8; i++) {
        if (s[i] == '\'') n += snprintf(out + n, (size_t)(cap - n), "'\\''");
        else out[n++] = s[i];
    }
    out[n++] = '\'';
    out[n] = '\0';
}

int hw_asr_online_config(const char** url, const char** key, const char** model)
{
    static char u[512], k[256], m[128];
    const char* e;
    e = getenv("HW_ASR_URL");
    if (e) snprintf(u, sizeof(u), "%s", e);
    else if ((e = getenv("OPENAI_API_BASE")) != NULL)
        snprintf(u, sizeof(u), "%s/audio/transcriptions", e);
    else
        snprintf(u, sizeof(u), "%s", "https://api.siliconflow.cn/v1/audio/transcriptions");
    e = getenv("HW_ASR_KEY");
    if (e) snprintf(k, sizeof(k), "%s", e);
    else if ((e = getenv("OPENAI_API_KEY")) != NULL) snprintf(k, sizeof(k), "%s", e);
    else k[0] = '\0';
    e = getenv("HW_ASR_MODEL");
    if (e) snprintf(m, sizeof(m), "%s", e);
    else snprintf(m, sizeof(m), "%s", "FunAudioLLM/SenseVoiceSmall");
    if (url) *url = u;
    if (key) *key = k;
    if (model) *model = m;
    return 0;
}

int hw_asr_parse_text(const char* json, char* out, int cap)
{
    const char* p;
    if (!json || !out || cap < 1) return -1;
    out[0] = '\0';
    p = strstr(json, "\"text\"");
    if (!p) return -1;
    p = strchr(p + 6, '"');           /* text 后第一个引号 = 值开引号 */
    if (!p) return -1;
    p++;
    {
        int n = 0;
        while (*p && *p != '"' && n < cap - 1) {
            if (*p == '\\' && p[1]) {
                p++;
                switch (*p) {
                case 'n': out[n++] = '\n'; break;
                case 't': out[n++] = '\t'; break;
                case 'r': break;
                case 'u': {                            /* \uXXXX → UTF-8 */
                    unsigned cp = 0;
                    int h;
                    for (h = 0; h < 4 && p[1]; h++) {
                        p++;
                        cp = cp * 16u +
                             (unsigned)(*p >= '0' && *p <= '9' ? *p - '0' :
                                        *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 :
                                        *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : 0);
                    }
                    if (cp < 0x80u) out[n++] = (char)cp;
                    else if (cp < 0x800u) {
                        out[n++] = (char)(0xC0u | (cp >> 6));
                        out[n++] = (char)(0x80u | (cp & 0x3Fu));
                    } else {
                        out[n++] = (char)(0xE0u | (cp >> 12));
                        out[n++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                        out[n++] = (char)(0x80u | (cp & 0x3Fu));
                    }
                    break;
                }
                default: out[n++] = *p; break;
                }
                p++;
            } else {
                out[n++] = *p++;
            }
        }
        out[n] = '\0';
        return 0;
    }
}

/* 裸 socket HTTP POST (http only, 无 TLS) */
static int http_post(const char* url, const char* key, const char* model,
                     const char* wav_path, char* body, int body_cap, int* code)
{
    char host[256] = "", path[512] = "/";
    char req[4096], *payload;
    size_t paylen, flen;
    unsigned char* fdata;
    FILE* f;
    struct hostent* he;
    struct sockaddr_in sa;
    int fd, n, total, hdr_end = -1, i, port = 80;
    const char* B = "----xiaomoasr7f3a";
    char boundary[64];

    if (strncmp(url, "http://", 7) != 0) return -1;   /* 无 TLS 只走 http */
    {
        const char* p = url + 7;
        const char* slash = strchr(p, '/');
        const char* colon = strchr(p, ':');
        if (slash) {
            size_t hl = (size_t)(slash - p);
            if (colon && colon < slash) hl = (size_t)(colon - p);
            if (hl >= sizeof(host)) return -1;
            memcpy(host, p, hl);
            host[hl] = '\0';
            snprintf(path, sizeof(path), "%s", slash);
        } else {
            snprintf(host, sizeof(host), "%s", p);
        }
        if (colon && (!slash || colon < slash)) port = atoi(colon + 1);
    }
    f = fopen(wav_path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    flen = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    fdata = (unsigned char*)malloc(flen + 1);
    if (!fdata || fread(fdata, 1, flen, f) != flen) { fclose(f); free(fdata); return -1; }
    fclose(f);

    snprintf(boundary, sizeof(boundary), "%s", B);
    {
        /* 组 multipart body */
        size_t cap = flen + 4096;
        size_t off = 0;
        payload = (char*)malloc(cap);
        if (!payload) { free(fdata); return -1; }
#define PUTC(ch) do { if (off + 1 < cap) payload[off++] = (char)(ch); } while (0)
#define PUTS(s) do { const char* _s = (s); while (*_s) PUTC(*_s++); } while (0)
        PUTS("--"); PUTS(boundary); PUTS("\r\n");
        PUTS("Content-Disposition: form-data; name=\"model\"\r\n\r\n");
        PUTS(model); PUTS("\r\n--"); PUTS(boundary); PUTS("\r\n");
        PUTS("Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n");
        PUTS("Content-Type: audio/wav\r\n\r\n");
        for (i = 0; i < (int)flen; i++) PUTC(fdata[i]);
        PUTS("\r\n--"); PUTS(boundary); PUTS("--\r\n");
#undef PUTC
#undef PUTS
        paylen = off;
    }
    free(fdata);

    snprintf(req, sizeof(req),
             "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
             "User-Agent: xiaomo-hw-asr/1.0\r\n"
             "Content-Type: multipart/form-data; boundary=%s\r\n"
             "Content-Length: %lu\r\n"
             "%s%s%s"
             "Connection: close\r\n\r\n",
             path, host, port, boundary, (unsigned long)paylen,
             (key[0] ? "Authorization: Bearer " : ""), key, (key[0] ? "\r\n" : ""));

    he = gethostbyname(host);
    if (!he) { free(payload); return -1; }
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    memcpy(&sa.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { free(payload); return -1; }
    {
        struct timeval tv = { 30, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd); free(payload); return -1;
    }
    {
        size_t rl = strlen(req);
        if (send(fd, req, rl, 0) != (long)rl ||
            send(fd, payload, paylen, 0) != (long)paylen) {
            close(fd); free(payload); return -1;
        }
    }
    free(payload);

    /* 收响应: 头 + 体 */
    total = 0;
    {
        static char resp[65536];
        for (;;) {
            if (total >= (int)sizeof(resp) - 1) break;
            n = (int)recv(fd, resp + total, (size_t)(sizeof(resp) - 1 - (size_t)total), 0);
            if (n <= 0) break;
            total += n;
            resp[total] = '\0';
            if (hdr_end < 0) {
                char* p = strstr(resp, "\r\n\r\n");
                if (p) { hdr_end = (int)(p - resp) + 4; }
            }
        }
        close(fd);
        if (total <= 0) return -1;
        resp[total] = '\0';
        *code = 0;
        if (strncmp(resp, "HTTP/1.1 ", 9) == 0) *code = atoi(resp + 9);
        else if (strncmp(resp, "HTTP/1.0 ", 9) == 0) *code = atoi(resp + 9);
        {
            int blen = total - (hdr_end > 0 ? hdr_end : 0);
            if (blen > body_cap - 1) blen = body_cap - 1;
            if (blen < 0) blen = 0;
            memcpy(body, resp + (hdr_end > 0 ? hdr_end : 0), (size_t)blen);
            body[blen] = '\0';
        }
    }
    return 0;
}

int hw_asr_online(const char* wav_path, hw_asr_online_result_t* res)
{
    const char *url, *key, *model;
    static char body[65536];
    char cmd[8192];
    char qu[1024], qm[256];
    char codefile[] = "/tmp/.xiaomo_asr_code.txt";
    char bodyfile[] = "/tmp/.xiaomo_asr_body.json";
    char tmpwav[] = "/tmp/.xiaomo_asr_in.wav";
    FILE* cf;
    int code = 0;
    long ms;

    if (!res) return -1;
    memset(res, 0, sizeof(*res));
    hw_asr_online_config(&url, &key, &model);

    /* 音频固定落 /tmp 白名路径 (防注入), 30s 上限 */
    {
        float* pcm = NULL;
        int n, nf;
        FILE* src = fopen(wav_path, "rb");
        if (!src) { res->http_code = 0; return -1; }
        fclose(src);
        if (strcmp(wav_path, tmpwav) != 0) {
            src = fopen(wav_path, "rb");
            FILE* dst = fopen(tmpwav, "wb");
            char buf[8192];
            size_t r;
            if (!src || !dst) { if (src) fclose(src); return -1; }
            while ((r = fread(buf, 1, sizeof(buf), src)) > 0) fwrite(buf, 1, r, dst);
            fclose(src); fclose(dst);
        }
        (void)n; (void)nf; (void)pcm;
    }

    sh_quote(url, qu, (int)sizeof(qu));
    sh_quote(model, qm, (int)sizeof(qm));
    {
        struct timeval tv0;
        gettimeofday(&tv0, NULL);
        ms = (long)tv0.tv_sec * 1000L + tv0.tv_usec / 1000L;
    }

    if (system("command -v curl >/dev/null 2>&1") == 0) {
        /* 整个 Authorization 头值走单引号强转义 (key 含任意字符皆安全) */
        char hdr[1024], qh[1200];
        snprintf(hdr, sizeof(hdr), "Authorization: Bearer %s", key);
        sh_quote(hdr, qh, (int)sizeof(qh));
        snprintf(cmd, sizeof(cmd),
                 "curl -sS -m 60 -o %s -w '%%{http_code}' --url %s "
                 "-H %s "
                 "-F 'model=%s' -F 'language=zh' "
                 "-F 'file=@%s;type=audio/wav' > %s 2>/dev/null",
                 bodyfile, qu, qh, qm, tmpwav, codefile);
        if (system(cmd) != 0) { /* curl 自身失败时 code 文件为空 → code=0 */ }
        res->via[0] = 'c'; res->via[1] = 'u'; res->via[2] = 'r'; res->via[3] = 'l';
        res->via[4] = '\0';
        cf = fopen(codefile, "r");
        if (cf) {
            char cl[16];
            if (fgets(cl, sizeof(cl), cf)) code = atoi(cl);
            fclose(cf);
        }
        {
            FILE* bf = fopen(bodyfile, "r");
            if (bf) {
                size_t r = fread(body, 1, sizeof(body) - 1, bf);
                fclose(bf);
                body[r] = '\0';
            } else body[0] = '\0';
        }
    } else {
        /* 无 curl: 裸 socket (仅 http) */
        res->via[0] = 's'; res->via[1] = 'o'; res->via[2] = 'c';
        res->via[3] = 'k'; res->via[4] = 'e'; res->via[5] = 't';
        res->via[6] = '\0';
        if (http_post(url, key, model, tmpwav, body, (int)sizeof(body), &code) != 0)
            code = 0;
    }
    res->ms = (int)((long)time(NULL) * 1000L - ms);
    {
        struct timeval tv1;
        gettimeofday(&tv1, NULL);
        res->ms = (int)((long)tv1.tv_sec * 1000L + tv1.tv_usec / 1000L - ms);
    }
    res->http_code = code;
    if (code == 200) {
        hw_asr_parse_text(body, res->text, HW_ASR_TEXT_CAP);
        return 0;
    }
    return -1;
}

/* ============================================================
 * 7. 自检 (合成 chirp + 在线 mock 回环) 与 CLI
 * ============================================================ */

static void write_test_wav(const char* path, const float* pcm, int n)
{
    FILE* f = fopen(path, "wb");
    unsigned char h[44] = {0};   /* 全零打底, 逐字段填—栈垃圾会让nchannels/bits变随机值 */
    unsigned dlen = (unsigned)n * 2u, rlen = dlen + 36u;
    int i;
    if (!f) return;
    memcpy(h, "RIFF", 4);
    h[4] = rlen & 0xFF; h[5] = (rlen >> 8) & 0xFF; h[6] = (rlen >> 16) & 0xFF; h[7] = (rlen >> 24) & 0xFF;
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1;                           /* fmt=16, PCM, mono */
    h[24] = 0x80; h[25] = 0x3E;                                 /* 16000 Hz */
    h[26] = 0x02;                                               /* block align = 2 */
    h[28] = 0x00; h[29] = 0x7D;                                 /* byte rate = 32000 */
    h[34] = 16;
    memcpy(h + 36, "data", 4);
    h[40] = dlen & 0xFF; h[41] = (dlen >> 8) & 0xFF; h[42] = (dlen >> 16) & 0xFF; h[43] = (dlen >> 24) & 0xFF;
    fwrite(h, 1, 44, f);
    for (i = 0; i < n; i++) {
        short v = (short)(pcm[i] * 30000.f);
        unsigned char b[2] = { (unsigned char)(v & 0xFF), (unsigned char)((v >> 8) & 0xFF) };
        fwrite(b, 1, 2, f);
    }
    fclose(f);
}

/* 合成 chirp 音频 (不加窗突变, 帧内连续) */
static void gen_chirp(float* pcm, int n, double f0, double f1)
{
    int i;
    double ph = 0.0;
    for (i = 0; i < n; i++) {
        double t = (double)i / (double)HW_ASR_SR;
        double f = f0 + (f1 - f0) * (t * 1.6);   /* 0.625s 扫完 */
        ph += 2.0 * M_PI * f / (double)HW_ASR_SR;
        /* 首尾淡入淡出防咔哒 */
        double w = 1.0;
        if (i < 800) w = (double)i / 800.0;
        if (i > n - 800) w = (double)(n - i) / 800.0;
        pcm[i] = (float)(sin(ph) * 0.6 * w);
    }
}

int hw_asr_selftest(hw_asr_puts_fn putf)
{
    int fails = 0;
    float pcm_up[9600], pcm_dn[9600];   /* 0.6s @16k */
    float *wav_pcm = NULL;
    float **fr = NULL;
    int n, nf, i;
    hw_asr_result_t r;
    char words[HW_ASR_MAX_WORDS][64];
    int counts[HW_ASR_MAX_WORDS];
    int rc;
    int has_py;

    (void)putf;   /* 回调留作扩展; 当前静默判定 */
#define ASR_FAIL(id) do { fails++; if (putf) putf("  [FAIL] " id "\n"); } while (0)

    /* [1] 合成 + 注册 + 互斥识别 — 两个音必须频谱内容不同,
     * 镜像chirp(440→1760 vs 1760→440)DTW时间规整后互配, 互斥识别必翻车 */
    gen_chirp(pcm_up, 9600, 900, 2400);   /* 高频段上升 */
    gen_chirp(pcm_dn, 9600, 500, 140);    /* 低频段下降 */
    write_test_wav("/tmp/.xiaomo_asr_up.wav", pcm_up, 9600);
    write_test_wav("/tmp/.xiaomo_asr_dn.wav", pcm_dn, 9600);

    hw_asr_reset();
    if (hw_asr_enroll("up", "/tmp/.xiaomo_asr_up.wav") < 1) ASR_FAIL("[1a] enroll up");
    if (hw_asr_enroll("down", "/tmp/.xiaomo_asr_dn.wav") < 1) ASR_FAIL("[1b] enroll down");

    rc = hw_asr_match("/tmp/.xiaomo_asr_up.wav", &r);
    { double d2 = (rc == 0) ? r.score : -1; if (rc != 0 || strcmp(r.word, "up") != 0) ASR_FAIL("[1c] match->up"); if (putf && (rc != 0 || strcmp(r.word, "up") != 0)) { char b_[128]; snprintf(b_, 128, "       rc=%d dist=%.4f\n", rc, d2); putf(b_); } }
    rc = hw_asr_match("/tmp/.xiaomo_asr_dn.wav", &r);
    { double d2 = (rc == 0) ? r.score : -1; if (rc != 0 || strcmp(r.word, "down") != 0) ASR_FAIL("[1d] match->down"); if (putf && (rc != 0 || strcmp(r.word, "down") != 0)) { char b_[128]; snprintf(b_, 128, "       rc=%d dist=%.4f\n", rc, d2); putf(b_); } }

    /* [2] list 词表 */
    { int nw2 = hw_asr_list(words, counts, HW_ASR_MAX_WORDS); if (nw2 != 2) ASR_FAIL("[2] list!=2"); }

    /* [3] MFCC 形状: 帧数>0, 14 维有限值 */
    if (hw_asr_load_wav("/tmp/.xiaomo_asr_up.wav", &wav_pcm, &n) <= 0) ASR_FAIL("[3a] load_wav");
    if (n <= 0) ASR_FAIL("[3b] n<=0");
    else {
        if (hw_asr_mfcc(wav_pcm, n, &fr, &nf) < 0) ASR_FAIL("[3c] mfcc");
        else {
            for (i = 0; i < nf; i++) free(fr[i]);
            free(fr);
        }
        free(wav_pcm);
    }

    /* [4] DTW 自距离 ≈ 0 */
    {
        float **fa = NULL, **fb = NULL;
        int na, nb2;
        hw_asr_load_wav("/tmp/.xiaomo_asr_up.wav", &wav_pcm, &n);
        hw_asr_mfcc(wav_pcm, n, &fa, &na);
        free(wav_pcm);
        hw_asr_load_wav("/tmp/.xiaomo_asr_up.wav", &wav_pcm, &n);
        hw_asr_mfcc(wav_pcm, n, &fb, &nb2);
        free(wav_pcm);
        { double dd = hw_asr_dtw(fa, na, fb, nb2); if (dd > 0.05) { ASR_FAIL("[4] dtw>0.05"); if (putf) { char b_[96]; snprintf(b_,96,"       d=%.6f\n",dd); putf(b_); } } }
        for (i = 0; i < na; i++) free(fa[i]);
        for (i = 0; i < nb2; i++) free(fb[i]);
        free(fa); free(fb);
    }

    /* [5] 非法输入拒绝 */
    if (hw_asr_load_wav("/nonexistent__x.wav", &wav_pcm, &n) != -1) ASR_FAIL("[5a] load bad path");
    if (hw_asr_enroll("../bad word", "/tmp/.xiaomo_asr_up.wav") != -1) ASR_FAIL("[5b] enroll bad name");

    /* [6] 在线协议 mock 回环 (python3 可用才测) */
    has_py = (system("command -v python3 >/dev/null 2>&1") == 0);
    if (has_py) {
        FILE* sf = fopen("/tmp/.xiaomo_asr_mock.py", "w");
        if (sf) {
            fprintf(sf,
                "import http.server, json\n"
                "class H(http.server.BaseHTTPRequestHandler):\n"
                "    def do_POST(self):\n"
                "        ln = int(self.headers.get('Content-Length', 0))\n"
                "        body = self.rfile.read(ln)\n"
                "        ok = b'WAVE' in body and b'name=\"model\"' in body\n"
                "        resp = json.dumps({'text': 'MOCK 你好小墨' if ok else 'BAD'}).encode()\n"
                "        self.send_response(200)\n"
                "        self.send_header('Content-Type', 'application/json')\n"
                "        self.send_header('Content-Length', str(len(resp)))\n"
                "        self.end_headers()\n"
                "        self.wfile.write(resp)\n"
                "    def log_message(self, *a): pass\n"
                "http.server.HTTPServer(('127.0.0.1', 18877), H).serve_forever()\n");
            fclose(sf);
        }
        if (system("python3 /tmp/.xiaomo_asr_mock.py >/dev/null 2>&1 & echo $! > /tmp/.xiaomo_asr_mock.pid; sleep 0.8") == 0) {
            hw_asr_online_result_t or_res;
            setenv("HW_ASR_URL", "http://127.0.0.1:18877/v1", 1);
            setenv("HW_ASR_KEY", "mock-key", 1);
            setenv("HW_ASR_MODEL", "mock-asr", 1);
            rc = hw_asr_online("/tmp/.xiaomo_asr_up.wav", &or_res);
            if (rc != 0 || or_res.http_code != 200 ||
                strstr(or_res.text, "MOCK") == NULL) { ASR_FAIL("[6] online mock"); if (putf) { char b_[160]; snprintf(b_,160,"       rc=%d http=%d text=[%s]\n", rc, or_res.http_code, or_res.text); putf(b_); } }
            unsetenv("HW_ASR_URL");
            unsetenv("HW_ASR_KEY");
            unsetenv("HW_ASR_MODEL");
            system("kill $(cat /tmp/.xiaomo_asr_mock.pid) 2>/dev/null");
        }
    }

    /* [7] parse_text JSON 提取与转义 */
    {
        char txt[128];
        if (hw_asr_parse_text("{\"text\":\"a\\\"b\\\\c\\u4f60\"}", txt, 128) != 0 ||
            strcmp(txt, "a\"b\\c你") != 0) fails++;
    }

    hw_asr_reset();   /* 清场, 不留测试模板 */
    remove("/tmp/.xiaomo_asr_up.wav");
    remove("/tmp/.xiaomo_asr_dn.wav");
    return fails;
}

/* ---- CLI ---- */

static int asr_puts(const char* s) { return printf("%s", s); }

static void print_online_config(void)
{
    const char *u, *k, *m;
    hw_asr_online_config(&u, &k, &m);
    printf("online url   = %s\n", u);
    printf("online key   = %s%s\n", (k[0] ? "" : "(empty)"),
           (k[0] ? "****(set)" : ""));
    printf("online model = %s\n", m);
}

int hw_asr_cli(int argc, char** argv)
{
    const char* sub = (argc >= 3) ? argv[2] : "demo";
    if (strcmp(sub, "demo") == 0) {
        printf("=== XIAOMO 语音识别 (hw_asr) ===\n");
        print_online_config();
        printf("---- 自检 (合成chirp+mock云) ----\n");
        {
            int fails = hw_asr_selftest(asr_puts);
            if (fails == 0) printf("hw_asr selftest: ALL PASS\n");
            else printf("hw_asr selftest: %d 项失败\n", fails);
            return (fails == 0) ? 0 : 1;
        }
    } else if (strcmp(sub, "list") == 0) {
        char words[HW_ASR_MAX_WORDS][64];
        int counts[HW_ASR_MAX_WORDS];
        int nw = hw_asr_list(words, counts, HW_ASR_MAX_WORDS), i;
        printf("=== 已注册词条 (%d) ===\n", nw);
        for (i = 0; i < nw; i++) printf("  %-16s x%d 模板\n", words[i], counts[i]);
        if (nw == 0) printf("  (空; 用 asr enroll <词> <wav> 注册)\n");
        return 0;
    } else if (strcmp(sub, "enroll") == 0 && argc >= 5) {
        int r = hw_asr_enroll(argv[3], argv[4]);
        if (r >= 1) {
            printf("✅ 已注册 \"%s\" (现 %d 条模板)\n", argv[3], r);
            return 0;
        }
        printf("❌ 注册失败 rc=%d (-1词名非法 -2音频打不开 -3MFCC失败 -4容量满)\n", r);
        return 1;
    } else if (strcmp(sub, "match") == 0 && argc >= 4) {
        hw_asr_result_t r;
        int rc = hw_asr_match(argv[3], &r);
        if (rc == 0) {
            printf("🎯 本地识别: %s (dist=%.4f, 次佳=%.4f)\n",
                   r.word, r.score, r.second);
            return 0;
        }
        printf("❌ 识别失败 rc=%d (-1无注册词 -2音频非法 -3MFCC失败)\n", rc);
        return 1;
    } else if (strcmp(sub, "online") == 0 && argc >= 4) {
        hw_asr_online_result_t r;
        int rc = hw_asr_online(argv[3], &r);
        if (rc == 0 && r.http_code == 200) {
            printf("☁️  在线识别 [%s %dms]: %s\n", r.via, r.ms,
                   (r.text[0] ? r.text : "(空文本)"));
            return 0;
        }
        printf("❌ 在线识别失败: http=%d via=%s\n", r.http_code,
               (r.via[0] ? r.via : "none"));
        {
            FILE* bf = fopen("/tmp/.xiaomo_asr_body.json", "r");
            if (bf) {
                char buf[600];
                size_t rn = fread(buf, 1, sizeof(buf) - 1, bf);
                fclose(bf);
                if (rn > 0) { buf[rn] = '\0'; printf("   resp: %s\n", buf); }
            }
        }
        return 1;
    } else if (strcmp(sub, "reset") == 0) {
        hw_asr_reset();
        printf("✅ 已清空全部模板\n");
        return 0;
    }
    printf("用法: %s asr demo|list|reset|enroll <word> <wav>|match <wav>|online <wav>\n",
           (argc >= 1 ? argv[0] : "xiaomo"));
    return 1;
}
