# handover8.md — WOW64 异常处理：首诊良性首契机异常 + WX86 int3 续跑

> 状态：**已实现**。修两处 aidbg.cpp 异常回调：①非断点类异常的首契机（first-chance）
> 不再停靠，交还被调试程序自身 SEH 处理，只有未被处理（second-chance）才停；②`continue`
> 对 32 位目标（WOW64）下目标自身 `int3`（STATUS_WX86_BREAKPOINT 0x4000001f）与原生
> STATUS_BREAKPOINT 同等处理——真正 0xCC 断点被消费、继续越过，不再原地死循环。
> 验证：`tests/run_tests.py --no-build` 35 全绿（原 4.30/4.31 的 quit 测试已随
> handover9 移除）；`mshtml.exe`（32 位 WOW64）`run` 直达首个真实断点。

## 0. 背景与复现

在排查 miniie6 浏览器（`mshtml.exe`，Win32 x86，运行于 x64 宿主 WOW64）"无法加载页面"
问题时，用 aidbg 调试暴露了两个 aidbg 自身的问题：

**问题 A — 良性 0x6ba 启动异常导致 `run` 停靠**

```
aidbg.exe --batch -ex "run" mshtml.exe
Target started. pid=... base=0x...00f90000
Stopped: exception  [thread ...]
  exception 0x6ba at 0x00000000762ea2b4
  rip = 0x00007ff87d4b7401  (Wow64AllocateTemp+0x1811)
```

0x6ba 发生在系统断点之后、任何用户代码之前，位于 64 位 KernelBase/WOW64 层；继续运行后
程序照常执行，说明是良性首契机异常（被调试程序/WOW64 内部吞掉）。`continue` 可越过它，
但 `run` 就停在这里是纯噪音。进一步追踪发现 0x6ba 之后还有第二个良性首契机异常
`0xc0020043`（rpcrt4.dll），说明"逐个硬编码异常码"治标不治本。

**问题 B — `continue` 无法越过 WX86 `int3`，原地死循环**

```
aidbg.exe --batch -ex "run" -ex "continue" -ex "continue" mshtml.exe
Stopped: exception  [thread ...]
  exception 0x4000001f at 0x000000007637ad12   (kernel32!DebugBreak 的 int3)
Continuing.
Stopped: exception  [thread ...]               # 同样的寄存器现场，无限重复
  exception 0x4000001f at 0x000000007637ad12
```

目标程序自身执行 `DebugBreak()`（本项目 hlframe.cpp:1556）触发 `int3`。在 WOW64 下，64 位
调试器看到的是 `STATUS_WX86_BREAKPOINT (0x4000001f)` 而非原生 `STATUS_BREAKPOINT
(0x80000003)`。旧代码只对 `STATUS_BREAKPOINT` 做 0xCC opcode 判定，WX86 断点永远按
"未处理异常"放行 → 调试对象没 handler → 二次契机 → aidbg 再次停靠 → 无限循环。

## 1. 根因分析（TitanEngine 派发链路）

TitanEngine `DebugLoop` 对 `EXCEPTION_DEBUG_EVENT` 的默认流程：

1. `DBGCode = DBG_EXCEPTION_NOT_HANDLED`（先默认交给被调试程序）；
2. 先触发 `chEverythingElse`（aidbg 的 `cb_log_event`，只记日志不停靠）；
3. 二次契机（`dwFirstChance==FALSE`）且 `enginePassAllExceptions==off` 时把
   `DBGCode` 置为 `DBG_CONTINUE`；
4. `switch(ExceptionCode)` 各具体类型分支：命中会调用对应回调；**不命中则保持
   NOT_HANDLED**；
5. 分支结束后 `if(DBGCode == NOT_HANDLED)` 触发 `chUnhandledException`（aidbg 的
   `cb_exception_stop`）。

由此推论：

- **0x6ba / 0xc0020043** 不在 switch 的任何 case 里 → `DBGCode` 保持 NOT_HANDLED →
  `cb_exception_stop` 被调 → 停靠。这是"首契机且被调试程序能处理"的良性异常，停靠是噪音。
- **目标自身 `int3`**（0x80000003 / 0x4000001f）不在 aidbg 断点缓冲区内 → TitanEngine 走
  "breakpoint not in list"→ NOT_HANDLED → `cb_exception_stop`。旧代码只认 0x80000003，
  WX86 走不到 `is_int3` 分支 → `continue` 不设 `DBG_CONTINUE` → 重新投递给调试对象 →
  无 handler → 二次契机再停 → 死循环。

## 2. 修复设计

### 2.1 非断点类异常：首契机放行，二次契机（未处理）才停

GDB/WinDbg 默认语义：断点/单步必停；其余异常首契机先交还给调试对象，只有调试对象也处理
不了（二次契机）调试器才停。这统一解决了 0x6ba、0xc0020043 以及未来任何良性首契机启动
异常，无需逐个硬编码异常码。

- `cb_exception_stop`（aidbg.cpp）：在断点类判定之后，若 `dwFirstChance` 且非断点类 →
  直接 `return`（不停靠，TitanEngine 按默认 NOT_HANDLED 投递给调试对象）。
- `cb_access_violation`（aidbg.cpp）：同样逻辑——首契机 AV 放行（调试对象 SEH 可能处理），
  仅二次契机 AV 停靠。

### 2.2 WX86 `int3` 与原生 `int3` 同等消费

- 将 0xCC opcode 判定从 `STATUS_BREAKPOINT` 扩展到
  `STATUS_BREAKPOINT || STATUS_WX86_BREAKPOINT`（`is_bp_cat`）。
- 真实短 `int3`（0xCC）→ 停靠并标注 `breakpoint`，`continue`/单步经
  `SetNextDbgContinueStatus(DBG_CONTINUE)` 消费，越过 int3 继续执行（DebugBreak 的 `ret`
  返回到调用方）。
- 非 0xCC 的断点类（如 `RaiseException(STATUS_BREAKPOINT)`）保持 `exception` 停靠且
  NOT_HANDLED，交还调试对象 SEH（与既有 G 设计一致）。

### 2.3 兼容性边界

- **内存断点**：guard-page/AV 由 TitanEngine 在 `BreakPointBuffer` 命中时内部消费并回调
  `cb_mem_bpx`，不会走到 `cb_access_violation` 的 NOT_HANDLED 分支 → 放行首契机不影响
  `mbreak`。
- **Case 4.15**：`RaiseException(STATUS_BREAKPOINT)` 属断点类（is_bp_cat=true），首契机
  仍停靠为 `exception 0x80000003` → 预期不变。
- **Case 4.4 / 4.4b**：divzero/AV 程序无 SEH，首契机放行后进入二次契机 → aidbg 照常停靠
  并显示异常码 → 预期不变。

## 3. 代码变更（aidbg.cpp）

```cpp
// cb_access_violation
if (de->u.Exception.dwFirstChance) return;   // 首契机 AV 交还被调试程序

// cb_exception_stop
bool is_bp_cat = (g_exception_code == STATUS_BREAKPOINT ||
                  g_exception_code == STATUS_WX86_BREAKPOINT);
if (is_bp_cat) { /* 原有 0xCC opcode 判定，现覆盖 WX86 */ }
if (de->u.Exception.dwFirstChance && !is_bp_cat)
    return;                                   // 首契机非断点 → 放行
```

其余（`g_quit_cmd` / `quit` 保留退出码等）为工作区既有的未提交改动，与本修复无关。

## 4. 验证

**复现命令（修复前）**
```
aidbg.exe --batch -ex "run" mshtml.exe                 # 停 0x6ba
aidbg.exe --batch -ex "run" -ex "continue" mshtml.exe  # WX86 int3 死循环
```

**修复后**
```
aidbg.exe --batch -ex "run" mshtml.exe
Stopped: breakpoint  [thread ...]
  rip = 0x000000007637ad13  (DebugBreak+0x3)     # 良性启动异常全部静默放行

aidbg.exe --batch -ex "start" -ex "break WebBrowser::Navigate" -ex "continue" -ex "bt" mshtml.exe
Temporary breakpoint 1, wWinMain () at mshtml.cpp:26
Breakpoint 2 at WebBrowser::Navigate (0x...1767fc0)
Breakpoint 2, WebBrowser::Navigate () at testdoc.cpp:1089   # 正常断点工作
```

`continue` 越过 int3 后寄存器/栈帧均变化（新的一次 `DebugBreak()` 调用），不再原地循环。

**测试套件**：`python tests/run_tests.py --no-build`（须在 aidbg 仓库根目录运行，测试脚本
不设 cwd）→ `35 passed, 0 failed`（原 4.30/4.31 的 quit 测试随 handover9 移除）。
重点相关用例：4.4/4.4b（异常捕获）、4.15（断点继续策略）、
4.16（WOW64 断点继续）全部 PASS。

## 5. 遗留 / 说明

- aidbg 的 `step`/`next` 仍是指令级单步（已知问题 B2），与本修复无关。
- `disas`/`dump`/`x`/`search`/`strings` 不解析符号（已知问题 C2），排查时用
  `SymGetLineFromAddr64`（dbghelp 脚本）按返回地址反查源码行定位调用方，已入本仓库未提交
  的临时脚本思路。
- WOW64 下调试器与目标的"32/64 位上下文混合"会使 `bt` 栈回溯在异常现场退化（仅显示
  `wow64.dll`），通过读取 `[esp]` 返回地址 + PDB 行号可精确反查。
