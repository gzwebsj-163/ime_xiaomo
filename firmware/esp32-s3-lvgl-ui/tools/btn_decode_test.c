/**
 * btn_decode_test.c — 按键解码状态机的宿主单元测试（不接真机、不接 RTOS）
 *
 * 为什么需要它：
 *   长按不触发的 bug 出在「物理触点 → 事件队列」这一段，
 *   而往队列里注入合成事件的自检（BTN_SELFTEST）注入点在这段**之后**，
 *   结构上抓不到它。这里直接喂"电平波形"给同一个解码器源码（btn_decode.h），
 *   包括真机上复现出来的「按住时慢速瞬断」波形。
 *
 * 编译运行: cc -std=c11 -O1 -I../main btn_decode_test.c -o /tmp/t && /tmp/t
 */
#include <stdio.h>
#include <string.h>

#include "btn_decode.h"

#define LONG_MS 1200                    /* 与 btn.h BTN_LONG_PRESS_MS 一致 */

typedef struct { int a, b; } span_t;     /* 按下区间 [a, b)，单位 ms */

typedef struct {
    const char *name;
    const span_t *sp;
    int n;
    int total_ms;
    int want_short;
    int want_long;
    const char *note;
} case_t;

/* ---- 波形定义 ---- */
static const span_t w_click[]  = { {100, 150} };                          /* 单击 */
static const span_t w_5s[]     = { {100, 5100} };                         /* 按住 5 秒 */
static const span_t w_flick[]  = { {100,400},{580,900},{1080,1500},{1680,4000} };
static const span_t w_flick2[] = { {100,400},{580,900},{1080,1500},{1680,4000},
                                   {4200,4400},{4600,4800},{5000,5200} };
static const span_t w_bounce[] = { {100,160},{165,175},{180,250} };  /* 按住中 5ms 级瞬断 */
static const span_t w_twice[]  = { {100,150},{1000,1050} };
static const span_t w_dbl[]    = { {100,150},{250,300} };
static const span_t w_tooshort[] = { {100,120} };                    /* 轻触 20ms */

static const case_t CASES[] = {
    { "单击                         ", w_click,  1, 3000, 1, 0, "1 短按（抬手后 300ms 上报）" },
    { "长按 5 秒（干净接触）        ", w_5s,     1, 7000, 0, 1,
      "🚨 定版：长按是锁定触发 → **只有长按，没有短按**（旧版会先蹦一格）" },
    { "长按 5 秒（按住中慢速瞬断）  ", w_flick,  4, 5000, 0, 1,
      "🐛 旧版：12 个短按 或 1短+1长；定版：**只有 1 个长按**" },
    { "长按 5 秒 + 尾段持续抖动     ", w_flick2, 7, 6500, 0, 1,
      "静默期吞掉尾部抖动 → 仍是 1 个长按，不蹦格" },
    { "按住中触点 5ms 级瞬断        ", w_bounce, 3, 2000, 1, 0,
      "瞬断不打断「按住」状态 → 未达阈值 → 抬手后 1 短按" },
    { "两次单击（间隔 850ms）       ", w_twice,  2, 3000, 2, 0, "2 短按" },
    { "快速双击（间隔 100ms）       ", w_dbl,    2, 2000, 1, 0,
      "⚠️ 已知副作用：<300ms 的连按合并为 1 短按（实测按压节拍 >400ms，够用）" },
    { "轻触 20ms（< 去抖窗口）      ", w_tooshort, 1, 1500, 0, 0,
      "⚠️ 有意取舍：轻触被当噪声吃掉。人手实际按压 50ms+，不受影响" },
};

static int pressed_at(const case_t *c, int t_ms)
{
    for (int i = 0; i < c->n; i++)
        if (t_ms >= c->sp[i].a && t_ms < c->sp[i].b) return 1;
    return 0;
}

static int run_case(const case_t *c, char *trace, size_t trace_sz)
{
    btn_dec_t s;
    memset(&s, 0, sizeof(s));
    s.last_raw = 0;
    s.stable   = 0;

    int ns = 0, nl = 0;
    size_t off = 0;
    trace[0] = 0;

    for (int t = 0; t <= c->total_ms; t += BTN_POLL_MS) {
        const int ev = btn_decode_step(&s, (uint8_t)pressed_at(c, t),
                                       (int64_t)t * 1000, LONG_MS);
        if (ev == BTN_EV_NONE) continue;
        if (ev == BTN_EV_SHORT) ns++;
        else                    nl++;
        off += (size_t)snprintf(trace + off, trace_sz - off, " %s@%dms",
                                (ev == BTN_EV_SHORT) ? "短按" : "长按", t);
        if (off >= trace_sz) break;
    }
    return (ns == c->want_short && nl == c->want_long) ? 1 : 0;
}

int main(void)
{
    const int n = (int)(sizeof(CASES) / sizeof(CASES[0]));
    int pass = 0;
    char trace[256];

    printf("\n按键解码状态机 宿主单元测试（按下确认 %dms / 抬起确认 %dms / "
           "真松开 %dms / 长按 %dms）\n", BTN_PRESS_N * BTN_POLL_MS,
           BTN_RELEASE_N * BTN_POLL_MS, BTN_TRUE_RELEASE_N * BTN_POLL_MS, LONG_MS);
    printf("%s\n", "---------------------------------------------------------------------------------");

    for (int i = 0; i < n; i++) {
        const int ok = run_case(&CASES[i], trace, sizeof(trace));
        if (ok) pass++;
        printf("[%s] %s  期望 %d短/%d长 | 实测:%s\n",
               ok ? "PASS" : "FAIL", CASES[i].name,
               CASES[i].want_short, CASES[i].want_long,
               trace[0] ? trace : " (无事件)");
        printf("       └ %s\n", CASES[i].note);
    }

    printf("%s\n", "---------------------------------------------------------------------------------");
    printf("结果: %d/%d 通过\n\n", pass, n);
    return (pass == n) ? 0 : 1;
}
