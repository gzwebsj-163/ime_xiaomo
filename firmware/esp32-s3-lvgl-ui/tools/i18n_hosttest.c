/**
 * i18n 宿主自检 —— 不依赖 ESP-IDF，用系统编译器直接跑。
 *
 * 为什么要它：i18n.c 是生成产物，光靠「编译过」证明不了
 *   · 双语切换是否真的取到不同字符串
 *   · 越界索引 / 坏语言值是否回落 MISSING 而不是读越界内存
 * 这两类问题在真机上表现为随机崩溃，极难复现。宿主上跑是秒级的。
 *
 * 判据：输出全对 + 编译零警告（-Wall -Wextra）。
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "i18n.c"

static int fails = 0;
static void chk(const char *what, const char *got, const char *want)
{
    if (got && want && strcmp(got, want) == 0) { printf("  ok   %s\n", what); return; }
    printf("  FAIL %s: got=%s want=%s\n", what, got ? got : "(null)", want ? want : "(null)");
    fails++;
}

int main(void)
{
    printf("[i18n host test] strings=%d\n", UI_STR_COUNT);
    if (UI_STR_COUNT < 1) { printf("  FAIL 表为空\n"); return 1; }

    /* 1. 出厂默认必须是中文（用户明确要求「默认全部中文」） */
    g_ui_lang = UI_LANG_ZH;
    chk("默认语言=中文", ui_tr(TID_M_PROBE), "探针检测");
    /* 2. 切英文必须换成英文 */
    g_ui_lang = UI_LANG_EN;
    chk("切英文生效", ui_tr(TID_M_PROBE), "Probe Detect");
    /* 3. 切回必须回得来（不能是单向棘轮） */
    g_ui_lang = UI_LANG_ZH;
    chk("切回中文", ui_tr(TID_S_OK), "确定");
    /* 4. 越界索引必须回落 MISSING */
    chk("越界索引", ui_tr(9999), UI_STR_MISSING);
    chk("负索引形态", ui_tr(UI_STR_COUNT), UI_STR_MISSING);
    /* 5. 坏语言值（NVS 被写坏）必须回落，不得越界读表 */
    g_ui_lang = 99;
    chk("坏语言值", ui_tr(TID_S_OK), UI_STR_MISSING);
    g_ui_lang = 0xFF;
    chk("坏语言值255", ui_tr(TID_S_OK), UI_STR_MISSING);
    /* 6. key 查表 */
    g_ui_lang = UI_LANG_ZH;
    if (ui_tr_id("S_OK") == 0) printf("  ok   ui_tr_id(S_OK)\n"); else { printf("  FAIL ui_tr_id(S_OK)\n"); fails++; }
    if (ui_tr_id("NO_SUCH_KEY") == 0xFFFF) printf("  ok   ui_tr_id 未命中\n"); else { printf("  FAIL 未命中应 0xFFFF\n"); fails++; }
    /* 7. 语言范围自洽 */
    if (UI_LANG_FIRST == 0 && UI_LANG_LAST == 1 && UI_LANG_COUNT == 2) printf("  ok   语言范围\n");
    else { printf("  FAIL 语言范围 FIRST=%d LAST=%d COUNT=%d\n", UI_LANG_FIRST, UI_LANG_LAST, UI_LANG_COUNT); fails++; }
    /* 8. MISSING 串必须非空（空串在屏上表现为「标签凭空消失」） */
    if (UI_STR_MISSING && UI_STR_MISSING[0]) printf("  ok   MISSING 非空\n"); else { printf("  FAIL MISSING 为空\n"); fails++; }

    printf("[i18n host test] fails=%d\n", fails);
    return fails ? 1 : 0;
}
