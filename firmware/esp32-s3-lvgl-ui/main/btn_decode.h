/**
 * btn_decode.h — 「电平 → 按键事件」解码状态机（纯逻辑，零 RTOS/驱动依赖）
 *
 * 为什么单独拆出来：
 *   长按不触发这类 bug 出在「物理触点 → 事件队列」这一段。
 *   往队列里注入合成事件的自检**结构上抓不到它**（注入点已在这段之后）。
 *   拆成纯函数后，可以用宿主单元测试喂合成波形（含瞬断/弹跳）来验证，
 *   而不是靠真机手按碰运气。测试见 tools/btn_decode_test.c
 *
 * 🕳️ 实机踩过的坑（2026-09-29，真机日志实锤）：
 *   按下沿和抬起沿用同一个 60ms 窗口 → 只挡得住毫秒级弹跳。
 *   按住不放时触点慢速瞬断（实测约 180ms 高电平）被当成
 *   「抬起 + 重新按下」→ 按下时刻被反复重置 → 长按计时器永远凑不满
 *   → 一次 5 秒长按被拆成 12 个「短按」事件，长按形同虚设。
 *   日志：19649/20549/21150/22050/… 每 300~900ms 一个 UP 短按。
 *
 * 修法（三重保险，全部在下面这一个函数里）：
 *   ① 非对称去抖：按下 30ms 就认，抬起要连续 300ms 才认 → 瞬断被当噪声吞掉，
 *      不结束「按住」状态、不重置长按计时器。
 *   ② 长按后静默（mute）：长按事件一旦发出，该键不再产生任何事件，
 *      直到检测到「真松开」（连续 500ms 高电平）→ 长按尾部的抖动不会变成乱翻页。
 *   ③ 副作用（明说）：同一键连按的快慢有限制 —— 松开后 <300ms 内再按会被
 *      当作同一次按住，即连按间隔需 ≥300ms 才会计数。
 *
 * 🚨 第二轮修正（同日，用户实测："长按必须锁定触发，并不是跳上一个"）：
 *   原实现在「按下瞬间」就上报短按 → 长按手势必然先附带一次短按：
 *     长按 UP  =  光标先上移一格（menu_move）  +  1.2s 后进入下一级（menu_enter）
 *   用户看到的正是"光标被跳走一格"。实机/宿主轨迹实锤：
 *     按住 UP 5 秒 → 短按@130ms + 长按@1340ms
 *   ⇒ 改为「一点判定」教科书语义：
 *      · 按下：只记时间，**不出事件**
 *      · 抬起（且时长 < 长按阈值）：出 1 个短按 —— **锁定、一次性**
 *      · 到达阈值：出 1 个长按，之后静默到真松开
 *   ⇒ 长按手势永远只出 1 个事件，绝不会先动光标。
 *   ⚠️ 代价：单击改为抬手后约 300ms 生效（那 300ms 就是用来吞瞬断的抗噪预算）。
 */
#pragma once

#include <stdint.h>

/* ---- 时间参数（毫秒）---- */
#define BTN_POLL_MS        10    /* 采样周期 */
#define BTN_PRESS_N         3    /* 3×10ms  =  30ms 按下确认 */
#define BTN_RELEASE_N      30    /* 30×10ms = 300ms 抬起确认（瞬断静默期） */
#define BTN_TRUE_RELEASE_N 50    /* 50×10ms = 500ms 判定「真松开」（解除静默） */

typedef struct {
    uint8_t  last_raw;     /* 上次原始电平（1=按下） */
    uint8_t  stable;       /* 去抖后的稳定电平 */
    uint16_t cnt;          /* 连续一致采样计数 */
    uint8_t  holding;      /* 是否处于「按住中」（含瞬断静默期内） */
    uint8_t  long_sent;    /* 本次按住的长按事件是否已上报 */
    uint8_t  mute;         /* 长按已发出 → 静默，等真松开 */
    int64_t  press_us;     /* 本次按住的起始时刻 */
} btn_dec_t;

enum { BTN_EV_NONE = 0, BTN_EV_SHORT = 1, BTN_EV_LONG = 2 };

/** 每 BTN_POLL_MS 调用一次。返回本次产生的事件（BTN_EV_*）。 */
static inline int btn_decode_step(btn_dec_t *s, uint8_t raw, int64_t now_us,
                                  int long_ms)
{
    /* 连续一致采样计数 */
    if (raw != s->last_raw) {
        s->last_raw = raw;
        s->cnt = 0;
    } else if (s->cnt < 60000) {
        s->cnt++;
    }

    if (raw && !s->stable) {
        /* ---------- 按下沿确认 ---------- */
        if (s->cnt < BTN_PRESS_N) return BTN_EV_NONE;   /* 还没稳定，继续等 */

        s->stable  = 1;
        s->holding = 1;                                 /* 先立"按住"旗，瞬断也拔不掉 */

        if (s->mute) return BTN_EV_NONE;                /* ② 长按余波：吞掉 */

        s->long_sent = 0;                               /* 全新的按住手势 */
        s->press_us  = now_us;

        /* 🚨 关键修正（2026-09-29 第二轮）：
         * 按下瞬间 **不再** 上报短按。否则「长按」必然先附带一次短按：
         *   长按 UP → 先 menu_move(-1) 光标跳一格 → 再到 1.2s 才 menu_enter()
         * 用户看到的现象就是"长按会把选中项先跳走一格"。
         * 短按改到「抬起」时判定 —— 见下面的抬起分支。 */
        return BTN_EV_NONE;
    }

    if (!raw && s->stable) {
        /* ---------- 抬起沿确认 ---------- */
        if (s->cnt < BTN_RELEASE_N) return BTN_EV_NONE; /* 慢速瞬断：吞掉，继续按住 */

        const uint8_t was_holding = s->holding;         /* 先取：本手势是否有效 */
        s->stable  = 0;
        s->holding = 0;

        /* 刚跑完一次长按手势 → 不补短按（否则长按后再蹦一格） */
        if (s->mute || s->long_sent) return BTN_EV_NONE;

        /* 上电时就一直被按住（没经过按下沿）→ 不算有效手势 */
        if (!was_holding) return BTN_EV_NONE;

        /* 完整的「按下 → 抬起」手势，且时长没够到长按阈值
         * ⇒ 此刻才上报短按：**锁定、一次性**，绝不与长按混在一起。
         * 代价：单击在抬手后约 BTN_RELEASE_N(300ms) 才生效（换取抗瞬断）。 */
        return BTN_EV_SHORT;
    }

    if (!raw && !s->stable) {
        /* 已确认抬起：连续够久 = 真松开 → 解除长按静默 */
        if (s->mute && s->cnt >= BTN_TRUE_RELEASE_N) s->mute = 0;
        return BTN_EV_NONE;
    }

    /* ---------- 按住中：长按判定 ---------- */
    if (s->holding && !s->long_sent &&
        (now_us - s->press_us) > (int64_t)long_ms * 1000) {
        s->long_sent = 1;
        s->mute      = 1;                               /* ② 进入静默 */
        return BTN_EV_LONG;
    }

    return BTN_EV_NONE;
}
