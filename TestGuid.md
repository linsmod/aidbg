# 构建合理测试程序与测试逻辑的指导（最终版）

## 1. 引言

`aidbg` 是一个基于 TitanEngine 的 GDB 风格原生调试器，支持多种调试功能。为了验证调试器的正确性和稳定性，需要构建合适的测试程序（被调试目标）并设计配套的测试逻辑。本文档旨在指导开发者和测试人员如何构建有针对性的测试程序，以及如何利用 `aidbg` 的命令集编写自动化测试脚本，确保各项功能得到充分覆盖。

---

## 2. 测试程序设计原则

- **确定性**：每次运行的行为一致，便于重复测试。
- **可控性**：通过命令行参数、环境变量或代码中的标记，能够触发特定代码路径（如函数调用、异常、内存访问等）。
- **可观测性**：程序中插入易于识别的输出或状态变化，便于验证调试命令的效果（例如打印变量值、栈回溯等）。
- **覆盖全面**：涵盖基本执行、断点、单步、内存操作、寄存器操作、异常处理、多线程、符号信息（PDB）等场景。
- **自包含**：避免外部依赖，编译后带上调试符号以便测试符号解析功能。

---

## 3. 推荐测试程序结构

建议构建一个或多个测试程序，分别针对不同功能领域。以下是核心模块的示例（使用 C/C++，MSVC 编译）。

### 3.1 基础功能测试程序 (`test_basic.c`)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int global_var = 42;

void func2(int a, int b) {
    int local = a + b;
    printf("func2: %d + %d = %d\n", a, b, local);
}

void func1(int x) {
    int y = x * 2;
    func2(x, y);
}

int main(int argc, char** argv) {
    printf("Hello, aidbg!\n");
    int i;
    for (i = 0; i < 3; i++) {
        func1(i);
    }
    global_var = 100;
    printf("global_var = %d\n", global_var);
    return 0;
}
```

**用途**：
- 设置代码断点（`main`, `func1`, `func2`, 循环内）。
- 测试条件断点（如 `i == 2` 时停止）。
- 测试 `info locals` / `info args` 显示局部变量和参数。
- 测试 `bt` 堆栈回溯。
- 测试单步（`stepi`, `nexti`）和继续运行。

### 3.2 内存与硬件断点测试程序 (`test_memory.c`)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int data[100];

void write_data(int idx, int val) {
    data[idx] = val;
}

int read_data(int idx) {
    return data[idx];
}

int main() {
    memset(data, 0, sizeof(data));
    write_data(10, 0x12345678);
    int v = read_data(10);
    printf("v = 0x%x\n", v);
    // 触发内存访问错误（可选，用于测试异常处理）
    // data[200] = 1;   // 可能访问越界
    return 0;
}
```

**用途**：
- 在 `data` 数组上设置内存断点（`mbreak` / `watch`），检测读写访问。
- 测试硬件断点（`hbreak`）在函数入口（`write_data`, `read_data`）。
- 测试 `dump` / `x` 命令查看内存内容。

### 3.3 异常与信号测试程序 (`test_exception.c`)

```c
#include <stdio.h>
#include <stdlib.h>

void cause_divide_by_zero() {
    volatile int a = 1, b = 0;
    int c = a / b;   // 除零异常
    (void)c;
}

void cause_access_violation() {
    int* p = NULL;
    *p = 0;          // 写入空指针
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "divzero") == 0) {
        cause_divide_by_zero();
    } else if (argc > 1 && strcmp(argv[1], "av") == 0) {
        cause_access_violation();
    } else {
        printf("Usage: %s [divzero|av]\n", argv[0]);
    }
    return 0;
}
```

**用途**：
- 测试调试器捕获异常（`exception` 停止），验证 `info proc` 和异常信息。
- 测试 `ignore` 或 `condition` 能否避免某些异常停止。

### 3.4 多线程测试程序 (`test_threads.c`)

```c
#include <windows.h>
#include <stdio.h>

DWORD WINAPI thread_func(LPVOID param) {
    int* p = (int*)param;
    for (int i = 0; i < 5; i++) {
        (*p)++;
        printf("Thread %lu: %d\n", GetCurrentThreadId(), *p);
        Sleep(100);
    }
    return 0;
}

int main() {
    HANDLE hThreads[2];
    int counter = 0;
    for (int i = 0; i < 2; i++) {
        hThreads[i] = CreateThread(NULL, 0, thread_func, &counter, 0, NULL);
    }
    WaitForMultipleObjects(2, hThreads, TRUE, INFINITE);
    CloseHandle(hThreads[0]);
    CloseHandle(hThreads[1]);
    printf("Final counter: %d\n", counter);
    return 0;
}
```

**用途**：
- 测试 `info threads` 和 `thread` 切换。
- 测试在多线程环境下断点命中。
- 测试 `continue` / `step` 在多线程中的行为。

### 3.5 符号与源码定位测试程序 (`test_symbols.c`)

```c
#include <stdio.h>

int add(int a, int b) {
    int result = a + b;   // line 5
    return result;
}

int main() {
    int x = 3, y = 4;
    int z = add(x, y);
    printf("z = %d\n", z);
    return 0;
}
```

**用途**：
- 测试 `break add` 按符号名设置断点。
- 测试 `list` 命令显示源码。
- 测试 `bt` 输出带有行号和文件名的堆栈帧。

---

## 4. 测试逻辑设计

测试逻辑以 `aidbg` 命令脚本或批处理形式组织。可以配合 `--batch`, `-ex`, `-x` 等选项实现自动化。

### 4.1 基本断点验证

```bash
aidbg --batch -ex "file test_basic.exe" -ex "break main" -ex "run" -ex "bt" -ex "continue" -ex "quit"
```
**预期**：
- 启动后停在 `main` 断点。
- `bt` 显示堆栈（至少包含 `main` 和调用链）。
- `continue` 使程序正常结束。

### 4.2 条件断点与忽略计数

```bash
aidbg --batch -ex "file test_basic.exe" -ex "break func1" -ex "ignore 1 2" -ex "run" -ex "info break" -ex "continue" -ex "quit"
```
**预期**：
- 忽略前两次命中，第三次停止。

### 4.3 内存断点

```bash
aidbg --batch -ex "file test_memory.exe" -ex "break main" -ex "run" -ex "watch data" -ex "continue" -ex "info break" -ex "quit"
```
**预期**：
- 当 `data` 被写入时触发断点。

### 4.4 异常捕获

```bash
aidbg --batch -ex "file test_exception.exe" -ex "set args divzero" -ex "run" -ex "info proc" -ex "quit"
```
**预期**：
- 在除零异常处停止，`info proc` 显示异常代码和地址。

### 4.5 单步与反汇编

```bash
aidbg --batch -ex "file test_basic.exe" -ex "break main" -ex "run" -ex "stepi 3" -ex "disas" -ex "quit"
```
**预期**：
- 单步三条指令后，反汇编显示当前指令。

### 4.6 变量与局部变量

```bash
aidbg --batch -ex "file test_basic.exe" -ex "break func2" -ex "run" -ex "info locals" -ex "info args" -ex "print a" -ex "set $rax = 5" -ex "continue" -ex "quit"
```
**预期**：
- `info args` 显示 `a=0,b=0`（第一次调用时）等。
- `print a` 显示变量值。
- `set $rax = 5` 修改寄存器（需确保寄存器有效）。

### 4.7 多线程切换

```bash
aidbg --batch -ex "file test_threads.exe" -ex "run" -ex "break thread_func" -ex "continue" -ex "info threads" -ex "thread 2" -ex "bt" -ex "quit"
```
**预期**：
- 在 `thread_func` 断点停止，`info threads` 显示所有线程，`thread 2` 切换上下文，`bt` 显示对应线程堆栈。

### 4.8 附加与分离（需要目标进程已运行）

```bash
# 先启动 test_basic.exe（不带调试）
test_basic.exe
# 然后运行 aidbg 附加
aidbg --batch -ex "attach <pid>" -ex "break func1" -ex "continue" -ex "detach" -ex "quit"
```
**预期**：
- 成功附加，设置断点，继续执行，分离后目标继续运行。

### 4.9 符号与源码行

```bash
aidbg --batch -ex "file test_symbols.exe" -ex "break add" -ex "run" -ex "list" -ex "bt" -ex "quit"
```
**预期**：
- `list` 显示 `add` 函数附近的源码，`bt` 包含行号信息。

### 4.10 内存搜索与字符串

```bash
aidbg --batch -ex "file test_basic.exe" -ex "run" -ex "strings main" -ex "search main 0x1000 48656c6c6f" -ex "quit"
```
**预期**：
- `strings` 输出可打印字符串，`search` 找到 "Hello" 的地址。

---

## 5. 测试程序编译策略（分层构建）

`aidbg` 的目标是像 Visual Studio 一样鲁棒，能够调试各种编译配置下的程序。但不同的编译设置对调试信息的完整度有直接影响。为高效验证调试器的各项能力，建议根据测试目的采用不同的编译策略。

### 5.1 方案 A：冒烟测试与鲁棒性验证（使用 VS 默认 Debug 配置）

- **编译方式**：直接使用 Visual Studio 生成的默认 Debug 构建（通常为 `/Zi` + `/DEBUG:FASTLINK`）。
- **适用场景**：日常开发、持续集成的冒烟测试。
- **验证重点**：断点命中、继续执行、指令级单步（`stepi`/`nexti`）、内存读写、异常捕获、栈回溯（`bt`）。
- **预期**：**必须 100% 通过**。这体现了 `aidbg` 像 VS 一样，能够适应主流编译环境开箱即用。
- **对应测试用例**：4.1, 4.2, 4.3, 4.4, 4.5, 4.7, 4.8, 4.10。

### 5.2 方案 B：深度符号与变量测试（专用内部测试构建）

- **编译方式**（推荐命令行，用于 CI/CD）：
  ```cmd
  cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:test.exe test.c /link /DEBUG:FULL /DYNAMICBASE:NO
  ```
- **关键参数解释**：
  - `/DEBUG:FULL`（替换默认的 `FASTLINK`）：强制生成包含完整类型信息的 PDB，这是 `info locals`、`info args` 和 `list` 能够稳定枚举变量的前提。
  - `/Od`（禁用优化）：防止变量被优化进寄存器或内联，确保变量可读。
  - `/Oy-`（保留帧指针）：辅助 x86 下的栈回溯稳定性。
  - `/DYNAMICBASE:NO`（关闭 ASLR）：固定模块基址，便于在自动化脚本中使用 `break 0x401000` 等硬编码地址进行回归测试。
- **适用场景**：仅用于验证 `aidbg` 符号解析引擎逻辑正确性的内部测试套件。
- **验证重点**：`info locals`、`info args`、源码列表（`list`）。
- **预期**：必须通过，以证明调试器自身的符号解析引擎没有逻辑缺陷。
- **注意**：此配置**仅供内部测试使用**，不应强制要求终端用户采用。
- **对应测试用例**：4.6, 4.9。

### 5.3 方案 C：Release 优化程序测试（验证逆向/反调试鲁棒性）

- **编译方式**：使用 `/O2` 优化，生成 PDB（`/Zi`）。
- **适用场景**：验证调试器在真实发布程序上的表现。
- **验证重点**：`disas` 反汇编、`stepi` 指令单步、内存断点、硬件断点。
- **预期**：即使在优化下，`aidbg` 的 `bt`（利用 `RtlVirtualUnwind`）和反汇编功能必须稳定工作。局部变量可能显示为 `<unreadable>`，这是优化导致的信息丢失，属于物理极限，并非调试器缺陷。

---

## 6. 自动化测试集成

可将上述测试命令整合为批处理文件或 Python 脚本，通过 `--batch` 模式运行，并检查返回码：

- `aidbg` 在 `--batch` 模式下，若目标程序崩溃或调试器命令失败（非软错误），返回非零。
- 可根据返回码判断测试通过与否。

### 6.1 环境前置检查

在执行批处理前，建议在脚本开头强制设置符号路径，避免因找不到 PDB 导致 `info locals` 空跑：
```cmd
set _NT_SYMBOL_PATH=%CD%
```
或在启动 `aidbg` 前，确保 `.pdb` 文件与 `.exe` 位于同一目录。

### 6.2 批处理示例

```cmd
@echo off
set _NT_SYMBOL_PATH=%CD%
aidbg --batch -ex "file test_basic.exe" -ex "break main" -ex "run" -ex "bt" -ex "quit"
if %errorlevel% neq 0 echo Test failed!
```

---

## 7. 注意事项

### 7.1 理解编译器优化对调试信息的影响（重要）

`aidbg` 的鲁棒性体现在“能调试任何构建”，但**没有任何调试器（包括 VS 和 WinDbg）能从已优化的二进制中凭空恢复被删除的局部变量信息**。

- 若使用 MSVC 默认的 **`/DEBUG:FASTLINK`**，`info locals` 可能因 PDB 缺少完整类型索引而枚举失败（显示为空）。这是 `dbghelp` API 的物理限制，**不是 `aidbg` 的 Bug**。
- 若使用 **`/O2`（Release 优化）**，局部变量会被优化掉，`info locals` 显示为 `<unreadable>`，单步（`step`）行为可能跳变。此时应使用指令级单步（`stepi`/`nexti`）和反汇编（`disas`）进行调试。

**建议**：在运行第 4.6 节（变量与局部变量）的测试用例时，**务必使用 5.2 节推荐的专用编译命令**，以确保测试结果反映调试器自身逻辑的正确性。

### 7.2 其他注意事项

- **符号路径**：确保 `_NT_SYMBOL_PATH` 环境变量或 PDB 文件位于可执行文件目录，否则 `info locals`/`list` 等依赖 PDB 的命令可能失效。
- **ASLR**：可通过 `set engine aslr off` 禁用 ASLR，使模块基址固定，便于测试地址相关断点（方案 B 已默认关闭）。
- **控制台输出**：在 `--batch-silent` 模式下，调试器输出被抑制，但错误信息仍会输出到 stderr，可用于日志记录。
- **异常处理**：某些异常（如访问违例）可能被操作系统或 CRT 处理，测试时应确保异常未被程序内部捕获，以便调试器可拦截。
- **多线程断点**：在多线程环境下，断点命中可能发生在任意线程，测试时需考虑线程切换带来的不确定性。
- **硬件断点限制**：x64 系统最多支持 4 个硬件断点（DR0~DR3），测试时避免同时设置过多。

---

## 8. 测试用例与编译配置映射（快速索引）

为高效执行测试，建议将第 4 节的测试用例按依赖的编译特性分类：

| 测试用例编号 | 测试内容 | 推荐编译配置 | 依赖说明 |
| :--- | :--- | :--- | :--- |
| 4.1, 4.2, 4.3, 4.4 | 断点、条件断点、内存断点、异常 | **方案 A（VS 默认 Debug）** | 仅需函数名和地址，`FASTLINK` 足够 |
| 4.5, 4.10 | 指令单步、内存搜索、反汇编 | **方案 A 或 方案 C** | 与 PDB 完整度无关 |
| 4.7, 4.8 | 多线程切换、附加与分离 | **方案 A（VS 默认 Debug）** | 依赖 TitanEngine 事件处理 |
| **4.6** | **变量与局部变量**（`info locals`） | **方案 B（内部专用构建）** | **必须**使用 `/DEBUG:FULL` + `/Od` |
| **4.9** | **源码与行号**（`list`） | **方案 B（内部专用构建）** | 行号信息在 `FASTLINK` 下可能缺失 |

---

## 9. 总结

通过构建覆盖不同功能的测试程序，并设计系统性的、分层级的测试逻辑，可以全面验证 `aidbg` 的调试能力：

1. **鲁棒性层面**：使用 VS 默认 Debug 配置验证断点、单步、异常捕获等基石功能，确保 `aidbg` 像 VS 一样开箱即用。
2. **深度逻辑层面**：使用专用内部构建（`/DEBUG:FULL`）验证符号解析和变量枚举功能，排除编译器优化干扰，确保调试器引擎逻辑无误。
3. **极限场景层面**：在 Release 优化程序上验证反汇编和指令级控制，确保调试器在逆向场景下的稳定性。

建议持续维护这些测试用例，并在每次代码变更后执行回归测试，确保调试器的稳定性和功能完整性。

C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe
C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe