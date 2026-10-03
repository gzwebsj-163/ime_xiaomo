#!/usr/bin/env python3
"""
pin_free.py — **禁脚表的唯一权威来源**（机器可查，不靠注释）

═══════════════════════════════════════════════════════════════════════
为什么要有这个文件
═══════════════════════════════════════════════════════════════════════
2026-10-03 真机事故：SPI A/B 实验的 B 臂选了 IO4/5/6/7 当 MOSI/CK/MISO/CS，
选脚处的注释写着「全部落在空闲区，且不与任何已记录用途冲突」——
**而 IO4/5/6/7 恰恰是本板 LCD 接线**（lcd_hw.h: MOSI 7 / SCLK 6 / DC 4 / RST 5 / CS 10）。
同一个工程的另一处（G2 诊断矩阵选脚注释）明确写了「已避开 LCD 4/5/6/7/10」，
说明这条约束**作者知道**，只是**没有被机器强制**，靠人记 → 漏了一处 → 一整轮实验作废。

教训（可迁移）：
    「我遵守了纪律」和「纪律被强制执行」是两件事。
    只要一条约束是靠注释/记忆承载的，它迟早会在第 N 个文件里漏掉，
    而漏掉的地方往往正是**唯一一个没被人重新读过的**地方。
    → 凡是「选 X 必须不碰 Y」这种约束，都必须有机器闸门，且闸门的数据源要能自证新鲜。

═══════════════════════════════════════════════════════════════════════
数据来源分两类，**可信度不同，不可混为一谈**
═══════════════════════════════════════════════════════════════════════
  [LIVE] 活体提取：从真工程的头文件里 parse 出来。改了接线，闸门自动跟着变。
  [HARD] 硬件事实：S3 芯片级不可用脚。来自 SoC 头文件/datasheet，改板子才变。

LIVE 侧刻意**不去**手抄数值 —— 数值一旦手抄进注释，就退化成与原处同源的单源记载，
而这正是「锚点 E：指纹/常量归属挂错」那类事故的温床。闸门每次运行都重新 parse。

═══════════════════════════════════════════════════════════════════════
用法
═══════════════════════════════════════════════════════════════════════
    python3 pin_free.py --list                 # 打印全部禁脚及其出处
    python3 pin_free.py --check 12 15 40 41     # 断言这 4 只脚可用
退出码：0=全可  1=有禁脚命中  2=活体源缺失（**不可静默放行**）
"""
import os
import re
import sys

# ── 活体工程路径：带屏产品固件（唯一带屏固件，禁脚表以它为准）──
LVGL = os.path.expanduser("~/cow/esp32-s3-lvgl-ui")

# ──────────────────────────────────────────────────────────────────────
# [HARD] 芯片级不可用脚。带出处，闸门会一并打印，便于人工复核。
# ------------------------------------------------------------------- */
HARD_FORBIDDEN = {
    # 八线(OCT) PSRAM：SPIIO4~7/DQS 硬占。配了不报错但 PSRAM 密集访问直接卡死
    # —— gpio_config() 不查保留表、返回 ESP_OK 静默通过（已实测）。
    33: "PSRAM OCT SPIIO4",
    34: "PSRAM OCT SPIIO5",
    35: "PSRAM OCT SPIIO6",
    36: "PSRAM OCT SPIIO7",
    37: "PSRAM OCT DQS",
    # strapping 脚：上电采样电平决定启动模式
    0: "strapping (boot mode)",
    3: "strapping (JTAG sel)",
    45: "strapping (VDD_SPI volt)",
    46: "strapping (boot mode)",
    # USB-Serial-JTAG：本板刷机与日志都走它
    19: "USB-Serial-JTAG D-",
    20: "USB-Serial-JTAG D+",
    # UART0 IOMUX 专属脚 = USB 串口本体，刷机与日志都靠它
    43: "UART0 TX (USB console)",
    44: "UART0 RX (USB console)",
}

# ──────────────────────────────────────────────────────────────────────
# [LIVE] 从活体头文件提取。**数值不写在这里**，每次运行现 parse。
# ------------------------------------------------------------------- */
LIVE_SOURCES = [
    # (说明, 头文件相对路径, 宏名前缀)
    ("LCD 屏接线", "main/lcd_hw.h", "LCD_PIN_"),
    ("v3 三键",   "main/btn.h",    "BTN_PIN_"),
]

DEF_RE = re.compile(r"^\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(\d+)\s*(?:/\*.*)?$")
MUST_DEFINE = {  # 期望从活体里抓到的宏。抓不到 = 源变了/改名了 = 必须报错，不许静默
    "main/lcd_hw.h": ["LCD_PIN_MOSI", "LCD_PIN_SCLK", "LCD_PIN_DC",
                      "LCD_PIN_RST", "LCD_PIN_CS"],
    "main/btn.h":    ["BTN_PIN_UP", "BTN_PIN_DOWN", "BTN_PIN_BACK"],
}


def harvest_live():
    """从活体头文件提取板载占用脚。任一源缺失 → 抛错（绝不返回空表静默放行）。"""
    used = {}
    for what, rel, prefix in LIVE_SOURCES:
        path = os.path.join(LVGL, rel)
        if not os.path.isfile(path):
            raise FileNotFoundError(
                f"活体源缺失：{path}\n"
                f"  [{what}] 读不到 → 禁脚表无法自证新鲜。\n"
                f"  绝不允许「读不到就当没占用」—— 那正是把「我没在看」当「不存在」。"
            )
        found = {}
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = DEF_RE.match(line)
                if m and m.group(1).startswith(prefix):
                    found[m.group(1)] = int(m.group(2))

        missing = [k for k in MUST_DEFINE.get(rel, []) if k not in found]
        if missing:
            raise ValueError(
                f"活体源 {rel} 里找不到必需宏：{missing}\n"
                f"  实际抓到：{sorted(found)}\n"
                f"  → 宏改名或被删。**不要**默默跳过，必须先弄清新名字再改本表。"
            )
        for k, v in found.items():
            used.setdefault(v, []).append(f"{what}:{k}")
        print(f"  [LIVE] {rel:18s} {what:10s} → {sorted(found)}")
    return used


def build():
    used = harvest_live()
    table = {}
    for pin, why in HARD_FORBIDDEN.items():
        table.setdefault(pin, []).append(f"[HARD] {why}")
    for pin, whys in used.items():
        table.setdefault(pin, []).extend(f"[LIVE] {w}" for w in whys)
    return table


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2

    print("═══ 禁脚表自证（LIVE 现 parse，非手抄）═══")
    try:
        table = build()
    except (FileNotFoundError, ValueError) as e:
        print(f"\n❌ {e}")
        return 2

    if args[0] == "--list":
        print(f"\n═══ 禁脚共 {len(table)} 只 ═══")
        for pin in sorted(table):
            print(f"  IO{pin:<3d} ← {'; '.join(table[pin])}")
        free = [p for p in range(54) if p not in table]
        print(f"\n═══ 可用 {len(free)} 只：{free}")
        return 0

    if args[0] == "--check":
        try:
            pins = [int(x) for x in args[1:]]
        except ValueError:
            print("❌ --check 参数必须是整数 GPIO 号")
            return 2
        if not pins:
            print("❌ --check 至少给一个脚")
            return 2

        bad = [(p, table[p]) for p in pins if p in table]
        print(f"\n═══ 断言 {pins} ═══")
        for p in pins:
            if p in table:
                print(f"  IO{p:<3d} ❌ 禁脚 ← {'; '.join(table[p])}")
            else:
                print(f"  IO{p:<3d} ✅ 未在任何占用名单中")
        if bad:
            print(f"\n❌ {len(bad)}/{len(pins)} 命中禁脚 —— 换脚，别靠注释解释。")
            return 1
        print(f"\n✅ {len(pins)} 只全部可用（已对 [HARD] 芯片级 + [LIVE] 活体头文件双向核过）")
        return 0

    print(f"❌ 未知参数 {args[0]!r}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
