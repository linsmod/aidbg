# archive/README.md — aidbg 历期实现细节与过程归档

本目录集中归档 aidbg 各阶段的 **handover（交接/设计）文档**，记录每一阶段的
背景、设计、实现结果、实测结论与已知限制。这些阶段均已**完成并合入** `aidbg.cpp`
与 `tests/`，归档仅作历史与设计依据查阅，不再随功能演进更新。

- 顶层 `README.md` 的「GDB 兼容命令清单」与「测试」两节是当前能力的**权威清单**。
- 测试用例（`tests/run_tests.py` 4.1–4.22）对每一阶段的核心行为做了自动化核对。

## 阶段总览（实现过程）

| 阶段 | 文档 | 主题 | 核心结果 |
| :--- | :--- | :--- | :--- |
| 1 | `handover.md` | 架构与引擎接入 | TitanEngine + dbghelp 职责切分、REPL/DebugLoop 双线程模型、命令集与 `--json` AI 接口 |
| 2 | `handover2.md` | 断点现场/栈/变量/源码 | 断点命中摘要、`bt`(StackWalk64)、`condition`/`ignore`、`thread` 切换、`list`、`info locals/args`、`watch` |
| 3 | `handover3.md` | GDB 兼容修复 | `finish` 停在调用方返回后、`x/i`、`hbreak` 符号、`--command <file>`、`thread` 内部编号、`disable` 无参、`info files` |
| 4 | `handover4.md` | 源码/PDB 校验 | `SymGetSourceFileChecksumW` 算法映射实测（MD5/SHA1/SHA256）、`info source`、`set source-checksum`、缓存 |
| 5 | `handover5.md` | 源码级单步 | `step`/`next` 源码行语义、区间端点内部断点、`JMP` thunk 跟随、无行号回退指令级 |

## 各阶段实现要点

### 阶段 1 — 架构与引擎接入（`handover.md`）

- **三方职责切分**：TitanEngine（进程/断点/单步/内存/反汇编）＋ dbghelp（符号/栈/变量）
  ＋ kernel32（线程上下文）。
- **线程模型**：REPL 线程驱动命令；worker 线程内 `InitDebugW` + `DebugLoop`（**必须
  同线程**），停止回调 `pause_until_continue` 经 mutex+CV 与 REPL 握手。
- **API 事实**（踩坑）：`UE_CH_EVERYTHINGELSE` 对每个异常都触发、退出回调不能阻塞、
  主模块不在 Librarian 列表、`fInvadeProcess=TRUE` 才能按 ASLR 基址注册符号等。
- **AI 接口**：`--json` JSON Lines、`--command`/`--commands` 进程级一次性命令。

### 阶段 2 — 断点现场 / 栈 / 变量 / 源码（`handover2.md`）

- 断点命中摘要 `Breakpoint 1, func (file.c:line) (hit N)`；`g_bps` 自维护 + `g_bp_mu`。
- `bt` 改 `StackWalk64`（`.pdata` 展开，与 `-O2`/omit-frame-pointer 无关）。
- `condition`/`ignore`：自研递归下降求值器 `eval_cond`（字面量/`$reg`/`*addr`/
  全局符号），**局部变量名不支持**。
- `thread <id>` 切换：kernel32 `OpenThread`+`GetThreadContext` 缓存显示线程。
- `info locals/args`：`SymSetContext` + `SymEnumSymbols`（`si->Flags` 分类），帧基址
  用 `RtlVirtualUnwind`，函数入口前 4 参从 RCX/RDX/R8/R9 读取。

### 阶段 3 — GDB 兼容修复（`handover3.md`）

- A 类（同名反义）全部对齐：`finish`（StepOut 后再 StepInto 越过 ret）、`x/i`、
  `hbreak <符号>`、`--command <file>`（文件存在按命令文件执行）、`thread <内部编号>`。
- B 类：`disable`/`enable` 无参作用于全部断点、`info files` 独立实现
  （符号文件/入口点/加载文件）。
- 暂不修并记录原因：源码级 `step`/`next`（后续阶段 5 实现）、`print <裸标识符>`、
  `condition` 局部变量、`watch` 硬件 DR。

### 阶段 4 — 源码/PDB 校验（`handover4.md`）

- `SymGetSourceFileChecksumW` 的 `pCheckSumType` 无公开枚举，实测映射：
  1=MD5(16B)、2=SHA1(20B)、3=SHA256(32B)；MSVC 默认 `/ZH:SHA_256`。
- 本地哈希用 BCrypt（`BCryptFinishHash` 缓冲必须等于摘要长度）；结果按
  mtime+size 缓存。
- `info source` 始终校验；`set source-checksum on` 后 `list`/`break <file.c:NN>`
  失配告警。

### 阶段 5 — 源码级单步（`handover5.md`）

- `step`（进入调用）/ `next`（越过调用）按源码行语义；无 PDB 行号回退指令级。
- `next`：`line_range_end` 顺序反汇编构建行区间，`cb_step_bp` 一次性断点打到
  区间端点 → 循环全速跑完；`ret`（opcode 0xC3/C2）不设断点改单步。
- `step`：逐指令 `StepInto`（调用自然进入）；无行信息先单步 32 条以跟随 `JMP`
  thunk 进入真实 callee，仍无行信息才跳到返回地址（跳过 printf 等）。
- 内部断点按发起线程 `g_step_tid` 过滤（其它线程命中直接续跑），不入 `g_bps`，
  `info break` 不可见。
- 与方案的偏差记录于 §6：`step` 不识别 call 而直接单步；无行信息先单步再跳返回地址。

## 查阅指引

- 需要某一阶段的完整设计/实现/坑 → 打开对应 `handoverN.md`。
- 需要当前全部命令与其 GDB 兼容性 → 顶层 `README.md`「GDB 兼容命令清单」。
- 需要当前测试覆盖与回归 → `tests/README.md` 与 `tests/run_tests.py`。
