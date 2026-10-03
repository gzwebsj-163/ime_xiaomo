/**
 * btn_trace_demo.c — 打印「一次手势 → 事件序列」的原始轨迹（排查用，非测试）
 *
 * 用途：把用户的真实手势（含按住时的慢速瞬断）喂给解码器，
 *       看它到底吐出几个事件、分别在第几毫秒。用来定位
 *       「长按 UP 结果光标先跳一格」这类问题的根因。
 *
 * 编译: cc -std=c11 -O1 -I../main btn_trace_demo.c -o /tmp/bt && /tmp/bt
 */
#include <stdio.h>
#include <string.h>
#include "btn_decode.h"

#define LONG_MS 1200

typedef struct { int a, b; } span_t;

/* 用户手势：按下 UP 不放 5 秒，中途触点慢速瞬断（真机实测约 180ms 高电平） */
static const span_t HOLD_UP_5S[] = { {100,400},{580,900},{1080,1500},{1680,5100} };

/* 单击 50ms */
static const span_t TAP[] = { {100,150} };

static int down(const span_t *sp, int n, int t)
{
    for (int i = 0; i < n; i++) if (t >= sp[i].a && t < sp[i].b) return 1;
    return 0;
}

static void trace(const char *title, const span_t *sp, int n, int total_ms)
{
    btn_dec_t s; memset(&s, 0, sizeof(s));
    printf("\n【%s】\n  t(ms) : 电平\n", title);
    int ne = 0;
    for (int t = 0; t <= total_ms; t += BTN_POLL_MS) {
        int raw = down(sp, n, t);
        int ev  = btn_decode_step(&s, (uint8_t)raw, (int64_t)t * 1000, LONG_MS);
        if (ev != BTN_EV_NONE) {
            /* 打印事件前 400ms 的波形，看清触发时刻的上下文 */
            printf("  %5d : %s   <-- 事件 #%d: %s  (按住 %dms)\n", t, raw ? "▔按下" : "＿松开",
                   ++ne, ev == BTN_EV_SHORT ? "短按" : "长按", t - (int)0);
        }
    }
    printf("  => 共 %d 个事件\n", ne);
}

int main(void)
{
    printf("解码参数：按下确认 %dms / 抬起确认 %dms / 真松开 %dms / 长按 %dms\n",
           BTN_PRESS_N * BTN_POLL_MS, BTN_RELEASE_N * BTN_POLL_MS,
           BTN_TRUE_RELEASE_N * BTN_POLL_MS, LONG_MS);
    trace("手势A：单击 UP", TAP, 1, 1200);
    trace("手势B：按住 UP 不放 5 秒（中途慢速瞬断）", HOLD_UP_5S, 4, 6000);
    printf("\n判定标准：手势B 必须 **只有 1 个长按**，\n"
           "          若先出现「短按」= 光标会先跳一格（用户实测的 bug）。\n");
    return 0;
}
