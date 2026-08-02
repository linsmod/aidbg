# aidbg 交接文档 (handover.md)

本文档记录基于 `C:\Users\linswin\TitanEngine` 构建的「AI 可用、类 GDB 的调试器」的实现结果。
实现语言为 **C++17（MSVC）**，单文件 `aidbg.cpp`，链接 `TitanEngine.lib`。

[text](TitanEngine/build_x64/Release/TitanEngine.lib)
---

## 1. 目标（已实现）

- 命令行为遵循 GDB 习惯（`run / break / continue / stepi / nexti / finish / registers / x / disas / bt ...`）。
- **AI 可用**：`--json` 机器接口（JSON Lines）、`--command "..."` 单命令退出、`--commands file` 批量脚本、stdin 管道批处理。
- 底层全部调用 `TitanEngine.dll`，不直接依赖 Windows Debug API。

---

## 2. TitanEngine 构建（已完成）

- 工具链：Visual Studio 2022 Community (`C:\Program Files\Microsoft Visual Studio\2022\Community`) + CMake。
- 构建（x64, Release）：
  ```
  cmake -B build_x64 -A x64
  cmake --build build_x64 --config Release
  ```
- 产物：`C:\Users\linswin\TitanEngine\build_x64\Release\TitanEngine.dll` (646144 B) + `TitanEngine.lib`。
- 把 `TitanEngine.dll` 复制到 `aidbg.exe` 旁即可运行。
- 分支：`x64dbg`（增强版 TitanEngine v2.0.3，commit ec7a8b9）。

### 关键 API 事实（源码确认）

- `InitDebugW(wchar_t*, wchar_t*, wchar_t*)` → `PROCESS_INFORMATION*`（局部静态，`TitanGetProcessInformation()` 可取）。
- **`InitDebugW` 与 `DebugLoop()` 必须同一线程调用**（`WaitForDebugEvent` 与创建线程绑定）。
  TitanEngine 自身的 `DebugLoopInSecondThread`（`Global.Debugger.cpp:70`）即同线程先 `InitDebugExW` 再 `DebugLoop`。
- `DebugLoopEx` / `SetDebugLoopTimeOut` 是 `__debugbreak()` stub，**不要调用**。
- 回调签名不统一，x64 下统一声明：
  - 无参（`SetBPX/StepInto/StepOver/StepOut`、attach）：`void TITCALL f()`。
  - 一参（硬件/内存断点、`SetCustomHandler` 全部）：`void TITCALL f(void*)`。
- `SetCustomHandler(UE_CH_*, cb)` 在 DebugLoop 线程内调用回调。
- **`UE_CH_EVERYTHINGELSE`（13）对每个 EXCEPTION_DEBUG_EVENT 都会触发（先于具体分发）**，不能用于阻塞停顿，只宜做日志；
  具体异常类型（AV、除零、非法指令等）走各自 `UE_CH_*`。
- `GetContextData(idx)` / `SetContextData(idx, val)` 作用于 `DBGEvent.dwThreadId` 对应线程。
- `GetDebugData()` → `DEBUG_EVENT*`。
- `ThreaderPauseAllThreads(false)` / `ThreaderResumeAllThreads(false)`：停顿时冻结/恢复其它线程。
- `LibrarianEnumLibraryInfoW(cb)` 只含 LOAD_DLL 事件记录的 DLL，**不含主模块**；主模块需用 `GetDebuggedFileBaseAddress()` 自行补充。
- `SetBPXOptions(UE_BREAKPOINT_TYPE_INT3)` 需在 `main` 开头调用一次。
- `StepOut(void* cb, bool final)`：`finish` 的步出。
- `FindEx(hProc, start, size, pattern, patSize, wildcardByte*)`：搜索；`EngineGetProcAddressRemote` 需要完整 DLL 名（带 `.dll`）。
- 断点常量：`UE_CH_SYSTEMBREAKPOINT=23, BREAKPOINT=1, SINGLESTEP=2, ACCESSVIOLATION=3, EXITPROCESS=17,
  LOADDLL=18, UNLOADDLL=19, OUTPUTDEBUGSTRING=20, EVERYTHINGELSE=13, UNHANDLEDEXCEPTION=24, DEBUGEVENT=26`。
- 寄存器索引（x64）：`UE_RAX=17..UE_R15=34, UE_RIP=25, UE_RSP=24, UE_RBP=23, UE_RFLAGS=26, UE_CIP=35, UE_CSP=36`；32 位 EAX=1..EFLAGS=10。
- 硬件断点：`UE_HARDWARE_EXECUTE=4, WRITE=5, READWRITE=6`；尺寸 `UE_HARDWARE_SIZE_1/2/4/8=7/8/9/10`；`GetUnusedHardwareBreakPointRegister` 给 0..3 → `UE_DR0+idx`。
- 引擎开关：`UE_ENGINE_DISABLE_ASLR=12, NO_CONSOLE_WINDOW=4, PASS_ALL_EXCEPTIONS=3, SAFE_STEP=13`。

---

## 3. 架构实现（C++）

文件：`aidbg/aidbg.cpp`（单文件，C++17，`/EHsc /std:c++17 /O2 /utf-8`）。

### 3.1 线程模型

```
┌───────────── 主线程（REPL / AI 命令）─────────────────┐
│  cmd_run → 启动 worker 线程（同线程 InitDebugW+DebugLoop） │
│  命令（SetBPX/StepInto/...）→ g_waiting=false → notify   │
└──────────────────────────────────────────────────────┘
┌──────────── DebugLoop 线程 ──────────────────────────┐
│  InitDebugW → DebugLoop()                            │
│  停止型回调 → pause_until_continue(reason):           │
│    1) 置 g_stopped=true, g_waiting=true, 通知主线程    │
│    2) ThreaderPauseAllThreads(false)                 │
│    3) cv.wait(!g_waiting || g_quit)  ← 主线程继续才返回 │
│    4) ThreaderResumeAllThreads(false)                │
│  DebugLoop 返回 → g_running=false, g_stopped=true     │
└──────────────────────────────────────────────────────┘
```

- 同步原语：`g_mu`(mutex) + `g_cv`(condition_variable) + `g_stopped/g_waiting/g_exited/g_quit`。
- `g_init_done/g_init_ok` 用于 `cmd_run` 等待 worker 线程的 `InitDebugW` 完成（失败则报错并 join）。
- `stop_session()`：置 `g_quit=true` 先解阻塞回调，再 `StopDebug()` + `join()`（用于 `--command`/脚本/REPL 退出清理）。

### 3.2 回调分工

- 停止型（阻塞）：`cb_system_bp→"initial-break"`、`cb_program_bp→"breakpoint"`、`cb_single_step→"single-step"`、
  `cb_hwbp→"hardware"`、`cb_membp→"memory"`、`cb_bpx→"breakpoint"`、`cb_step→"step"`、`cb_attach→"attach"`、
  `cb_access_violation / cb_exception_stop→"exception"`（附异常码/地址）。
- 仅日志：`cb_log_event`（LOAD/UNLOAD DLL、CREATE/EXIT THREAD、DEBUGSTRING；**对 EXCEPTION 事件直接返回**，
  避免被 `UE_CH_EVERYTHINGELSE` 每异常触发刷屏）。同时注册为 `UE_CH_EVERYTHINGELSE`。
- 退出：`cb_exit` 不阻塞，仅置 `g_exited/g_stopped` 并通知。

### 3.3 断点管理

- `g_bps`(map<int,Bpx>) 自己维护（TitanEngine 无枚举 API）；`Bpx.kind`：0=code, 1=api, 2=hardware, 3=memory。
- API 断点：`SetAPIBreakPoint(dllname(.dll 自动补齐), api, UE_BREAKPOINT|INT3, UE_APISTART, cb)`；
  `Bpx.dll` 存**带 `.dll` 的完整名**（`DeleteAPIBreakPoint` 需要）。
- `delete/disable/enable` 分别映射 `DeleteBPX/DeleteAPIBreakPoint/DeleteHardwareBreakPoint/RemoveMemoryBPX`。

### 3.4 地址/表达式解析 `parse_addr`

- `0x...` / 十进制；`$reg`（含 `$rip/$rsp`）；`*addr` 解引用；`a+b`；**`module!off`**（模块名可省略扩展名，解析到 `模块基址+偏移`）。

---

## 4. 命令集（已实现）

| 命令 | 别名 | 说明 |
|---|---|---|
| `file <path>` | | 设置目标文件 |
| `run [args]` | `r` | 启动/重启调试，停在系统断点 |
| `attach <pid>` | | 附加到进程（`AttachDebugger(pid,true,NULL,cb_attach)`） |
| `detach` | | 分离并保留进程 |
| `continue` | `c` `cont` | 继续运行到下一事件 |
| `stepi [n]` | `si` | 单步进 |
| `nexti [n]` | `ni` | 单步越 |
| `finish` | | 步出当前函数 |
| `break <addr/dll!api>` | `b` `br` | 软件断点；`dll!api` → API 断点；无参列出 |
| `hbreak <addr> [r/w/x] [1/2/4/8]` | `hb` | 硬件断点 |
| `mbreak <addr> <size> [r/w/x]` | `mb` | 内存断点 |
| `info break` / `info b` | | 列出断点 |
| `delete/disable/enable <id>` | `del` | 删除/禁用/启用断点 |
| `registers` | `regs` | 显示全部通用寄存器 |
| `set <reg> = <val>` / `set *addr = val` / `set addr = val` | | 改寄存器/内存（`$` 前缀可选） |
| `x/<n><fmt> <addr>` 或 `x/<n><fmt><addr>` | | 检视内存（b/h/w/g + x/d/u/i/s/c/f） |
| `dump <addr> <bytes>` | | 原始 hex+ascii 转储 |
| `disas [addr] [count]` | `u` | 反汇编 |
| `bt [n]` | | 栈回溯（RBP 链） |
| `search <addr> <size> <hexpattern>` | | 搜索（`?` 通配） |
| `strings <addr> <size>` | | 扫描 ASCII 字符串 |
| `info modules / threads / proc / events / registers` | | 模块/线程/进程/事件日志/寄存器 |
| `set engine <var> on/off` | | aslr / console / passexc |
| `echo <text>` / `help` / `quit` | `q` | 输出 / 帮助 / 退出 |

---

## 5. AI 机器接口（已实现）

- `--json`：所有输出为单行 JSON：
  - 成功：`{"ok":true,"result":{...}}`
  - 失败：`{"ok":false,"error":"..."}`
  - 停止通知：`{"type":"stopped","reason":"...","thread":N,"rip":"0x...","registers":{...}}`
    （reason 为 exception 时含 `"exception":{"code":"0x..","address":"0x.."}`）
  - 运行中：`{"type":"running"}`；退出：`{"type":"exited","code":N}`；分离：`{"type":"detached","pid":N}`
- `--command "<cmd>" [target]`：单命令执行后自动清理退出（退出码 0/1）。
- `--commands <file> [target]`：批量脚本。
- stdin 非 TTY：自动批处理模式。
- 位置参数 `<target>`：等价 `file <target>`（`--command "run"` 配合使用）。

### 典型 AI 调用序列

```
# 1. 启动并停到 initial-break
aidbg.exe --json --command "run" target.exe
# → {"type":"stopped","reason":"initial-break",...}

# 2. 设断点
aidbg.exe --json --command "break kernel32!Sleep" target.exe
# → {"ok":true,"result":{"breakpoint":{"id":1,...}}}

# 3. 运行到断点
aidbg.exe --json --command "continue" target.exe
# → {"type":"stopped","reason":"breakpoint","rip":"0x..."}

# 4. 查寄存器/内存/反汇编
aidbg.exe --json --command "registers" target.exe
aidbg.exe --json --command "x/4gx $rsp" target.exe
aidbg.exe --json --command "disas test_target!0x10a0 8" target.exe
```

> 注意：`--command` 是进程级一次性命令；若要"一次会话内多命令"，用 `--commands` 脚本或 stdin 管道。

---

## 6. 文件结构（实际）

```
miniie6/aidbg/
├── aidbg.cpp          # 主程序（单文件调试器，C++17）
├── aidbg.exe          # 已编译
├── TitanEngine.dll    # 已复制（运行时依赖）
├── handleover.md      # 本文档
├── test_target.c/.exe # 冒烟测试目标（循环+int3+除零+魔数）
└── exit_test.c/.exe   # 干净退出测试目标
```

---

## 7. 测试结果（已通过）

- `run` → `initial-break`；`info modules` 含主模块 + 3 个系统 DLL。
- `break kernel32!Sleep`（自动补 `.dll`）→ `info break` 列出 → `continue` 停在 Sleep。
- `delete 1` 后 `continue` 不再停（删除生效）。
- `registers` / `x/4gx $rsp` / `dump $rsp 32` / `disas module!off` / `bt` / `strings` 均输出正确。
- `stepi` / `nexti` / `finish` 均推进 RIP 并返回 step 事件。
- `search test_target!0x0 0x200000 DEADBEEFCAFEBABE` 命中 `.rdata` 两处。
- `set $rax = 0x1234` 后 `registers` 校验写回；`set addr = val` / `set *addr = val` 写内存并 `x` 校验。
- 干净退出：`{"type":"exited","code":42}`。
- `attach <pid>`（notepad）→ `attach` 事件、`info proc` 正确。

---

## 8. 风险与注意（踩过的坑）

- **`InitDebugW`/`DebugLoop` 同线程**：否则系统断点永远不来（卡死）。已改为 worker 线程内先 init 再 loop。
- **`UE_CH_EVERYTHINGELSE` 对每个异常都触发**：不能注册阻塞停顿回调，否则 `initial-break` 也被吞成 `exception`。
- **退出回调不能阻塞**：`cb_exit` 只置事件。
- **停止型回调阻塞期间 DebugLoop 线程被冻结**：主线程才能安全调用 `SetContextData/StepInto` 等。
- **软件断点恢复时序**：非单次 BPX 命中时 TitanEngine 内部"恢复字节→TF 单步→重置"，回调时 RIP 已回到断点、内存为原指令。
- **API 断点 DLL 名需带 `.dll`**：`EngineGetProcAddressRemote` 按模块全名匹配。
- **主模块不在 Librarian 列表**：`info modules` 需自行用 `GetDebuggedFileBaseAddress()` 补主模块。
- **`getline`/`cin` 需要 `<iostream>`**；`$` 寄存器名在 `set` 里需剥前缀。
- 32 位目标（WOW64）未验证；寄存器表已含 EAX..EIP 分支，但需真实 x86 目标测试。

---

## 8.5 PDB 符号（dbghelp，非 DIASDK）

- **机制**：`SymInitializeW(hProcess, NULL, TRUE)`（**`fInvadeProcess=TRUE`**，自动按运行时 ASLR 基址注册全部已加载模块）+ `SYMOPT_UNDNAME|SYMOPT_LOAD_LINES`。
  - 关键坑：`fInvadeProcess=FALSE` + 手动 `SymLoadModuleExW` 会用**镜像偏好基址**注册，而 ASLR 下运行时基址不同 → `SymFromAddr` 返回 126（ERROR_MOD_NOT_FOUND）。必须用 `TRUE` 让 dbghelp 读进程加载列表。
- 主模块 PDB 旁载（同名 `.pdb` 与 exe 同目录）即可被自动解析；系统 DLL 符号需 Microsoft 符号服务器（未配置 `_NT_SYMBOL_PATH`，故 ntdll/kernel32 只会显示 `RtlGetReturnAddressHijackTarget+0x6ee` 等导出名）。
- **输出**：无 PDB 时 `resolve()` 回退 `module+0x123`；有 PDB 时输出 `boom+0x14 (symtest.c:2)`。用于 stop 横幅 `rip = 0x.. (...)`、`bt` 帧、JSON `rip_symbol` 字段。
- **符号断点**：`break boom` 或 `break symtest!boom` 经 `SymFromNameW` 解析为运行时地址再 `SetBPX`（需会话已启动）。`break module!func` 优先按 PDB 符号匹配，失败才走 TitanEngine API 断点。
- 生命周期：`cmd_run`/`cmd_attach` 停后 `sym_begin()`；`emit_stop` 里 `sym_sync()` 补新加载 DLL（`SymLoadModuleExW`，late-load 场景）；`stop_session`/`cmd_detach` 里 `sym_end()`。

## 8.6 第二阶段：断点现场 / 栈回溯 / 变量 / 源码 / 条件断点

完整设计与实现细节见 **handover2.md**。要点：

- **断点命中摘要**：`Breakpoint 1, add_and_mul (symtest2.c:4) (hit 6)`（ID + 函数 + 行号 + 命中次数）；JSON 含 `breakpoint_id`/`hits`。
- **bt 已改为 StackWalk64**（弃用 rbp 链），与 -O2/omit-frame-pointer 无关。
- **condition / ignore / watch / rwatch / awatch / thread / list / info locals / info args** 均已实现。
- **info locals/args**：`SymSetContext` + `SymEnumSymbols`（`si->Flags` 分类 param/local），帧基址用 `RtlVirtualUnwind`，函数入口参数从 RCX/RDX/R8/R9 读。
- **thread 切换**：kernel32 `OpenThread`+`GetThreadContext`（TitanEngine 无按线程上下文 API）；`info threads` 标当前线程 `*`。

---

## 9. 后续可选项

- [ ] 构建 x86 版 TitanEngine（`cmake -B build_x86 -A Win32`）并验证 WOW64 调试（bt/thread/locals 的 x86 展开未验证）。
- [ ] 未运行时的符号断点（当前 `break boom` 需在会话内，因 dbghelp 需进程句柄；GDB 可在 run 前解析）。
- [ ] 断点条件里的局部变量名（当前条件支持字面量/`$reg`/`*addr`/全局变量；局部名需命中时实时 SymSetContext）。
- [ ] `bt` 各帧的 `info locals`（当前只查当前显示线程的最顶层帧）。
- [ ] 内存断点按字节粒度（当前 TitanEngine guard-page 页级）。
- [ ] `--host/--port` 长驻 socket AI 协议（当前为进程级 `--command`）。
- [ ] 断点条件 / `ignore` 计数等 GDB 高级特性（已实现 condition/ignore，见 handover2.md）。
