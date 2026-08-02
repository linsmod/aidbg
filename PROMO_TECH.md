# linux有gdb，windows却没有一个像样的适合ai工具的命令行调试工具。如何让ai在windows上愉快的开发调试win32程序呢？aidbg 完全可以是你新的选择！

> **项目地址：** https://github.com/linsmod/-vc-dev-debuging-tool-for-ai-agent.git

## 问题背景

在 Linux 生态中，`gdb` 的批处理模式（`gdb -batch -ex`）一直是 AI 编程工具与自动化脚本
对接原生调试的工业标准：机器可读、进程级调用、命令语义稳定。反观 Windows：

- Visual Studio 调试器是 GUI 形态，无法被 AI/CLI 自动化消费；
- cdb/WinDbg 命令体系自成一体，输出冗长，与 GDB 心智模型差异大，AI 学习成本高；
- MinGW gdb 对 MSVC 产物、PDB 符号、Win32 异常与系统 API 断点支持不足。

**需求缺口**：一个命令集对齐 GDB、输出可机器解析、可进程级一次调用的 Windows 原生
调试器。aidbg 即为此而设计。

## 定位与实现

- **实现**：单文件 C++17（MSVC），链接 `TitanEngine.dll`，x64 Release 约 500KB。
- **架构**：主线程 REPL 与 DebugLoop 工作线程通过 condition_variable 同步；
  停止型回调阻塞等待主线程指令，退出型回调仅置事件；符号统一走 dbghelp
  （`SymInitializeW` + `fInvadeProcess=TRUE`，天然适配 ASLR 运行时基址）。
- **文档**：`handleover.md` / `handover2.md` / `handover3.md` 记录设计、实现与踩坑。

## 核心能力

### 1. GDB 命令集对齐（含语义对齐）

```
run / start [func] / continue(c) / stepi(si) / nexti(ni) / finish / bt
break / hbreak / mbreak / watch / rwatch / awatch / condition / ignore
registers / set / x / dump / disas(u) / search / strings / list
info break / threads / modules / proc / files / locals / args
attach / detach / delete / disable / enable / thread / set engine / file
```

关键语义与 GDB 一致（handover3.md，均已实测）：

| 行为 | 对齐结果 |
| :--- | :--- |
| `finish` | 停在调用方返回后的下一条指令（`main+0x21`），非被调函数末尾 |
| `start` / `start <func>` | 临时断点停在入口，输出 `Temporary breakpoint 1, main () at file.c:line` |
| run 前符号断点 | `break main` 记为 pending，`run`/`attach` 后按实际基址落地（A1） |
| `break <行号>` / `break file.c:NN` | 行号→地址解析，超范围行号报 `No line N`（A2） |
| `x/<n>i` | 指令反汇编（格式字符集 `xduicsfi`，`i` 不再落入尺寸集） |
| `thread <id>` | 先按 OS TID 精确匹配，再按 1-based 内部编号匹配 |
| `disable`/`enable` 无参 | 作用于全部断点（GDB 语义） |
| `info files` | 符号文件 + 入口点（PE `AddressOfEntryPoint`）+ 加载文件列表 |
| `--command <file>` | 参数为已存在文件时按 GDB 命令文件语义执行 |

### 2. JSON Lines 机器接口（`--json`）

所有输出单行 JSON，事件/结果/错误三型分明，AI 一次解析即可消费：

```
{"ok":true,"result":{"breakpoint":{"id":1}}}
{"type":"stopped","reason":"breakpoint","thread":27828,"rip":"0x..","registers":{...}}
{"type":"stopped","reason":"exception","exception":{"code":"0xc0000005","address":"0x0"}}
{"type":"running"} / {"type":"exited","code":42} / {"type":"detached","pid":1234}
```

### 3. 进程级一次性命令

```
aidbg.exe --json --command "break kernel32!Sleep" target.exe
aidbg.exe --json --command "x/4gx $rsp" target.exe
aidbg.exe --json --command "bt" target.exe
```

单进程、单命令、单行输出、退出码 0/1；多命令用 `--commands <file>` 或 stdin 管道。

### 4. Win32 场景能力

- **断点**：软件 / 硬件（DR0~DR3）/ 内存（guard-page）/ API（`dll!api`，自动补 `.dll`）；
- **符号**：PDB 经 dbghelp 旁载；`info locals`/`info args` 用
  `SymSetContext`+`SymEnumSymbols`+`RtlVirtualUnwind`；`bt` 用 `StackWalk64`
  （与 -O2/omit-frame-pointer 无关）；
- **异常**：`0xc0000094`（除零）、`0xc0000005`（访问违例）等精确停住并报告；
- **多线程**：`info threads` 标当前线程 `*`，`thread` 切换上下文后跨线程 `bt`；
- **附加**：`attach <pid>` / `detach`（分离后目标存活）；
- **内存**：`x/<n><fmt>`（b/w/g + x/d/u/i/s/c/f）、`search`（`?` 通配）、`strings`。

## 已知边界（源码级记录，见 README.md）

- `print <裸标识符>`、`condition` 局部变量、`step`/`next` 源码级单步未实现
  （`stepi`/`nexti` 指令级可用）；
- 32 位（WOW64）目标已支持：软件/硬件断点、单步、bt（用例 4.16–4.18）。

## 验证现状

`tests/` 套件：6 个测试目标（基础/内存/异常/多线程/符号/驻留进程）+ 19 项自动化用例
全绿，覆盖断点、条件/ignore、内存观察点、异常、单步、变量枚举、线程切换、
attach/detach、搜索、行号断点、GDB 兼容性。运行方式：

```
tests\build.cmd
tests\run_tests.cmd        :: 全绿返回 0，可接入 CI
```
