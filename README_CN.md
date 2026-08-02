# aidbg — 为 AI 而生的 Windows 命令行调试器

> **English**: [README.md](README.md) · **中文**: 本文件

> Linux 有 gdb，Windows 却没有一个像样的适合 AI 工具的命令行调试工具。
> **aidbg** 让 AI 在 Windows 上，像在 Linux 上用 gdb 一样，愉快地调试 Win32 程序。

aidbg 是一个基于 [TitanEngine](https://github.com/x64dbg/TitanEngine) 的 GDB 风格
Windows 原生调试器：单文件 C++17 实现，x64 Release 仅约 500KB，无需安装。
命令集与语义对齐 GDB，内置 **JSON Lines 机器接口**与**进程级一次性命令**，
专为 AI 编程助手、CI 自动化与脚本化调试设计。

## 目录

- [特性](#特性)
- [安装](#安装)
- [快速开始](#快速开始)
- [构建（源码）](#构建源码)
- [AI 使用示例](#ai-使用示例)
- [命令一览](#命令一览)
- [GDB 兼容命令清单](#gdb-兼容命令清单)
- [测试](#测试)
- [Roadmap](#roadmap)
- [文档](#文档)
- [已知边界](#已知边界)
- [致谢与许可](#致谢与许可)

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

## 安装

### winget

```powershell
winget install linsmod.aidbg
```

### Chocolatey

```powershell
choco install aidbg
```

两种方式安装后，`aidbg` 命令自动加入 PATH，可直接调用（`TitanEngine.dll` 会随包部署
在同一目录）。验证安装：

```powershell
aidbg --version
```

### 手动下载（GitHub Releases）

从 [Releases](../../releases) 下载 `aidbg-x64-v<版本>.zip`，解压后把 `aidbg.exe` 与
`TitanEngine.dll` 放在同一目录即可直接使用——无需安装、无需管理员权限。

## 快速开始

假设 `aidbg` 已在 PATH 上（未安装请先看 [安装](#安装)）。

像 gdb 一样，直接在命令行传命令，读回普通文本输出：

```powershell
aidbg.exe --batch -ex "start" -ex "break kernel32!Sleep" -ex "continue" -ex "bt" target.exe
```

输出就是普通的 GDB 文本：

```
Temporary breakpoint 1, main () at target.c:29
Stopped: breakpoint
Breakpoint 2 at 0x00007fff... (kernel32!Sleep)
Stopped: breakpoint
#0  0x00007fff... in kernel32!Sleep ()
#1  0x00000001400011b2 in target!main () at target.c:31
#2  0x0000000140001450 in target!mainCRTStartup ()
```

也可以 `--command <cmd>` 传单条命令：

```powershell
aidbg.exe --command "bt" target.exe
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

发布相关脚本：`release.py`（构建 + 测试）、`make_zip.py`（打 zip）、
`winget_publish.py`（提交 winget manifest PR）、`publish_chocolatey.py`
（打包并推送 Chocolatey）；一键流程见 `publish.py`。

## AI 使用示例

AI 直接读文本最顺手，所以典型循环是：跑一条 `-ex` 命令行、读文本输出、决定下一步。

```powershell
aidbg.exe --batch -ex "start" -ex "bt" target.exe
```

```
Temporary breakpoint 1, main () at target.c:29
Stopped: breakpoint
#0  0x0000000140001450 in target!main () at target.c:29
#1  0x0000000140009a10 in target!mainCRTStartup ()
```

之后设断点、继续、检视都用同样的方式，全程普通 GDB 文本：

```powershell
aidbg.exe --batch -ex "break kernel32!Sleep" -ex "continue" -ex "bt" target.exe
```

`--json` 仍然可用，适合想要结构化输出流的场景；但对绝大多数 AI / 脚本场景，
上面的普通 GDB 文本就够了。

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
set pagination on|off / show pagination
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
| `condition <id> [expr]` | 断点条件 | 同左（支持局部变量，求值失败保守停止） | ✅ | 4.2b/4.27 |
| `ignore <id> <count>` | 忽略前 N 次命中 | 同左 | ✅ | 4.2 |
| `delete` / `disable` / `enable` | 删除/禁用/启用断点 | 同左（无参 = 全部） | ✅ | 4.12 |
| `mbreak <addr> <size>` | — | aidbg 独有（内存范围断点） | ⓘ | 4.3 |

### 栈与线程（Stack / Threads）

| `bt` / `where` | 栈回溯 | 同左（StackWalk64；帧显示为 `module!func+off (file:line)`；优化构建与 WOW64 32 位目标均可用） | ✅ | 4.1/4.21 |
| `info locals` / `info args` | 局部变量 / 函数参数 | 同左（需完整 PDB，见已知边界） | ✅ | 4.6 |
| `thread <id>` | 切换线程 | 同左（内部编号或 OS TID） | ✅ | 4.7b |
| `info threads` | 列出线程 | 同左（`*` 标当前线程） | ✅ | 4.7 |
| `frame <N>` / `up` / `down` | 帧导航 | 同左（`info locals`/`print`/`registers` 按选中帧） | ✅ | 4.29 |

### 源码与符号（Source / Symbols）

| `list` / `l [func\|line]` | 显示当前行附近源码 | 同左（±5 行，`>` 标当前行） | ✅ | 4.9/4.21 |
| `info source` | 当前源文件信息 | 同左（另做 PDB 校验和校验） | ✅ | 4.14 |
| `info files` | 符号文件 / 入口点 / 加载文件 | 同左 | ✅ | 4.13 |

### 数据（Data）

| `print` / `p` | 打印表达式 | 支持 `$reg` / 字面量 / `*addr` / **局部变量** / 全局符号（函数打印地址）/ 算术表达式；**不支持数组下标/成员/`&`** | ✅ | 4.6/4.25/4.26 |
| `x/<n><fmt> <addr>` | 检视内存 | 同左（b/h/w/g + x/d/u/i/s/c/f，支持符号） | ✅ | 4.12/4.25 |
| `set $reg = <val>` | 写寄存器 | 同左（右侧支持表达式） | ✅ | 4.6/4.28 |
| `set *addr = <val>` | 写内存 | 同左（支持符号地址） | ✅ | 4.6/4.25 |
| `set <局部变量> = <val>` | 写局部变量 | 同左 | ✅ | 4.28 |
| `registers` / `regs` | `info registers` 快捷别名 | 同左 | ✅ | — |

### 反汇编与工具（Disassembly / Misc）

| `disas` / `disassemble [start,end]` | 反汇编 | 同左（GDB 区间语法，支持符号） | ✅ | 4.5/4.25 |
| `set pagination on\|off` / `show pagination` | 开关 GDB 式分页 | 同左（仅交互 REPL；batch / `--command` / JSON / 管道模式不分页） | ✅ | — |
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
4.25 符号解析、4.26–4.29 局部变量/表达式/帧导航），全部通过（35 项）：

```cmd
set _NT_SYMBOL_PATH=%CD%\..
tests\build.cmd
tests\run_tests.cmd        :: 全绿返回 0，可接入 CI
```

## Roadmap

- [x] **局部变量 + 表达式求值**（已实现，handover7）：`print`/`condition`/`set` 支持
  局部变量与算术表达式；`frame`/`up`/`down` 帧导航（用例 4.26–4.29）
- [ ] **AI 接口增强**：`--host/--port` 长驻 socket 协议（一次会话多命令，免每次
  启动进程）；JSON 结构化增强（dump/x 字节数组、断点字段补全）
- [ ] **引擎稳定与边界**：WOW64 下 `info locals`/`thread` 的 32 位展开验证
  （`bt` 已完成——x86 布局上下文喂 StackWalk64，用例 4.16/4.18）；
  `list` 支持非 ASCII 源文件路径（宽字符打开）；`set scheduler-locking on|off`
- [ ] **断点高级**：`commands <id>` 断点命令列表（自动 continue 等）；
  `until` / `advance <loc>` 运行到指定位置

## 文档

| 文档 | 说明 |
| :--- | :--- |
| `TestGuid.md` | 测试程序与测试逻辑设计指导 |
| `tests/README.md` | 测试套件说明 |
| `archive/README.md` | 历期实现细节与过程总览（handover 1–5 索引） |

## 已知边界

- `print`/`condition`/`set` 支持局部变量与表达式（handover7）；**数组下标 /
  成员访问 / `&` 取址暂不支持**（求值器按值计算，用 `print *ptr` 解引用替代）。
- `print` 支持 `$reg`/字面量/`*addr`/全局符号（数据打印值、函数打印地址）。
- `disas`/`x`/`set`/`print` 支持 PDB 符号（`parse_addr` 符号回退，handover6）；
  `break`/`list`/`hbreak`/`watch`/`mbreak`/`condition` 亦支持。
- 源码级 `step`/`next` 已实现（`archive/handover5.md`）；无 PDB 行号时回退指令级单步
  （`stepi`/`nexti`）并提示。
- 32 位（WOW64）目标已支持（软件/硬件断点、单步；用例 4.16–4.18）。`bt` 通过把寄存器
  打包成 x86 布局的 `WOW64_CONTEXT` 再交给 `StackWalk64` 展开（此前 AMD64 布局不匹配导致
  回溯在 #0 后即中断）。
- `bt` 只显示展开器从元数据（x64 `.pdata` / x86 FPO + EBP 链）得到的帧，不做裸栈扫描，
  因此没有猜测帧。展开元数据耗尽处（如 WOW64 消息泵内的重入分发）列表即到此为止。
- `info locals` 对 `/DEBUG:FASTLINK` 或 `/O2` 构建的变量枚举受 dbghelp 物理限制。
- **ASLR 关闭不可靠（TitanEngine 引擎限制）**：`set engine aslr off`（或 GDB 对齐别名
  `set engine disable-randomization on`）映射到 `UE_ENGINE_DISABLE_ASLR`，依赖 TitanEngine
  的 `HollowProcessWithoutASLR`（CREATE_SUSPENDED + 映像重映射 + 再 attach）。本机构建下该
  路径不可靠，常致 `InitDebugW failed to create the target process`。设置本身跨 `run` 持久化
  （GDB 语义），仅底层引擎实现缺陷。`set engine aslr on`（默认）正常。

## 致谢与许可

调试引擎基于 [x64dbg/TitanEngine](https://github.com/x64dbg/TitanEngine)（增强版
v2.0.3）构建，按 TitanEngine 开源许可使用。aidbg 自身代码为单文件 C++17，可自由
使用、修改与分发。
