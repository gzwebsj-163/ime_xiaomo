# 构造体深化路线图

## 总体目标
从「能跑程序的计算机」深化为「有现代计算机核心抽象的完整系统」。

## 四个阶段

### 第一阶段：中断系统
- 中断控制器 (PIC)：8 路中断源，可屏蔽，优先级仲裁
- 中断向量表 (IVT)：16 个入口，每个 4 字节
- 中断响应流程：保存上下文 → 查向量表 → 跳转 ISR → 恢复 → IRET
- 新增指令：IRET (中断返回)、STI/CLI (开/关中断)
- 演示：定时器中断驱动的多任务切换

### 第二阶段：内存保护与特权级
- 特权级：Ring 0 (内核) / Ring 3 (用户)
- 内存段：基址+界限，每段有读/写/执行权限
- 新增指令：SYSCALL/SYSRET (系统调用)
- 演示：用户程序通过系统调用请求内核服务

### 第三阶段：流水线
- 5 级流水线：IF → ID → EX → MEM → WB
- 数据冒险：旁路 (forwarding) 解决
- 控制冒险：分支预测 (简单 2-bit)
- 流水线可视化：每个周期显示各阶段状态
- 演示：对比流水线 vs 非流水线的指令吞吐量

### 第四阶段：总线与 DMA
- 系统总线：地址总线 + 数据总线 + 控制总线
- 总线仲裁：固定优先级
- DMA 控制器：4 通道，源/目标/长度
- 新增指令：OUT/IN (端口 I/O)
- 演示：DMA 传输数据块，CPU 同时执行其他任务

### 第五阶段：FreeRTOS 风格 RTOS 内核
- 每任务独立栈 + ucontext 上下文 (makecontext 创建, swapcontext 切换)
- 8 级优先级就绪链表 (同优先级 FIFO→轮转), 调度器总选最高优先级
- 单一「睡眠链表」统一管理所有阻塞 (延时/信号量/队列超时), 按唤醒时刻排序
- 每任务 wait_kind 标记等待对象类型, 精确摘链
- 互斥锁 + 优先级继承 (解决优先级反转)
- 软件定时器 (one-shot / 周期)
- 调度模型: 任务在阻塞点自动让出; 调度器空闲时推进 tick, 唤醒到期任务并
  重新仲裁最高优先级 → 等价于真 FreeRTOS 的 tick 级抢占
- 演示 (freertos_demo.c): 6 场景 — 优先级调度 / vTaskDelayUntil 精确周期 /
  计数信号量 / 互斥锁+优先级继承 / 消息队列 / 软件定时器

### ⚠️ macOS ucontext 大坑 (已修复, 必须记住)
- **症状**: 调度器 tick 变垃圾值, 段错误
- **根因**: macOS 的 getcontext/swapcontext 把 mcontext **内嵌**写到
  `ucontext_t` 结构体 +0x38..+0xC8 处 (uc_mcsize=0x2C8), **越界约 0x90 字节**!
  若 ucontext_t 紧邻其他数据会踩坏邻居 (实测覆盖 g_tick/g_sleep_list)
- **修复**: UContext = ucontext_t + pad[0x300], 每个上下文 (含调度器) 都包一层
- **验证**: 硬断点 (watchpoint) 抓写入者 = swapcontext→getcontext; lldb 反汇编
  getmcontext 确认 `movq $0x2c8, 0x28(%rdi); movq %r14, 0x30(%rdi)`
- 命令行: `make run5`