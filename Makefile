# xiaomo - Mo 语言轻量级虚拟机 (纯 C 实现 + C++ 底层积木)
# 架构: .mo 源码 -> Lexer -> Parser -> AST -> VM 执行
# VM 复用 linux-xiaomo 的 stack_memory (C++) 作为执行栈
#
# 说明: 因复用的 stack_memory.h 是 C++ 头文件(<cstdlib>/<mutex>/<thread>),
#       所有源文件统一用 g++ 编译 (C 代码兼容 C++, 仅需 malloc 强转等微调)

CC = g++
CXX = g++
CFLAGS = -Wall -std=c++17 -g -Iinclude -Isrc/core/ch340
CXXFLAGS = $(CFLAGS)

# 源文件
C_SRCS = src/lexer.c src/parser.c src/ast.c src/vm.c src/vm_core.c src/vm_stack.c src/mo2kbc.c src/tensor.c src/nd_tensor.c src/weights.c src/debug_tool.c src/main.c
HW_SRCS = src/hw/hw_direct.c src/hw/hw_demo.c src/hw/hw_oem.c src/hw/hw_hex.c src/hw/hw_dev.c src/hw/hw_token.c src/hw/hw_asr.c src/infer.c src/hw/hw_fault.c src/hw/hw_core.c src/hw/hw_main.c src/hw/hw_wdbg.c src/hw/hw_flash.c src/hw/hw_pin.c src/hw/hw_dc.c src/hw/hw_dmc.c src/hw/hw_dmc_base.c
CORE_SRCS = src/core/mc12026a.c
CPP_SRCS =

C_OBJS = $(C_SRCS:.c=.o)
HW_OBJS = $(HW_SRCS:.c=.o)
CORE_OBJS = $(CORE_SRCS:.c=.o)
CPP_OBJS = $(CPP_SRCS:.cpp=.o)
OBJS = $(C_OBJS) $(HW_OBJS) $(CORE_OBJS) $(CPP_OBJS)

TARGET = xiaomo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread -framework IOKit -framework CoreFoundation

# xdebugd —— 调试软件 (持久会话守护进程 + Web 工作台), 排除 main.o (自有 main)
DAEMON_OBJS = $(filter-out src/main.o,$(OBJS)) src/debug_daemon.o

xdebugd: $(DAEMON_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ -lpthread -framework IOKit -framework CoreFoundation

# 所有源统一 g++ 编译
%.o: %.c
	$(CXX) $(CXXFLAGS) -x c++ -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 测试
test: $(TARGET)
	@bash tests/run_tests.sh

clean:
	rm -f $(OBJS) src/debug_daemon.o $(TARGET) xdebugd

.PHONY: all clean test
