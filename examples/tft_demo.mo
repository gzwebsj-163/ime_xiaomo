# tft_demo.mo — xiaomo MMU TFT 显示屏驱动 · Ring3 演示
#
# 用 xiaomo 的 .mo 语法在 Ring3 写入帧缓冲
# 然后通过 SYSCALL 触发刷屏
#
# 10-Pin TFT 接线对照:
#   Pin 1(VCC)  → 3.3V
#   Pin 2(GND)  → GND
#   Pin 3(CS)   → SPI1_CS1 (PH9)
#   Pin 4(RST)  → PG13
#   Pin 5(DC)   → PG11
#   Pin 6(MOSI) → SPI1_MOSI (PH7)
#   Pin 7(SCK)  → SPI1_CLK (PH6)
#   Pin 8(LED)  → PG12 / 3.3V
#   Pin 9(MISO) → SPI1_MISO (PH8)
#   Pin 10(NC)  → 悬空
#
# MMU 段布局:
#   Seg 0: 内核代码 (Ring0, R+W+X)
#   Seg 1: 帧缓冲     (Ring3, R+W,   dpl=3)  ← 本程序画的区域
#   Seg 2: 驱动控制块 (Ring0, R+W)
#   Seg 3: DMA 缓冲区  (Ring0, R+W)

# ============================================================
# 常量定义
# ============================================================
const TFT_W    = 170        # 屏幕宽度
const TFT_H    = 320        # 屏幕高度
const FB_SIZE  = 170 * 320 * 2  # 帧缓冲字节数 (RGB565)

# SYSCALL 编号
const SYS_FLUSH     = 1     # 刷屏
const SYS_CLEAR     = 2     # 清屏 (R1=颜色)
const SYS_SET_BL    = 3     # 背光 (R1=亮度0-255)
const SYS_DISPLAY_ON  = 4   # 显示开
const SYS_DISPLAY_OFF = 5   # 显示关

# 颜色 (RGB565)
const COLOR_BLACK   = 0x0000
const COLOR_WHITE   = 0xFFFF
const COLOR_RED     = 0xF800
const COLOR_GREEN   = 0x07E0
const COLOR_BLUE    = 0x001F
const COLOR_YELLOW  = 0xFFE0
const COLOR_CYAN    = 0x07FF
const COLOR_MAGENTA = 0xF81F
const COLOR_ORANGE  = 0xFC00

# ============================================================
# RGB565 辅助函数
# ============================================================
fun rgb565(r, g, b): int
    # r: 0-31 (5bit), g: 0-63 (6bit), b: 0-31 (5bit)
    return (r << 11) | (g << 5) | b
end

# ============================================================
# 像素绘制 (直接写帧缓冲)
# ============================================================
# 帧缓冲基地址通过 MMU Seg 1 映射到 Ring3 的地址空间
# 地址: fb_base + (y * TFT_W + x) * 2

fun set_pixel(fb_base, x, y, color): void
    # 帧缓冲是 uint16_t 数组, 每个像素 2 字节
    # 用 STORE16 写入 (注意: .mo 语法中 STORE16 写入 2 字节)
    int addr : int = ${fb_base} + (${y} * TFT_W + ${x}) * 2
    store16(${addr}, ${color})
end

# ============================================================
# 矩形填充
# ============================================================
fun fill_rect(fb_base, x0, y0, x1, y1, color): void
    int y : int = ${y0}
    while ${y} <= ${y1}:
        int x : int = ${x0}
        while ${x} <= ${x1}:
            set_pixel(${fb_base}, ${x}, ${y}, ${color})
            x = ${x} + 1
        end
        y = ${y} + 1
    end
end

# ============================================================
# 画线 (Bresenham)
# ============================================================
fun draw_line(fb_base, x0, y0, x1, y1, color): void
    int dx : int = abs(${x1} - ${x0})
    int dy : int = abs(${y1} - ${y0})
    int sx : int = if ${x0} < ${x1} then 1 else -1
    int sy : int = if ${y0} < ${y1} then 1 else -1
    int err : int = ${dx} - ${dy}

    while true:
        set_pixel(${fb_base}, ${x0}, ${y0}, ${color})
        if ${x0} == ${x1} and ${y0} == ${y1}: break
        int e2 : int = 2 * ${err}
        if ${e2} > -${dy}:
            err = ${err} - ${dy}
            x0 = ${x0} + ${sx}
        end
        if ${e2} < ${dx}:
            err = ${err} + ${dx}
            y0 = ${y0} + ${sy}
        end
    end
end

# ============================================================
# 画圆 (Bresenham)
# ============================================================
fun draw_circle(fb_base, cx, cy, r, color): void
    int x : int = 0
    int y : int = ${r}
    int d : int = 3 - 2 * ${r}

    while ${x} <= ${y}:
        set_pixel(${fb_base}, ${cx}+${x}, ${cy}+${y}, ${color})
        set_pixel(${fb_base}, ${cx}-${x}, ${cy}+${y}, ${color})
        set_pixel(${fb_base}, ${cx}+${x}, ${cy}-${y}, ${color})
        set_pixel(${fb_base}, ${cx}-${x}, ${cy}-${y}, ${color})
        set_pixel(${fb_base}, ${cx}+${y}, ${cy}+${x}, ${color})
        set_pixel(${fb_base}, ${cx}-${y}, ${cy}+${x}, ${color})
        set_pixel(${fb_base}, ${cx}+${y}, ${cy}-${x}, ${color})
        set_pixel(${fb_base}, ${cx}-${y}, ${cy}-${x}, ${color})

        if ${d} < 0:
            d = ${d} + 4*${x} + 6
        else:
            d = ${d} + 4*(${x} - ${y}) + 10
            y = ${y} - 1
        end
        x = ${x} + 1
    end
end

# ============================================================
# 十六进制字符显示 (简易)
# ============================================================
# 5×7 点阵 '0'-'9', 'A'-'F'
const FONT5X7:
    [0x7C, 0x82, 0x82, 0x82, 0x7C],  # 0
    [0x00, 0x42, 0xFE, 0x02, 0x00],  # 1
    [0x42, 0x86, 0x8A, 0x92, 0x62],  # 2
    [0x84, 0x82, 0x92, 0x92, 0x6C],  # 3
    [0x18, 0x28, 0x48, 0xFE, 0x08],  # 4
    [0xE4, 0xA2, 0xA2, 0xA2, 0x9C],  # 5
    [0x3C, 0x52, 0x92, 0x92, 0x0C],  # 6
    [0x80, 0x8E, 0x90, 0xA0, 0xC0],  # 7
    [0x6C, 0x92, 0x92, 0x92, 0x6C],  # 8
    [0x60, 0x92, 0x92, 0x94, 0x78],  # 9
    [0x7E, 0x88, 0x88, 0x88, 0x7E],  # A
    [0xFE, 0x92, 0x92, 0x92, 0x6C],  # B
    [0x7C, 0x82, 0x82, 0x82, 0x44],  # C
    [0xFE, 0x82, 0x82, 0x44, 0x38],  # D
    [0xFE, 0x92, 0x92, 0x92, 0x82],  # E
    [0xFE, 0x90, 0x90, 0x90, 0x80],  # F
end

# ============================================================
# 主程序 — 演示画面
# ============================================================
fun main(): void
    # 获取帧缓冲基地址 (来自 Ring0 的 MMU Seg 1 映射)
    # 在真实环境下, Ring0 会把帧缓冲地址填入一个固定寄存器或数据段
    # 这里假设 R0 已由 Ring0 填入帧缓冲地址
    int fb : int = r0       # 帧缓冲基地址

    # ========== 1. 清屏 ==========
    # 通过 SYSCALL 刷屏 (R0=SYS_CLEAR, R1=COLOR_BLACK)
    r0 = SYS_CLEAR
    r1 = COLOR_BLACK
    syscall                   # 清屏 (Ring0 通过 DMA 快速填充)
    # 或者在 Ring3 手动清:
    # fill_rect(${fb}, 0, 0, TFT_W-1, TFT_H-1, COLOR_BLACK)

    # ========== 2. 画彩色边框 ==========
    fill_rect(${fb}, 0, 0, TFT_W-1, 2, COLOR_RED)
    fill_rect(${fb}, 0, TFT_H-3, TFT_W-1, TFT_H-1, COLOR_BLUE)
    fill_rect(${fb}, 0, 0, 2, TFT_H-1, COLOR_GREEN)
    fill_rect(${fb}, TFT_W-3, 0, TFT_W-1, TFT_H-1, COLOR_YELLOW)

    # ========== 3. 画对角线 ==========
    draw_line(${fb}, 0, 0, TFT_W-1, TFT_H-1, COLOR_WHITE)
    draw_line(${fb}, TFT_W-1, 0, 0, TFT_H-1, COLOR_WHITE)

    # ========== 4. 画圆 ==========
    draw_circle(${fb}, TFT_W/2, TFT_H/3, 30, COLOR_CYAN)
    draw_circle(${fb}, TFT_W/2, TFT_H/3, 20, COLOR_MAGENTA)
    draw_circle(${fb}, TFT_W/2, TFT_H/3, 10, COLOR_YELLOW)

    # ========== 5. 画矩形渐变 ==========
    int y : int = 0
    while ${y} < 64:
        int color : int = rgb565(0, ${y} * 63 / 64, 31)
        fill_rect(${fb}, 10, TFT_H-80+${y}, TFT_W-11, TFT_H-80+${y}, ${color})
        y = ${y} + 1
    end

    # ========== 6. 刷屏 (SYSCALL) ==========
    r0 = SYS_FLUSH
    syscall

    # ========== 7. 延时后切换颜色 ==========
    # 等待 (无 sleep, 用空循环模拟)
    int i : int = 0
    while ${i} < 1000000:
        i = ${i} + 1
    end

    # ========== 8. 换色: 红色边框 ==========
    fill_rect(${fb}, 0, 0, TFT_W-1, 2, COLOR_ORANGE)
    fill_rect(${fb}, 0, TFT_H-3, TFT_W-1, TFT_H-1, COLOR_ORANGE)
    fill_rect(${fb}, 0, 0, 2, TFT_H-1, COLOR_ORANGE)
    fill_rect(${fb}, TFT_W-3, 0, TFT_W-1, TFT_H-1, COLOR_ORANGE)

    r0 = SYS_FLUSH
    syscall

    # ========== 9. 最后: 显示全屏颜色渐进 ==========
    # xiaomo OS 启动完毕!
    # print("TFT display demo running on xiaomo MMU!")
end