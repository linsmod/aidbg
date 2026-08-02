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
- **程序自身 int3（DebugBreak）**：停一次即可用 `continue` 越过；aidbg 通过
  TitanEngine 的继续状态 API 明确消费该异常，无需修改调试引擎的默认异常语义。
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

依赖：Visual Studio 2022（MSVC，C++17）和 CMake。TitanEngine 与 x64dbg 一样作为
Git submodule 固定在官方 `x64dbg` 分支的 `ccac889`；仓库不再提交预编译 DLL。

```cmd
git clone --recurse-submodules https://github.com/linsmod/-vc-dev-debuging-tool-for-ai-agent aidbg
cd aidbg
cmake -S . -B build -A x64
cmake --build build --config Release
```

构建结果位于 `build\bin\`，其中包含可直接运行的 `aidbg.exe` 和 `TitanEngine.dll`。
已有 clone 可运行 `git submodule update --init --recursive` 初始化依赖。

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

## GDB 兼容命令清单

以下为已实现命令与 GDB 的行为对齐情况（按 GDB 手册章节归类）。图例：
**✅** = 语义与 GDB 实测一致；**⚠️** = 部分一致（差异见"已知边界"或附注）；
**ⓘ** = aidbg 独有扩展（GDB 无对应命令）。"用例"列指向 `tests/run_tests.py`
中的自动化核对（4.21 为 man page 核心命令端到端走查，4.22 核对 CLI 批处理语义）。

### 运行与单步（Running / Stopping）

| 命令 | GDB 语义 | aidbg 行为 | 兼容 | 用例 |
| :--- | :--- | :--- | :--- | :--- |
| `run [args]` | 启动程序 | 同左（batch 下继续到退出/崩溃） | ✅ | 4.1b |
| `start [func]` | 启动并停到入口函数 | 停到 main/WinMain（或指定函数） | ✅ | 4.1 |
| `continue` / `c` | 继续运行 | 同左 | ✅ | 4.1 |
| `step` / `s [n]` | 源码行单步，**进入**调用 | 同左（无行号回退 stepi 并提示） | ✅ | 4.19/4.21 |
| `next` / `n [n]` | 源码行单步，**越过**调用 | 同左（无行号回退 nexti 并提示） | ✅ | 4.20/4.21 |
| `stepi` / `si [n]` | 指令单步 | 同左 | ✅ | 4.5 |
| `nexti` / `ni [n]` | 指令单步（越过 call） | 同左 | ✅ | 4.5 |
| `finish` | 运行到当前帧返回 | 停在调用方返回后的下一条指令 | ✅ | 4.12/4.21 |
| `kill` | 终止运行 | 同左 | ✅ | — |
| `attach <pid>` | 附加进程 | 同左 | ✅ | 4.8 |
| `detach` | 分离 | 同左（目标保持运行） | ✅ | 4.8 |

### 断点（Breakpoints）

| `break <symbol>` | 按符号设断点 | 同左（PDB 解析，run 前可 pending） | ✅ | 4.9/4.1b |
| `break <行号>` / `<file.c:行号>` | 按源码行设断点 | 同左（越界行号报 `No line N`） | ✅ | 4.11 |
| `break <addr>` / `*addr` | 按地址设断点 | 同左 | ✅ | 4.1 |
| `hbreak <addr\|sym> [r/w/x]` | 硬件断点 | 同左 | ✅ | 4.17/4.18 |
| `watch` / `rwatch` / `awatch` | 数据观察点 | 同左（TitanEngine guard-page，页级粒度） | ⚠️ | 4.3 |
| `condition <id> [expr]` | 断点条件 | 同左（不支持局部变量名） | ⚠️ | 4.2b |
| `ignore <id> <count>` | 忽略前 N 次命中 | 同左 | ✅ | 4.2 |
| `delete` / `disable` / `enable` | 删除/禁用/启用断点 | 同左（无参 = 全部） | ✅ | 4.12 |
| `mbreak <addr> <size>` | — | aidbg 独有（内存范围断点） | ⓘ | 4.3 |

### 栈与线程（Stack / Threads）

| `bt` / `where` | 栈回溯 | 同左（StackWalk64，优化构建可用） | ✅ | 4.1/4.21 |
| `info locals` / `info args` | 局部变量 / 函数参数 | 同左（需完整 PDB，见已知边界） | ✅ | 4.6 |
| `thread <id>` | 切换线程 | 同左（内部编号或 OS TID） | ✅ | 4.7b |
| `info threads` | 列出线程 | 同左（`*` 标当前线程） | ✅ | 4.7 |

### 源码与符号（Source / Symbols）

| `list` / `l [func\|line]` | 显示当前行附近源码 | 同左（±5 行，`>` 标当前行） | ✅ | 4.9/4.21 |
| `info source` | 当前源文件信息 | 同左（另做 PDB 校验和校验） | ✅ | 4.14 |
| `info files` | 符号文件 / 入口点 / 加载文件 | 同左 | ✅ | 4.13 |

### 数据（Data）

| `print` / `p` | 打印表达式 | 支持 `$reg` / 字面量 / `*addr` / 全局符号（数据打印值、函数打印地址）；**不支持局部标识符与表达式** | ⚠️ | 4.6/4.25 |
| `x/<n><fmt> <addr>` | 检视内存 | 同左（b/h/w/g + x/d/u/i/s/c/f，支持符号） | ✅ | 4.12/4.25 |
| `dump <addr>` | 原始 hex+ascii 转储 | 同左 | ✅ | 4.23 |
| `set $reg = <val>` | 写寄存器 | 同左 | ✅ | 4.6 |
| `set *addr = <val>` | 写内存 | 同左（支持符号地址） | ✅ | 4.6/4.25 |
| `registers` / `regs` | `info registers` 快捷别名 | 同左 | ✅ | — |

### 反汇编与工具（Disassembly / Misc）

| `disas` / `disassemble [start,end]` | 反汇编 | 同左（GDB 区间语法，支持符号） | ✅ | 4.5/4.25 |
| `help` / `quit` / `echo` | 帮助 / 退出 / 输出 | 同左 | ✅ | — |
| `dump` / `search` / `strings` / `info modules\|events\|proc` | — | aidbg 独有扩展 | ⓘ | 4.10 |

### 命令行（Invocation，见 gdb_quick_reference.txt OPTIONS）

| `--batch` | 批处理，命令出错退出码非零 | 同左（用例 4.22 核对） | ✅ | 4.22 |
| `-ex "cmd"` / `-x file` | 执行命令 / 命令文件 | 同左 | ✅ | 4.22 |
| `-e file` (`--exec=file`) | 选择可执行文件 | 同左 | ✅ | 4.22 |
| `--args prog arg...` | 设置目标与参数 | 同左（裸 `run` 用该参数） | ✅ | — |
| `-q` / `--quiet` | 抑制启动横幅 | 同左 | ✅ | — |
| `--command` | 单命令执行 | 同左（参数为文件路径时按命令文件执行） | ✅ | 4.12 |
| `--json` | — | aidbg 独有 JSON Lines 机器接口 | ⓘ | — |

## 测试

`tests/` 套件：7 个测试目标（基础 / 内存 / 异常 / 多线程 / 符号 / 源码单步 /
驻留进程）+ 自动化用例，覆盖断点、条件 / ignore、内存观察点、异常、单步、
源码级 `step`/`next`、变量枚举、线程切换、attach/detach、搜索、行号断点、
bp 命中后上下文命令、GDB 兼容性（含 4.21 命令走查、4.22 批处理退出码、
4.25 符号解析），全部通过（31 项）：

```cmd
set _NT_SYMBOL_PATH=%CD%\..
tests\build.cmd
tests\run_tests.cmd        :: 全绿返回 0，可接入 CI
```

## 文档

| 文档 | 说明 |
| :--- | :--- |
| `TestGuid.md` | 测试程序与测试逻辑设计指导 |
| `ISSUES.md` | 已知问题清单与修复记录 |
| `tests/README.md` | 测试套件说明 |
| `archive/README.md` | 历期实现细节与过程总览（handover 1–5 索引） |

## 已知边界

- `print <局部标识符>`、`condition` 局部变量未实现（`print` 支持 `$reg`/字面量/
  `*addr`/**全局符号**：数据符号打印值、函数符号打印地址）。
- `disas`/`x`/`set`/`print` 支持 PDB 符号（`parse_addr` 符号回退，handover6）；
  `break`/`list`/`hbreak`/`watch`/`mbreak`/`condition` 亦支持。
- 源码级 `step`/`next` 已实现（`archive/handover5.md`）；无 PDB 行号时回退指令级单步
  （`stepi`/`nexti`）并提示。
- 32 位（WOW64）目标已支持（软件/硬件断点、单步；用例 4.16–4.18）。
- `info locals` 对 `/DEBUG:FASTLINK` 或 `/O2` 构建的变量枚举受 dbghelp 物理限制。

## 致谢与许可

调试引擎基于 [x64dbg/TitanEngine](https://github.com/x64dbg/TitanEngine)（增强版
v2.0.3）构建，按 TitanEngine 开源许可使用。aidbg 自身代码为单文件 C++17，可自由
使用、修改与分发。
