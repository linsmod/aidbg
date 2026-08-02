# aidbg 测试套件（tests/）

本目录根据 `TestGuid.md` 构建一套针对 `aidbg`（基于 TitanEngine 的 GDB 风格调试器）
的自动化测试：测试程序源码、编译脚本、每节测试用例的 aidbg 命令脚本，以及一个
Python 驱动的测试运行器。

## 目录结构

```
tests/
  src/                 测试程序源码（TestGuid.md 第 3 节）
    test_basic.c       基础功能：断点 / ignore / condition / locals / stepi / search
    test_memory.c      内存 / 观察点（全局数组读写）
    test_exception.c   异常：除零(divzero) / 访问违例(av)
    test_threads.c     多线程：两个工作线程 + 主线程
    test_symbols.c     符号与源码定位：add() 函数
    test_source_step.c 源码级 step/next：callee() + 循环
    test_vars.c        局部变量 / 表达式 / 帧导航：level2 -> level1 -> main
    test_attach.c      常驻目标：供 attach/detach 测试
  cases/               各测试用例的 aidbg 命令脚本（TestGuid.md 第 4 节）
  build.cmd            MSVC 编译脚本（方案 B 专用参数，见下）
  build_x86.bat        x86 (WoW64) 目标编译脚本（test_wow64.exe）
  run_tests.py         Python 测试运行器
  run_tests.cmd        run_tests.py 的 cmd 封装
```

## 快速开始

```cmd
set _NT_SYMBOL_PATH=%CD%\..

tests\build.cmd              :: 编译全部测试目标（输出到仓库根，紧邻 aidbg.exe）
tests\run_tests.cmd          :: 运行全部用例并报告 PASS/FAIL
tests\run_tests.cmd --case 4.4   :: 只运行某个用例
```

运行器会自动检测缺失的 exe 并调用 `build.cmd`；可加 `--no-build` 跳过。

## 编译策略（TestGuid.md 第 5 节）

所有测试目标统一使用**方案 B（内部专用构建）**以保证符号与变量命令稳定：

```
cl /nologo /Zi /Od /Oy- /GS- /MTd /Fe:<target>.exe <target>.c /link /DEBUG:FULL /DYNAMICBASE:NO
```

- `/DEBUG:FULL`：完整 PDB，`info locals` / `info args` / `list` 依赖它。
- `/Od`：禁用优化，变量不被优化进寄存器/内联。
- `/Oy-`：保留帧指针，辅助 x86 栈回溯（本套件为 x64）。
- `/DYNAMICBASE:NO`：固定模块基址 `0x140000000`，`search 0x140000000 ...`
  这类硬编码地址在回归测试中可用。

## 测试用例与 TestGuid.md 映射

| 用例 | 脚本 | 验证点 | 预期 |
| :--- | :--- | :--- | :--- |
| 4.1 | `case_4_1_basic_break.txt` | `start` 停在 main + bt | `Temporary breakpoint 1, main`，`bt` 含 `main` |
| 4.1b | `case_4_1b_pending_break.txt` | **run 前符号断点**（A1） | `break main` 标 `pending`，run 后命中 |
| 4.2 | `case_4_2_ignore_count.txt` | ignore 计数 | `ignore 2 2` 后第 3 次命中才停（`hit 3`） |
| 4.2b | `case_4_2b_condition.txt` | 条件断点（全局变量） | `g_loop_index == 2` 时停（`hit 3`） |
| 4.3 | `case_4_3_memory_watch.txt` | 内存观察点 | `watch data` 命中，`Stopped: memory` |
| 4.4 | `case_4_4_exception_divzero.txt` | 除零异常 | `exception 0xc0000094` |
| 4.4b | `case_4_4b_exception_av.txt` | 访问违例 | `exception 0xc0000005` |
| 4.5 | `case_4_5_stepi_disas.txt` | 指令单步 + 反汇编 | `stepi 3` 后 `disas` 输出指令 |
| 4.6 | `case_4_6_locals_args.txt` | 变量枚举 + 寄存器写 | `info args`/`info locals`，`set $rax=5` |
| 4.7 | `case_4_7_threads.txt` | 线程枚举 | 停在 `thread_func`，`info threads` 多线程 |
| 4.7b | （运行器动态驱动） | 线程切换 + bt | `thread <id>` 切换后 `bt` 有帧 |
| 4.8 | （运行器动态驱动） | 附加与分离 | attach 成功、命中断点、detach 后目标仍在 |
| 4.9 | `case_4_9_symbols_list.txt` | 符号断点 / list / bt | `break add`，`list` 显示源码，`bt` 含 `add` |
| 4.10 | `case_4_10_search.txt` | 内存搜索 | `search` 命中 "Hello"，`strings` 确认 |
| 4.11 | `case_4_11_line_break.txt` | 源码行断点 `file.c:NN` | 停在 `add () at test_symbols.c:13` |
| 4.11b | `case_4_11b_pending_line.txt` | 行号断点 run 前 pending | `break test_symbols.c:15` → run 后命中 |
| 4.11c | `case_4_11c_line_oob.txt` | 超范围行号 | 报错 `No line 999 ...` |
| 4.12 | `case_4_12_gdb_compat.txt` | GDB 兼容（finish/x-i/disable） | `main+0x21`、`MOV`、`disabled all breakpoints` |
| 4.13 | `case_4_13_info_files.txt` | `info files`（符号/入口/加载文件） | `Symbols from ...`、`Entry point: 0x...` |
| 4.14 | （运行器动态驱动） | 源码/PDB 校验（`info source`、list 警告） | 源未改动 `Checksum: ok`；篡改后 `mismatch` + `!! Checksum mismatch`（仅开关开启时） |
| 4.15 | （运行器动态驱动） | breakpoint continue 状态 | `DebugBreak()` 被 aidbg 消费；`RaiseException(STATUS_BREAKPOINT)` 仍进入 SEH |
| 4.16 | `case_4_16_wow64_bp.txt` | 32 位（WoW64）断点延续 | x86 目标 `break wow_target` 命中 3 次、`continue` 推进、`stepi` 正常 |
| 4.17 | `case_4_17_hwbreak_x64.txt` | 硬件断点（x64） | `hbreak write_data` 命中、`Stopped: hardware` |
| 4.18 | `case_4_18_hwbreak_wow64.txt` | 硬件断点（WoW64） | 同上，x86 目标 |
| 4.19 | `case_4_19_source_step.txt` | 源码级 `step`（进入被调函数） | `step` 进入 `callee` 首行（`callee (test_source_step.c:15)`），`step 2` 推进两行 |
| 4.20 | `case_4_20_source_next.txt` | 源码级 `next`（跳过调用/循环） | `next` 越过 `callee()` 停到下一行，`next` 进循环体、`next 2` 全速跑完循环 |
| 4.21 | `case_4_21_gdb_compat2.txt` | GDB 兼容命令走查（man page 核心） | `start`→`break 29`→`continue`→`step` 进 `callee`→`bt`→`next`→`finish` 回 main→`list` 标当前行 |
| 4.22 | （运行器动态驱动） | GDB 调用兼容（--batch 退出码、`-e`） | `--batch` 成功退出 0、命令出错退出非 0；`-e <file>` 选中目标并运行 |
| 4.23 | `case_4_23_context_cmds.txt` | bp 命中后上下文查看命令 | `registers`/`x` 多格式/`dump`/`print` 多格式/`set *addr`/`info modules\|target\|events`/`show args\|source-checksum`/`echo`/`nexti` |
| 4.24 | `case_4_24_bp_ops.txt` | bp 命中后断点管理 + 观察点变体 | `disable`/`enable`（`keep n/y`）、`delete`、`watch`/`rwatch`/`mbreak` |
| 4.25 | `case_4_25_sym_addr.txt` | 符号解析：`disas`/`x`/`set`/`print`（handover6 P0/P1） | `disas func2, func2+0x10` 反汇编、`print global_var`→42、`print func2`→地址、`x global_var`、`set *global_var=5` 后 `print`→5 |
| 4.26 | `case_4_26_print_expr.txt` | `print` 局部变量与表达式（handover7） | `print local_sum`→36、`local_prod`→180、`a+b`→216、`a*b`→6480 |
| 4.27 | `case_4_27_condition_local.txt` | `condition` 用局部变量 | `condition 2 local_x2 == 6`（level1(3)）停到 `level1` |
| 4.28 | `case_4_28_set_local.txt` | `set` 局部变量 + 表达式右侧 | `set local_sum = 50`、`set local_prod = local_sum + 5`（55） |
| 4.29 | `case_4_29_frame.txt` | `frame N` 帧导航 + 按帧 `info locals` | `frame 1`→level1 的 `local_x2=6`；`frame 2`→main 的 `v` |

> **断点编号**：`start` 的一次性入口断点占用 id 1（GDB 一致），因此 4.2/4.2b 的
> `break func1` 为 id 2、4.9 的 `break add` 为 id 2，脚本与断言均已按此编号。

### 用例 4.7b / 4.8 / 4.10b（动态用例）

这三个场景的值（线程 id、PID、搜索命中地址）在每次运行都不同，无法写死在脚本里，
因此由 `run_tests.py` 在运行时动态驱动：

- **4.7b**：解析 `info threads` 输出 → 取一个非当前线程 → `thread <id>` + `bt`。
- **4.8**：`Popen` 启动 `test_attach.exe` → 取其 PID → `attach <pid>` →
  `break tick_func` → `continue` → `bt` → `detach` → 确认目标仍存活。
- **4.10b**：解析 `search` 返回的地址 → `strings <addr> 0x1000` → 确认含
  `Hello, aidbg!`。
- **4.14**：对 `test_checksum.exe` 先 `info source` 断言 `ok`；二进制篡改源文件后断言
  `mismatch`；`set source-checksum on` 下 `list` 输出 `!! Checksum mismatch`；
  关闭时静默。`finally` 中按字节恢复源文件（避免文本模式改写行尾导致 PDB 校验失配）。
- **4.15**：`test_debugbreak.exe` 验证两条路径：真实短 `int3` 停止后，aidbg 在
  `continue` 时通过 `SetNextDbgContinueStatus(DBG_CONTINUE)` 消费异常并正常续跑；
  `RaiseException(STATUS_BREAKPOINT)` 保持 `DBG_EXCEPTION_NOT_HANDLED`，由被调试程序的
  SEH 处理。两条路径都断言没有同一事件的重复回调，并由 `finally` 清理标记文件。
- **4.16**：`test_wow64.exe`（x86，`build_x86.bat` 编译）验证 WoW64 交叉调试下的软件
  断点：aidbg 为 x64，32 位目标的 `int3` 以 `STATUS_WX86_BREAKPOINT`（0x4000001f）
  上报，TitanEngine 需消费该断点。断言 `break wow_target` 命中 3 次（无重复回调）、
  `continue` 正常推进、`stepi` 可单步。
- **4.22**：动态调用三次 `aidbg --batch`，断言 man page 的 OPTIONS 语义——成功脚本
  退出码 0、命令出错退出码非 0、`-e <file>`（`--exec`）选中目标并运行（退出码 42）。

## 与 TestGuid.md 的差异说明（实现现实）

测试套件基于 aidbg 的**实际行为**编写，与文档示例有几处适配：

1. **`start` 与 GDB 一致：停在 `main`（或 `start <func>`）**，输出
   `Temporary breakpoint 1, main () at file.c:line`。
2. **`break <symbol>` 可在 `run` 之前设置**（A1 已实现）：`file` 预加载 PDB，断点标
   pending，`run` 启动后落地（见 4.1b）。
3. **异常用例按输出文本断言**，不断言退出码。aidbg 在 batch 模式下 `quit`
   会先行解除 stop 等待，进程随后退出，`final_reason` 存在竞态，退出码不可靠。
4. **`print a`（裸标识符）不受支持**，`print` 只接受 `$reg` / 字面量 / `*addr`。
   因此 4.6 用 `info args` 代替 `print a` 验证参数值。
5. **`hbreak`/`search`/`strings` 只接受地址**（符号需要先经 `start` 解析），
   且 `/DYNAMICBASE:NO` 固定了基址，故搜索/反汇编用例使用显式地址。
6. 线程 id、PID 为非确定性值 → 动态用例由运行器驱动（见上）。
7. `start` 的一次性入口断点占用断点编号 1，用户断点从 2 开始（与 GDB 一致）。
8. **`break <行号>`（GDB 语义）**：`break 31` 取当前源码文件第 31 行（运行中取
   `rip` 所在文件，run 前取 `main` 所在文件）；`break file.c:NN` 指定文件。超范围
   行号报 `No line N ...`（dbghelp 默认会静默钳制到末行，已用交叉校验拒绝）。
9. **GDB 兼容修复（handover3.md）**：`finish` 停在调用方返回后（`main+0x21`）、
   `x/i` 反汇编、`hbreak` 支持符号名、`--command <file>` 按 GDB 语义执行命令文件、
   `thread <内部编号>` 切换（`info threads` 显示 `* 1 <tid>`）、`disable`/`enable`
   无参作用于全部断点、`info files` 输出符号文件/入口点/加载文件。
10. **源码级 `step`/`next`（archive/handover5.md）**：需要 PDB 行号；无行号时回退指令单步
    （`stepi`/`nexti`）并提示 `(no source line info; stepping by instruction)`。
    `next` 用"行区间端点一次性断点"跑同一行的循环（全速）；`step` 逐指令单步并
    跟随跳转 thunk（`call` 经 `JMP` thunk 仍能进入被调函数），无行信息区域单步
    32 条后跳回返回地址。

## 退出码约定

`run_tests.py` 全绿返回 0，任一断言失败返回 1，便于接入 CI：
`tests\run_tests.cmd || exit /b 1`
