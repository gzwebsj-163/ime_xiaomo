CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2 -g

# 阶段一: 构造体 + 中断系统
TARGET = primitive
SRCS = primitive.c interrupt.c demo.c
OBJS = $(SRCS:.c=.o)

# 阶段二: 内存保护与特权级
TARGET2 = stage2
SRCS2 = mmu.c stage2.c
OBJS2 = $(SRCS2:.c=.o)

# 阶段三: 5 级流水线
TARGET3 = pipeline_demo
SRCS3 = pipeline.c pipeline_demo.c
OBJS3 = $(SRCS3:.c=.o)
TARGET3T = pipeline_test

.PHONY: all clean run run2 run3 testpipeline

all: $(TARGET) $(TARGET2) $(TARGET3) $(TARGET3T)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET2): $(OBJS2)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET3): $(SRCS3)
	$(CC) $(CFLAGS) -o $@ $^

$(TARGET3T): pipeline.c pipeline_test.c
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c primitive.h interrupt.h
	$(CC) $(CFLAGS) -c -o $@ $<

# 阶段二文件依赖 mmu.h
mmu.o: mmu.c mmu.h
	$(CC) $(CFLAGS) -c -o $@ $<
stage2.o: stage2.c mmu.h
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

run2: $(TARGET2)
	./$(TARGET2)

run3: $(TARGET3)
	./$(TARGET3)

testpipeline: $(TARGET3T)
	./$(TARGET3T)

clean:
	rm -f $(TARGET) $(TARGET2) $(TARGET3) $(TARGET3T) $(OBJS) $(OBJS2)
