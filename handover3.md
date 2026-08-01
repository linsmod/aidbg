# handover3.md — GDB 兼容性修复（第三阶段）

> 状态：**已实现**（除第 9 节暂不修项）。本文件记录与 GDB 同名功能的行为对齐方案、
> 实现结果与取舍。代码变更见 `aidbg.cpp`，测试见 `tests/`。
> 本次新增/更新：`finish`、`x/i`、`hbreak <符号>`、`--command`、`thread <内部编号>`、
> `disable/enable` 无参、`info files`。验证：`tests/run_tests.py` 19 项全绿。

## 0. 目标

`aidbg` 宣称"GDB 风格"，但若干命令与 GDB **同名不同义**。AI 按 GDB 手册调用会
产生错误结果。本阶段把 A 类（同名反义）全部对齐，B 类（格式/精度差异）择机对齐，
C 类（aidbg 独有）保持并文档化。

分三级：
- **A（修）**：`finish`、`x/i`、`hbreak <符号>`、`--command`、`thread <id>`。
- **B（择机）**：`disable` 无参、`info files` 语义、`print` 格式扩展、`step/next` 源码级。
- **C（文档）**：`--json`、`strings`、`info modules`、`info events`、`registers`。

---

## 1. `finish` 停在调用方返回后（A）

### 现状
`finish` → `cmd_step("out")` → `StepOut(cb)`。TitanEngine 的 StepOut 停在**被调函数
的 `ret` 指令处**（实测 `finish` 后 `rip = add+0x22`，仍在 add 内）。
GDB `finish` 停在**调用方返回后的下一条指令**。

### 方案
`StepOut` 完成后检查当前 `rip` 是否仍属于进入时所在的函数：
1. 记录进入 `finish` 时 `rip` 的符号名 `f0`（`SymFromAddrW`）。
2. `StepOut`。
3. 若 `rip` 符号名仍 == `f0`（尚未执行 ret），则 `StepInto` 一次。
4. 重复 3，最多 3 次（防御性，正常 1 次即离开）。

### 边界
- 无符号（stripped）：`f0` 为空 → 不额外步进，保持现状（引擎语义）。
- 叶子函数 / 尾调用：符号名变化即停止，避免过度步进。
- `StepInto` 也可能触发其他断点 → 由既有停止逻辑处理。

### 实现结果
`sym_func_name()` 记录进入时函数名；`StepOut` 后若 `rip` 仍在该函数内则 `StepInto`
（最多 3 次）。实测 `break add; continue; finish` → 停在 `main+0x21`（调用方返回后）。
用例：4.12。

---

## 2. `x/i` 指令反汇编格式（A / 原 C3）

### 现状
`cmd_x` 的格式字符集是 `"xduicsf"`，**不含 `i`**，因此 `x/3i` 中 `i` 落进
`"bhdwgi"` 尺寸集 → 退化为十六进制字节 dump。

### 方案
把 `i` 加入格式判定：`strchr("xduicsfi", c)`。格式 `i` 分支已存在
（`if (fmt == 'i')` → `disasm`），仅需让 `i` 能进入 `fmt`。

### 实现结果
`i` 同时从尺寸集 `"bhdwgi"` 移除（否则先被 size 捕获）。实测 `x/3i` 输出反汇编文本。
用例：4.12。

---

## 3. `hbreak <符号>` 支持符号名（A / 原 C2）

### 现状
`cmd_hbreak` 只用 `parse_addr`（只认地址），`hbreak write_data` → `bad address`。
而 `break`/`mbreak` 已支持 `sym_lookup`。

### 方案
`parse_addr` 失败后回退 `sym_lookup(args[0], addr)`，成功则继续设硬件断点；
两者都失败才报 `bad address`。

### 实现结果
符号解析已生效（`hbreak write_data` 不再 `bad address`）。但 `SetHardwareBreakPoint`
在本机（TitanEngine）**始终失败**，属预先存在的环境问题（改前地址形式同样失败），
与本修复无关。后续需排查 TitanEngine 硬件断点注册逻辑。

---

## 4. `--command` 兼容 GDB 命令文件语义（A）

### 现状
GDB `--command=FILE` = 执行命令文件（`-x` 的别名）。aidbg `--command "cmd"` =
执行**单条命令**。GDB 用户传 `--command somefile` 会被 aidbg 当命令字符串执行 → 失败。

### 方案（兼收并蓄）
`--command` 参数按优先级判定：
1. 若参数是**已存在文件路径** → 按 GDB 语义执行命令文件（与 `-x` 一致）；
2. 否则 → 保持 aidbg 单命令语义（AI 友好）。

保留 `--commands`（脚本）与 `-x`/`--command-file` 不变。文档注明优先级。

### 实现结果
支持 `--command file` 与 `--command=file`（`--command=` 前缀），文件存在则按命令
文件执行，否则单命令。实测均通过。

---

## 5. `thread <id>` 支持 GDB 内部编号（A）

### 现状
`thread 2` 要求参数是 **OS TID**（如 27828），`thread 2` → `no thread 2`。
GDB `thread 2` = **第 2 个线程**（内部编号 1..N）。

### 方案
`cmd_thread` 解析顺序：
1. 先按 TID 精确匹配（`list[i].dwThreadId == arg`）→ 命中即切换；
2. 否则把参数当 **1-based 内部编号**，取 `list[arg-1].dwThreadId` → 切换；
3. 都不匹配 → 报错。

`info threads` 输出补充内部编号列（文本与 JSON），与 GDB 显示一致：
```
* 1  27828  start=...
  2  29184  start=...
```

### 实现结果
`info threads` 输出 `* <内部编号> <tid> start=...`；`thread <id>` 先按 OS tid 精确
匹配，再按 1-based 内部编号匹配。测试 4.7b 改用内部编号切换。用例：4.7b、4.12。

---

## 6. `disable` 无参 = 禁用全部断点（B）

### 现状
GDB `disable`（无参）禁用所有断点；aidbg 无参报 `usage: disable <id ...>`。

### 方案
`cmd_bp_ops` 对 `disable`/`enable` 无参时遍历全部断点（GDB 语义），
`delete` 无参已实现（删全部），保持一致。

### 实现结果
`disable`/`enable` 无参作用于全部断点。实测 `disable` → `disabled all breakpoints`，
`info break` 全部 `Enb=n`。用例：4.12。

---

## 7. `info files` 语义（B，**已实现**）

### 现状
`info files`/`info target` 曾都是 `info modules` 的别名，只列模块 base/name。
GDB `info files` 列出符号文件/exec + 入口点 + 加载的目标文件。

### 方案
独立实现 `cmd_info_files()`：
- 首行 `Symbols from "<exe>".` + `Local exec file:` + 文件类型（pei-x86-64/pei-i386）
- `Entry point: 0x...`（解析 PE 头 `AddressOfEntryPoint` + 基址）
- `Loaded files:` 模块列表（含完整路径）

`info files` → `cmd_info_files`；`info modules`/`info target` 保持简洁 base/name 列表。

### 实现结果
`info files` 输出符号文件、入口点（实测 `0x140002f5e` = `mainCRTStartup`）与加载
文件列表（含 DLL 完整路径）；JSON 输出 `symbols/exec/entry/files` 对象。
用例：4.13。

---

## 8. C 类（aidbg 独有，文档化）

| 功能 | 说明 |
| :--- | :--- |
| `--json` | GDB 无机器接口，aidbg 独有（AI 卖点），保留 |
| `strings` | GDB 用 `x/s`/`find`，aidbg `strings` 为扩展，保留 |
| `info modules` | GDB `info sharedlibrary` 的对应；`info target` 为别名（`info files` 已独立实现） |
| `info events` | 调试事件日志，aidbg 独有 |
| `registers` | `info registers` 的快捷别名 |

---

## 9. 暂不修（记录原因）

| 项 | 原因 |
| :--- | :--- |
| `step`/`next` 源码级单步 | 需要行表驱动（`SymEnumLines`）定位下一条语句，改动大，单列阶段 |
| `print <裸标识符>` | 需接入变量读取（`var_read`）+ 表达式增强，成本高 |
| `watch` 硬件 DR + 地址跟踪 | 现为内存页断点；硬件 DR 实现与 TitanEngine 断点管理耦合 |
| `condition` 局部变量 | 需要每次命中 `SymSetContext` 重算，成本高 |
| `-q` 抑制 stop 寄存器 dump | 与 GDB 一致（GDB -q 也只抑制 banner），不改 |

---

## 10. 验证

- 现有 `tests/run_tests.py` **19 项全绿**（含新增 4.12、4.13）。
- 新增用例 4.12（GDB compat）：`finish` 后 `rip` 为 `main+0x21`、`x/3i` 输出 MOV、
  `disable` 无参禁用全部。
- 新增用例 4.13：`info files` 输出符号文件、入口点、加载文件列表。
- 4.7b 改用 GDB 内部编号切换线程。
