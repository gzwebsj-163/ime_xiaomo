/**
 * dmc_slave.h — 板载 DMC 从设备仿真器（补上「建链成功路径」的真机证据）
 *
 * ┌─ 为什么需要它 ────────────────────────────────────────────────────────┐
 * │ 常驻建链模式里 PH_UP（在线保活）**一次都没跑过** —— 真实引脚模式下没有
 * │ 从设备，握手必然失败，60 秒只能证「失败路径 + 退避重连在跑」。
 * │ 要证成功路径就必须有对端。板载仿真器 = 用**另一组真实 UART** 当对端，
 * │ 让主机帧真正走「引脚 → UART 外设 → 波特率采样 → CRC 校验 → 应答」。
 * └───────────────────────────────────────────────────────────────────────┘
 *
 * ┌─ 接线两种模式（电气上等价，证据强度不同）─────────────────────────────┐
 * │  DMC_SLAVE_LINK=0（默认，免接线）                                       │
 * │      主机 UART1: TX=IO17 → RX=IO18                                     │
 * │      从机 UART2: TX=IO18 ← RX=IO17                                     │
 * │      走 ESP32-S3 的 **GPIO Matrix**：IO17 这个 pad 由 UART1 驱动、       │
 * │      同时被 UART2 的 RX 采样；IO18 反之。电路上等于 TX 与 RX 对接，      │
 * │      但没有一根线。⇒ 可立刻跑，但**没有外部波形**。                      │
 * │                                                                        │
 * │  DMC_SLAVE_LINK=1（外部跳线，最硬的证据）                               │
 * │      从机 UART2: TX=IO8 / RX=IO9，需用户接两根杜邦线：                  │
 * │          IO17 ──→ IO9   (主机发 → 从机收)                              │
 * │          IO8  ──→ IO18  (从机发 → 主机收)                              │
 * │      此时可在 **任意节点接示波器**（DM40A）看到真实波形。                 │
 * │      ⚠️ 两种模式互斥：LINK=1 时不可再短接 IO17↔IO18，                   │
 * │         否则主机 TX 的输出会与从机 TX 的输出在 IO18 上对打。              │
 * └───────────────────────────────────────────────────────────────────────┘
 *
 * ┌─ 三种应答模式 = 三条可区分的实验臂（阳性 + 阴性 + 故障注入）──────────┐
 * │  DMC_SLAVE_MODE=0  正常应答 → 主: ok↑ reconnect↑        crc_err=0       │
 * │  DMC_SLAVE_MODE=1  全程静默 → 主: ok=0 reconnect=0      crc_err=0 to_err↑│
 * │  DMC_SLAVE_MODE=2  破坏 CRC → 主: ok=0 reconnect=0      crc_err↑ to_err↑ │
 * │  三臂统计签名**互不相同** ⇒ 只靠日志就能判定「到底发生了什么」，         │
 * │  且 MODE=2 证明 CRC 校验在真实外设链路后**依然生效**（不是摆设）。        │
 * └───────────────────────────────────────────────────────────────────────┘
 *
 * ⚠️ 从机**不使用** hw_dmc 的链路 API（BSP 是全局单例，主从同用会互相踩
 *    收发缓冲，见 app_main.c 注释）。它只调用模块的**纯函数** hw_dmc_pack /
 *    hw_dmc_unpack / hw_dmc_crc16 —— 即线格式与校验算法仍以模块为唯一真相源，
 *    只是状态机与字节收发由从机自己驱动（这本来就是「另一个设备」该有的样子）。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 编译期配置（缺省值 = 免接线 + 正常应答）---- */
#ifndef DMC_SLAVE_ON
#define DMC_SLAVE_ON 1          /* 1=装板载从机  0=不装（纯对外部署形态）*/
#endif
#ifndef DMC_SLAVE_LINK
#define DMC_SLAVE_LINK 0        /* 0=GPIO Matrix 内部互连  1=外部跳线     */
#endif
#ifndef DMC_SLAVE_MODE
#define DMC_SLAVE_MODE 0        /* 0=正常 1=静默 2=坏CRC                  */
#endif

/* 从机 UART：与主机 UART1(IO17/IO18) 不同的外设
 * ⚠️ LINK=2 是 2026-10-02 实测后新增的**唯一结构性可行**方案：
 *    受控 A/B（C0 vs B，同芯片/同 UART1/同突发/同样采样函数，唯一变量是
 *    「IOMUX 脚」vs「matrix 脚」）证明 S3 上 **matrix 出向对 UART TX 无效**
 *    ——IO17 采样 1974/7000，IO9 采样 0/7000。
 *    而 S3 只有 UART0(IO43/44) 和 UART1(IO17/18) 有 IOMUX 脚
 *    （uart_pins.h: U2TXD_GPIO_NUM = -1）⇒ 免接线互连在 S3 上做不出来，
 *    只能退回「两对 IOMUX 脚 + 2 根跳线」。
 *    console 已确认走 USB-Serial-JTAG（CONFIG_ESP_CONSOLE_SECONDARY_
 *    USB_SERIAL_JTAG=y），**不占用 UART0 引脚**，所以 UART0 可以当从机。*/
#if DMC_SLAVE_LINK == 2
#define DMC_SLAVE_PORT 0       /* 从机改用 UART0（IO43/IO44，IOMUX 专属脚）*/
#else
#define DMC_SLAVE_PORT 2
#endif
#define DMC_SLAVE_BAUD 115200
#define DMC_SLAVE_ADDR 0x02U    /* 从机地址（与主机 DMC_LINK_SLAVE_ADDR 对齐）*/

#if DMC_SLAVE_LINK == 0
#define DMC_SLAVE_TX 8          /* 内部：驱动 IO8 = 主机 UART1 的 RX pad */
#define DMC_SLAVE_RX 9          /* 内部：读    IO9 = 主机 UART1 的 TX pad */
#define DMC_SLAVE_WIRE "GPIO-Matrix 双 matrix 交叉（免接线，无外部波形）"
#elif DMC_SLAVE_LINK == 2
#define DMC_SLAVE_TX 43         /* IOMUX 专属脚，UART0 唯一 TX pad */
#define DMC_SLAVE_RX 44         /* IOMUX 专属脚，UART0 唯一 RX pad */
/* ⚠️ 2026-10-02 修正：此处原写 "IO17↔IO43 / IO18↔IO44"，是**错的**。
 *    那是 TX↔TX + RX↔RX：主机 TX(IO17) 与从机 TX(IO43) 两个输出对打，
 *    主机 RX(IO18) 与从机 RX(IO44) 两个输入并联且都无人驱动 ⇒ 链路恒不通。
 *    正确是 TX↔RX 交叉（对照 LINK=1 的 "IO17→IO9 / IO8→IO18" 即可看出
 *    本串是照抄数字没照抄语义）。代码自身的注释也一直是对的：
 *    dmc_slave.c 里 `gpio_set_direction(DMC_SLAVE_RX,...)` 旁注
 *    「（主机UART1 TX 驱动）」⇒ 从机 RX 才是被主机 TX 驱动的那根。
 *    这也正好解释了「主机 tx 递增 / 从机 rx 恒 0」：线本来就没接对。*/
#define DMC_SLAVE_WIRE "IOMUX 双口 + 2 根跳线(TX↔RX 交叉)：IO17↔IO44 / IO43↔IO18（可任意接示波器）"
#else
#define DMC_SLAVE_TX 8          /* 外部：需跳线 IO8  → IO18 */
#define DMC_SLAVE_RX 9          /* 外部：需跳线 IO17 → IO9  */
#define DMC_SLAVE_WIRE "外部跳线 IO17→IO9 / IO8→IO18（可在任意节点接示波器）"
#endif

#if DMC_SLAVE_MODE == 0
#define DMC_SLAVE_MODE_NAME "正常应答"
#elif DMC_SLAVE_MODE == 1
#define DMC_SLAVE_MODE_NAME "全程静默（阴性对照）"
#else
#define DMC_SLAVE_MODE_NAME "破坏 CRC（完整性故障注入）"
#endif

#if DMC_SLAVE_LINK == 0
/* ★ 真机踩到的坑（2026-10-02，臂 0 首跑即现）——修了三轮才收敛：
 *
 *   现象 = 从机 rx=66 / tx=83（双向都有字节在动），但主机 rx=0（只发不收）。
 *          主机单看「tx=66 to_err=66」完全正常 —— 是**双口供词**交叉核对
 *          才暴露的（这一手上一轮刚加上，当场就兑现了）。
 *
 *   ❌ 根因假设①（错）：「uart_set_pin 的 RX 分支 gpio_set_direction(INPUT)
 *      关掉了对端 TX 驱动」。按这个修 → 从机 rx 从 66 掉到 **0**，更糟。
 *   ❌ 修复①（被证伪）：gpio_config(IO17/18, INPUT_OUTPUT)。看着「只改方向」，
 *      实际 gpio_config() 结尾有 gpio_hal_iomux_func_sel(io, PIN_FUNC_GPIO)
 *      会把 IOMUX 路由切回普通 GPIO ⇒ 外设 TX 路由一并被顶掉。
 *      **教训：「只改方向」是对 API 语义的想当然 —— 同一个「设方向」，
 *        gpio_config 和 gpio_set_direction 的副作用完全不是一回事。**
 *   ❌ 修复②（编译不过）：gpio_output_enable() 在 IDF 5.2 非公开 API。
 *
 *   ✅ 根因（读 IDF 5.2 源码定案，三处证据）：
 *      components/soc/esp32s3/include/soc/uart_pins.h:
 *          U1TXD_GPIO_NUM=17  U1RXD_GPIO_NUM=18      ← 主机 UART1 的**专属 IOMUX 脚**
 *          U2TXD_GPIO_NUM=-1  U2RXD_GPIO_NUM=-1      ← 从机 UART2 无专属脚，**只能走 matrix**
 *      components/driver/uart/uart.c: uart_set_pin() 按 default_gpio 是否
 *      等于传入脚号分两条路。于是这对 pad 上叠了两个致命冲突：
 *        (1) 从机 TX 走 matrix，第一件事就是
 *            gpio_hal_iomux_func_sel(18, PIN_FUNC_GPIO)
 *            把 pad18 输出 func 从 U1RXD 改回 GPIO；而 pad18 的**输入源**仍被
 *            gpio_iomux_in(18, U1RXD_IN_IDX) 硬接在 U1RXD 信号上、bypass 掉
 *            matrix ⇒ 主机 UART1 RX 采到的是 U1RXD 自己 ⇒ **自环**。
 *            （对上实测 tx=66 rx=0）
 *        (2) 从机 RX 走 matrix，gpio_set_direction(17, INPUT) 关掉 pad17
 *            输出驱动 ⇒ 主机 TX 经 IOMUX 发不出去。
 *            （对上实测 从机 rx 66→0）
 *      ⇒ **免接线互连用 IO17/IO18 结构性不可行**，不是配错，是物理冲突。
 *
 *   ✅ 终解 = 两边都换到**非默认脚**，双双走 matrix（matrix 路不碰 IOMUX
 *      输入源，互不破坏）：
 *          主机 UART1: TX=IO9  RX=IO8
 *          从机 UART2: TX=IO8  RX=IO9     ← 电气上 TX↔RX 对接
 *      再在**两个 uart_set_pin 都跑完之后**统一
 *      gpio_set_direction(pin, INPUT_OUTPUT) 补回被 RX 分支关掉的输出驱动
 *      （用 INPUT_OUTPUT 而非 OUTPUT：后者会 gpio_input_disable()，
 *        而对端 RX 正是靠 matrix in 采这个 pad 的输入缓冲器）。
 *      此时每个 pad 仍只有**一个**输出驱动者（IO9 主机 / IO8 从机），
 *      不产生输出对打 —— 这正是两线半双工总线的物理模型。
 */
void dmc_bus_pads_bidir(void);
#endif

/* 启动板载从机任务（幂等）。返回 0 成功 / -1 失败。 */
int dmc_slave_start(void);

/* 只打开从机 UART、**不起任务** —— 供判别探针在无竞争窗口里用。 */
int dmc_slave_open_only(void);

#if DMC_SLAVE_LINK == 0
/* 判别性探针：在两个任务启动前同步跑 A/B 两向测试，一次定位哪一向不通。
 * 返回通过的向数（0/1/2）。必须在 dmc_slave_start / dmc_resident_start
 * 之前调用，否则从机任务会抢走测试字节。 */
int dmc_wire_probe(void);
#endif

/* 从机自己的统计：收到的帧 / 回过的帧 / CRC 错 / 主动推送次数。
 * 与主机侧统计**互相独立**，两边对得上才叫「双口供词」。 */
void dmc_slave_stats(uint32_t* rx_frames, uint32_t* tx_frames,
                     uint32_t* err_crc, uint32_t* push);

#ifdef __cplusplus
}
#endif
