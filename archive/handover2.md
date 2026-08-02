# handover2.md — 第二阶段改进：断点现场 / 栈回溯 / 变量 / 源码 / 条件断点

> 状态：方案已确认（全部实现，按建议顺序）。本文档先记录方案与职责切分，随实现推进同步更新。

## 0. 总体思路：三方库 + 官方库职责切分

- **TitanEngine（三方，保留现状）**：进程生命周期、断点机制（软件 INT3 / 硬件 DR / 内存页保护）、单步、内存读写、模块枚举、反汇编。
- **dbghelp（官方 SDK，已在用）**：符号解析（`SymFromAddrW`/`SymGetLineFromAddrW64`）、栈回溯（`StackWalk64`+`SymFunctionTableAccess64`+`SymGetModuleBase64`）、局部变量/参数（`SymSetContext`+`SymEnumSymbols`+`SymGetTypeInfo`）、源码行号。
- **kernel32（官方）**：线程上下文（`OpenThread`+`GetThreadContext`，返回原生 `CONTEXT` 直接喂 `StackWalk64`）。**不用** `GetFullContextDataEx`（自定义 `TITAN_ENGINE_CONTEXT_t` 需手动转 `CONTEXT`）。

| 能力 | 负责方 | API |
|---|---|---|
| 断点命中摘要 (ID/次数/func/行号) | TitanEngine 回调 + dbghelp | `cb_bpx` 里 `GetContextData(UE_CIP)` 反查地址 → `Bpx`；行号走 `sym_resolve` |
| bt / where | dbghelp | `StackWalk64`（`.pdata` 展开，与 -O2 无关） |
| info locals / info args | dbghelp | `SymSetContext`(RIP) + `SymEnumSymbols` + `TI_GET_DATAKIND` 分 param/local + `TI_GET_BASETYPE/LENGTH` 定型 |
| list（源码） | dbghelp | `SymGetLineFromAddrW64` → 读源文件 ±5 行 |
| condition / ignore | TitanEngine 回调 + 自研表达式求值 | `cb_bpx` 求值，假则直接 return 不 pause（引擎自动继续） |
| thread <id> | kernel32 + TitanEngine | `GetThreadContext` 缓存显示线程；`ThreaderEnumThreadInfo` 列线程 |
| watch/rwatch/awatch | TitanEngine（复用 mbreak） | `watch`=写、`rwatch`=读、`awatch`=`UE_MEMORY`(读写执行) |

## 1. 断点命中现场摘要（必须，低成本）

- `Bpx` 扩展：`hits`（命中计数）、`ignore`（忽略前 N 次）、`condition`（条件表达式）、`symbol`（原 spec 用于显示）。
- `cb_bpx`（无参回调）：`GetContextData(UE_CIP)` 得到断点地址 → 在 `g_bps` 里按 `addr` 反查 id；命中次数 +1；`ignore>0` 则递减并**跳过**；`condition` 非空则求值，假则**跳过**（不 pause）。
- 命中信息存 `g_hit_bp`（DebugLoop 线程写，REPL 线程读，配合 `wait_stop_consume` 时序天然同步）。
- `emit_stop`：`breakpoint` 原因时首行输出 GDB 风格 `Breakpoint 1, boom (symtest.c:2)`（hits 显示在附注）；JSON 增加 `"breakpoint_id"`/`"hits"`。
- `g_bps` 加独立 `g_bp_mu` 互斥：REPL 设/删断点 vs DebugLoop 回调命中并发访问安全。
- API 断点（addr=0）无法反查 id：不影响停止，id 显示为 `-`。

## 2. 可靠 bt：StackWalk64（致命缺陷修复，中成本）

- 弃用手撕 rbp/ebp 链（-O2 / -fomit-frame-pointer 下失效）。
- 事件线程：用 `GetContextData` 各寄存器拼 `CONTEXT`；显示线程（thread 切换后）：用缓存的 `GetThreadContext` 结果。
- `StackWalk64(IMAGE_FILE_MACHINE_AMD64|I386, hProcess, hThread, &sf, &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL)`。
  - x64 下 `hThread` 可传 NULL；为兼容 x86 拿 `ThreaderGetOpenHandleForThread(tid)`。
  - 前提：模块基址已由 `SymInitialize(fInvadeProcess=TRUE)` 注册（已有），`SymFunctionTableAccess64` 才能找到 `.pdata`。
- 输出保持 `#N  addr (resolve)` 形状；`resolve()` 已能出 `func (file:line)`。

## 3. condition / ignore（中成本）

- 命令：`condition <id> [expr]`（空 expr 清条件）、`ignore <id> <count>`。
- 求值器 `eval_cond`：字面量(0x/10)、`$reg`、`*addr`、`module!off`、`+`/`-`/`*`/`/`、比较 `== != < <= > >=`、逻辑 `&& || !`、括号；裸标识符走 `sym_lookup` 当全局变量读内存。
- **局部变量名暂不支持**（地址随 RSP/RBP 变化，需每命中一次 `SymSetContext`，代价高）→ v2 可选慢路径。
- 求值失败 → 保守停止（不自动继续，避免漏掉）。
- 自动继续 = 回调直接 return（TitanEngine 内部恢复字节→TF 单步→继续，既有时序保证）。

## 4. thread 切换（中成本）

- `thread`（无参）：显示当前线程；`thread <id>`：切换并停在该线程视角；`info threads` 当前线程标 `*`。
- 缓存：`g_ctx_tid`（0=事件线程）+ `g_ctx`（CONTEXT）。切换时 `OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION|THREAD_SUSPEND_RESUME, FALSE, tid)` + `GetThreadContext`（目标已挂起，可用）。
- 显示命令（`info reg`、`bt`、`info locals/args`、`list`、`disas` 默认）改读显示线程快照；控制流（`SetContextData`/step/continue）仍用事件线程（TitanEngine）。
- `continue`/`run` 时重置 `g_ctx_tid=0`。

## 5. list（源码上下文，低-中成本）

- `list`（无参=当前行）、`list N`（当前文件第 N 行）、`list func`（函数首行，`sym_lookup` 定位）。
- `SymGetLineFromAddrW64` 得 `file:line`；打开源文件读行（处理 CRLF/制表符），打印 `line±5`，当前行用 `>` 标出。
- 源文件为相对路径时尝试拼接（目标同目录、绝对路径原样）。

## 6. info locals / info args（高成本，需实测校准）

- `SymSetContext(proc, &sf{InstructionOffset=RIP}, NULL, 0)` + `SymEnumSymbols(proc, 0, NULL, cb, ...)`。
- 回调：`TI_GET_DATAKIND` 分类（PARAM→args，LOCAL/STATIC_LOCAL→locals），`TI_GET_LENGTH` 定大小，`TI_GET_BASETYPE` 定型名。
- 取值：栈地址 → `mem_read`；寄存器参数 → `GetContextData`。**x64 CV_REGISTER 地址编码需用 `/Od /Zi` 目标实测校准**（见 symtest2）。
- 输出：`name = value (type)`；无法读取标 `<unreadable>`。

## 7. watch/rwatch/awatch（低成本，纯复用）

- `watch *addr` = `mbreak addr 8 w`；`rwatch` = 读；`awatch` = `UE_MEMORY`（TitanEngine 无 read+write 组合 flag，用 access 近似）。
- 断点 kind 仍为 memory，走既有 `cb_membp`。

## 8. 测试目标 symtest2.c（新增）

```c
#include <stdio.h>
volatile int g_counter = 0;
__declspec(noinline) int add_and_mul(int a, int b) {
    int sum = a + b; int prod = a * b;
    long long big = (long long)a * 1000000000LL; double pi = 3.14159;
    return sum + prod + (int)(big / 1000000000LL);
}
__declspec(noinline) int level2(int x) { int local2 = x * 2; char ch = 'Z'; return add_and_mul(local2, x); }
__declspec(noinline) int level1(int x) { int local1 = x + 1; return level2(local1); }
int main(void) {
    printf("symtest2 start\n"); fflush(stdout);
    for (int i = 0; i < 100; i++) { g_counter = i; level1(i); }
    printf("symtest2 done %d\n", g_counter); return 0;
}
```

- 构建：`/Od /Zi /DEBUG`（保帧指针+行号，locals 在栈上）→ `symtest2.exe/.pdb`。
- 覆盖：断点摘要/计数、`ignore 5`、`condition g_counter==50`、多层 `bt`、`info locals/args`、`list`、`thread`。
- 旧回归：`test.gdb`(17条)、crasher(无PDB)、symtest(带PDB)、`--json`、`exit_test`(码42)。

## 9. 实施顺序与进度

- [x] 1. 断点摘要 + 命中计数（Bpx 扩展 / cb_bpx / emit_stop / g_bp_mu）——输出 `Breakpoint 1, add_and_mul (symtest2.c:4) (hit 6)`；JSON 含 `breakpoint_id`/`hits`。
- [x] 2. bt → StackWalk64（context 快照 + StackWalk）——已验证 symtest2 完整栈 `level2→level1→main→CRT`；symtest3 切换线程后 `NtWaitForSingleObject→WaitForSingleObjectEx`。
- [x] 3. condition / ignore（Bpx 字段 + eval_cond + cb_bpx 跳过逻辑）——`condition 1 g_counter == 5` 在第 6 次命中停下；`ignore 1 5` 跳前 5 次。
- [x] 4. thread 切换（GetThreadContext 缓存 + 显示命令改读快照）——`info threads` 标 `*`，`thread <id>` 后 info reg/bt/locals 均读切换线程；Python 驱动实测通过。
- [x] 5. list 源码（SymGetLineFromAddr + 文件读取）——`list` 显示 ±5 行，当前行 `>` 标出。
- [x] 6. info locals / args（SymSetContext 枚举 + 类型 + 取值校准）——函数入口处参数从 RCX/RDX/R8/R9 读取（a=16,b=8 正确）；进入函数后局部变量正确（sum=24,prod=128,pi=3.14159）。校准要点见 §10。
- [x] 7. watch/rwatch/awatch 别名——`watch g_tick` 在 worker 线程写全局时停下。
- [x] 8. symtest2 构建 + 全回归 + 更新 handleover.md——test.gdb(exit 0)/crasher(1)/symtest(1)/exit_test(0) 全过；JSON 流正常。

## 10. 实现要点与坑（第二阶段补充）

- **断点命中识别**：TitanEngine 的 `cb_bpx` 无参，用 `GetContextData(UE_CIP)` 在 `g_bps` 反查（代码/硬件断点按 addr；内存断点按 `[addr, addr+size)` 区间）。API 断点（addr=0）无法映射 id。
- **条件断点自动继续** = 回调直接 return 不 `pause_until_continue`（TitanEngine 内部恢复字节→TF 单步→继续的既有时序保证）。求值失败→保守停止。
- **`eval_cond`**：自研递归下降求值器，支持字面量/`$reg`/`*addr`/全局变量（`sym_lookup`）/四则/位运算/比较/逻辑/括号。**局部变量名暂不可用于条件**（地址随帧变化，需命中时实时 SymSetContext，代价高）。
- **`bt` 用 StackWalk64**（`SymFunctionTableAccess64`+`SymGetModuleBase64`），x64 下 `hThread=NULL`；事件线程 CONTEXT 由 `GetContextData` 拼接（`ctx_from_titan`），显示线程用 `GetThreadContext` 缓存。**`SegCs` 必须设为 0x33**（否则 RtlVirtualUnwind/栈展开按内核模式崩）。
- **thread 切换**：TitanEngine 无按线程取上下文的导出（`GetFullContextDataEx` 是自定义结构），直接用 kernel32 `OpenThread(THREAD_GET_CONTEXT|THREAD_QUERY_INFORMATION|THREAD_SUSPEND_RESUME)` + `GetThreadContext`（目标已挂起，可用）。`continue/step/run` 时 `ctx_reset()`。
- **info locals/args**：
  - `SymSetContext`（需 `IMAGEHLP_STACK_FRAME`，非 `STACKFRAME64`；本 SDK 3 参）或 `SymSetScopeFromAddr` 兜底。
  - 分类用 **`si->Flags`（SYMFLAG_PARAMETER/LOCAL）**，`TI_GET_DATAKIND` 在该构建里全部失败（返回 FALSE 过滤掉所有变量）。
  - 地址：dbghelp 返回的是**相对帧基址的偏移**（MSVC x64 是 RSP 相对），需要自行 `frameBase + offset`。frameBase 由 `RtlVirtualUnwind`（SEH 包裹，取 post-prolog RSP/establisher frame）计算，失败回退当前 RSP。
  - **函数入口处**：prolog 未执行，home slot 未写，参数在寄存器里 → 用 `SymFromAddrW disp==0` 判断"在函数首指令"，前 4 参从 RCX/RDX/R8/R9 按 home-slot 顺序读。局部变量此时未初始化（显示垃圾是真实的）。
  - 类型：`SymGetTypeInfo` 的 `TI_GET_SYMTAG` 查 si->TypeIndex 得到类型 tag（BASETYPE=16），再 `TI_GET_BASETYPE`/`TI_GET_LENGTH` 定类型名与大小；递归 `TI_GET_TYPE` 穿过指针/typedef。
- **符号解析增强**：`sym_lookup` 先 `SymFromNameW`，失败再 `SymEnumSymbolsW(主模块基址, NULL, cb)` 全量枚举按精确名匹配——**全局数据变量（如 g_counter）不在 SymFromName 索引里**，必须枚举。
- **坑**：`std::max` 与 windows.h 的 `max` 宏冲突（用 `(std::max)`）；x64 下 CONTEXT 无 E* 字段（32 位寄存器名走 `reg_get` 回退）；回调第二参是 `ULONG` 不是 `ULONG_PTR`。

## 11. 遗留/可选

- [ ] 局部变量名用于 `condition`（需每命中实时 SymSetContext 解析帧内偏移）。
- [ ] 内存断点粒度是页级（TitanEngine guard-page），`watch` 尺寸参数仅提示。
- [ ] `bt` 里 `info locals` 按帧切换（当前只查当前显示线程的最顶层帧）。
- [ ] x86/WOW64 目标：bt/thread/locals 的展开未验证（CONTEXT 布局不同）。
- [ ] 源文件路径非 ASCII 时 `list` 打开失败（std::ifstream 窄路径）。
- [ ] 去掉临时文件：`C:\Users\linswin\AppData\Local\Temp\opencode\thrtest.py/.ps1`。
