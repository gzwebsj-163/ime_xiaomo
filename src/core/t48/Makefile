# Makefile - XGecu T48 自研烧录工具
# 构建：make
# 清理：make clean
# 测试：make test

CC = gcc
CFLAGS = -Wall -Wextra -O2 -g -I. -I/usr/local/opt/libusb/include/libusb-1.0
LDFLAGS = -L/usr/local/lib
LDLIBS = -lusb-1.0

# 源文件
SRCS = t48_usb.c t48_db.c t48_proto.c t48_main.c
OBJS = $(SRCS:.c=.o)
TARGET = t48

# xiaomo VM 集成（可选）
XIAOMO_DIR = ..
XIAOMO_T48_SRCS = t48_usb.c t48_db.c t48_proto.c
XIAOMO_T48_OBJS = $(XIAOMO_T48_SRCS:.c=.o)

.PHONY: all clean test install uninstall

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 安装到系统路径
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/
	@echo "✅ 已安装到 /usr/local/bin/$(TARGET)"

uninstall:
	rm -f /usr/local/bin/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f t48_usb.o t48_db.o t48_proto.o t48_main.o

# 快速测试：探测 T48 是否在线
test: $(TARGET)
	@echo "=== 探测 T48 ==="
	./$(TARGET) probe
	@echo "=== 列出芯片 ==="
	./$(TARGET) list

# 完整测试：需要 T48 硬件 + 芯片
test-full: $(TARGET)
	@echo "=== 探测 T48 ==="
	./$(TARGET) probe
	@echo "=== 芯片列表 ==="
	./$(TARGET) list
	@echo "=== 自动检测 ==="
	./$(TARGET) detect

# 调试模式（详细 USB 日志）
debug: CFLAGS += -DDEBUG -g -O0
debug: $(TARGET)
	@echo "🔧 调试模式已构建"

# ========== xiaomo VM 集成 ==========

# 编译为 xiaomo VM 的 T48 模块（.o 供链接）
xiaomo-lib: CFLAGS += -fPIC
xiaomo-lib: $(XIAOMO_T48_OBJS)
	@echo "✅ T48 模块已编译（供 xiaomo VM 链接）"
	@echo "   链接时需加: $(XIAOMO_T48_OBJS) $(LDFLAGS) $(LDLIBS)"

# 清理 xiaomo 集成产物
xiaomo-clean:
	rm -f $(XIAOMO_T48_OBJS)