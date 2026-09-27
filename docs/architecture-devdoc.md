# xiaomo 架构 C 源码 · 超详细开发文档

> **适用对象**: ESP32 Simulator IDE 后端（用户口中的「xiaomo 项目 / 基础架构，不能改」）
> **源码位置**: `~/Desktop/esp32-sim/src/*.c` + `~/Desktop/esp32-sim/include/*.h`（IDE 内架构区快照＝此目录）
> **规模**: 16 个 .c（9283 行）+ 16 个 .h（1437 行），共 10720 行 C89/C99
> **文档版本**: 2026-09-06 · 基于逐文件全量扫描编写

---

## 📖 卷首 · 阅读指南

本文档按 **5 卷 + 附录** 组织，每个文件的章节都包含：**职责定位 → 依赖关系 → 数据结构逐字段解析 → 函数逐个详解（签名/参数/返回值/内部逻辑）→ 已知坑**。

| 卷 | 内容 | 文件 |
|---|---|---|
| 第 1 卷 | 语言工具链（.mo → 字节码） | token.h lexer.c ast.h ast.c parser.c mo2kbc.c mo2kbc_main.c |
| 第 2 卷 | Kills VM 内核 | vm_stack.h/.c vm_core.h/.c |
| 第 3 卷 | 模拟器与集成层 | xiaomo_integration.c chip_config.c emulator.c hw_direct_stub.c |
| 第 4 卷 | 服务层（HTTP/WS/DB/入口） | httpd.c db.c main.c |
| 第 5 卷 | 真机外设与 Shell | serial_hw.c shell.c |
| 附录 A~E | API/协议速查、字节码格式、坑清单 | — |

---

## 0. 系统全景

### 0.1 这套 C 代码是什么

ESP32 Simulator IDE 的**单进程后端**（`esp32sim` 二进制，默认端口 9877/9878），一个进程里同时承载 6 个子系统：

```
┌────────────────────────────────────────────────────────────────────┐
│                    esp32sim (单进程, C99, pthread)                  │
│                                                                    │
│  ① 语言工具链   .mo 源码 → lexer → parser → AST → mo2kbc → .kbc     │
│  ② Kills VM    64 寄存器字节码解释器 + FFI + 输入桥 + 序列化         │
│  ③ ESP 模拟器  UART/GPIO/SPI/I2C/Flash/波形 状态机 + 伪执行          │
│  ④ 服务层      kqueue HTTP 服务器 + WebSocket 广播 + SQLite        │
│  ⑤ 真机链路    串口枚举/连接 + esptool 烧录 + 行协议分流             │
│  ⑥ Shell      内置命令解释器 (ls/cat/run/send...) + 独立 VM 线程    │
└────────────────────────────────────────────────────────────────────┘
```

### 0.2 模块依赖图（谁 include 谁）

```
                     ┌────────────┐
                     │  main.c    │  ← 组装所有模块的主程序
                     └─────┬──────┘
        ┌──────────┬───────┼────────┬──────────┬─────────┐
        ▼          ▼       ▼        ▼          ▼         ▼
    httpd.c     db.c  emulator.c  shell.c  serial_hw.c  xiaomo_integration.c
        │          │       │         │         │              │
        │          │       │         ▼         │              ▼
        │          │       │      vm_core.c◄────┼──── vm_core.c + emulator.c
        │          │       │         │          │
        │          │       │         ▼          │
        │          │       │      vm_stack.c    │
        │          ▼       │
        │      (SQLite3)   │
        │                  ▼
        │            chip_config.c
        │
        └─ 不依赖业务模块（纯传输层，回调注入）

    语言工具链独立成链: lexer.c → parser.c → ast.c → vm_core.c
                                              ▲
                                    mo2kbc.c (编译器, 产出 KillsProgram)
                                    mo2kbc_main.c (独立 CLI: .mo → .kbc 文件)
```

**层次规则**：下层不允许反向依赖上层。`vm_core.c` 只依赖 `vm_stack.h`；`xiaomo_integration.c` 同时依赖 `vm_core + emulator`（是两个世界的桥）；`main.c` 是唯一知道所有模块的「装配点」。

### 0.3 构建

```bash
cd ~/Desktop/esp32-sim
make                 # 产出 ./esp32sim 单二进制 + ./mo2kbc 独立编译器 CLI
./esp32sim 9877      # 启动服务（⚠️ 必须以端口号为第一个参数）
```

- 编译器：clang（macOS），链接 `-lsqlite3 -lpthread`
- `mo2kbc` 是工具链的独立入口（`mo2kbc_main.c`），用于把 `.mo` 离线编译成 `.kbc` 文件
- macOS `.app` 打包：二进制作为 sidecar 放进 `Resources/backend/`，首次启动时复制到 userData 根再执行（`find_base_path` 决定 cwd，详见 4.3 节 main.c）

### 0.4 两条执行路径（理解本项目的钥匙）

| 路径 | 输入 | 过程 | 用途 |
|---|---|---|---|
| **模拟路径** | `.mo` 源码 | `xvm_compile_mo`（内存中编译）→ `xvm_run` / 或 `.kbc` 直接加载 | IDE 的 VM 面板、Shell 的 `run/mo` 命令 |
| **C 运行路径** | C 源码（ESP32 风格） | `mkdtemp` 临时目录 → clang 编译 → 子进程执行 → 输出回传 | 「▶ 运行」按钮，跑 `app_main`/FreeRTOS 风格代码（经 shim 层） |

真机路径则是：串口连接 → esptool 烧录（见 5.1 serial_hw.c），与模拟路径完全平行。

---

# 第 1 卷 · 语言工具链（.mo 语言 → Kills 字节码）

> .mo 语言是类 Python 语法的自定义语言：`#` 注释、缩进块、`void x : int = 0` 声明、`fn` 函数、`>> print` 输出、汇编风格指令（`0x00 : jmp = ...`）。工具链四件套：**lexer → parser → AST → mo2kbc 编译器**。

## 1.1 `include/token.h`（155 行）— 词法单元定义

**职责**：定义 .mo 语言的全部 Token 类型枚举、Token 结构体。是 lexer/parser 的共同语言。

**`TokenType` 枚举**（完整分组）：

| 分组 | 成员 | 说明 |
|---|---|---|
| 基础 | `TOK_EOF=0` | 输入结束 |
| 字面量 | `TOK_IDENT` `TOK_INT` `TOK_STR` `TOK_FLOAT` `TOK_BYTES` | 标识符/整数/字符串/浮点/字节数据 |
| 关键字 | `TOK_VOID` `TOK_CONST` `TOK_FN` `TOK_FUNC`(旧版) `TOK_IF` `TOK_ELSE` `TOK_WHILE` `TOK_FOR` `TOK_RETURN` `TOK_NULL` `TOK_TRY` `TOK_CATCH` `TOK_SWITCH` `TOK_CASE` `TOK_DEFAULT` `TOK_BREAK` `TOK_CONTINUE` `TOK_CLASS` `TOK_EXTENDS` `TOK_THIS` `TOK_SUPER` `TOK_PRINT` `TOK_TRUE` `TOK_FALSE` | 注意：class/extends/this/super/try/catch/switch **只有 token、解析器未实现** |
| 类型 | `TOK_TYPE_INT/STR/FLOAT/BOOL/BYTES` + `TOK_TYPE_U/INT8~64`（12 个定宽类型） | 13 个类型关键字 |
| 汇编指令 | `TOK_INST_JMP/JNZ/JZ/JE/JNE/JG/JL/MOV/DNP/PUSH/POP/ADD/SUB/MUL/DIV/AND/OR/XOR/NOT/CALL/RET/NOP` | 供 `0xNN : 指令` 形式的内嵌汇编块 |
| 标点/运算符 | `TOK_COLON` `TOK_SEMICOLON` `TOK_COMMA` `TOK_DOT` `TOK_LPAREN`…`TOK_RBRACKET` `TOK_ASSIGN` `TOK_PLUS` `TOK_MINUS` `TOK_STAR` `TOK_SLASH` `TOK_PERCENT` `TOK_EQ` `TOK_NEQ` `TOK_GT` `TOK_LT` `TOK_GE` `TOK_LE` `TOK_AND_AND` `TOK_OR_OR` `TOK_NOT` `TOK_RSHIFT`(» 既右移也输出流也链接) `TOK_LSHIFT` `TOK_AMP` `TOK_PIPE` `TOK_CARET` `TOK_ARROW`(->) `TOK_DOLLAR_LBRACE`(${) `TOK_HASH`(#) | `>>` 是多义 token：右移运算 / print 输出语法 / 数据声明（`>> uint32 : ...`） |
| 缩进 | `TOK_NEWLINE` `TOK_INDENT` `TOK_DEDENT` | Python 风格缩进块，解析器靠 INDENT/DEDENT 判断块结构 |
| 特殊 | `TOK_ADDR_REF` | `0xNN_name` 内存地址引用形式 |

**`Token` 结构体**（逐字段）：

```c
typedef struct {
    TokenType type;
    const char* lexeme;   // 指向源文本的起始指针（⚠️ 不是拷贝！要求源串在整个编译期间存活）
    int length;           // lexeme 长度
    long ival;            // 整数值（TOK_INT 时有效）
    double fval;          // 浮点值（TOK_FLOAT 时有效）
    int line;             // 行号（1 基）
    int col;              // 列号（1 基）
} Token;
```

**导出函数**：
- `int token_is_type(TokenType t)` — 判断是否 13 个类型关键字之一（parser 用它认类型）
- `const char* token_type_name(TokenType t)` — 调试用，返回类型名字符串

---

## 1.2 `src/lexer.c`（467 行）— 词法分析器

**职责**：把 .mo 源码字符串切分成 Token 流。处理：关键字/标识符、十进制+十六进制+浮点数字、字符串、`#` 注释、双字符运算符（`==` `&&` `->` 等）、**Python 风格缩进栈（INDENT/DEDENT）**。

**`Lexer` 结构体**：

```c
typedef struct {
    const char* src;    // 源码（不拷贝）
    int pos, len;       // 当前位置 / 总长
    int line, col;      // 当前行列（1 基）
    int error_count;    // 累计错误数
    char error_msg[512];// 首个错误信息
} Lexer;
```

**函数详解**：

| 函数 | 详解 |
|---|---|
| `void lexer_init(Lexer*, const char* src)` | 初始化：pos=0、line=1、col=1、error 清零。不复制 src |
| `static TokenType lookup_keyword(word, len)` | 标识符→关键字分派表；查不到返回 TOK_IDENT |
| `static char peek/peek2/advance(Lexer*)` | 前看 1/2 字符、推进（advance 自动维护 line/col，遇 `\n` 换行归 col=1） |
| `static void lexer_error(Lexer*, msg)` | 记录首个错误 + error_count++（不中断，继续扫） |
| `static void lex_number(...)` | 数字扫描：`0x` 前缀→十六进制（支持 `0xNN_name` 地址引用形式的识别）；小数点→浮点；写 ival/fval |
| `TokenList lexer_tokenize(Lexer*)` | **主入口，循环扫描整个源码**。逐字符分派：空白跳过；`#` 到行尾全跳（注释）；字母/下划线→单词→lookup_keyword；数字→lex_number；`"`→字符串（处理转义）；运算符→先试双字符（`==` `!=` `>=` `<=` `&&` `\|\|` `->` `>>` `<<` `${`）再单字符。**核心机制——缩进栈**：维护缩进层级变量，每行第一个实质 token 之前按行首空格数与栈顶比较：更深→emit `TOK_INDENT`（记录 indent_level）；更浅→逐级 emit `TOK_DEDENT` 直到匹配。行尾 emit `TOK_NEWLINE` |
| `void token_list_free(TokenList*)` | 释放 token 数组（lexeme 不释放——指向源文本） |
| `void token_list_dump(TokenList*)` | 调试：打印全部 token |

**`TokenList`**：`{ Token* items; int count; int capacity; }` 动态数组，容量倍增。

**已知坑/约定**：
- lexeme 是源串指针 → 源码字符串必须在 `parser_parse` 完成前保持有效（main.c 全程持有，安全）
- 缩进必须一致（tab 与空格混用未做归一化）
- 字符串转义只支持基本集合，`\uXXXX` 未实现

---

## 1.3 `include/ast.h` + `src/ast.c`（90+113 行）— AST 节点

**职责**：定义语法树的节点类型、节点结构与构造/销毁/打印。

**`NodeType` 枚举**（23 种）：

| 节点 | 语法形式 | 说明 |
|---|---|---|
| `NODE_PROGRAM` | 整个文件 | 根节点 |
| `NODE_VAR_DECL` | `void name : type = value` | 变量声明 |
| `NODE_CONST_DECL` | `const name : type = addr : value` | 常量声明（带地址段） |
| `NODE_ASSIGN` | `name = expr` | 赋值 |
| `NODE_PRINT` | `>> print >> a >> b` | 链式打印（首段换行、后续追加，见 mo2kbc Bug2） |
| `NODE_IF` / `NODE_WHILE` / `NODE_FOR` | `if cond:` / `while cond:` / `for ...` | 控制流（for 未完全支持，见 parser） |
| `NODE_FN_DECL` | `fn name(p1, p2) { }` | 函数定义 |
| `NODE_CALL` | `name(args)` | 函数调用（含内建 `print(...)` 风格） |
| `NODE_EXPR` / `NODE_BINOP` / `NODE_LITERAL` | 表达式 / 二元运算 / 字面量 | |
| `NODE_INSTRUCTION` | `0xNN : jmp = expr` | 内嵌汇编指令行 |
| `NODE_DATA_DECL` | `>> uint32 : 0x00 >> uint16 : ...` | 数据段声明（编译进 data 区） |
| `NODE_BLOCK` | 缩进块 | 语句容器 |
| `NODE_TEMPLATE_REF` | `${name}` 或 `${0xNN}` | 模板引用（占位） |
| `NODE_INDEX`(旧) / `NODE_ARRAY_LIT` / `NODE_INDEX_ACCESS` | `[a,b]` / `a[i]` | 数组字面量与下标访问 |
| `NODE_RETURN` / `NODE_BREAK` / `NODE_CONTINUE` | | |

**`MoType` 枚举**：`TYPE_UNKNOWN/INT/STR/FLOAT/BOOL/BYTES/UINT8~64/INT8~64` —— 与 token 的类型关键字一一对应；**编译期字符串类型追踪**（mo2kbc 的 print 字符串回归修复）就依赖节点的 `mtype == TYPE_STR` 判定。

**`AstNode` 结构体**（字段复用型设计，一个 struct 服务所有节点）：

```c
struct AstNode {
    NodeType type;  int line, col;
    char* text;         // 标识符名 / 字符串内容（malloc 拷贝）
    long ival;  double fval;  MoType mtype;
    AstNode *left, *right;         // 二元运算左右子
    AstNode *cond, *then_block, *else_block;  // if/while
    NodeList body;                 // 语句块（if/while/fn/block）
    NodeList args;                 // 调用参数 / 声明列表
    TokenType inst;                // NODE_INSTRUCTION 的指令类型
};
```

**函数详解**：

| 函数 | 详解 |
|---|---|
| `AstNode* ast_new(type, line, col)` | calloc 一个节点（所有指针域 NULL） |
| `void ast_set_text(n, text)` / `ast_set_text_len(n, text, len)` | strdup 拷贝文本进节点（len 版本支持非零结尾片段） |
| `void node_list_add(NodeList*, node)` | 追加子节点，容量倍增 |
| `void ast_free(node)` | **递归**释放：text、left/right/cond/then/else、body、args 全部遍历 free |
| `void node_list_free(list)` | 释放数组（节点所有权归 ast_free 链） |
| `void ast_dump(node, depth)` | 递归缩进打印（`node_type_name` 静态表），调试神器 |

---

## 1.4 `include/parser.h` + `src/parser.c`（33+775 行）— 语法分析器

**职责**：Token 流 → AST。递归下降 + 运算符优先级链式下降。

**`Parser` 结构体**：`{ TokenList tokens; int pos; int error_count; char error_msg[1024]; }`

**入口两个**：
- `AstNode* parser_parse(Parser*)` — 标准 API，返回 NODE_PROGRAM
- `AstNode* xiaomo_parse_source(src, errbuf, errlen)` — **便捷总入口**（内部先 tokenize 再 parse），main.c/xiaomo_integration 用这个；失败返回 NULL 并写 errbuf

**内部工具**：`cur()` `peek(k)` `advance()` `check(t)` `match(t)`（标准 LL 组合子）、`parse_error`（记录首个错误 + error_count++，不抛异常、靠 NULL 传播恢复）。

**优先级链**（低→高）：
`parse_expr` → `parse_logic`（`||` `&&`）→ `parse_comparison`（`== != > < >= <=`）→ `parse_add`（`+ -`）→ `parse_mul`（`* / %`）→ `parse_unary`（`! -`）→ `parse_primary`

**`parse_primary`（98-208 行，最大最难）**：分派字面量（INT/STR/FLOAT/TRUE/FALSE/NULL）、`(`expr`)`、数组字面量 `[e1, e2]`、标识符（带后缀链：`a[i]` 下标访问 → NODE_INDEX_ACCESS；`(args)` 调用 → NODE_CALL）、`${...}` 模板引用。

**语句分发 `parse_statement`（608 行起）**：

| 看到 | 走向 | 备注 |
|---|---|---|
| `void` | `parse_var_decl` | `void name : type = expr`（支持数组声明 `void a : int[10]`? → 数组用字面量赋值） |
| `const` | `parse_const_decl` | 双冒号地址形式 |
| `fn` | `parse_fn_decl` | 参数列表 + 块体 |
| `if/while/for/return` | 对应函数 | **for 只支持 C 风格三段式头部的一部分**；break/continue 直接建节点（Bug：continue 曾显式报错，后来放开；break 在 mo2kbc 有专门支持） |
| `print` 后跟 `(` | 直接建 NODE_CALL("print") | 兼容标准函数调用风格 |
| `>>` 后跟 `print` | `parse_print` | 链式 `>> print >> a >> "b"` — **parse_print 曾有 Bug2：链式只解析第一段**（见 mo2kbc 卷） |
| `>>` 后跟类型 token | `parse_data_decl` | 数据段声明 |
| `0xNN :` 或 name `:` | `parse_instruction` | 内嵌汇编行 |
| 其他 | `parse_expr` → 表达式语句（赋值在 parse_expr 内部处理 `=`） | |

**块的识别**：`:` 后跟 `TOK_NEWLINE` + `TOK_INDENT ... TOK_DEDENT`（Python 风格）；或 `{ }` 花括号（fn 体用花括号）。

**`parse_block`**：循环 parse_statement 直到 DEDENT/RBRACE/EOF，包进 NODE_BLOCK。

**错误恢复**：parse_error 后靠调用方 NULL 判断；error_msg 只保存**第一个**错误。

---

## 1.5 `include/mo2kbc.h` + `src/mo2kbc.c`（33+773 行）— 字节码编译器 ⭐

**职责**：AST → `KillsProgram`（Kills 内核字节码，内存表示）。这是连接语言世界和 VM 世界的桥。

**寄存器约定**（编译器与 VM 的 ABI，写在 mo2kbc.h 头注释里）：

```
R0..R3     函数形参（CALL 自动从操作数栈弹出填入）
R4..R23    全局变量
R24..R49   函数局部变量
R50 = TMP  表达式结果寄存器
R51 = R2   表达式辅助（右操作数）
R63 = R0   返回值载体（CALL 结果读这里）
R_VAR_MAX  56（上限，曾从 40 提到 56）
```

**内部数据结构**：

| 结构 | 字段 | 说明 |
|---|---|---|
| `SymTab` | name/reg/is_array 链表 | 变量→寄存器映射；**⚠️ 曾经跨函数不清空导致 Bug1（见下）** |
| `CgFuncTab` | name + 局部 SymTab 数组 | 函数表 |
| `LabelTab` | 计数器 | 标签编号分配 + 回填 |
| `Cg` | 总符号表/字符串变量集/错误标志/输出 KillsProgram | 编译上下文 |

**字符串变量追踪**（print 字符串回归修复的核心）：`cg_mark_strvar` / `cg_is_strvar` —— 赋值右侧是字符串字面量时把变量名标记为字符串变量；print 输出该变量时按常量池字符串（FFI 1）而不是整数打印。**判断字符串字面量用 `n->mtype == TYPE_STR`（空串 `""` 也曾被漏判——教训：类型判断不能只看 ival/长度）**。

**函数详解**：

| 函数 | 详解 |
|---|---|
| `sym_lookup/add/free` | 符号表线性查找/追加/释放。**Bug1 根因：sym_lookup 线性返回首个匹配 + locals 表跨函数未清空 → agent_reply 的参数 cid 错绑到 R19**。修复：`cg_func` 入口 `sym_free(&locals)` 保证函数独立作用域 |
| `emit(cg, op, a, b, imm)` | 追加一条指令到程序 |
| `emit_jmp_lbl / place_label` | 跳转指令先占位（lbl 编号入 imm），place_label 时回填偏移 |
| `alloc_reg / lookup_var / declare_var / declare_array_var` | 寄存器分配：全局区 R4..R23、局部区 R24..R49；数组变量记录 is_array（下标访问走 LOAD64/STORE64 地址运算） |
| `cg_expr(cg, AstNode*)`（285 行起） | **表达式编译**（前向声明在 207 行）：LITERAL→MOV imm；IDENT→MOV reg；BINOP→cg_binop；CALL→压参+CALL+结果进 R63；INDEX_ACCESS→地址运算+LOAD64；赋值表达式→求右值+MOV 到变量寄存器 |
| `cg_binop` | 左子→R50，右子→R51，再发 OP_ADD/SUB/...（b 操作数：寄存器编码为非负数，立即数编码为负数——`is_reg_operand` 约定 b>=0 是寄存器下标） |
| `cg_cond(cond, lbl_true)` | 条件编译到「为真跳 lbl_true」：比较运算直接映射 JZ/JNZ/JE/JNE/JG/JL 系列 |
| `cg_stmt`（435-678 行） | **语句编译**：VAR_DECL/ASSIGN（+字符串变量标记）、PRINT（**链式多段：首段 FFI print 换行、后续 append**——Bug2 修复点：parse_print 曾只解析首段，修复后 NODE_PRINT 的 args 循环全部编译）、IF/WHILE/FOR（标签回填）、FN_CALL、BREAK（跳到循环出口标签，标签栈实现）、RETURN（值进 R63+JMP 到函数尾）、INSTRUCTION/DATA_DECL（数据段 kprog_alloc_data） |
| `cg_linux`（221 行） | `linux_init/exec/read/end` 语句 → OP_LINUX_* 指令（阶段六桥接）；`linux_str_const` 判断命令参数是否字符串常量 |
| `cg_func(cg, fn)` | 函数编译总控：**先 sym_free(locals)**、形参依次绑 R0..R3、编译体、放置返回标签 |
| `KillsProgram* mo2kbc_compile(program, err, errlen)`（718 行） | **总入口**：遍历 program->args（顶层语句/函数），逐个 cg_stmt/cg_func；任何 fail() 置错误返回 NULL；成功返回 KillsProgram（调用方 kprog_free） |

**已知坑（全部修复，写在知识库）**：
1. **Bug1** 变量作用域跨函数污染 → 参数错绑寄存器（修复：cg_func 入口清 locals）
2. **Bug2** 链式 print 只编译第一段（修复：parser 循环解析 + cg 循环编译，首段 imm=0 新行 / 后续 imm=1 追加）
3. **CALL 多参交换 bug（已定位未修）**：cg_expr 逆序压参 + kvm OP_CALL 逆序弹参 = 双重逆序 → 参数颠倒；**单参不触发**。修复方向 = cg_func 弹栈方向不动 kvm
4. 数组声明不能带尺寸（`.mo` 语法约定）、元素赋值用 `a[i]=v` 裸语句
5. break 支持是后补的（NODE_BREAK type=21）；continue 曾显式报错

---

## 1.6 `src/mo2kbc_main.c`（90 行）— 独立编译器 CLI

**职责**：把 mo2kbc 包装成命令行工具：`.mo 文件 → .kbc 文件`（离线编译，供 ESP32 固件内嵌 xxd -i 数组、或 /api/vm/load 加载）。

| 函数 | 详解 |
|---|---|
| `static char* read_file(path)` | fopen rb → fseek END → ftell → malloc(sz+1) → fread。**全项目统一读文件模式**（见附录 E） |
| `int main(argc, argv)` | 用法 `mo2kbc input.mo [-o out.kbc]`：read_file → xiaomo_parse_source → 失败打印 err 返回 1 → mo2kbc_compile → kprog_serialize 落盘（wb）。`-o` 缺省写 stdout |

**产出 .kbc 格式**见附录 C。

---

# 第 2 卷 · Kills VM 内核

> 「Kills」是字节码内核的名字。设计目标：纯 C、可塞进 ESP32（后来的 openclaw-mini 移植证明可行）、支持函数调用与序列化。两个文件：`vm_stack.c`（字节栈）是地基，`vm_core.c`（解释器）是主体。

## 2.1 `include/vm_stack.h` + `src/vm_stack.c`（53+70 行）— 轻量字节栈

**职责**：VM 底层的统一内存原语——**同一个结构体当三种栈用**：操作数栈（表达式求值/传参）、调用栈（保存返回 PC + 寄存器现场）、以及线性内存的底层支撑。

**结构**：

```c
typedef struct {
    uint8_t* buf;   // 栈底
    size_t cap;     // 容量(字节)
    size_t top;     // 栈顶偏移(已用字节数) —— push 后 top 增大
} VmStack;
```

**函数详解**（11 个，全部短小）：

| 函数 | 语义 | 注意点 |
|---|---|---|
| `vstack_init(s, stack_size)` | malloc stack_size 字节，top=0。返回 0 成功 |失败 buf=NULL |
| `vstack_destroy(s)` | free + 三字段清零 | |
| `vstack_push(s, data, len)` | len 向上对齐 8 字节再压入（保证后续 u64 读对齐）；容量不足返回 -1 | 数据 memcpy 拷入 |
| `vstack_pop(s, out, len)` | 弹出 len 字节到 out（**out 可为 NULL = 纯丢弃**）；top 回退 | 不足返回 -1 |
| `vstack_peek(s, out, len)` | 看栈顶 len 字节不动 top | 用于 CALL 前偷看参数 |
| `vstack_pop_u64 / push_u64` | 8 字节快捷方式（内部 memcpy 避免对齐问题） | 最常用 |
| `vstack_used / remain` | 已用/剩余字节数 | 状态展示 |
| `vstack_reset(s)` | top=0（不释放内存） | kvm_run 每次开始时复位 |

**设计要点**：对齐到 8 是硬约定——调用栈的每帧是「1×u64 返回 PC + 64×u64 寄存器现场 = 520 字节」，pop/peek 都按 u64 粒度读写，不对齐会崩。

---

## 2.2 `include/vm_core.h`（201 行）— VM 数据模型与 ABI

**职责**：定义 Kills 指令集、程序表示、VM 实例、输入桥、输出 sink、序列化 API。**这份头文件是整个 VM 世界的宪法**。

### 2.2.1 指令集 `KillsOp`（完整表）

| 组 | 指令 | 编码语义 |
|---|---|---|
| 基础 | `OP_NOP=0` `OP_MOV` | MOV dst, imm64 或 MOV dst, src（b>=0 表寄存器） |
| 算术逻辑 | `OP_ADD SUB MUL DIV MOD AND OR XOR NOT SHL SHR` | dst op= (b>=0 ? regs[b] : imm)；DIV/MOD 除零→报错返回 1（带 pc） |
| 栈 | `OP_PUSH` `OP_POP` | PUSH src_reg 或 imm；POP dst |
| 内存 | `OP_LOAD` `OP_STORE`（1 字节）`OP_LOAD64` `OP_STORE64`（8 字节小端） | 地址来自寄存器；越界 → error_msg + 返回 1 |
| 跳转 | `OP_JMP` `OP_JZ JNZ JE JNE JG JGE JL JLE` | 相对跳转：next_pc = pc + (int32)imm；条件读 regs[a]，与 b>=0?regs[b]:0 比较 |
| 调用 | `OP_CALL` `OP_RET` | CALL fn_index：从操作数栈弹 nparams 个实参（**逆序**弹入 R0..，联动 Bug3）+ 压入完整现场（返回 PC+64 寄存器）到调用栈；RET 弹现场恢复 |
| 外部 | `OP_FFI` | imm = FFI 编号（0..6，见 2.3.3），结果写 result_reg |
| 输出 | `OP_PRINT` | 打印寄存器（按常量池类型或整数） |
| 系统 | `OP_HALT` | halted=1，kvm_run 正常返回 0 |
| 硬件直访 | `OP_HW_PCI_ENUM USB_ENUM SERIAL_ENUM CPU_INFO PCI_RD UART_OPEN UART_CLOSE UART_RD WR SYS_INFO SPI_XFER SPI_OPEN SPI_CLOSE GPIO_SET GPIO_EXPORT` | 宿主 hw_direct/hw_direct_stub 实现；PC_RD 的 imm 打包 (dev<<16\|func<<8\|offset) |
| Linux 桥 | `OP_LINUX_INIT EXEC READ END` | 阶段六：调弱符号 linux_init/linux_exec/...（无实现时弱符号返回 -1/0，VM 继续跑） |

**指令操作数编码总约定**：`int32_t b >= 0` → 寄存器下标；`b < 0` → 立即数（`is_reg_operand()`）。这是 is_reg_operand 唯一职责。

### 2.2.2 程序表示

```c
typedef struct { uint8_t op; int32_t a, b; int64_t imm; } KillsIns;   // 四元组指令
typedef struct { char* name; uint32_t pc, nparams; } KillsFunc;        // 函数表项
typedef struct { uint8_t type; int64_t iv; double fv; char* sv; } KillsConst;  // type: 0=int 1=str 2=float
typedef struct {
    KillsIns*  code;        uint32_t code_count;
    KillsConst* consts;     uint32_t const_count;
    uint8_t*   data;        uint32_t data_size;   // 线性内存(数据段)
    KillsFunc* funcs;       uint32_t func_count;
} KillsProgram;
```

### 2.2.3 VM 实例 `KillsVM`（逐字段）

| 字段 | 说明 |
|---|---|
| `int64_t regs[64]` | 通用寄存器堆（R63=返回值/FFI 参数载体） |
| `VmStack operand, callstack` | 操作数栈 + 调用栈（同一 VmStack 类型两个实例） |
| `const KillsProgram* prog` `uint32_t pc` | 当前程序与程序计数器 |
| `char** output; int output_count, output_cap` | 输出行缓冲（动态数组） |
| `kvm_output_sink_t output_sink; void* output_sink_ud` | **flush 边界回调**：只在整行 flush 时触发，不破坏链式 print 拼接（Shell 靠它拿增量输出） |
| `char io_q[16][2048]; int io_head,tail,count; char input_line[2048]; pthread_mutex_t io_mutex` | **per-VM 输入桥队列**：16 槽 × 2048B，WS 线程 push / VM 线程 pop，互斥保护；**队列满丢最老一行** |
| `int halted` | 停机标志（FFI input_wait 靠它逃逸——置位后最多 20ms 退出） |
| `volatile int in_input_wait, in_llm` | 看门狗豁免位：VM 阻塞等输入/等 LLM 网关时不算卡死（单写者=VM 线程） |
| `int error_count; char error_msg[1024]` | 运行时错误（除零/越界/step 超限） |
| `uint32_t step_limit, steps` | 防死循环上限（默认值见 kvm_init） |

### 2.2.4 头文件导出的 API 速查

```
程序构建:   kprog_new / kprog_add_ins / kprog_add_const / kprog_add_func
            kprog_alloc_data / kprog_free
序列化:     kprog_serialize(p, &out) -> len(-1 失败)   kprog_deserialize(buf, len, err, errlen)
执行:       kvm_init / kvm_free / kvm_run(vm, prog)
输出:       kvm_output_count / kvm_output / kvm_clear_output / kvm_set_output_sink
输入桥:     kvm_io_push_vm / kvm_io_pushf_vm          (外部线程注入一行)
调试:       kvm_disassemble(p, buf, buflen)
```

---

## 2.3 `src/vm_core.c`（841 行）— 解释器实现 ⭐⭐

**职责**：VM 全部运行时逻辑——程序构建 API、输出管理、输入桥、FFI 表、**主解释循环**、序列化、反汇编。本文件同时是宿主版（macOS/Linux/ESP32）三端共用的核心，ESP32 移植时只换 FFI 实现层。

### 2.3.1 弱符号 Linux 桥（43-47 行）

```c
__attribute__((weak)) LinuxVM *linux_init(...)  { return NULL; }
__attribute__((weak)) int linux_exec(...)       { return -1; }
__attribute__((weak)) int linux_read_output(...){ return 0; }
__attribute__((weak)) int linux_has_output(...) { return 0; }
__attribute__((weak)) void linux_end(...)       {}
```

OP_LINUX_* 到 TinyEMU 真内核的桥。**坑（已修）**：C/C++ 混编时弱符号名失配 → 需要包 `extern "C"`。宿主无 linux_embed 时 VM 正常执行、Linux 语句返回失败。

### 2.3.2 程序构建 API（53-104 行）

| 函数 | 详解 |
|---|---|
| `kprog_new` | calloc 程序，四个动态数组空 |
| `kprog_add_ins(p, op, a, b, imm)` | 追加指令，容量倍增 |
| `kprog_add_const(p, type, iv, fv, sv)` | 追加常量；**type=1 时 sv strdup 拷贝** |
| `kprog_add_func(p, name, pc, nparams)` | 追加函数表项（name strdup） |
| `kprog_alloc_data(p, size)` | **在数据段尾部划出 size 字节，返回起始偏移**（DATA_DECL 数组用；data 区按需 realloc） |
| `kprog_free` | 释放全部（含每个 const 的 sv、每个 func 的 name） |

### 2.3.3 输出系统（105-160 行）— 三层设计

```
kvm_add_output_ex(vm, s, append)
   append=0 → 新增一行（output 数组追加）
   append=1 → 拼到最后一行尾巴（链式 print / LLM 回复 append）
kvm_flush_output(vm)
   → 把未 flush 的输出发给 output_sink（若设置），并打 stdout
   ⚠️ 只在「阻塞边界」调用：input_read / input_wait 轮询里
      （Bug3 教训：每指令后 flush 会打断链式 append 拼接 → 输出拆行）
kvm_set_output_sink(vm, cb, ud)
   → Shell 用：拿整行增量广播 WS；须在 kvm_run 前设置
```

**kvm_output / kvm_output_count / kvm_clear_output**：给同步调用方（HTTP /api/vm/run 响应）读输出数组；clear 在 kvm_run 开头自动做。

### 2.3.4 输入桥（165-201 行）与分类器

| 函数 | 详解 |
|---|---|
| `kvm_io_push_vm(vm, line)` | 加锁，队列满时 **丢最老一行**（保证批量注入不覆盖最新消息），尾部入队 |
| `kvm_io_pushf_vm` | printf 风格包装 |
| `kvm_io_pending_vm / kvm_io_pop_vm` | 非空判断 / 出队并把行拷进 `vm->input_line` |
| `static int input_classify(s)` | 关键词分类器：返回 cid（0=默认/1=问候/2=算数/3=状态查询…按 openclaw 交互脚本约定），供 VM 脚本 `input_read` 的返回值分支 |

### 2.3.5 FFI 表 `kvm_ffi`（277-332 行）— 7 个内建外部函数

| idx | 函数 | 行为 |
|---|---|---|
| 0 | print_int | 读 `regs[63]`（约定参数寄存器），整数转字符串输出（append=0） |
| 1 | print_str | regs[63] 是**常量池下标**，取 consts[ci].sv 输出（type 必须=1） |
| 2 | sqrt | 读 regs[63]，写 result_reg（当前实现是平方——历史上简化过，语义名保留） |
| 3 | input_pending | result_reg = 队列非空 ? 1 : 0 |
| 4 | input_read | 出队一行 → 回显 `> 行`（append=0）→ result_reg = input_classify(行)；空队列返回 0 |
| 5 | input_wait | **阻塞版**：循环轮询 + usleep(20ms) + 每轮 kvm_flush_output；`vm->halted` 置位立即逃逸（否则阻塞中的 VM 永远杀不掉——看门狗强杀依赖此逃逸）；期间 `in_input_wait=1`（看门狗豁免）；拿到后同 4 |
| 6 | llm_query | 调 `llm_query_host(vm)`（219 行起）：**连本地网关 127.0.0.1:9101（llm_gate.py → SiliconFlow）**，TCP 长度前缀协议收单行回复 → append 到当前输出行；OK 返回 1。期间 `in_llm=1` 看门狗豁免 |

**FFI 参数约定**：参数固定从 `regs[63]`（R0 别名）读，结果写 `result_reg`（指令的 a 域）——单参数 FFI 的 ABI。

### 2.3.6 主解释循环 `kvm_run`（368-692 行）

```
初始化: prog/pc=0/halted=0/steps=0/error 清零/输出清空/双栈 reset/regs 清零
循环 while(!halted):
  1. steps++ > step_limit → error "step limit exceeded" 返回 1
  2. pc >= code_count → break（跑出代码段按正常结束）
  3. switch(ins->op) 执行（语义见 2.2.1 表）
     - 算术: dst op= (b>=0 ? regs[b] : imm)
     - LOAD/STORE/LOAD64/STORE64: 地址越界 → error_msg + return 1
     - JCxx 家族: next_pc = pc + (uint32)(int32)imm
     - CALL: 弹 nparams 实参 → R0..（⚠️ 逆序，联动 Bug3）→ 调用栈压 [返回PC + 64 寄存器] → pc = 函数入口
     - RET: 调用栈弹现场恢复 → pc = 返回 PC
     - OP_FFI: kvm_ffi(vm, (int)ins->imm, ins->a)
     - OP_PRINT: 按 imm 标志 append(1)/新行(0)，寄存器值输出（字符串常量走常量池）
     - OP_HALT: halted=1
  4. pc = next_pc
返回: halted=1 → 0；错误 → 1
```

**返回值约定**：0=正常 HALT；1=运行错误（error_msg 有内容）。调用方（xvm_run）据此把状态置 XVM_HALTED 或 XVM_CRASHED。

### 2.3.7 序列化（744-841 行）— .kbc 二进制格式

**`kprog_serialize(p, &out)`**：malloc 连续缓冲，按附录 C 的格式打包（魔数 "KILLS" + 版本 1 + flags + 三计数 + 常量池[类型+值+字符串长度+内容] + 数据段 + 指令表[每条 17 字节定长: op u8, a i32, b i32, imm i64] + 函数表）。返回总长，失败 -1。

**`kprog_deserialize(buf, len, err, errlen)`**：逐段校验（魔数、版本、长度哨兵）；任何不匹配 → err 写原因返回 NULL。**16MB 硬上限**在 xvm_load_kbc 层先拦（文件大小），本函数二次校验字段自洽。⚠️ 实测坑：**内嵌数组丢魔数头 → "bad magic" → VM 从未启动**（ESP32 移植时 xxd -i 少包了头，症状是「TCP 推了没人回」的假象）。

### 2.3.8 反汇编 `kvm_disassemble`（693-743 行）

`kvm_opname(op)` 静态表 → 遍历 code 输出 `NNN OP a b imm` 多行文本。调试用（HTTP 曾有 /api/vm/disasm 入口的构想，实际主要在开发期用）。

---

# 第 3 卷 · ESP 模拟器与集成层

> 这一组文件负责「芯片世界」：芯片参数表（chip_config）→ 模拟器状态机（emulator）→ VM 与模拟器的绑定（xiaomo_integration）→ 硬件直访 stub（hw_direct_stub / hw_direct.h 接口）。

## 3.1 `include/chip_config.h` + `src/chip_config.c`（118+261 行）— 芯片参数表

**职责**：10 款 ESP 芯片的官方参数静态表（数据来自 Espressif datasheet/TRM），供模拟器初始化和前端 `/api/chips` 展示。

**枚举**：
- `arch_type_t`：`ARCH_XTENSA_LX6 / LX106 / LX7 / ARCH_RISV32`
- `chip_id_t`：`CHIP_ID_ESP32=0, ESP8266, ESP32_S2, ESP32_S3, ESP32_C2, C3, C5, C6, C61, H2, CHIP_ID_COUNT` —— ⚠️ 与 emulator.h 的 `chip_type_t`（CHIP_ESP32/CHIP_ESP8266 只有 2 个）是**两套枚举**，历史遗留，main.c 做映射

**`ChipConfig` 结构**（完整字段）：`id` / `name` / `arch_name` / `arch` + 五个嵌套组：
- `chip_memory_t mem`：iram_addr/size、dram_addr/size、flash_addr/size、rtc_addr/size
- `chip_clock_t clock`：cpu_mhz / max_mhz / xtal_mhz
- `chip_uart_t uart`：count / default_baud / max_baud
- `chip_spi_t spi`、`chip_i2c_t i2c`
- `chip_gpio_t gpio`：total_pins / adc_channels / dac_channels / pwm_channels
- `chip_radio_t radio`：has_wifi / has_ble / wifi_2ghz / wifi_5ghz
- 散字段：has_bluetooth / has_usb_otg / has_i2s / core_count

**配置表要点**（g_chip_configs[]，249 行的数据体）：

| 芯片 | 架构 | IRAM | DRAM | Flash | CPU | UART/SPI/I2C | GPIO/ADC | 特性 |
|---|---|---|---|---|---|---|---|---|
| ESP32 | LX6 双核 | 320K | 520K | 4M | 240M | 3/2/2 | 34/18 | BLE+经典蓝牙, I2S×2, DAC×2 |
| ESP8266 | LX106 | 80K | 80K | 4M | 80(160)M | 1/1/**0** | 17/1 | 无蓝牙无硬件I2C |
| ESP32-S2 | LX7 单核 | 320K | 320K | 4M | 240M | 2/2/2 | 43/20 | USB OTG, 无BLE, 5G WiFi |
| ESP32-S3 | LX7 双核 | **512K** | **512K**+PSRAM | **8M** | 240M | 3/2/2 | 45/20 | BLE+USB OTG+I2S×2 |
| ESP32-C2 起 | RISC-V | 128K | 128K | 4M | 120M | … | … | C 系全部 RISC-V 32 |

**API**：`chip_get_config(id)`（边界检查→&g_chip_configs[id]）、`chip_get_name(id)`、`chip_get_count()`。

---

## 3.2 `include/emulator.h` + `src/emulator.c`（216+526 行）— 模拟器核心

**职责**：ESP32/ESP8266 外设状态机 + **指令级伪执行**。模拟层级是诚实的：不做真 Xtensa 指令译码（只有少量模式匹配），核心价值是**外设状态追踪 + 波形捕获 + UART/GPIO 事件流**，足以支撑 IDE 的波形面板和串口面板。

### 3.2.1 数据结构（emulator.h 全量）

| 结构 | 关键字段 | 说明 |
|---|---|---|
| `waveform_sample_t` | `uint64_t timestamp_us; uint32_t gpio_state` | 一个采样点：时间戳 + 全 GPIO 位图 |
| `waveform_capture_t` | `samples[262144]` `count` `head` `gpio_mask` | **环形缓冲**（~2.6s @100kHz），mask 决定监控哪些脚 |
| `uart_state_t` | `tx_buf/rx_buf[4096]` + 各自 head/tail + baud_rate + tx/rx_bytes | 环形 FIFO |
| `gpio_state_t` | `direction / output_level / input_level / pullup / pulldown`（u32 位图，GPIO_MAX=40）+ `adc_values[16]` | 一张位图管全部脚 |
| `spi_state_t` | 四 pin 号 + tx/rx_data[4096] + active | |
| `i2c_state_t` | sda/scl + dev_addr/reg_addr + data_buf[256] + ack | |
| `flash_t` | `uint8_t data[4MB]` + dirty | **4MB 直接 memset/fmemcpy 模拟**（注意：esp_sim_t 栈上/静态分配都很大，实例是静态全局） |
| `timer_state_t` | reload/counter/enabled/auto_reload/last_tick_us | |
| `sim_state_t` | `SIM_STOPPED/RUNNING/PAUSED/CRASHED` | 生命周期 |
| `esp_sim_t` | chip/chip_name + pc/reg[16]/sp/ps/cycle_count/clock_us/clock_mhz + uart0/uart1 + gpio + spi + i2c + timer[4] + flash + waveform + 统计 5 项 + iram/dram 指针与大小 | **模拟器总状态**（main.c 静态全局 `sim`） |

### 3.2.2 函数详解（emulator.c 全部 24 个）

| 函数 | 详解 |
|---|---|
| `now_us()` *static* | gettimeofday 微秒（真实墙钟，用于统计） |
| `esp_sim_init(sim, chip)` | 清零 + 按 chip_type 填 chip_name/clock_mhz（ESP32=240, 8266=80）+ iram/dram malloc + flash 清 0 |
| `esp_sim_reset` | 状态机回 STOPPED、pc/寄存器/外设/波形/统计全清（保留 flash 内容） |
| `esp_sim_load_firmware(data, len)` | **加载固件镜像**：识别头部——ESP32 image magic 0xE9 → 解析段表把各段 copy 到 IRAM/DRAM 对应地址；raw bin → 直接进 IRAM @0x40000000；失败返回 -1 |
| `esp_sim_load_raw(addr, data, len)` | 直接按地址写入 iram/dram/flash（边界检查） |
| `track_gpio_change(sim)` *static* | GPIO 变化时 total_gpio_changes++（波形采样在 waveform_start 后由 step 周期做） |
| `uart_tx_push(uart, byte)` | TX 环形 FIFO 入队（满则丢弃最老） |
| `emulate_pattern(sim)` *static*（250-341 行） | **伪执行核心**：① IRAM 空 → demo 模式：clock 每 1ms 步进、每 500ms 翻转 GPIO bit0、每 1s 向 uart0 推 `[ESP32] Hello #N (t=…us)` ② 有固件 → 逐 word 扫描做**最小 Xtensa 模式匹配**：NOP(0x1C)/RET/J/CALL/L32R 按操作码低 4 位识别，其余未知指令 +4 跳过；PC 出界 → CRASHED；每指令 cycle_count++、clock_us += 1MHz 比例 |
| `esp_sim_start(sim, entry)` | state=RUNNING、pc=entry、统计清零、start_time 记录 |
| `esp_sim_stop / pause / resume` | 状态机切换 |
| `esp_sim_step(sim, max_instructions)` | **驱动入口**（sim 线程每 20ms 调一次，max=100）：state 必须 RUNNING → emulate_pattern → 波形采样（若 capture 激活：按 10µs 间隔补样写入环形缓冲）→ 返回执行数 |
| `esp_sim_uart_write(sim, uart, data, len)` | 外部→RX FIFO（模拟设备收数据） |
| `esp_sim_uart_read(sim, uart, buf, max)` | TX FIFO → 调用方（主循环取走去 WS 广播） |
| `esp_sim_gpio_write(pin, value)` | 外部信号注入：input_level 位图置位/清零 + track_gpio_change |
| `esp_sim_gpio_read(pin)` | 输出方向回 output_level，输入方向回 input_level |
| `esp_sim_adc_write(channel, value)` | 注入 ADC 0-4095（模拟传感器） |
| `esp_sim_spi_transfer / i2c_transfer` | 状态记录 + 回显式回包（MOSI→RX 镜像），支撑协议面板 |
| `esp_sim_waveform_start(sim, gpio_mask)` | 清缓冲、记 mask、**记初始电平基线样本**（「点开始立刻见线」的修复） |
| `esp_sim_waveform_stop` | 停止采样 |
| `esp_sim_waveform_read(buf, max)` | 从环形缓冲顺序拷出最多 max 个样本，返回个数 |
| `esp_sim_status_text` | `"running @240MHz"` 风格状态串 |
| `esp_sim_chip_info` | JSON 对象字符串（`{"chip":"ESP32","freq":240,...}`——main.c 直接拼进 status 帧） |
| `esp_sim_destroy` | free iram/dram |

**架构诚实声明**：`emulate_pattern` 不是指令集仿真器——它让「加载了固件」有行为（能崩、能跳、能演示），真正的可编程执行走 VM 路径（xiaomo_integration）。前端波形面板吃的 GPIO 变化要么来自 demo 模式、要么来自 VM 脚本的 GPIO FFI、要么来自 WS `gpio_write` 注入。

---

## 3.3 `include/xiaomo_integration.h` + `src/xiaomo_integration.c`（118+387 行）— VM↔模拟器集成层 ⭐

**职责**：把 Kills VM 和 ESP 模拟器绑成一个「虚拟开发板」：.kbc 加载、FFI 注册（UART/GPIO/SPI/I2C 事件落进模拟器状态）、执行控制、输出缓冲→WS。头文件注释里画了完整三层架构图（模拟器/集成层/内核）。

### 3.3.1 数据结构 `xvm_instance_t`（逐字段）

| 字段 | 说明 |
|---|---|
| `KillsVM* vm; KillsProgram* prog` | 内核实例与程序（integration 拥有两者） |
| `esp_sim_t* sim; const ChipConfig* chip` | 绑定的模拟器（**可 NULL = 脱机模式**）+ 芯片参数 |
| `xvm_state_t state` | `XVM_IDLE/RUNNING/PAUSED/HALTED/CRASHED` |
| `uint32_t step_count, step_limit` | 已执行/单次上限（防死循环） |
| `char** output_buf; int output_count, cap` | 输出行缓冲（同 VM 的输出，集成层独立缓存一份便于 HTTP 读取） |
| `xvm_output_cb output_cb; void* output_ud` | 输出回调（main.c 用它把每行输出 WS 广播成 `vm_output` 帧） |
| `int uart_bound, uart_num` | VM 的 UART FFI 绑定到哪个模拟器串口 |

### 3.3.2 函数详解（18 个）

| 函数 | 详解 |
|---|---|
| `xvm_init(inst)` | vm=kvm_init 出一个、state=IDLE、step_limit 默认值、输出缓冲空 |
| `xvm_free(inst)` | kvm_free + kprog_free + 输出缓冲释放 |
| `xvm_bind_sim(inst, sim, uart_num)` | 存指针 + `kvm_ffi` 的硬件类操作重定向到模拟器（GPIO FFI → esp_sim_gpio_write 等） |
| `xvm_load_kbc(inst, path)` | **磁盘加载 .kbc**：fopen rb → ftell → **>16MB 拒绝** → 全读 → 交给 load_kbc_data。⚠️ 路径解析、魔数校验失败均返回 -1 带错误串 |
| `xvm_load_kbc_data(inst, data, len)` | kprog_deserialize → 成功则 kprog_free 旧的、替换 prog、state=IDLE |
| `xvm_compile_mo(inst, mo_source)` | **即时编译**：xiaomo_parse_source → 失败写 inst 错误串返回 -1 → mo2kbc_compile → 替换 prog（这是「编辑器里点运行」的主链路） |
| `xvm_push_output` *static* | 行存缓冲 + 若设了 output_cb 则回调（WS 广播点） |
| `xvm_output_count / get_output / clear_output` | 缓冲读取三件套 |
| `xvm_set_output_cb(inst, cb, ud)` | 注册输出回调（main.c 的 `xvm_output_ws_cb` 每行构造 `{"type":"vm_output","line":...}` 广播） |
| `xvm_run(inst)` | **执行到 HALT 或 step_limit**：state=RUNNING → kvm_run → 按返回值置 HALTED/CRASHED → 把 kvm 输出行同步进集成缓冲 |
| `xvm_step(inst, n)` | 单步 n 条（配合 kvm_run 内部？实际是对 prog 做 n 条的局部执行——用于单步调试视图） |
| `xvm_stop(inst)` | `vm->halted = 1`（**协作式停止**：FFI input_wait 20ms 内逃逸）+ state=IDLE |
| `xvm_reset(inst)` | kvm 重置现场（regs/栈/pc），保留 prog |
| `xvm_input_push(inst, line)` | → `kvm_io_push_vm`（WS `vm_input` 消息的落点） |
| `xvm_state_text(state)` / `xvm_is_running` | 状态字符串（"idle/running/paused/halted/crashed"） |
| `xvm_chip_info_json(inst)` | 用绑定的 chip 配置产 JSON（未绑 sim 用默认 ESP32） |

**线程模型**：xvm 本身不管线程；main.c 的 WS 线程只做 load/stop/input（快进快出），`vm_run` 在 WS 线程同步执行（HALT 型快程序）——**而 Shell 的 VM 跑独立线程**（见 5.2）。两套 VM 实例（模拟器 xvm + Shell xvm）靠 per-VM 输入队列互不串扰。

---

## 3.4 `include/hw_direct.h` + `src/hw_direct_stub.c`（80+131 行）— 硬件直访 stub

**职责**：`hw_direct.h` 定义 30+ 个「汇编级硬件访问」接口（x86 IO 端口/PCI 配置空间/MMIO/MSR/设备枚举/UART/SPI/GPIO sysfs）；`hw_direct_stub.c` 是 **macOS 上的桩实现**——绝大多数函数直接返回 -1 并置 `g_hw_error`，让 VM 的 OP_HW_* 指令**优雅失败**而不是崩溃。

**接口分组**（hw_direct.h）：

| 组 | 函数 |
|---|---|
| 错误 | `hw_get_error()` |
| x86 IO | `hw_inb/outb/inl/outl` |
| PCI | `hw_pci_read/write(bus,dev,func,offset,…)` |
| MMIO | `hw_mmio_map/unmap/read/write` |
| MSR | `hw_msr_read/write` |
| 枚举 | `hw_pci_enumerate / hw_usb_enumerate / hw_serial_enumerate(buf,len)` |
| CPU | `hw_cpu_info(buf,len)` |
| UART | `hw_uart_open(path,baud) / close / read / write` |
| 系统 | `hw_phys_mem_info(&total,&free)` |
| SPI | `hw_spi_open(dev_path,mode,bits,speed) / close / transfer` |
| GPIO | `hw_gpio_export/set_direction/write/read` |

**stub 行为明细**：
- 全部 IO/PCI/MMIO/MSR → return -1（g_hw_error = "not supported on this platform"）
- `hw_pci/usb_enumerate` → 空串返回 0（VM 枚举输出为空行，不报错）
- `hw_serial_enumerate` → 扫 `/dev` 返回 `cu.*`（**只认 cu. 不认 tty.**——PTY 枚举坑的宿主版源头）
- `hw_cpu_info` → 返回 sysctl 采集的真实 CPU 型号/核数
- `hw_phys_mem_info` → `#ifdef __APPLE__` 走 mach host_statistics（其他平台 sysconf）——**头文件里 `#if defined(__APPLE__) #include <mach/mach.h>` 是跨平台哨兵**
- `hw_gpio_*` → 全部 return 0（假成功，无副作用）

**注**：Linux/kickpi 版有真实现（spidev/sysfs），stub 只服务 macOS 构建与 ESP32 移植（ESP32 版同样是桩）。

## 3.5 `include/hw_demo.h`（17 行）— 硬件探测演示

单函数 `int hw_demo_run(void)`：跑一遍全部 hw_* 接口并打印结果（开发期自检工具，主程序未挂菜单）。实现体在 hw_direct_stub.c 之外的演示文件或同文件尾部（视构建裁剪）。

---

# 第 4 卷 · 服务层（httpd / db / main）

> 服务层三件套：`httpd.c` 是纯传输层（HTTP+WebSocket，零业务）；`db.c` 是 SQLite + 文件系统双写层（含路径保护模型）；`main.c` 是 1721 行的装配点+业务路由（HTTP 端点、WS 消息分发、C 运行时、线程编排）。

## 4.1 `include/httpd.h` + `src/httpd.c`（91+570 行）— HTTP + WebSocket 服务器

**职责**：kqueue 驱动的监听循环 + 每连接一线程处理 HTTP；WebSocket 升级、帧解析/发送、**广播**；木有任何业务知识（路由回调由 main.c 注入）。

### 4.1.1 数据结构

| 结构 | 字段 | 说明 |
|---|---|---|
| `http_request_t` | method / path[2048] / content_type[128] / host[256] / origin[256] / **sec_key[64]** / content_length / fd / body 指针+长度 | sec_key 是 WS 升级的握手钥匙；body 指向接收缓冲内部（零拷贝） |
| `http_response_t` | status / content_type[128] / body / body_len | body 由 handler malloc，发送后 httpd free |
| `ws_opcode_t` | `WS_TEXT=0x01 BINARY=0x02 PING=0x09 PONG=0x0A CLOSE=0x08` | |
| `httpd_t` | listen_fd / port / **on_request / on_ws_message / on_ws_connect** 三个回调 / running | 回调注入式解耦；on_ws_connect 用于连接后推初始状态快照 |

常量：`HTTP_MAX_CLIENTS 32`、`HTTP_BUF_SIZE 65536`、`WS_MAX_CLIENTS 16`。

### 4.1.2 函数详解（17 个）

| 函数 | 详解 |
|---|---|
| `base64_encode(in, len, out, max)` *static* | WS 握手响应要算 Sec-WebSocket-Accept（SHA1+base64 的 SHA1 部分在握手函数内联） |
| `ws_handshake(fd, sec_key)` *static* | 拼魔法串 `key + 258EAFA5-E914-47DA-95CA-C5AB0DC85B11` → SHA1 → base64 → 发 101 响应头 |
| `ws_parse_frame(buf, buflen, &op, &payload, &payload_len)` *static* | **三值语义（重构后）**：`0`=不完整帧（等更多字节，调用方拼帧）、`-1`=坏帧（断连）、`>0`=该帧总长并填出 op/payload。payload 指针指向缓冲内（不拷贝）。⚠️ 历史坑：旧版「只解析第一帧丢剩余」→ WS 粘包丢帧 20 连发丢 19 帧 → 重构为返回长度+调用方循环消费+memmove 搬残余 |
| `ws_send_frame(fd, op, data, len)` | 组服务端帧（无 mask）写 fd；**调用方持有 fd 锁语义**（广播靠互斥） |
| `ws_broadcast_text(data, len)` | 遍历注册表向所有 WS 客户端发文本帧；坏 fd 顺手 unregister |
| `ws_register / ws_unregister(fd)` *static* | 16 槽客户端 fd 表 + 互斥 |
| `parse_request(buf, buflen, req)` *static* | 手写 HTTP 头解析：请求行 method/path/version、逐行找 Content-Type/Host/Origin/Sec-WebSocket-Key/Content-Length，body 指针定位 |
| `http_send_response(fd, res)` | 拼 status 行 + 通用头（CORS `*`、Connection: close）+ body 发送，free body |
| `http_send_json(fd, status, json)` | 便捷包装（Content-Type: application/json） |
| `http_send_file(fd, path, ctype)` | fopen rb 全读（统一读文件模式）→ 发送；**404 时 Content-Type 仍是 text/html 的坑**（前端按扩展名推断） |
| `ws_thread_func(arg)` *static* | ——实际上 WS 处理在 handle_client 内联（此函数名历史上是线程体，现为每连接线程入口的一部分） |
| `handle_client(fd)` *static* | **连接主处理**：recv 循环 → parse_request → **若带 Sec-WebSocket-Key** → ws_handshake + register → 进入 WS 长连接循环（recv → ws_parse_frame 循环消费多帧 → op 分派：CLOSE/PING→PONG/TEXT→on_ws_message 回调；**SO_RCVTIMEO 5s 空闲误判断连坑的修复点：EAGAIN=空闲继续等**）→ 否则走 HTTP：on_request 回调 → 发送 → close |
| `httpd_start(server)` | socket/bind/listen（SO_REUSEADDR）→ 循环 accept（每连接 pthread 一个跑 handle_client） |
| `httpd_stop(server)` | running=0 + close listen_fd + join |

**性能模型**：单线程 accept + 每连接一线程；64KB 接收缓冲；WS 广播 O(n) 遍历。对 IDE 单用户场景足够，坑都出在「时间」（超时/粘包/阻塞）而不是「规模」——三次事故（preconnect 卡死、WS 粘包、5s 误判）全是时间语义 bug。

---

## 4.2 `include/db.h` + `src/db.c`（89+1100 行）— SQLite + 文件系统层 ⭐⭐

**职责**：两副担子——① SQLite 表的 CRUD（项目/固件/串口日志/仿真会话/模板/文件索引）② **磁盘文件管理的核心逻辑**（fm_* 路径模型 + 双区保护 + 双写同步）。文件树真相在磁盘，DB 只是镜像。

### 4.2.1 数据库表（db_init 建）

| 表 | 关键列 | 用途 |
|---|---|---|
| projects | id, name, chip_type, source_code, created, updated | 项目 |
| firmwares | id, project_id, name, data(BLOB), size, created | 固件二进制 |
| serial_logs | id, project_id, direction(tx/rx), data, created | 串口日志 |
| sim_sessions | id, project_id, chip_type, config, created | 仿真会话 |
| templates | id, name, chip, code | 代码模板（db_init_templates 播种 5 个） |
| files | id, project_id, name, path, content, is_dir, updated | 文件索引（**磁盘镜像**） |

### 4.2.2 路径与保护模型（fm_* 前缀，收集在 772-813 行）

| 函数 | 详解 |
|---|---|
| `fm_valid_rel(p)` *static* | 相对路径合法性闸：非空 / 不以 `/` 开头 / **不含 `..`**（防穿越）/ 长度 ≤900。所有写入口的第一道门 |
| `int fm_is_protected(const char* p)` | **架构区判定**：`xiaomo` 或 `xiaomo/...` → 1（只读）；`mycode/...` 与其他 → 0。导出（main.c 也用）。⚠️ 教训：保护必须覆盖 write/create/delete/move **四个入口**，只拦 write 会漏出幽灵记录 |
| `fm_resolve_path(p, out, outsz)` *static* | 相对→绝对映射规则：`xiaomo/…` → cwd/xiaomo/…、`mycode/…` → cwd/mycode/…、**裸路径兜底 → cwd/mycode/…**（防止旧客户端裸路径误写架构区） |
| `db_file_root_path(out, sz)` | 返回项目根：cwd/xiaomo 存在且是目录则用它，否则回退 cwd（旧版兼容） |

### 4.2.3 JSON 辅助

| 函数 | 详解 |
|---|---|
| `json_escape(s)` *static* | `"` `\` `\n` `\r` `\t` 与控制字符转义。**⚠️ 三次同款堆越界的 origin**：曾按原文长度 malloc，转义膨胀（尤其 `\n`→`\\n`、控制字符→`\u00XX` 6 倍）→ 越界写 → 大文件必崩。修复＝按最坏 6x 分配 |
| `json_array_from_db(sql)` *static* | 执行 SELECT → 按行拼 JSON 数组（列按序号手取） |

### 4.2.4 CRUD 函数全表

| 函数 | 详解 |
|---|---|
| `db_init(db_path)` | sqlite3_open + 建表 + **db_init_templates 播种**（首启才插模板，INSERT OR IGNORE 防重复）。返回 0 成功 |
| `db_close` | sqlite3_close |
| 项目 | `db_project_create(name, chip, code)` → 新 id；`db_project_list()` → JSON 数组（malloc，调用方 free）；`db_project_get(id)`；`db_project_update(id,…)`；`db_project_delete(id)` |
| 固件 | `db_firmware_save(project_id, name, data, len)`（BLOB bind）；`db_firmware_list(project_id)`（不含 data 只含元信息）；`db_firmware_get(firmware_id, buf, max_len)`（拷出到调用方缓冲）；`db_firmware_delete(id)` |
| 串口日志 | `db_serial_log_save(project_id, direction, data)`；`db_serial_log_list(project_id, limit)`（倒序取 N 条） |
| 仿真会话 | `db_sim_session_save / db_sim_session_list` |
| 模板 | `db_init_templates`（播种）；`db_template_get(name)`；`db_template_list` |
| 文件(id 版) | `db_file_create(project_id, name, path, content, is_dir)`；`db_file_list(project_id)`；`db_file_get(file_id)`；`db_file_update_content(id, content)`；`db_file_delete(id)`；`db_file_delete_by_project(project_id)`（级联，删项目时用） |

### 4.2.5 磁盘文件管理（path 版，双写层）⭐

| 函数 | 详解 |
|---|---|
| `db_file_create(...)`（537 行） | DB INSERT + 磁盘创建双动作。规则：`project_id≤0` 兜底挂项目 1；`untitled*` 名字**只进 DB 不落盘**（前端占位符）；**⚠️ 架构区拦截必须在 INSERT 之前**（曾先插后拦 → xiaomo/ 区出幽灵记录，前端显示假文件） |
| `scan_dir_recursive(base, rel, …)` *static*（594 行） | **文件树真相来源**：opendir 递归扫磁盘。跳过：`.` 开头、node_modules、.git、db、dist、web、public、*.dSYM、macos-app、staging、esp32sim 二进制等；**先收集目录（排序后输出 `path/` 形式，id=0），再扫文件**（带扩展名过滤）。产出 JSON：`{"id":0,"name":"xx","path":"xx/","is_dir":1,"updated":""}` |
| `db_file_list(project_id)` | = scan_dir_recursive(root) 的 JSON（**不读库**）+ DB 中 untitled 占位记录合并 |
| `db_file_delete_path(path, err, errsz)` | fm_valid_rel → 保护区拦截 → fm_resolve_path → `rmdir`（目录须空）/ `unlink` → DB 目录**级联删**（`path LIKE ?\|\|'/%'`）+ 自身删。err 返回人类可读原因 |
| `db_file_move_path(from, to, err, errsz)`（860 行，**全项目最长函数之一**） | 七步链：双端保护拦截 → 源存在/目标不存在（stat）→ fm_resolve 双端 → 逐级 mkdir 父目录 → `rename()` → **磁盘级复核**（源消失 + 目标存在，防文件系统假成功）→ DB：先 UPDATE 所有 `path LIKE from/%`（子孙前缀改写），再改自身 path+name。加固史：连续斜杠折叠（防 rename 自空操作）、尾斜杠归一化（移入目录保留原名） |
| `db_file_write_path(path, content, project_id, err, errsz)` | fm_valid_rel → 保护区拦截 → resolve → fopen wb 落盘 → DB upsert（按 path 找 id 更新 content，没有则 INSERT） |

**设计总纲**（读这层的钥匙）：**磁盘为真相，DB 为索引**；写=双写、删=双删、移动=双改；列表=扫盘；保护=fm_is_protected 四入口全拦；路径穿越=fm_valid_rel 全入口先验。

---

## 4.3 `src/main.c`（1721 行）— 主程序：装配 + 路由 + C 运行时 ⭐⭐⭐

**职责**：① 全局装配（httpd 回调注入、db 初始化、线程启动）② 全部 HTTP 端点业务（~30 个）③ 全部 WS 消息类型业务（~26 个）④ **C 运行时**（编译+执行用户 C 代码 + FreeRTOS shim 生成）⑤ 模拟器线程 ⑥ 启动播种与打包路径探测。

### 4.3.1 全局与辅助（1-160 行）

| 函数/全局 | 详解 |
|---|---|
| 全局 `esp_sim_t sim` | 模拟器实例（含 4MB flash，必须静态） |
| 全局 `xvm_instance_t g_xvm` | 模拟器侧 VM |
| 全局 `int current_project_id` | 当前选中项目（串口日志归属） |
| `json_get_string(json, key)` *static* | 手写 JSON 取值：找 `"key"` → 跳过 `:"` → 到未转义 `"` 止，malloc strdup。**够用但简陋**：不支持嵌套/数组转义 |
| `json_get_body(req)` *static* | req->body 副本（malloc） |
| `json_response(code, msg)` *static* | `{"code":N,"msg":"..."}` |
| `json_escape_local(s)` *static* | 同 db.c 的教训：**按最坏 6x malloc**（修过的堆越界） |
| `xvm_output_ws_cb(line, ud)` *static* | VM 每行输出 → WS `{"type":"vm_output","line":"..."}` 广播 |
| `find_base_path()` | **打包路径探测**：以二进制所在目录为 cwd 基准（`.app` 里从 userData 根启动）；找不到资源依次回退 |
| `on_signal` | SIGINT/SIGTERM → 优雅停机（httpd_stop + db_close + shell_shutdown） |

### 4.3.2 模拟器线程（161-238 行）

`sim_thread_func`：`sim_running` 期间循环——sim.state==RUNNING 时：
1. `esp_sim_step(&sim, 100)`；返回 ≤0 且 CRASHED → **广播 `sim_event: crashed`**（修复：此前 crash 静默，前端永远显示 running）并退出循环
2. `esp_sim_uart_read` 非空 → 转义拼 `{"type":"uart","port":0,"data":"..."}` 广播 + `db_serial_log_save(tx)`
3. 每 25 次（≈500ms）广播 `{"type":"status", …chip_info}`
4. usleep 20ms（防止 WS 消息洪流淹没浏览器）

`sim_thread_stop_join`：置停 + join（修复：此前 sim_stop 只置状态，线程与 sim_running 残留 → crash 后重启/多窗口状态全失联）。

### 4.3.3 WS 消息分发 `on_ws_message`（255-573 行）— 26 种 type 全表

| type | 行为 |
|---|---|
| `sim_start / sim_stop / sim_pause / sim_resume / sim_reset / sim_step` | 模拟器状态机操作；step 带 n 参数；各回 `sim_event` 帧 |
| `chip_select` | esp8266/esp32 → chip_type_t 映射 + esp_sim_reset → `chip_changed` |
| `flash_firmware` | base64 解码 → esp_sim_load_firmware |
| `uart_input` | → esp_sim_uart_write（RX 方向注入） |
| `gpio_write` | **按钮/前端注入**：置模拟器输出电平位图 + **按固件同款位串广播 `hw_gpio` 帧**（`bits[i]` ↔ IO(i+1) 约定；修复「采集卡抓不到模拟器电平」——两条链路原本完全平行） |
| `gpio_read` | 回 `gpio_state` 帧 |
| `hw_write` | 真机串口直写（hw_serial_write） |
| `adc_write` | esp_sim_adc_write 注入 |
| `waveform_start / stop / read` | 波形捕获三操作；read → 采样数组回帧（前端 150ms 轮询；mask 用 strtoul base0——`parseInt('0xFF')=0` 坑的后端侧修复） |
| `spi_transfer` | esp_sim_spi_transfer 回包 |
| `vm_load` | path → xvm_load_kbc（磁盘 .kbc） |
| `vm_load_data` | data → xvm_load_kbc_data |
| `vm_run / vm_step / vm_stop / vm_reset` | xvm 执行控制；**vm_run 同步执行**（HALT 型快程序），输出走 xvm_output_ws_cb 逐行广播 |
| `vm_input` | → g_xvm.input_push（交互输入桥） |
| `vm_compile` | **编辑器主链路**：读 path（全读文件模式）→ xvm_compile_mo → 回 `vm_compile_result`（成功带指令数） |

WS 连接建立（on_ws_connect）：推 `hello` 快照（chip/状态/vm 状态/shell 状态）。

### 4.3.4 HTTP 路由 `handle_request`（970-1603 行）— 全端点表

| 端点 | 方法 | 行为 |
|---|---|---|
| `/api/projects` | GET/POST | 列表 / 创建（name/chip_type/source_code） |
| `/api/projects/` | PUT/DELETE（带 id） | 更新（含 source_code 保存）/ 删除（级联文件记录） |
| `/api/project/select/<id>` | GET | 置 current_project_id |
| `/api/files/read/<path>` | GET | **手写 %xx URL 解码** → 双前缀路径模型（`xiaomo/`、`mycode/` 前缀按原样；裸路径补 `xiaomo/`）→ 磁盘读（统一读文件模式）→ 返回 `{code:0, name, content}` |
| `/api/files/write` | POST | `{path, content}` → db_file_write_path（保护+双写） |
| `/api/files/delete` | POST | `{path}` → db_file_delete_path |
| `/api/files/move` | POST | `{from, to}` → db_file_move_path |
| `/api/files/` (list) | GET | db_file_list（扫盘 JSON 树） |
| `/api/firmware` | POST/GET | 固件保存（BLOB）/列表 |
| `/api/firmware/<id>` | GET/DELETE | 固件取回（二进制）/删除 |
| `/api/templates` `/<name>` | GET | 模板列表/取模板 |
| `/api/hw/ports` | GET | hw_list_ports 枚举串口 |
| `/api/hw/connect` `/disconnect` `/status` `/reset` | POST/GET | 真机连接管理；reset → hw_reset_target（DTR/RTS 复位脉冲） |
| `/api/hw/detect` | POST | hw_detect_start（异步芯片检测，结果 WS `hw_detect` 广播） |
| `/api/hw/chunk` | POST | `{name, idx, data(b64)}` → hw_chunk_save（烧录文件分片上传） |
| `/api/hw/assemble` | POST | `{name, total}` → hw_assemble → 返回文件路径 |
| `/api/hw/flash` | POST | `{port, baud, chip, file, offset}` → hw_flash_start（异步，进度 WS 广播） |
| `/api/c/run` | POST | **C 运行时**（见 4.3.5） |
| `/api/chips` | GET | chip_config 十款芯片 JSON |
| `/api/sim/status` | GET | 模拟器状态快照 |
| `/api/vm/compile` | POST | `{code}` → xvm_compile_mo → 结果 JSON |
| `/api/vm/load` | POST | `{path}` → xvm_load_kbc |
| `/api/vm/run` | POST | 同步执行 → `{code:0, output:[...]}` |
| `/api/vm/status` `/api/vm/reset` | GET/POST | VM 状态/复位 |
| `/api/shell/history` | GET | shell_history_json（800 行环形历史快照） |
| 其他 | * | 静态文件服务：public/ 下按 mime 表（get_mime：html/js/css/png/svg…）http_send_file；`/` → index.html |

**响应约定**：业务端点统一 `{"code":0,...}` / `{"code":-1,"msg":"..."}`（⚠️ 曾有创建路由漏 code 字段 → 前端 `r.code!==0` 把成功判败 → 修复为规范化输出 + 前端 `code===0||id>0` 双保险）。

### 4.3.5 C 运行时（574-968 行）— 「▶ 运行」按钮的后端 ⭐

| 函数 | 详解 |
|---|---|
| `c_run_kill_handler(sig)` *static* | 子进程 SIGTERM 处理器：直接 `_exit(0)`（避免析构卡住） |
| `c_run_json_escape(s)` *static* | 输出转义（6x 分配同款教训） |
| `c_run_write_frt_shims(dir)` *static*（599 行） | **FreeRTOS/ESP-IDF 兼容 shim 生成**：mkdir -p 后在临时目录写 5 个头文件——`freertos/FreeRTOS.h`（pdMS_TO_TICKS/pdTRUE/TickType_t 等基础宏）、`freertos/task.h`（vTaskDelay→usleep、xTaskCreate→pthread 真并发、xTaskGetTickCount）、`freertos/semphr.h`（**#error 英文报错**明确不支持——防沉默，错误信息指引看 API docs）、`driver/gpio.h`（gpio_set_level 等→`[GPIO] set_level IO2=1` 可读日志行）、`driver/uart.h`（uart_config_t 结构 + param_config/install/read/write→`[UART]` 日志行）。⚠️ 三个坑：注释里含 `gpio_*/uart_*` 的 `*/` 会提前终止注释（改写文案）；引号 include 的解析起点是 includer 所在目录（driver/ 下要 `../freertos/...`）；`#error` 中文在无 locale 子进程被 clang 吃掉（改英文） |
| `c_run_compile_and_execute(code, run_timeout)` *static*（786 行） | **C 运行主链路**：① `mkdtemp("/tmp/esp32sim_crun_XXXXXX")` ② 写 main.c ③ 写 shims ④ `cc -O1 -o prog main.c shim_main.c`（编译先 60s 超时）→ err.txt/exit.txt 收产物 ⑤ 编译失败 → 返回 `{ok:false, compile_error}` ⑥ 成功 → fork + pipe + select 读 stdout（行缓冲聚合）+ waitpid（**漏 WIFSIGNALED 曾把崩溃误报 exit 0** → 修复 + crash:true + 前端 💥 分支）⑦ 超时 → kill（killed:true 语义：无限循环程序如 Blink 模板靠这个判别）⑧ 返回 `{ok, exit_code, killed, output, compile_error?}` ——目录用完即焚。app_main 缺省时自动写 shim_main.c 桥接重试（ESP32 风格源码零改动） |

### 4.3.6 `main()`（1649-1721 行）

流程：信号注册 → find_base_path → **启动播种**（mkdir mycode + hello.c 等种子文件，fopen wb）→ db_init(esp32sim.db) → esp_sim_init(ESP32) → xvm_init + bind_sim + set_output_cb → shell_init(send=ws_broadcast) → httpd 回调注入 → pthread 起 sim 线程 → `httpd_start(&server)`（阻塞循环）。argv[1] = 端口（**必须给**，否则默认 9877）。

---

# 第 5 卷 · 真机外设与 Shell

## 5.1 `include/serial_hw.h` + `src/serial_hw.c`（46+712 行）— 真机串口 + esptool 烧录

**职责**：真实硬件链路——串口枚举（`/dev/cu.*`）、termios 连接、读线程行协议分流（`#ADC:`/`#GPIO:` 不进串口日志）、DTR/RTS 复位唤醒、esptool 芯片检测与烧录（异步线程）、烧录文件分片重组。

### 5.1.1 数据结构

| 对象 | 说明 |
|---|---|
| `hw_port_t` | `{path[256], type[64], name[128]}`；type 取值 "USB CDC/JTAG" / "USB-UART" / "UART" |
| `HW_MAX_PORTS 32` | 枚举上限 |
| 模块级静态状态 | 连接 fd、读线程句柄、pending_adc[]（批量缓冲）、pending_gpio_bits（位串）、pending_log（日志聚合缓冲）、busy 标志（detect/flash 互斥） |

### 5.1.2 函数详解（24 个）

| 函数 | 详解 |
|---|---|
| `b64_decode(in, &out)` *static | base64 → 字节（固件分片） |
| `json_escape(in, out, outlen, escape_nl)` *static | **这里有 out 上限保护**（按目标缓冲截断）；escape_nl 控制换行是否转义（串口日志保留换行、JSON 帧转义） |
| `find_python()` *static | 找 python3 解释器路径（esptool 依赖）：`/usr/bin/python3` → PATH which → homebrew 路径 |
| `hw_list_ports(ports, max)` | opendir("/dev") → **只认 `cu.*`**（tty.* 会让 macOS 重复+占用问题）→ ioctl/usb 细节分类 type → 返回数量 |
| `baud_const(baud)` *static | 波特率 → speed_t 常量表（9600~921600） |
| `hw_parse_line(line, len)` *static（197 行） | **行协议分流核心**：`#ADC:` → strtof 循环读逗号/空格分隔的批量数值进 pending_adc（非数字跳一字符，容错强）；`#GPIO:` → 逐字符收 `0/1` 位串进 pending_gpio_bits（固件遥测格式：`bits[i]` ↔ IO(i+1)，**与波形样本 bit n=GPIO n 差 1 是约定非 bug**）；其余行返回 0 |
| `hw_feed_bytes(data, n)` *static | 按行切分（\n/\r），static line 缓冲跨窗口保留不完整行；协议行分流（不进日志）、普通行进 pending_log；**单行超 4KB 强制按块交日志**（防缓冲区无限涨） |
| `serial_read_thread` *static | **60ms 聚合窗口**：read → hw_feed_bytes → 每 60ms 把 pending_log（串口日志帧）、pending_adc（`hw_adc` WS 帧，进度波形）、pending_gpio（`hw_gpio` 帧，电平徽标）打包广播并清空。**协议行不进串口日志**（前端三面板数据分流的根） |
| `hw_connect(port, baud)` | open + termios 配置（raw 模式、VTIME/VMIN）+ 起读线程。返回 0 ok |
| `hw_disconnect` | 关 fd + 线程收尾 |
| `hw_is_connected / hw_serial_write` | 状态查询 / write 发送（WS `hw_write` 落点） |
| `hw_reset_target` | **DTR/RTS 复位脉冲**（先拉序列再复位——ESP32 进入 bootloader/唤醒的key手法；排障技能里「串口 DTR/RTS 复位唤醒」即此） |
| `hw_status_json(buf, len)` | `{"type":"hw_status","connected":..,"port":..,"baud":..}` |
| `for_each_line(data, len, carry, carrysz, cb, ud)` *static | 通用行遍历器（carry 保留不完整行）——detect/flash 输出解析共用 |
| `detect_line_cb` *static | 解析 esptool flash_id 输出：chip 名、MAC、flash 大小 → 组 `hw_detect` WS 帧 |
| `detect_thread` *static | 子进程跑 `esptool --port X flash_id` → 逐行回调解析 → 广播；退出码非 0 回错误 |
| `hw_detect_start(port)` | busy 检查 + 起 detect_thread。返回 0=已启动 |
| `flash_line_cb` *static | 解析烧录进度（`Writing at 0x... (xx %)`）→ `hw_flash_progress` 帧；其余行 → `hw_flash_log`；结束 → `hw_flash_done` |
| `flash_thread` *static | esptool write_flash 子进程（波特率/chip/offset 参数拼接）→ 行回调 → done 帧 |
| `valid_chip(chip)` *static | 白名单：esp8266/esp32/esp32s2/s3/c2/c3/c6/h2/p4 |
| `hw_flash_start(port, baud, chip, file, offset)` | 参数校验 + busy 检查 + 起 flash_thread |
| `hw_busy` | detect/flash 进行中 |
| `hw_uploads_dir(buf, len)` | 上传根目录（cwd/uploads） |
| `hw_chunk_save(name, idx, b64data)` | b64 解码 → mkdir uploads/<name> → 写 `chunk_%04d`（idx 从 0） |
| `hw_assemble(name, total_chunks)` | **顺序拼接** total 个分片 → uploads/<name>.bin → 返回路径（malloc，NULL=失败） |

**与前端的三面板契约**：串口日志面板吃 `serial_log` 帧；ADC 波形面板吃 `hw_adc` 帧（自动量程）；GPIO 面板吃 `hw_gpio` 帧（电平徽标）。行协议是固件侧的约定（`#ADC:1.23,2.34` / `#GPIO:0110`）。

---

## 5.2 `include/shell.h` + `src/shell.c`（50+746 行）— 内置 Shell 后端

**职责**：模拟 IDE 的「模拟 Shell」窗口——11 个内建命令、历史环形缓冲、**独立线程跑交互 VM**（openclaw_interact.kbc：input_wait 阻塞等 send 注入 → 分类 cid → llm_query 网关回复）、8s 看门狗。头注释有完整架构图与 7 条严谨性设计。

### 5.2.1 严谨性设计 7 条（头文件原文概括）

1. VM 跑独立线程——input_wait 内部阻塞轮询，绝不占 WS 线程
2. per-VM IO 队列——与模拟器 xvm 并发互不串扰，互斥保护
3. run/mo 超时看门狗 8s——`vm->halted=1` 强杀，input_wait 有 halted 逃逸
4. 路径防穿越——realpath 校验必须落在工程根内
5. cat/mo 64KB 上限 + 二进制检测（防巨文件/二进制糊屏）
6. 历史 800 行环形缓冲 + 互斥锁；JSON 转义按最坏 6x 分配
7. 输出 sink 只挂 input_wait flush 边界（不破坏链式 print 拼接语义）

### 5.2.2 数据结构

| 对象 | 说明 |
|---|---|
| 历史行 | `{char level; char* text; long ts;}` 环形 800 行（level: `i`=输入回显 `o`=输出 `e`=错误 `s`=系统） |
| VM 会话状态 | 线程句柄、运行标志、当前程序名、swrl 互斥 |
| `shell_send_fn` | 广播函数指针（main.c 注入 = ws_broadcast_text） |

### 5.2.3 函数详解（28 个）

| 函数 | 详解 |
|---|---|
| `now_ms` *static | 墙钟毫秒 |
| `json_escape_dup` *static | 转义 + strdup（**6x 分配**——堆越界教训的又一处受害者） |
| `ws_json_str(json, key)` *static | **这里的 JSON 解析更完整**：处理 `\n` `\t` `\"` `\\` `\uXXXX` 转义（比 main.c 的 json_get_string 强，因为 Shell 要吃前端发来的任意文本） |
| `shell_output_line(level, fmt, ...)` *static | 格式化一行 → 存历史 + 组 `{"type":"shell_out","level":"i","text":"..."}` 广播 |
| `shell_vm_event(event, steps)` *static | VM 状态变化回帧 |
| `shell_xvm_sink(line, ud)` *static | VM 输出 sink → shell_output_line('o')（**只在 flush 边界收到整行**） |
| `shell_resolve_path(arg)` *static | 参数→绝对路径：相对路径基于工程根；**realpath 后校验必须落在根内**（防 `../../etc/passwd`） |
| `shell_read_file(path, &len, &is_bin)` *static | ≤64KB 读取；检测二进制（含 \0 或高比例非打印）→ is_bin 置位 |
| `shell_emit_text_lines(text)` *static | 多行文本逐行 output |
| `shell_vm_run_thread` *static | VM 线程体：set sink → kvm_run → 结束回 vm_event(idle) |
| `shell_vm_spawn_locked` *static | 起 VM 线程（持锁） |
| `shell_watchdog_thread` *static | **8s 看门狗**：VM 运行中且超 8s 且不在 in_input_wait/in_llm → halted=1 + 错误行输出 |
| 命令实现（388-609） | 见下表 |
| `shell_exec(raw)` *static | 去首尾空白 → 回显 `xiaomo> cmd` → 拆命令/参数 → 命令表分派（未知命令提示 help） |
| `shell_handle_ws(msg)` | WS 分发入口：msg 是 `{"type":"shell_exec","cmd":"..."}` → 执行，返回 1 已处理 / 0 非 shell 消息（main.c 优先问它） |
| `shell_history_json()` | 800 行历史全量快照 `{"lines":[{"l":"o","t":"...","ts":123},...],"vm":"idle","prog":"..."}`（HTTP /api/shell/history） |
| `shell_vm_state` | "idle/running/paused/halted/crashed" |
| `shell_init(send_fn, ud)` | 注册广播函数 + 起看门狗线程 |
| `shell_shutdown` | 收 VM 线程 + 看门狗（可安全重复调） |

**命令表**（11 个）：

| 命令 | 行为 |
|---|---|
| `help` / `?` | 命令帮助 |
| `ls [path]` | opendir+stat 列目录（字母序，目录加 `/`） |
| `cat <file>` | ≤64KB 文本输出；二进制拒绝 |
| `mo <file.mo>` | 读 .mo → xiaomo_parse_source+mo2kbc_compile → 装入 Shell VM 但**不运行**（报指令/常量/函数计数） |
| `run <file.mo>` | 编译 + 起 VM 线程运行（交互式：input_wait 等 send）；8s 看门狗护航 |
| `send <text>` | kvm_io_push_vm 注入一行（唤醒 input_wait）→ VM 分类 → llm_query → 回复经 sink 广播 |
| `ps` | VM 状态 + steps |
| `status` | VM 状态/当前程序 |
| `stop` | halted=1 停 VM |
| `reset` | VM 复位（保留程序） |
| `clear` | 清历史缓冲 + 广播清屏 |

**Shell VM与模拟器 VM 的关系**：两个独立 xvm/KillsVM 实例。模拟器 VM 跑用户在 VM 面板的程序；Shell VM 专为交互对话（openclaw_interact.kbc）。输入队列 per-VM 隔离是并发正确性的关键。

---

# 附录

## 附录 A · HTTP API 速查（30 端点）

```
GET  /api/projects                项目列表            POST /api/projects            创建项目
PUT  /api/projects/<id>           更新项目            DELETE /api/projects/<id>     删除项目
GET  /api/project/select/<id>     选中项目
GET  /api/files/list              文件树(扫盘)        GET  /api/files/read/<path>   读文件
POST /api/files/write             写文件(双写)        POST /api/files/delete        删文件
POST /api/files/move              移动/重命名
GET  /api/firmware                固件列表            POST /api/firmware            存固件
GET  /api/firmware/<id>           取固件(bin)         DELETE /api/firmware/<id>     删固件
GET  /api/templates               模板列表            GET  /api/templates/<name>    取模板
GET  /api/chips                   芯片参数表          GET  /api/sim/status          模拟器状态
GET  /api/hw/ports                串口枚举            POST /api/hw/connect          连接
POST /api/hw/disconnect           断开                GET  /api/hw/status           连接状态
POST /api/hw/reset                DTR/RTS复位         POST /api/hw/detect           芯片检测(异步)
POST /api/hw/chunk                分片上传            POST /api/hw/assemble         分片拼装
POST /api/hw/flash                esptool烧录(异步)
POST /api/c/run                   C编译执行(▶运行)
POST /api/vm/compile              编译.mo             POST /api/vm/load             加载.kbc
POST /api/vm/run                  运行VM              GET  /api/vm/status           VM状态
POST /api/vm/reset                VM复位              GET  /api/shell/history       Shell历史
其余路径                           静态文件(public/, mime按扩展名)
```

## 附录 B · WebSocket 协议速查

**客户端 → 服务端（26 种 type）**：
`sim_start sim_stop sim_pause sim_resume sim_reset sim_step chip_select flash_firmware uart_input gpio_write gpio_read hw_write adc_write waveform_start waveform_stop waveform_read spi_transfer vm_load vm_load_data vm_run vm_step vm_stop vm_reset vm_input vm_compile shell_exec`

**服务端 → 客户端（广播帧）**：

| type | 载荷要点 |
|---|---|
| `sim_event` | event: started/stopped/paused/running/reset/step/crashed |
| `chip_changed` | chip 名 |
| `uart` | port + data（转义文本） |
| `status` | chip_info JSON 展开（500ms 周期） |
| `vm_output` | line（VM 每行输出） |
| `vm_compile_result` | 成功/错误 + 指令数 |
| `hw_status` | connected/port/baud |
| `hw_detect` | chip/mac/flash 检测结果 |
| `hw_flash_progress/log/done` | 烧录进度/日志/完成 |
| `hw_adc` | 批量 ADC 数值（真机 50Hz 遥测） |
| `hw_gpio` | bits 位串（`bits[i]`↔IO(i+1)） |
| `gpio_state` | 模拟器电平位图 |
| `shell_out` | level(i/o/e/s) + text |
| `shell_vm` | shell VM 状态事件 |
| `hello` | 连接建立快照 |

## 附录 C · .kbc 字节码格式（Kills binary v1）

```
偏移  内容
0     魔数 "KILLS"       (5B)
5     版本 u8 (=1)
6     flags u8
7     寄存器数 u32 (=64)
11    数据段大小 u32
15    指令数 u32
19    常量池 × N:  [type u8][iv i64][fv f64][sv_len u32][sv bytes]   (type: 0=int 1=str 2=float)
      数据段 bytes (data_size)
      指令表 × M:  每条 17B 定长 [op u8][a i32][b i32][imm i64]
      函数表 × K:  [name_len u32][name][pc u32][nparams u32]
```

⚠️ 教训：ESP32 内嵌数组方式分发时**必须包含从魔数开始的完整缓冲**——丢头 → deserialize "bad magic" → VM 从未启动。

## 附录 D · 真机串口行协议

```
#ADC:<v1>,<v2>,...     → 批量 ADC 电压值（固件 50Hz 遥测，strtof 容错解析）→ WS hw_adc
#GPIO:<0101...>        → GPIO 电平位串，bits[i] = IO(i+1)                → WS hw_gpio
其他行                  → 串口日志（WS serial_log 面板）
```

- 固件侧由 telemetry_task 输出（50Hz ADC + IO2 LED 2Hz 位串）
- 波形样本 bit n = GPIO n，与广播位号差 1 —— **约定非 bug**
- 复位唤醒：`hw_reset_target` 用 DTR/RTS 脉冲（⭐ 排障关键手法：设备掉线 ping 不通先复位）

## 附录 E · 全项目通用模式与坑清单（读代码前必看）

**通用模式**：
1. **统一读文件**：所有模块同款 `fopen rb → fseek END → ftell → malloc(sz+1) → fread`（main/db/shell/httpd/mo2kbc_main/xiaomo_integration）
2. **JSON 转义按最坏 6x 分配**：三处 json_escape 全部因此修过堆越界（db.c/main.c/shell.c）
3. **回调注入**：httpd 三回调、shell send_fn、kvm output_sink、xvm output_cb——模块不反向依赖
4. **静态大对象**：esp_sim_t（含 4MB flash）、xvm 实例全为静态全局，避免栈溢出
5. **协作式停止**：一切长循环（VM/watchdog）都检查 halted/volatile 标志，20ms 粒度让步

**坑清单（已修复，防复发）**：

| # | 坑 | 根因 | 修复 |
|---|---|---|---|
| 1 | json_escape 堆越界（×3 处） | 按原文 malloc，转义膨胀 | 6x 分配 |
| 2 | preconnect 空连接卡死全服务 | 单线程 recv 无超时 | SO_RCVTIMEO |
| 3 | WS 5s 空闲误判断连 → 324 次重连风暴 | EAGAIN 被当 EOF | EAGAIN=继续等 |
| 4 | WS 粘包丢帧 | ws_parse_frame 只消费第一帧 | 三值返回+循环+memmove |
| 5 | PTY/串口枚举漏设备 | 只认 cu.* | 约定（真机列表只走 cu.*） |
| 6 | C 运行崩溃误报 exit 0 | waitpid 漏 WIFSIGNALED | 补判 + crash:true |
| 7 | sim crash 静默 | 前端永远显示 running | 线程广播 crashed |
| 8 | mo2kbc Bug1 参数错绑 | locals 跨函数污染+线性查找 | cg_func 入口清表 |
| 9 | mo2kbc Bug2 链式 print 断行 | parse 只解析首段 | 循环多段(imm=0/1) |
| 10 | ESP32 版输出拆行 | 每指令 flush 打断 append | flush 移到阻塞边界 |
| 11 | CALL 多参颠倒（**未修，单参不触发**） | 双重逆序 | 定位了，待修 |
| 12 | xiaomo/ 区幽灵文件 | create 先插 DB 后拦保护 | INSERT 前 fm_is_protected |
| 13 | 移动「原地留副本」 | 疑似旧前端 + rename 假成功 | 磁盘级复核+斜杠折叠+尾斜杠归一 |
| 14 | `parseInt('0xFF')=0` | 前端 mask 恒 0 | 传字符串+后端 strtoul base0 |
| 15 | GPIO 采集卡抓不到模拟器电平 | 两条链路平行无广播 | gpio_write 按固件位串广播 |
| 16 | 20 连发 WS 丢 19 帧 | 同 #4 | 同 #4 |
| 17 | FreeRTOS 模板编译不过 | 无 shim | c_run_write_frt_shims 运行时生成 |
| 18 | semphr 静默不支持 | 无提示 | #error 英文报错+文档指引 |

---

## 📎 文档覆盖度声明

- **16 个 .c + 16 个 .h 全部覆盖**，每个文件的每个导出函数（及关键 static）均有条目
- 函数行号基于 2026-09-06 版源码（src 共 9283 行）
- 「已知坑」均出自真实事故复盘（对应知识库 esp32-simulator-ide / esp32-sim-file-api-c-layer 页面）

