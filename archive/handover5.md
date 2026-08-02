# handover5.md — 源码级单步（`step` / `next`）

> 状态：**已实现**。GDB `step` / `next` 现为源码行级单步；无 PDB 行号时回退指令级
> 单步。代码变更见 `aidbg.cpp`，测试：`tests/run_tests.py` 26 项全绿（含新增
> 4.19 / 4.20）。旧的 1–4 期文档已归档到 `archive/`。

## 0. 背景与目标

GDB 的 `step` / `next` 是**源码行**级语义，而 aidbg 目前把两者直接映射成指令级
`stepi` / `nexti`（`cmd_step("into"/"over")`，见 aidbg.cpp:1988），只是符号别名。

目标：
- `step`（`s`）：执行到**下一源码行**，进入函数调用（停在 callee 首行）。
- `next`（`n`）：执行到**下一源码行**，不进入调用。
- 无 PDB / 无行号时**回退**到现有指令单步（保持现状）并给出提示。

语义单位取"源码行"（不是"语句 statement"）。MSVC 行表把一条 C 语句的每行都记为
可停靠行，`step`/`next` 以"行号变化"为停靠判据，与 x64dbg 的源码单步一致。

## 1. 现状与可复用设施

| 能力 | 现有实现 | 位置 |
| :--- | :--- | :--- |
| 行号查询 | `SymGetLineFromAddrW64` 包装 | aidbg.cpp:1036、2772 |
| 行 → 地址 | `resolve_line()`（交叉校验防越界钳制） | aidbg.cpp:1103 |
| 当前源文件 | `current_source_file()` | aidbg.cpp:1083 |
| 顺序反汇编 | `disasm()` / `LengthDisassembleEx` | aidbg.cpp:1500 |
| 单步原语 | `StepInto` / `StepOver` / `StepOut` + `cb_step` | aidbg.cpp:88-90、544 |
| 停止/恢复握手 | `pause_until_continue` / `resume_waiting_callback` / `wait_stop_consume` | aidbg.cpp:382、399、682 |
| 一次性断点 | `SetBPX` + `Bpx.oneshot`（命中自动移除） | aidbg.cpp:231、484-526 |
| 函数符号 | `sym_func_name()`（`finish` 已用） | aidbg.cpp:1976 |

步进状态机复用 `cmd_step`（aidbg.cpp:1988）的 REPL 线程驱动模式：
`ctx_reset()` → 发起引擎动作 → `resume_waiting_callback()` → `wait_stop_consume()`。

## 2. 核心设计

### 2.1 行信息辅助

- `src_line(addr) -> (file, line)`：包装 `SymGetLineFromAddrW64`；返回 false 表示
  该地址无行信息（无 PDB / 落入非源码区）。
- `build_line_range(addr) -> [start, end)`：从 `addr` 起用 `LengthDisassembleEx`
  顺序前扫，凡 `src_line(a) == src_line(addr)` 的地址都划入区间，直到**行号变化 /
  反汇编失败 / 无行信息**为止。`end` = 第一个行号不同（或失效）的地址。
  - 回边（如 `while` 条件映射到同一行）天然落在区间内 → **循环以全速运行**，
    不必逐指令单步。
  - 必须设上界防止扫进调用者代码：
    - x64：用 `RtlFunctionTable`（`SymFunctionTableAccess64`）取函数范围；
    - WoW64 / 32 位：扫描到 `ret` / `jmp` / 行变化即止。

### 2.2 `next`（源级 over）——区间端点断点

1. 取 `(file, line)`；无行信息 → 回退 `cmd_step("over")` 并提示。
2. `end = build_line_range(rip).end`。
3. `SetBPX(end, UE_BREAKPOINT | UE_BREAKPOINT_TYPE_INT3, cb_step_bp)` 设**内部
   一次性断点**：新回调 `cb_step_bp`，**不写入 `g_bps`**，不污染 `info break` /
   命中计数 / 断点 banner。
4. `resume_waiting_callback()` + `wait_stop_consume()`；命中后：
   - 新行 != 起始行 → 完成一次 `next`；
   - 仍同行（编译器把同一语句拆成非连续块，区间过短）→ 以当前位置重建区间、
     重新武装，继续；
   - 异常 / 退出 → 清理临时断点并照常报停。

调用语义：区间端点断点在调用返回之后，callee 执行期间不会命中 → 天然"不进入
调用"，且循环、库调用均全速执行。

### 2.3 `step`（源级 into）——单步 + 区间混合

1. 无行信息 → 回退 `cmd_step("into")`。
2. 主循环（直到 `src_line(rip)` 变化且有效）：
   - **rip 无行信息**（已进入 CRT / 系统 DLL 等无 PDB 区域）：从 `rsp/esp`
     （`UE_CSP` / `UE_ESP`）读返回地址，设一次性断点并 `continue`，回到有行信息
     处再判；
   - **当前指令是 `call`**：识别用 `DisassembleEx` 文本以 "call" 开头，或更稳的
     opcode 判定（E8 近call / E9 近jmp / FF /2 间接 call）。`StepInto` 进入，
     下一次迭代行已变即停（停在 callee 首行）；
   - **其它**：与 `next` 同款区间端点断点继续（同一行的循环全速跑完）。

### 2.4 内部步进断点的生命周期与线程

- 新增 `g_step_bp_addr` / `g_step_tid`：
  - 发起步进前记录当前事件线程 `g_step_tid`；`cb_step_bp` 命中线程 !=
    `g_step_tid` 时**不暂停直接续跑**（防止另一线程误吞步进断点，与既有
    per-thread 单步思路一致）。
  - 步进完成 / 进程退出 / 异常路径统一 `DeleteBPX` 清理。
- `reset_state()`（aidbg.cpp:664）中清零 `g_step_bp_addr` / `g_step_tid`。
- 状态机仍走现有 REPL 线程驱动：回调在 DebugLoop 线程触发，
  `pause_until_continue` 负责同步，无需新增握手。

### 2.5 命令分发与输出

- aidbg.cpp:3366/3370：`step`/`s` → `cmd_source_step("into", n)`，
  `next`/`n` → `cmd_source_step("over", n)`；`stepi`/`nexti` 保持指令级不动。
- 复用 `emit_stop()`（aidbg.cpp:859）输出；可选在 `stop_json`（aidbg.cpp:817）
  补充 `"file"` / `"line"` 字段。
- `finish`（`cmd_step("out")`）已是函数级步出，不受影响。

## 3. 边界情况

- **一行多语句 / 语句跨多行**：以行为单位停靠（GDB 以语句为单位）。逐条 `step`
  可覆盖同一物理行的多条语句，属已知近似。
- **循环体与条件同行**：区间覆盖整行，`next` 全速跑完整行后才停（正确）。
- **`next` 在循环体内（体与条件不同行）**：行号变化即停在循环条件行，与 GDB
  行为一致。
- **无行信息返回地址 = 起始行**：步进断点命中后 `src_line` 仍等于起始行 →
  重新武装继续，最多若干次后停在真正的新行。
- **区间端点 == 当前 rip**（单指令行）：视为步进完成，避免立即重复命中。
- **异常打断**：照常报 `exception` 停，清理内部断点，不吞异常。
- **多线程**：命中线程不匹配时续跑；事件线程才是步进状态机的权威。
- **WoW64（32 位目标）**：`src_line`/`SetBPX` 经 TitanEngine 既有 WoW64 路径，
  `build_line_range` 用 32 位 `ret/jmp` 截止；返回地址按 4 字节读。

## 4. 测试计划（沿用 run_tests.py harness）

- 新增/扩展 `tests/src/test_symbols.c`：多行 main、被调函数、**同一行含循环**、
  **多语句同行**。
- 新用例：
  - `step` 进入 `add()` 首行（行号变化）；
  - `next` 跳过 `add()`，停到 `printf` 行；
  - `next` 越过 `while` 停在循环后首行（验证全速跑循环）；
  - `step`/`next` 带次数 `N`（循环 N 次源码行）；
  - 无行信息二进制（去掉 `/DEBUG`）回退指令单步并提示；
  - 多线程下 `step`（另一线程命中内部断点被续跑）。
- 回归：既有全部用例保持绿。

## 5. 实施顺序

1. `src_line` / `build_line_range` 辅助（含 x64 函数范围上界）。
2. `cmd_source_step("over")`：内部断点 + 命中判行 + 重新武装 + 清理。
3. `cmd_source_step("into")`：call 识别 + 无行信息返回地址续跑。
4. 命令分发切换 + `stop_json` 加 file/line。
5. 测试目标与用例，全量回归。

## 6. 实现结果（对照方案）

全部落地，但两处与 §2 原始方案**不同**，已在代码中按更稳妥的做法实现：

| 项 | §2 方案 | 实际实现 |
| :--- | :--- | :--- |
| `step` 的 call 识别 | 用反汇编文本/opcode 判断当前指令是 `call` 再 `StepInto` | **不识别**：`step` 一律逐指令 `StepInto`，调用自然被进入；同一行循环仅当被 `next` 覆盖时才用区间断点提速（`step` 逐指令穿过单行循环会慢，但语义正确） |
| 无行信息续跑 | 立即读返回地址并 continue | **先单步 32 条**：跟随 1 条 `JMP` 跳转 thunk（`call` 目标常是 `JMP 目标函数` 的 thunk，无行号），仍能进入真正有行号的被调函数；只有持续无行信息（如 `printf`）才跳到返回地址 |

- `build_line_range` 用 **opcode 判 `ret`（0xC3/0xC2）**，命中则让调用方改单步（`ret` 后
  的字节不是返回目标，不能设断点）；`jmp`/`jcc` 不特判、线性前扫（回边映射同一行，
  循环以全速跑完）。
- 内部断点经独立回调 `cb_step_bp`，按**发起线程 `g_step_tid`** 过滤（其它线程命中则
  直接续跑），不写入 `g_bps`，`info break` 不可见，命中即删；`reset_state()` 清零。
- `stop_json` 增加 `file` / `line` 字段（有行号时）。
- 命令行 `step`/`s` → `cmd_source_step("into", n)`，`next`/`n` → `cmd_source_step("over", n)`。

### 6.1 测试（tests/ 用例 4.19 / 4.20）

- 新增目标 `tests/src/test_source_step.c`：`callee()`（noinline）+ main 中调用与
  `for` 循环，行号清晰。
- 4.19：`step` 从 `v = callee(1,2);` 进入 `callee` 首行（`test_source_step.c:15`）；
  `step 2` 推进两行。
- 4.20：`next` 越过 `callee()` 停到循环头（`:30`）；`next` 进循环体；`next 2` 全速
  跑完循环停到 `printf` 行（`:33`）。
- 手工验证：用户断点在 `next` 路径中被调函数内时优先停下（`break callee` +
  `next` → `Stopped: breakpoint`）；多线程目标 `test_threads.exe` 中 `step` 只跟随
  事件线程、其它线程不吞内部断点；无 PDB 目标（`exit_test.exe`）回退指令单步并提示。

## 7. 已知限制

- `step` 在**整行都是循环体**（单条源行含百万次迭代）会逐指令单步，慢；可用
  `next` 或改以多条源行表达循环规避。
- 单条物理行的多条语句无法一次 `step` 跨过（GDB 以语句为单位，此处以行为单位）；
  逐条 `step` 可分别停靠。
- `step` 进入目标与用户断点同址时报告 `step`（trap 优先于 int3），GDB 会报断点。
- `next` 的区间端点是"首个行号不同地址"的近似；极端布局（dead code 后设断点）可能
  不命中，需用户断点或手动 `continue` 打断。
