# linux有gdb，windows却没有一个像样的适合ai工具的命令行调试工具。如何让ai在windows上愉快的开发调试win32程序呢？aidbg 完全可以是你新的选择！

> **项目地址：** https://github.com/linsmod/aidbg.git

## 为什么 Windows 的 AI 开发者总是"调试不顺"？

在 Linux 上，AI 编程助手调试 C/C++ 程序就像呼吸一样自然：`gdb -batch -ex "bt"`、
`-ex "x/8gx $rsp"`，一条命令一个结果，机器看得懂、脚本接得住，从定位到修复一气呵成。

可一换到 Windows，画风立刻崩坏：

- **Visual Studio** 的调试器再强大，也是给"人"用的。AI 不会点按钮，自动化脚本也点不了；
- **cdb / WinDbg** 命令行能力是够，但命令自成一套、输出又臭又长，AI 得专门重学一门外语；
- 想退回 **MinGW gdb**？它对 MSVC 编译的程序、PDB 符号、Win32 异常和系统 API 断点，
  支持得相当勉强。

结果就是——**AI 在 Windows 上写代码得心应手，一到"跑起来调试"就集体抓瞎。**
不是 printf 硬上，就是得喊人肉去 IDE 里手动点断点。AI 的半条命，被 Windows 硬生生砍没了。

## aidbg：把"AI 友好的 gdb"带回 Windows

**aidbg** 是一个为 AI 而生的 Windows 原生命令行调试器，单文件、仅 500KB、无需安装。
它的使命只有一句：**让 AI 在 Windows 上，像在 Linux 上用 gdb 一样，愉快地调试 Win32 程序。**

为此，它只做了三件"对 AI 至关重要"的事：

### 第一件：命令集 = GDB 全家桶，AI 零学习成本

AI 不需要学任何新命令。GDB 的常用操作，aidbg 全部支持，而且**不止名字像，行为也对齐**：

```
run / start / continue / stepi / nexti / finish / bt
break / hbreak / mbreak / watch / condition / ignore
registers / set / x / dump / disas / search / strings
info break / threads / modules / proc / locals / args
attach / detach / thread / file / list
```

别小看"行为对齐"这四个字：

- `finish` 停在**调用方返回之后**，而不是在被调函数里磨蹭；
- `start` 直接停在 `main`，横幅都是 GDB 味道的
  `Temporary breakpoint 1, main () at file.c:line`；
- `break main` 在 `run` 之前就能设置（pending 断点，启动后自动落地，天然兼容 ASLR）；
- `x/3i` 输出的是真·反汇编指令，不是十六进制字节；
- `thread 2` 按 GDB 内部编号切换，`info threads` 长一个样；
- 连 `--command <文件>` 都兼容 GDB 的命令文件语义。

**AI 从 GDB 手册里学到的每一个习惯，都能在 aidbg 上直接照搬，一条都不用改。**

### 第二件：JSON Lines 输出，AI 真正读得懂的话

这是 aidbg 最"懂 AI"的设计：`--json` 模式下，所有输出都是**单行 JSON**。
再也不用从二十几行寄存器转储里人肉翻找信息了。

```json
{"ok":true,"result":{"breakpoint":{"id":1}}}
{"type":"stopped","reason":"breakpoint","thread":27828,"rip":"0x140001234"}
{"type":"stopped","reason":"exception","exception":{"code":"0xc0000005","address":"0x0"}}
{"type":"exited","code":42}
```

事件、结果、错误三种消息清清楚楚，**AI 一个 JSON 解析器通吃所有场景**。
甚至 `rip` 还带符号名——`boom+0x14 (symtest.c:2)`，定位问题连猜都不用猜。

### 第三件：一次命令一结果，AI 的"一问一答"

AI 调试的典型节奏是"查一步、看一步、走一步"。aidbg 为此做成了**进程级一次性命令**：

```powershell
aidbg.exe --json --command "run" target.exe
aidbg.exe --json --command "break kernel32!Sleep" target.exe
aidbg.exe --json --command "continue" target.exe
aidbg.exe --json --command "registers" target.exe
aidbg.exe --json --command "bt" target.exe
```

没有长驻进程、没有状态管理、没有会话记忆负担——**每次调用都是独立的、干净的、可并发的**。
这正是自动化与 AI Agent 最理想的操作形态。

## 真本事：为真实 Win32 场景打磨

- **断点三件套 + API 断点**：软件断点、硬件断点（DR0~DR3）、内存断点，还能直接
  `break kernel32!Sleep` 断到系统调用上；
- **深度符号**：`info locals` / `info args` 枚举局部变量和参数，`bt` 栈回溯在
  Release 优化下照样稳；
- **异常一网打尽**：除零 `0xc0000094`、访问违例 `0xc0000005`，精确停住、明确报告；
- **多线程不慌**：线程枚举、上下文切换、跨线程回溯，一条 `thread 2` 搞定；
- **附加与分离**：`attach <pid>` 挂到已运行进程，`detach` 后目标继续跑；
- **内存军火库**：`dump` / `x/<n><fmt>` / `search`（支持 `?` 通配）/ `strings` 一应俱全；
- **工程化细节**：`set engine aslr off` 固定基址便于回归，退出码反映崩溃与否，可直接接 CI。

## 底气：19 项自动化测试全绿

aidbg 不是概念演示。配套 `tests/` 套件包含 6 个测试目标程序和 **19 项自动化用例**，
从断点、条件、内存观察点、异常、单步、变量、多线程、attach/detach 到 GDB 兼容性，
**全部通过**。改完代码跑一条 `tests\run_tests.cmd`，心里就有底。

## 结语

Linux 有 gdb，AI 如虎添翼；Windows 缺的就是这件趁手的兵器。

aidbg 把 **GDB 的命令习惯 + JSON 机器接口 + 进程级简单调用** 三者合一，
让 AI 在 Windows 上调试 Win32 程序不再是"将就"，而是"愉快"。

如果你是 AI 编程助手 / Agent 的开发者，
如果你需要在 Windows 上给自动化插上"原生调试"的翅膀，
如果你受够了 printf 和手动断点——

**aidbg，完全可以是你新的选择！**

> 仓库内 `handleover.md` / `handover2.md` / `handover3.md` 记录完整设计与踩坑，
> `TestGuid.md` 提供系统化测试指导。欢迎试用、欢迎来稿、欢迎共建。
