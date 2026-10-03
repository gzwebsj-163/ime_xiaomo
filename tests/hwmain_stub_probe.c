/* hw_main 双形态对拍的**探针桩**: 给 9 个弱符号供真定义
 * ------------------------------------------------------------------
 * 存在理由: macOS ld64 不把 weak undefined 解析为 0 (clang+gcc 双验,
 *   3 行最小复现) -> hw_main.c 单独链接必失败, 必须有人提供定义。
 *   而拉真模块会拖进整个 VM (hw_direct(kvm_*) / hw_oem(kprog_*)),
 *   把与 hw_main 无关的变量掺进双形态对拍。
 *
 * ⚠️ 纪律: 桩必须让 9 个探针**全部 OK** (阳性对照)。
 *   首版桩让 HEX 报 BAD —— 因为 main_probe_hex 要求
 *   fmt64/parse64 构成**往返闭环**, 而桩返回 NULL, 探针按契约判 BAD。
 *   那是桩违约, 不是被测物缺陷。教训: 桩错了会被读成"被测物坏了"。
 * ------------------------------------------------------------------ */
#include <string.h>
#include "hw_main.h"
/* 🔴 桩必须 include【真头】usbpd/include/hw_usbpd.h, 不能抄被测物的签名。
 *    放在下方 extern "C" 【之外】 —— 真头自带 linkage 守卫(其 39-40 行),
 *    与 hw_main.c 的做法一致; 若放进 extern "C" 内则出现嵌套 extern "C"。 */
#include "hw_usbpd.h"

/* 🕳️ 双形态对拍必踩: 桩被复制成 .cpp 编译时, 桩函数会被 C++ mangle
 *    成 __Z10hw_oem_sigv, 而 hw_main.c 里的弱声明是【C 链接】的
 *    _hw_oem_sig —— 名字对不上, 10 个符号全部 undefined, 矩阵第4段
 *    六模式全军覆没 (而第1段因为桩是 .c 仍过, 极具迷惑性)。
 *    修法: extern "C" 守卫包住全部桩定义, 一处修好两形态。
 *    纪律: 桩不是"附属品", 它进了对拍链路, 就是被测链路的一环。 */
#ifdef __cplusplus
extern "C" {
#endif

/* ================= 探针桩的【三档剖面】=================
 * 🕳️ 为什么需要剖面 (锚点 D 阴性对照 / 锚点 H 不具区分力的信号):
 *   原先只有"全好"一档。于是 9 个探针在矩阵里【全部只跑过 OK 这一条路】——
 *   哪怕 main_probe_asr 里那句 `>= 0` 恒真、哪怕某探针根本接错了符号,
 *   矩阵都会照样全绿。阳性对照只能证"桩能让它绿", 证不了"它会红"。
 *   => 纪律: 任何只测过"好"的探针都还没被验过。
 *
 *   STUB_BREAK(i) = 本桩把第 i 号类的契约【违反】掉
 *     STUB_BAD_ALL  = 1 -> 8 个可判别类全违反
 *     STUB_BAD_ONE  = k -> 只违反第 k 号 (逐探针独立对照用)
 *
 * ⚠️ 2 号 DEV 是【结构性无区分力】的, 见下方 main_probe_dev 的说明:
 *   底层 hw_dev_registered() 是无失败态的纯 getter, 不存在 BAD 状态,
 *   故它在任何剖面下都保持 OK。本桩不伪造判据, 由矩阵如实钉住这个事实。
 */
#ifndef STUB_BAD_ALL
#define STUB_BAD_ALL 0
#endif
#ifndef STUB_BAD_ONE
#define STUB_BAD_ONE (-1)
#endif
#define STUB_BREAK(i) (STUB_BAD_ALL || (STUB_BAD_ONE) == (i))

/* HEX: 往返闭环。探针喂 0x123456789ABCDEF0 期望原值吐回。 */
char* hw_hex_fmt64(unsigned long long v, char* buf, int n)
{
    static const char* d = "0123456789ABCDEF";
    int i;
    if (STUB_BREAK(5)) return 0;     /* 契约违反: 装得下也拒收 -> 探针须判 BAD */
    if (n < 17) return 0;            /* 契约: 装不下就失败 */
    for (i = 15; i >= 0; i--) { buf[i] = d[v & 0xF]; v >>= 4; }
    buf[16] = 0;
    return buf;
}
unsigned long long hw_hex_parse64(const char* s)
{
    unsigned long long v = 0; int i;
    for (i = 0; i < 16; i++) {
        char c = s[i]; unsigned k;
        if (c >= '0' && c <= '9') k = (unsigned)(c - '0');
        else if (c >= 'A' && c <= 'F') k = (unsigned)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | k;
    }
    return v;
}

/* OEM: 熔丝签名黄金 (与 hw_oem.c 一致) */
unsigned long long hw_oem_sig(void)
{
    if (STUB_BREAK(6)) return 0;     /* 契约违反: 签名不符 */
    return 0x5849414F4D4F3031ULL;
}

/* ASR: 剧目列表, max=0 只读不写; 返回 >=0 即健康 */
int hw_asr_list(char (*names)[64], int* max_out, int max_in)
{
    (void)names;
    if (max_out && max_in > 0) *max_out = 0;
    if (STUB_BREAK(0)) return -1;   /* 契约违反: 负数 = 不健康 */
    return 3;                        /* 健康: 非负 */
}

/* CORE / FAULT / TOKEN: selftest 返回 0 = 全过 */
int hw_core_selftest(int (*p)(const char*))
{ (void)p; if (STUB_BREAK(1)) return 1;  return 0; }
int hw_fault_selftest(int (*p)(const char*))
{ (void)p; if (STUB_BREAK(4)) return 1;  return 0; }
int hw_token_selftest(int (*p)(const char*))
{ (void)p; if (STUB_BREAK(7)) return 1;  return 0; }

/* DEV: 注册数 >= 0 恒真 */
unsigned int hw_dev_registered(void)
{
    (void)STUB_BREAK(2);   /* 故意【不】用它改返回值 —— 见文件头剖面说明 */
    return 1;
}

/* DIRECT: 只读最近错误串 */
const char* hw_get_error(void)
{
    if (STUB_BREAK(3)) return 0;   /* 契约违反: NULL = 取不到 */
    return "";
}

/* USBPD: 唯一真表出口, 指针非 NULL 且 n>=4 (与 selftest [1] 同判据) */
/* 🔴 首版这里写的是 `const void* hw_usbpd_qc_table(int*)` —— 与 hw_main.c 里的
 *    抹型弱声明【同型】, 于是二者互相"合法", 冲突永远暴露不出来:
 *    桩跟着被测物一起错, 对拍就成了一台复印机。
 *    纪律: 桩的签名必须锚定【真头】(usbpd/include/hw_usbpd.h), 不能抄被测物。
 *    锚定后本桩与真模块可互换(真模块也 include 真头), 差在只有 4 行。
 *
 * 🕳️ 第二版写的是 `{{0,0,0,0,0,0}}` —— 6 个初值是【猜的】, 真结构只有
 *    3 个字段 (dp/dm/v)。因为值不被解引用, 编译器对多余初值只告警不报错,
 *    差一点就带着"看起来能跑"的姿态混进矩阵。
 *    现改【指定初始化器】: 字段一旦改名或删除, 桩【编译失败】。
 *    纪律: 选一个能当金丝雀的写法 —— {0} 只保证"能编译", 指定初始化器
 *    才保证"字段还对得上"。 */
const hw_usbpd_qc_row_t* hw_usbpd_qc_table(int* n_rows)
{
    static const hw_usbpd_qc_row_t tbl[4] = {
        { .dp = 0, .dm = 0, .v = HW_USBPD_UNKNOWN },
        { .dp = 0, .dm = 0, .v = HW_USBPD_UNKNOWN },
        { .dp = 0, .dm = 0, .v = HW_USBPD_UNKNOWN },
        { .dp = 0, .dm = 0, .v = HW_USBPD_UNKNOWN },
    };   /* 只需非空 + 行数>=4; 探针从不解引用, 故取值无关 */
    if (STUB_BREAK(8)) {            /* 契约违反: 表塌成空 -> 探针须判 BAD */
        if (n_rows) *n_rows = 0;
        return 0;
    }
    if (n_rows) *n_rows = 4;
    return tbl;
}

#ifdef __cplusplus
}
#endif
