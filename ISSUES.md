# aidbg 问题清单（ISSUES）

本文档记录在构建 `tests/` 测试套件过程中发现的 aidbg 问题，包含与 GDB 不兼容、
测试用到但不受支持、输出格式问题及其他 bug / 文档不符。所有结论均已通过
`tests/run_tests.py` 或手动 `--batch -x` 脚本实测验证（标注"源码依据"的除外）。

参照文件：`TestGuid.md`、`handleover.md`、`handover2.md`、`aidbg.cpp`。

---

## A. 符号断点一致性

### A1. `break <symbol>` 在 `run` 之前失败（**已修复**）
- **现象**：`-ex "break main" -ex "run"` 报 `cannot parse breakpoint location: main`。
- **原因**：`sym_lookup`（`aidbg.cpp`）依赖目标进程句柄，PDB 只在 `run` 后才通过
  `sym_begin` 加载；GDB 在 `file` 时就读取符号文件。
- **已修复**：
  - `file <path>` 时用当前进程做 dbghelp 宿主预加载 PDB（`sym_load_file`），
    `break <symbol>` 在 run 前即可解析；
  - 无目标进程时 `break` 记为 **pending**（GDB 风格，`info break` 标注
    `pending (not yet applied; will arm on run)`）；
  - `run`/`start`/`attach` 启动后 `apply_pending_bps()` 重新解析符号并按实际基址
    落地（天然支持 ASLR）。

### A2. `break <lineno>`（如 `break 31`）被当作地址解析（**已修复**）
- **现象**：`break 31` → `error: SetBPX failed`（把 `31` 当作地址 `0x1F`）。
- **原因**：未实现 GDB 的"裸数字 = 当前文件源码行号"语义。
- **已修复**：支持 `break 31`（当前文件行号）与 `break file.c:NN`，用 dbghelp
  `SymGetLineFromNameW64` 解析行号→地址，并交叉校验（`SymGetLineFromAddrW64`）
  拒绝超范围行号（dbghelp 默认会静默钳制到末行）。行号断点同样支持 run 前
  pending 落地。

---

## B. 与 GDB 不兼容的命令语义

### B1. `run` 的停止点（`start` 与 `start <func>` 已对齐 GDB）
- **现象**：交互下 `run` 停在 `RtlGetReturnAddressHijackTarget+0x6ee`（ntdll 系统
  断点），而非 GDB 的 `main`；batch 下 `run` 直接越过系统断点跑到结束。
- **已修复**：`start` 与 GDB 一致——停在 `main`（临时断点），输出
  `Temporary breakpoint 1, main () at file.c:line`；`start <func>` 停在指定函数。
  `run` 保持 GDB batch 语义不变（继续到结束/崩溃）。

### B2. `step`/`next` 实际是指令单步
- **现象**：`step` 后 `rip` 仍停留在同一源码行（`main+0x0 → main+0x5`，行号不变）。
- **原因**：`aidbg.cpp` 中 `step`/`next` 直接映射 `StepInto`/`StepOver`，
  HELP 声称的 "source-line fallback" 从未实现。

### B3. `finish` 停在被调函数末尾而非返回地址（**已修复**）
- **现象**：`break add; continue; finish` 停在 `add+0x22 (test_symbols.c:15)`，
  仍在 callee 内，而非 GDB 的"调用方返回后的下一条指令"。
- **原因**：`cmd_step("out")` 直接 `StepOut`，TitanEngine 语义即停在被调函数末尾。
- **已修复**：`StepOut` 后用符号名判断是否仍在原函数内，是则 `StepInto` 越过 `ret`
  （最多 3 次）。实测 `finish` → `main+0x21`（调用方）。见 handover3.md。

### B4. `detach` 与 batch 退出语义不清
- **现象**：`detach` 后目标存活、aidbg 返回，但 `quit` 直接 `exit(0)`（见 E3），
  "已分离"状态无独立退出表现。

---

## C. 测试用到但不受支持

### C1. `print a`（裸标识符 / 局部变量）不支持
- **现象**：`print a` → `error: cannot parse expression: a`。
- **原因**：`cmd_print`（`aidbg.cpp:2511`）只认 `$reg`/字面量/`*addr`，不查符号表。
- **影响**：TestGuid 4.6 的 `print a` 无法使用，测试套件改用 `info args` 替代。

### C2. `disas`/`dump`/`x`/`search`/`strings` 均不解析符号（`hbreak` 已修）
- **现象**：`disas func1` → `bad address`；`hbreak write_data` 曾同样失败。
- **原因**：`cmd_hbreak`/`cmd_disas` 等用 `parse_addr`（只认地址），与
  `break`/`mbreak`/`list` 的 `sym_lookup` 行为不一致。
- **已修复（hbreak）**：`hbreak` 现回退 `sym_lookup`。但 `SetHardwareBreakPoint`
  在本机（TitanEngine）始终失败，属预先存在的环境问题，待排查。
- **待办**：`disas`/`dump`/`x`/`search`/`strings` 的符号解析。

### C3. `x/i`（指令反汇编格式）不工作（**已修复**）
- **现象**：`x/3i 0x140007660` 输出的是十六进制字节 `48 89 54`，不是指令。
- **原因**：格式字符集为 `"xduicsf"`，不含 `i`，`i` 被当尺寸处理。
- **已修复**：`i` 加入格式集 `"xduicsfi"` 并从尺寸集移除。实测 `x/3i` 反汇编。

### C4. `set *symbol = val` 不解析符号
- **现象**：`set *global_var = 123` → `error: bad address`（`aidbg.cpp:1940`）。
- **影响**：内存写只能用裸地址；TestGuid 的 `set $rax = 5` 可用（寄存器路径）。

### C5. 条件断点仅支持全局变量 / 寄存器
- **现象**：`condition` 表达式求值器（`eval_cond`，`aidbg.cpp:1050-1060`）对裸标识符
  只做全局 `sym_lookup` 并读内存，局部变量（TestGuid 4.2 的 `i == 2`）不支持。
- **注**：handover2.md 已声明此限制（源码依据，未实测）。

---

## D. 输出格式问题

### D1. 每次停止都打印完整寄存器表
- **现象**：每次 stop（含 `initial-break`、普通断点）都输出约 23–47 行寄存器
  （`stop_banner`，`aidbg.cpp:742`）；JSON 模式每条 stop 也带完整 `registers` 对象。
- **影响**：batch / AI 解析噪音极大，输出量是本该的十几倍。

### D2. JSON 输出 schema 不一致
- **现象**：命令结果用 `{"ok":true,"result":...}` 信封；停止事件由 `emit_stop`
  直接 printf 裸 `{"type":"stopped",...}`，不经过 `print_result`。
- **现象2**：`bt` 的 JSON 只含 `frame/rip`，缺符号名（文本输出含 `in add`）。

### D3. `-q/--quiet` 只抑制启动 banner
- **现象**：batch 下 `-q` 后 `start` 仍输出 23 行寄存器，`-q` 对 stop dump 无效。

---

## E. 其他 bug / 文档不符

### E1. 已停止时再次 `run`/`start` 死锁
- **现象**：`start` 后再 `run`，或断点命中后 `run`，aidbg 永久挂起。
- **原因**：`cmd_run`（`aidbg.cpp:1477`）`ForceClose()+join()`，但 DebugLoop 线程阻塞在
  `pause_until_continue` 的 condition_variable（`aidbg.cpp:359`）等待 `!g_waiting`，
  `run` 从不唤醒它。
- **影响**：GDB 允许任意时刻 `run` 重启；aidbg 不支持。

### E2. `set engine` 设置被 `run` 无条件重置 + 语义与文档相反
- **现象**：`set engine passexc on` 后 `run`，异常仍停止（设置未生效）；ASLR 开关
  无论 on/off 基址不变。
- **原因**：`cmd_run`（`aidbg.cpp:1481-1482`）每次硬编码
  `SetEngineVariable(PASS_ALL_EXCEPTIONS, false)`、`SetEngineVariable(DISABLE_ASLR, false)`。
- **现象2**：`cmd_set_engine`（`aidbg.cpp:2574`）把 `aslr` 映射到 `UE_ENGINE_DISABLE_ASLR`，
  即 `set engine aslr on` 才是"禁用 ASLR"，与 TestGuid 7.2 的 `aslr off` 描述相反。

### E3. `quit` 直接 `exit(0)` 覆盖累计退出码
- **现象**：batch 下命令列表含 `quit` 时：目标异常崩溃返回 0，无 `quit` 返回 1；
  命令硬错误（如无 target 的 `run`）同样 0 vs 1。
- **原因**：`quit`（`aidbg.cpp:2670`）调用 `exit(0)`，绕过 main 里基于 `rc` 的返回。
- **影响**：TestGuid 6 节"崩溃/命令失败返回非零"的设想在带 `quit` 的脚本里失效；
  测试套件因此只能断言输出文本，不能依赖退出码。

### E4. `--version` 不支持
- **现象**：`--version` → `unknown option: --version`，GDB 支持。

### E5. `attach` 无效 PID 直接崩溃
- **现象**：`attach 99999999` → aidbg 崩溃（exit 0xC0000409），无优雅报错。

### E6. `--args` 的陷阱
- **现象**：`--args` 消费命令行剩余部分，其后若跟 `-ex` 会被当作 inferior 参数
  （符合 GDB 语义，但易误用）；`-ex` 须放在 `--args` 之前。

---

## 修复优先级建议

| 优先级 | 问题 | 影响 |
| :--- | :--- | :--- |
| P0 | E1 死锁、E5 attach 崩溃、E2 engine 重置 | 稳定性/健壮性 |
| P1 | E3 quit 退出码、D2 JSON schema | 自动化/CI 依赖 |
| P1 | C1/C4、C2 剩余命令符号解析 | TestGuid 用例覆盖 |
| P2 | B2 step/next、D1、D3、E4 | 体验/文档对齐 |

> **已解决**：A1（run 前符号断点，pending）、A2（`break <行号>`/`file.c:NN`）、
> B1（`start`/`start <func>` 停在入口、GDB 横幅）、B3（`finish` 停在调用方）、
> C2（`hbreak` 符号解析）、C3（`x/i`）、`--command` 文件语义、`thread <内部编号>`、
> `disable`/`enable` 无参、`info files`（符号文件/入口点/加载文件）。
> 详见 `handover3.md`。

## F. 源码/PDB 校验（第四阶段，handover4.md）

- `SymGetSourceFileChecksumW` 的 `pCheckSumType` **无公开枚举**；本机实测映射
  1=MD5(16B)、2=SHA1(20B)、3=SHA256(32B)，MSVC 默认 `/ZH:SHA_256`(type=3)。
  未知类型降级为 `unknown-algorithm(N)` 报告。
- 该 API 的 `Base` 必须传模块基址（传 0 返回 err 126）。
- BCrypt `BCryptFinishHash` 的输出缓冲需等于摘要长度（传 64 返回 `0xc000000d`）。
- `info source` 始终执行校验（显式诊断）；`list` / `break <file.c:NN>` 仅在
  `set source-checksum on` 时校验并告警（默认 off，避免干扰）。
- `list` 的 JSON 输出已从数组改为 `{"lines":[...]}`（新增可选 `"checksum"` 字段），
  schema 变化请以 4.14 用例为准。

## G. 程序自身 int3 / DebugBreak()（**已修复**，TitanEngine fork）

**现象（修复前实测）**：`DebugBreak()` / `__debugbreak()`（程序生成的 `int 3`）在 aidbg
下不能正确工作：首轮停止被误报为 `breakpoint`，继续后停为 `exception 0x80000003`
（第二次机会），地址振荡、多次 continue；最终进程以 `0x80000003` 退出或 SEH 处理器
不执行。`After DebugBreak` 永不打印。

**根因（TitanEngine `DebugLoop.cpp`）**：游离 int3（不在断点表）被置
`DBGCode = DBG_EXCEPTION_NOT_HANDLED` 并回调 `chBreakPoint`（aidbg `cb_program_bp`）。
NOT_HANDLED 使异常穿透到被调试程序 → 无 SEH 则二次机会崩溃、有 SEH 则被拦截执行，
均非调试器对 DebugBreak 的预期行为。

**修复（改 TitanEngine fork，`TitanEngine.Debugger.DebugLoop.cpp` 游离 int3 分支）**：
先读 `ExceptionAddress` 处字节判别——**真实单字节 `int 3`（0xCC）**才 `DBG_CONTINUE`
（同 VS；单字节 int3 触发异常时 CPU 已把 RIP 指到其后的指令，无需写寄存器，寄存器
回卷 `Rip -= BreakPointSize` 仅用于调试器自设断点，见 DebugLoop.cpp:556）；**其余
情况（如 `RaiseException(0x80000003)`）保持 `DBG_EXCEPTION_NOT_HANDLED` 原语义**，
因此对其他异常处理功能无影响（实测 `RaiseException`+`__except` 行为与修复前一致）。

**修复后实测**：
- `DebugBreak()` / `__debugbreak()` / 带 SEH 处理器三种场景均**只停一次**（报
  `breakpoint`，地址 `DebugBreak+0x3`），`continue` 后程序越过 int3 正常跑完、退出码 0；
- 用例 4.15（`test_debugbreak.exe` 写 before/after 标记文件）验证：单停 + after 标记
  存在 + 无回归；全套 21 项通过。
- **注意**：`DBG_CONTINUE` 会消费异常，程序自身的 `__try/__except(STATUS_BREAKPOINT)`
  处理器在调试器下不会执行（与 VS 一致）；停止原因仍显示为 `breakpoint`（待美化，
  可改为 `exception 0x80000003` 更准确）。
- **依赖**：需要随发行版一起替换 `TitanEngine.dll`（本仓库已跟踪该二进制）。
