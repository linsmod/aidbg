# handover7.md — 局部变量访问与表达式求值

> 状态：**已实现**。`eval_expr` 公开并支持帧内局部变量；`print`/`set`/`condition`
> 均可用局部变量与表达式；新增 `frame`/`up`/`down` 帧导航。代码变更见 `aidbg.cpp`，
> 测试：`tests/run_tests.py` 35 项全绿（含新增 4.26–4.29）。

## 0. 背景与目标

当前兼容表剩余的最大 ⚠️ 项集中在"局部变量 + 表达式"：

| 命令 | 现状 | GDB |
| :--- | :--- | :--- |
| `print var`（局部） | `cannot parse expression` | 打印帧内局部变量值 |
| `print a+b` / `print *ptr` / `print arr[i]` | 不支持表达式 | 支持 |
| `condition <id> g_local == N` | 不支持局部变量名 | 支持 |
| `set localvar = N` | 不支持 | 支持 |
| `frame N` / `up` / `down` | 无 | 帧导航 + 各帧 locals |

复用点（均已实现，见手over2/handover6）：
- 局部变量定位：`cmd_info_vars`（SymSetContext + SymEnumSymbols + `frame_base_rsp` +
  `var_read`），aidbg.cpp:1807-1891
- 帧基址：`frame_base_rsp`（RtlVirtualUnwind，SEH 包裹），aidbg.cpp:1787
- 求值器：`eval_cond` 递归下降，parse_* 层级**已返回数值**（`unsigned long long& v`），
  aidbg.cpp:1325-1452
- 符号：`sym_lookup`（全局）+ `parse_addr` 符号回退（handover6）

## 1. 核心设计

### 1.1 局部变量定位器 `scope_resolve(name)`

封装 cmd_info_vars 的作用域枚举，返回变量地址/大小：

```
输入: 变量名 + 当前显示线程 CONTEXT（ctx_display）
流程: frame_base_rsp(ctx) -> SymSetContext(InstructionOffset=rip) ->
      SymEnumSymbolsW -> 按名字匹配（isParam/isLocal）
输出: ScopeVar{addr(帧基址偏移), size, baseType}
```

- 函数入口处参数走寄存器（复用 ARG_REGS + atEntry 逻辑，aidbg.cpp:1866）。
- 供 `print` / `condition` / `set` 三处复用。

### 1.2 表达式求值 `eval_expr(expr, ULONG_PTR& out)`（核心，改动最小）

`eval_cond` 的 parse_* 已返回数值，只差把它**公开**并扩展标识符分支：

- 把 `eval_cond` 顶层拆成 `eval_expr(expr, v)`（返回数值）+ `eval_cond` 包装
  （`out = v != 0`）。
- `parse_primary` 的标识符分支（aidbg.cpp:1436-1443）改为**局部优先**：
  1. 当前帧 `scope_resolve(name)` 命中 → 读帧内值；
  2. 否则 `sym_lookup(name)`（全局）→ 函数符号返回地址、数据符号读值；
  3. 否则失败。
- 扩展（可选 P1）：下标 `arr[i]`、指针 `->`/`.`、`&` 取址。
- `cmd_print` 改用 `eval_expr`（保留 `/fmt` 与 value/decimal 输出），
  `print a+b`、`print *ptr`、`print arr[3]`、`print var` 一次全通。

### 1.3 `condition` 局部变量（慢路径）

- 断点命中回调（DebugLoop 线程）里 `eval_expr` 解析裸标识符时，`sym_lookup` 失败则
  `scope_resolve` 局部（用命中线程 CONTEXT，`ctx_from_titan` 已有）。
- 代价：每次命中枚举作用域符号，与 GDB 相当，可接受。
- 求值失败仍保守停止（既有策略）。

### 1.4 `set <局部变量> = <值>`

- 左值：裸标识符先 `scope_resolve`（局部）→ 写其地址；否则走既有
  `parse_addr`（全局/寄存器/内存，handover6 已支持符号）。
- 右值：`eval_expr`。
- 按变量 `size` 写回（`mem_write`）。

### 1.5 `frame <N>` / `up` / `down`（帧导航）

- `cmd_bt` 的 StackWalk64 目前只保留 PC 列表（aidbg.cpp:2932-2944），改为同时
  缓存每帧 `{CONTEXT 快照, pc, frameBase}` 到 `g_frames`（struct FrameInfo）。
- `frame <N>`：`g_frame_idx = N`，重走到第 N 帧取该帧 CONTEXT；
  `up`/`down`：±1；`frame`（无参）：显示当前帧 `#N  pc in sym`。
- 显示类命令（`info locals`/`print`/`bt`）默认用当前帧 CONTEXT（仿现有
  `g_ctx_tid` 线程切换模式）；`continue`/`step`/`run` 时 `ctx_reset()` 重置
  `g_frame_idx`。
- `info locals`/`info args` 接入帧索引：`frame N` 后列出第 N 帧的局部变量。

## 2. 边界情况

- 帧基址：RtlVirtualUnwind 失败回退该帧 RSP（沿用）；无 `.pdata` 时帧导航受限。
- 函数入口处：参数在寄存器（沿用 ARG_REGS + atEntry）。
- 无符号 / stripped：`print` 回退全局符号或地址（不报错升级）。
- 多线程：局部变量绑定线程帧；`thread` 切换后按显示线程解析。
- `/DEBUG:FASTLINK` / `/O2`：局部变量枚举受 dbghelp 物理限制（既有边界）。
- `print func`（函数符号）打印地址（handover6 行为，经 eval_expr 标识符分支保持）。

## 3. 测试计划

- 新目标 `tests/src/test_vars.c`：多局部变量函数 + 指针/数组/结构体 + 两层调用
  （`level1 -> level2`），供 print/condition/set/frame 用。
- 新用例：
  - 4.26 `print` 局部变量与表达式：`print local_int`、`print a+b`、`print *ptr`、
    `print arr[2]`
  - 4.27 `condition` 局部变量：`condition <id> local_x == N`
  - 4.28 `set` 局部变量 + 表达式右侧
  - 4.29 `frame`/`up`/`down` + 各帧 `info locals`
- 回归：既有全部用例（31 项）保持绿。

## 4. 实施顺序

1. `eval_expr` 公开 + 标识符分支局部优先（改动最小，先打通 print 表达式）。
2. `scope_resolve` 局部定位器（复用 cmd_info_vars 逻辑，抽成公共函数）。
3. `cmd_print` 切到 eval_expr。
4. `condition` 局部变量慢路径。
5. `set <局部变量> = expr`。
6. `frame`/`up`/`down`（g_frames 缓存 + 显示命令接入帧索引）。
7. `test_vars.c` 与用例 4.26-4.29，全量回归。

## 5. 实现结果（对照方案）

全部落地，三处与方案不同：

| 项 | 方案 | 实际实现 |
| :--- | :--- | :--- |
| `eval_cond` 改造 | 公开 + 局部优先 | `eval_expr(expr, ull&)` 返回数值，`eval_cond` 变为 `v != 0` 包装；标识符分支先 `local_lookup`（当前帧），再 `sym_lookup`（全局），`addr_is_code` 判函数→地址 / 数据→读值 |
| `local_lookup` | scope_resolve 定位器 | 抽出 `scope_vars_for`（frameBase + SymSetContext + SymEnumSymbols + atEntry）供 `cmd_info_vars` 与 `local_lookup` 复用；参数在函数入口走 ARG_REGS 寄存器 |
| 关键根因修复 | 未预料 | **`sym_lookup` 必须过滤 `SYMFLAG_LOCAL/PARAMETER`**：一次 `SymSetContext`（print/info locals）后作用域残留，`sym_lookup` 枚举会返回局部变量的**帧偏移**（非绝对地址），导致后续 `set`/`x` 用偏移当地址写失败。过滤后局部变量只能经 `local_lookup` 显式解析 |
| `set` 右侧 | 未预料 | `cmd_set` 原来只取 `=` 后第一个 token；改为拼接完整 RHS 表达式，`set local_prod = local_sum + 5` 生效 |

帧导航：
- `bt_walk` 抽自 `cmd_bt`，缓存每帧 `{CONTEXT, pc, frameBase}` 到 `g_frames`；
  `ctx_display` 在 `g_frame_idx > 0` 时返回选中帧的 CONTEXT（registers/print/
  info locals 自动按帧）；`ctx_reset()` 清空帧缓存。
- 帧索引 0 = 顶层（实时上下文）；StackWalk64 首次调用重复报 frame 0，需跳过。

### 5.1 测试（tests/ 用例 4.26–4.29）

- 新目标 `tests/src/test_vars.c`：`level2 -> level1 -> main` 两层调用 + 数组 + 全局。
- 4.26：`print local_sum`（36）/`local_prod`（180）/`local_sum + local_prod`（216）/
  `local_sum * local_prod`（6480）。
- 4.27：`condition 2 local_x2 == 6`（level1(3) 满足）停到 `level1`。
- 4.28：`set local_sum = 50`、`set local_prod = local_sum + 5`（55）。
- 4.29：`frame 1` 显示 level1 的 `local_x2=6`/`local_arr`；`frame 2` 显示 main 的 `v`。

## 6. 已知限制（补充）

- **数组下标 / 成员访问 / `&` 取址暂不支持**：求值器按值（rvalue）计算，不支持
  lvalue 语义（需 `arr[i]`/`p->f` 需先拿地址与元素类型）。用 `print *ptr` 解引用
  替代。用例 4.26 未含 `arr[2]`。
- 函数入口（`atEntry`）参数从寄存器读，写回寄存器无内存地址（`set` 参数入口处
  不可写）。
- `/DEBUG:FASTLINK` / `/O2` 下局部变量枚举受 dbghelp 物理限制（沿用既有边界）。
- `frame N` 的帧缓存随 `continue`/`step`/`run` 失效（`ctx_reset` 清空）；`bt`
  总是从顶层重建。
