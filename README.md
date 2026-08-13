# xiaomo — Mo 语言轻量级虚拟机

基于 mocode-cli 思路重构的**独立轻量级 VM**，纯 C 实现（用 g++ 编译以复用 C++ 底层积木）。目标是**不修改现有 VM**，自研一套完整的 `.mo` 语言工具链 + 一个字节码内核。

## 架构

```
.mo 源码 → Lexer(纯C) → Parser(纯C) → AST → VM 执行 → 输出
                                        │
                                        └─ Kills 字节码内核 (vm_core.c)
```

- **完全独立**：不依赖/不修改用户现有 VM
- **两层执行**：
  1. **解释器层**（`src/vm.c`）：直接解释执行 AST，支持变量/控制流/函数/递归/数组
  2. **Kills 内核层**（`src/vm_core.c`）：64 寄存器 + 操作数栈 + 调用栈 + 线性内存的字节码机，支持序列化/反汇编

## 构建

```bash
make            # 编译生成 xiaomo
make test       # 跑全量回归测试
make clean      # 清理
```

> 说明：因复用的 `stack_memory.h` 是 C++ 头文件（`<cstdlib>/<mutex>/<thread>`），所有源统一用 `g++` 编译（C 代码兼容 C++，仅需少量微调）。

## 命令

```bash
./xiaomo run <file.mo>        # 执行 .mo 文件
./xiaomo parse <file.mo>      # 解析并打印 AST
./xiaomo tokens <file.mo>     # 打印 Token 序列
./xiaomo kvm [-] [file.kbc]   # Kills 内核: 内嵌演示(-)或执行二进制
```

## 示例 (examples/)

| 文件 | 覆盖能力 |
|------|---------|
| `hello.mo` | 变量声明 / 算术 `${x}+${y}` / if-else 条件分支 |
| `control.mo` | while 循环求和 / 数组字面量 `[a,b]` |
| `func.mo` | **函数 / return / 局部作用域 / 递归**（fact 阶乘） |
| `array.mo` | **数组字面量 / 下标访问 / 二维下标** `grid[1][1]` |

## 测试 (tests/)

`make test` 跑全量回归，任何失败返回非零退出码：

- **A. .mo 解释器层**：每个 `examples/*.mo` 与 `tests/expected/*.txt` 期望输出比对
- **B. Kills 内核层**：内嵌 demo 检测关键输出 `720`（循环）/ `5`（CALL-RET）/ `120`（递归）

## 已验证能力

### 解释器层 (.mo)
- 变量声明 / 常量 / 算术表达式 / 字符串拼接
- `if/elif/else` 条件分支、`while` / `for` 循环、`break`/`continue`
- **函数定义 `fn name(params){...}`、return、局部作用域、递归**
- **数组字面量 `[a,b,c]`、下标访问、二维下标**

### Kills 字节码内核
- 64 寄存器 + 操作数栈 + 调用栈 + 线性内存
- 完整指令集：MOV / 算术 / 逻辑 / 位移、PUSH/POP、LOAD/STORE、
  JMP/JZ/JNZ 及 JE/JNE/JG/JGE/JL/JLE 比较跳转、**CALL/RET**（递归）、FFI、PRINT、HALT
- **二进制序列化/反序列化（往返一致）** + 反汇编器
- 已验证：`6! = 720`（JNZ 循环）、`add(2,3)=5`（CALL/RET）、`fact(5)=120`（递归多帧）

## 关键设计约定

- **R0 为函数返回值寄存器**；RET 恢复 R1..R63 现场但保留 R0（不被覆盖）
- CIP 调用时实参经操作数栈弹出填入 R0..；CALL 保存 next_pc + 64 寄存器到调用栈
- 比较跳转指令 `b<0` 时比较基数为 **0**，`imm` 专作跳转偏移
  （如 `JG R1, -1, off` = 若 `R1 > 0` 跳 `pc+off`）
- **保留字不可作标识符**：如 `add`（指令操作符）不能用于函数名

## 已知限制 / 待办

- 不支持 tab 缩进（只识别空格）
- `-Wunused-function`：部分演示函数仅在 demo 流程内联使用
- 更多内建数组操作（`.len` / push 等）待实现

## 参考

- 私有 IR 虚拟机知识页（服务器侧 VM 架构）
