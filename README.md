# aidbg — 为 AI 而生的 Windows 命令行调试器

> Linux 有 gdb，Windows 却没有一个像样的适合 AI 工具的命令行调试工具。
> **aidbg** 让 AI 在 Windows 上，像在 Linux 上用 gdb 一样，愉快地调试 Win32 程序。

aidbg 是一个基于 [TitanEngine](https://github.com/x64dbg/TitanEngine) 的 GDB 风格
Windows 原生调试器：单文件 C++17 实现，x64 Release 仅约 500KB，无需安装。
命令集与语义对齐 GDB，内置 **JSON Lines 机器接口**与**进程级一次性命令**，
专为 AI 编程助手、CI 自动化与脚本化调试设计。

## 特性

- **GDB 命令集对齐**：`run / start / continue / stepi / nexti / finish / bt / break /
  hbreak / mbreak / watch / condition / ignore / registers / set / x / dump / disas /
  search / strings / info ... / attach / detach / thread`，关键行为与 GDB 实测一致
  （`finish` 停在调用方返回后、`start` 停在 `main`、run 前符号断点 pending 落地、
  `break <行号>`、`x/<n>i` 反汇编、`thread` 内部编号、`disable` 无参等）。
- **JSON Lines 机器接口（`--json`）**：所有输出单行 JSON，事件 / 结果 / 错误三型分明，
  一次解析即可消费，`rip` 带 PDB 符号名。
- **进程级一次性命令**：`--command` 单命令执行、`--commands` 批量脚本、stdin 管道，
  每次调用独立干净、可并发。
- **三类断点 + API 断点**：软件断点、硬件断点（DR0~DR3）、内存断点（guard-page）、
  `dll!api` 系统 API 断点（自动补 `.dll`）。
- **源码/PDB 校验**：`info source` 校验当前源文件与 PDB 记录的校验和是否一致，
  `set source-checksum on` 后 `list` / `break <file.c:NN>` 在失配时告警（默认 off）。
- **深度符号**：dbghelp 加载 PDB；`info locals` / `info args` 枚举局部变量与参数，
  `bt` 基于 `StackWalk64`（优化构建下依然稳定）。
- **异常拦截**：除零 `0xc0000094`、访问违例 `0xc0000005` 等精确停住并报告。
- **程序自身 int3（DebugBreak）**：对齐 VS，停一次即可 `continue` 越过
  `DebugBreak()` / `__debugbreak()` 继续执行（需配套本仓库跟踪的 TitanEngine.dll）。
- **多线程 / 附加**：线程枚举、上下文切换、跨线程回溯；`attach <pid>` / `detach`。

## 快速开始

从 [Releases](../../releases) 下载 `aidbg-x64.zip`，解压后把 `aidbg.exe` 与
`TitanEngine.dll` 放在同一目录，直接命令行使用：

```powershell
# 启动目标并停在 initial-break
aidbg.exe --json --command "run" target.exe

# 设断点（符号 / 行号 / dll!api 均可）
aidbg.exe --json --command "break kernel32!Sleep" target.exe
aidbg.exe --json --command "break target.c:31" target.exe

# 运行到断点 / 查寄存器 / 内存 / 反汇编 / 栈回溯
aidbg.exe --json --command "continue" target.exe
aidbg.exe --json --command "registers" target.exe
aidbg.exe --json --command "x/4gx $rsp" target.exe
aidbg.exe --json --command "disas target!0x10a0 8" target.exe
aidbg.exe --json --command "bt" target.exe
```

交互式使用（REPL）：直接运行 `aidbg.exe target.exe` 进入命令行，`help` 查看命令。

### 构建（源码）

依赖：Visual Studio 2022（MSVC，C++17）+ TitanEngine（见下）。

```cmd
cl /nologo /std:c++17 /EHsc /O2 /utf-8 aidbg.cpp /Fe:aidbg.exe TitanEngine.lib
copy /y <TitanEngine 构建产物>\TitanEngine.dll .
```

TitanEngine 构建（x64dbg 增强版 v2.0.3，commit ec7a8b9）：

```cmd
cmake -B build_x64 -A x64
cmake --build build_x64 --config Release
```

## AI 使用示例

```powershell
# 1. 启动并停到 initial-break
aidbg.exe --json --command "run" target.exe
# → {"type":"stopped","reason":"initial-break","thread":...,"rip":"0x.."}

# 2. 设断点
aidbg.exe --json --command "break kernel32!Sleep" target.exe
# → {"ok":true,"result":{"breakpoint":{"id":1,...}}}

# 3. 运行到断点
aidbg.exe --json --command "continue" target.exe
# → {"type":"stopped","reason":"breakpoint","rip":"0x.."}

# 4. 查寄存器/内存/反汇编
aidbg.exe --json --command "registers" target.exe
aidbg.exe --json --command "x/4gx $rsp" target.exe
```

## 命令一览

```
file / run(r) / start [func] / attach / detach / continue(c,cont)
stepi(si) / nexti(ni) / finish
break(b,br) / hbreak(hb) / mbreak(mb) / watch / rwatch / awatch
condition / ignore / delete / disable / enable
registers(regs) / set / x / dump / disas(u) / bt / search / strings / list
info break / threads / modules / proc / files / locals / args / events / source / registers
set engine <aslr|console|passexc> on/off
set source-checksum on|off / show source-checksum
echo / help / quit(q)
```

## 测试

`tests/` 套件：6 个测试目标（基础 / 内存 / 异常 / 多线程 / 符号 / 驻留进程）+ 19 项
自动化用例，覆盖断点、条件 / ignore、内存观察点、异常、单步、变量枚举、线程切换、
attach/detach、搜索、行号断点与 GDB 兼容性，全部通过：

```cmd
set _NT_SYMBOL_PATH=%CD%\..
tests\build.cmd
tests\run_tests.cmd        :: 全绿返回 0，可接入 CI
```

## 文档

| 文档 | 说明 |
| :--- | :--- |
| `handleover.md` | 架构实现、TitanEngine API 事实、命令集、AI 接口 |
| `handover2.md` | 断点现场 / 栈回溯 / 变量 / 源码 / 条件断点 |
| `handover3.md` | GDB 兼容性修复（finish / x-i / thread / info files 等） |
| `handover4.md` | 源码/PDB 校验（info source / source-checksum，算法映射实测） |
| `TestGuid.md` | 测试程序与测试逻辑设计指导 |
| `ISSUES.md` | 已知问题清单与修复记录 |
| `tests/README.md` | 测试套件说明 |

## 已知边界

- `print <裸标识符>`、`condition` 局部变量、源码级 `step`/`next` 未实现
  （`stepi`/`nexti` 指令级可用）。
- 32 位（WOW64）目标未验证。
- 硬件断点 `SetHardwareBreakPoint` 在 TitanEngine 下始终失败（引擎环境问题）。
- `info locals` 对 `/DEBUG:FASTLINK` 或 `/O2` 构建的变量枚举受 dbghelp 物理限制。

## 致谢与许可

调试引擎基于 [x64dbg/TitanEngine](https://github.com/x64dbg/TitanEngine)（增强版
v2.0.3）构建，按 TitanEngine 开源许可使用。aidbg 自身代码为单文件 C++17，可自由
使用、修改与分发。
