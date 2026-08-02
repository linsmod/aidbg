# handover6.md — 符号解析缺口修复与命令边界完善

> 状态：**方案待实现**。补测 bp-hit 上下文命令（用例 4.23/4.24）时发现两个 GDB
> 可用的命令因 `parse_addr` 不解析 PDB 符号而失败，已用 4.25 xfail 记录。本文档
> 记录修复方案与"已知边界"的完善清单。代码变更见 `aidbg.cpp`，测试见 `tests/`。

## 0. 背景与目标

在补测「断点命中后上下文命令」（4.23 registers/x/dump/print/set、4.24 断点管理）
时，`disas <符号>` 与 `print <全局符号>` 失败。根因相同：**`parse_addr` 只解析地址
形式，不解析 PDB 符号名**（aidbg.cpp:1667）。GDB 中 `disas func`、`print var`、
`x var`、`set var = v` 均接受符号。

目标：
- P0：`parse_addr` 增加符号回退 → 修复 `disas`、`x`、`set` 等地址侧命令。
- P1：`print <全局符号>` 打印**变量值**（而非地址），与 GDB 语义一致。
- 完善"已知边界"：把缺口、限制、命令支持面整理成明确清单。

## 1. 发现的缺口（实测）

| 命令 | 现状 | GDB |
| :--- | :--- | :--- |
| `disas func2, func2+0x10` | `bad address` | 按符号解析并反汇编 |
| `print global_var` | `cannot parse expression: global_var` | 打印变量值 42 |
| `x func2` | `bad address` | 按符号检视内存 |
| `set *global_var = 5` | `bad address` | 写符号地址 |

已支持符号的命令（无缺口）：`break`、`list`、`hbreak`（handover3）、`watch`/
`rwatch`/`awatch`/`mbreak`（`cmd_mbreak` 已有 sym_lookup 回退，aidbg.cpp:2653）、
`condition`（eval_cond 裸标识符走 sym_lookup）。

## 2. 方案：`parse_addr` 符号回退（P0）

在 `parse_addr` 末尾（`module!off` / `$reg` / 数字 / 加表达式均失败后）追加：

```cpp
// bare identifier: fall back to PDB symbol lookup (needs symbols loaded)
if (sym_lookup(t, out)) return true;
return false;
```

- **影响面**：`disas`、`x`、`set`、`search`/`strings`/`dump` 的地址参数、`break <addr>`
  等全部获得符号解析；`mbreak`/`hbreak` 已有的 fallback 可保留（幂等）。
- **安全**：`sym_lookup` 依赖 `g_sym_active`，run/start 前符号未加载时返回 false，
  行为与现状一致；不会把"符号断点 pending"逻辑破坏（`cmd_break` 的符号分支在其
  内部先处理，parse_addr 只在地址形式失败时兜底）。
- **模块限定**：`sym_lookup` 已支持 `SymFromNameW` 精确名 + 全量枚举回退，`mod!sym`
  由 `module!off` 分支处理（off 部分仍走 parse_addr → 符号回退），保持现状。
- **不解析局部变量**：parse_addr 不做帧内偏移解析（地址随 RSP/RBP 变化，需命中时
  SymSetContext），与 `condition` 局部变量限制一致，保持现状。

## 3. 方案：`print <全局符号>` 打印值（P1）

`parse_addr` 符号回退只能让 `print global_var` 打出**变量地址**，不符合 GDB
（GDB 打印值）。在 `cmd_print`（aidbg.cpp:3359）中，对**裸标识符**专门处理：

1. 若表达式不是 `$reg`、`*`、可解析地址，则 `sym_lookup(expr, addr)`；
2. 成功后按 `sizeof(ULONG_PTR)` 读该地址的值（与 `eval_cond` 裸标识符语义一致，
   aidbg.cpp:1441-1443），打印十进制 + hex。
3. 失败保持现状 `cannot parse expression`。

- `print *<符号>`：走既有 deref 分支（先 parse_addr 得到地址再读值），修复后
  `print *ptr` 与 GDB 一致。
- 大小：固定读 8 字节（对齐 eval_cond）；对 32 位 `int` 全局变量，若高 4 字节为 0
  则十进制正确（`global_var = 42`）。精确按类型大小（SymGetTypeInfo TI_GET_LENGTH）
  列为可选增强，非本阶段必需。
- 输出格式：`value = 42  (0x2a)`（复用现有 print 的 hex+decimal 输出）。

## 4. 已知边界完善（文档）

修复后把以下清单写入 README「已知边界」与「GDB 兼容命令清单」：

| 边界 | 说明 | 状态 |
| :--- | :--- | :--- |
| `print <裸局部标识符>` | 局部变量地址随帧变化，不解析（与 `condition` 一致） | 保持限制 |
| `condition` / `set` 局部变量 | 同上，需每命中 SymSetContext，成本高 | 保持限制 |
| 观察点/内存断点页级粒度 | TitanEngine guard-page，尺寸参数仅提示 | 保持 |
| 同地址重复内存断点 | 再次 `SetMemoryBPXEx` 失败（TitanEngine 限制），测试需换地址 | 文档化 |
| 内存断点尺寸 | `watch` 默认 8 字节，页级生效 | 文档化 |
| 多线程步进 | 停顿时冻结其它线程（等价 `set scheduler-locking on`），GDB 默认 off | 文档化 |
| `step` 单行循环 | 整行循环逐指令单步（慢）；`next` 用区间断点全速 | 文档化 |
| 符号命令支持面 | `disas`/`x`/`set`/`print` 修复后支持符号；`mbreak`/`hbreak`/`break`/`list`/`condition` 已支持 | 修复后更新 |
| WOW64 | 软件/硬件断点、源码单步已支持；`info locals` 未验证 | 保持 |

## 5. 测试计划

- **4.25（xfail → PASS）**：修复后 `disas func2, func2+0x10` 输出 `MOV`；
  `print global_var` 输出 `value = 42`。
- 新增断言（可并入 4.23/4.25）：
  - `x func2`（符号）反汇编/检视成功；
  - `set *global_var = 5` 后再 `x` 校验；
  - `print global_var` 十进制 42。
- 回归：既有全部用例（31 项）保持绿。

## 6. 实施顺序

1. `parse_addr` 末尾符号回退（P0，一处改动）。
2. `cmd_print` 裸标识符取值（P1，复用 eval_cond 全局读取思路）。
3. 用例 4.25 去 xfail 改常态断言；必要时补 `x <符号>` / `set *<符号>` 断言。
4. README「已知边界」与「GDB 兼容命令清单」按 §4 更新。
5. 全量回归。
