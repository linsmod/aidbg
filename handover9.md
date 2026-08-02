# handover9.md — 停止输出 GDB 化、JSON 信封统一、run 重启死锁与引擎设置修复

> 状态：**已实现**。本轮修复已知问题清单剩余 5 项：D1 / D2 / D3 / E1 / E2。
> 验证基线：`tests/run_tests.py --no-build`（须在 aidbg 仓库根目录运行）35 全绿。
> 代码变更见 `aidbg.cpp`。

## 0. 背景

handover8 之后已知问题仍开放 5 项，需求方明确处理方向：

| 编号 | 问题 | 处理方向 |
| :--- | :--- | :--- |
| D1 | 每次 stop 都打印完整寄存器表 | **按 GDB 逻辑**：停止只打印位置，寄存器改为 `info registers` 按需查看 |
| D2 | JSON schema 不一致；`bt` JSON 缺符号名 | **统一信封** + `bt` 补符号名 |
| D3 | `-q/--quiet` 不抑制 stop 寄存器 dump | **按 D1 同**：随 D1 移除自动寄存器 dump 而解决 |
| E1 | 停止后再次 `run`/`start` 死锁 | **Fix** |
| E2 | `set engine` 被 `run` 无条件重置；`aslr` 语义与文档相反 | **按 GDB 同**：设置跨 run 持久化；语义对齐 GDB `disable-randomization` |

## 1. 修复设计

### 1.1 D1/D3 —— 停止输出 GDB 化

GDB 停止时只输出位置行（如 `Breakpoint 1, main () at file.c:29`），寄存器需显式
`info registers`。因此：

- **文本模式** `stop_banner()`（aidbg.cpp:933）：删除末尾
  `"  registers:\n" << regs_text()`。
- **JSON 模式** `stop_json()`（aidbg.cpp:903）：删除 `"registers":<regs_json()>` 字段，
  与 GDB MI `*stopped`（不含寄存器）对齐。
- `info registers`/`info regs`（`cmd_registers`，aidbg.cpp:2843）保持不动，文本/JSON 均可
  按需取寄存器。
- D3：`-q/--quiet` 按 GDB 语义只抑制启动 banner（`g_quiet`），停止信息本就精简；无需改动
  `g_quiet`/`g_silent` 的分工。

### 1.2 D2 —— JSON 统一信封 + `bt` 补符号名

统一约定：**aidbg 输出的每一行 JSON 都以 `{"ok":true,` 开头**；事件类行再带
`"type":"..."` 供消费方分发。

- 停止事件 `stop_json`：`{"ok":true,"type":"stopped","reason":...,"thread":...,
  "rip":...,"rip_symbol":...,["file","line"],["breakpoint_id","hits","temporary"],
  ["exception":{...}]}`（去掉 registers）。
- 其余事件类 JSON 一并补 `"ok":true`：
  - `{"ok":true,"type":"running"}`
  - `{"ok":true,"type":"exited","code":N}`
  - `{"ok":true,"type":"detached","pid":N}`
- 命令结果信封 `{"ok":true,"result":...}` / `{"ok":false,"error":...}` 不变。
- `bt` JSON（aidbg.cpp:3140-3148）：每帧追加符号名
  `"symbol":"<resolve(a)>"`，与文本输出 `in <symbol>` 对齐。

### 1.3 E1 —— 停止后再次 `run`/`start` 死锁

根因：`cmd_run`（aidbg.cpp:2045）重启时直接
`ForceClose(); join();`，但 DebugLoop 线程此刻阻塞在 `pause_until_continue` 的条件变量上
（`g_cv.wait(lk, { return !g_waiting || g_quit; })`），`ForceClose` 不会唤醒它 → join 永久
等待。

修复：改用现成的正确拆解序列 `stop_session()`（aidbg.cpp:697），它先置 `g_quit=true`、
`g_waiting=false` 并 `notify_all` 唤醒回调，再 `StopDebug()` 并 join，最后 `sym_end()`。
`cmd_run` 随后 `reset_state()`（含 `g_quit=false`）后即可干净地启动新会话。与 quit 路径
共用同一套拆解逻辑。

### 1.4 E2 —— 引擎设置持久化 + aslr 语义对齐 GDB

GDB 语义：设置跨 `run` 持久化；`set disable-randomization on` 表示禁用 ASLR。

- **持久化**：删除 `cmd_run`（aidbg.cpp:2049-2050）对
  `SetEngineVariable(UE_ENGINE_PASS_ALL_EXCEPTIONS,false)` 与
  `SetEngineVariable(UE_ENGINE_DISABLE_ASLR,false)` 的硬编码重置，让 `set engine` 的
  设置在会话内跨 run 生效。实测 `set engine aslr off` 后 `start` 会真正走到 TitanEngine
  的 ASLR 关闭路径（旧代码会被 `run` 清零、设置根本不生效）。
- **语义**：`cmd_set_engine`（aidbg.cpp:3617）——
  - `aslr` 按自然语义取反：`set engine aslr on` = 保持 ASLR 开启（默认），
    `set engine aslr off` = 禁用 ASLR。实现：`SetEngineVariable(DISABLE_ASLR, !on)`，
    与 TestGuid 7.2 的 `aslr off` 描述一致。
  - 新增 GDB 对齐别名 `disable-randomization`：`on` = 禁用 ASLR，
    `off` = 保持。实现：`SetEngineVariable(DISABLE_ASLR, on)`，与 GDB 同名命令语义一致。
  - 更新 usage/HELP 文案（aidbg.cpp:3614、3737）。
- **已知限制（TitanEngine 引擎，非本次引入）**：`UE_ENGINE_DISABLE_ASLR=true` 走
  TitanEngine 的 `HollowProcessWithoutASLR`（CREATE_SUSPENDED + 映像重映射 + 再 attach），
  本机构建下该路径**不可靠**——实测 `set engine aslr off` / `disable-randomization on`
  后 `start` 大概率 `InitDebugW failed to create the target process`（ISSUES E2 早已记录
  "ASLR 开关无论 on/off 基址不变"，即该特性从未真正生效）。语义与持久化已按 GDB 修复，
  底层引擎实现缺陷属遗留项。

## 2. 代码变更清单

1. `stop_banner`：删 `registers:` 行。
2. `stop_json`：删 `"registers"` 字段；对象开头加 `"ok":true,`。
3. 全部 `{"type":"exited|running|detached",...}` 输出加 `"ok":true,`。
4. `cmd_bt`：JSON 帧加 `"symbol"`。
5. `cmd_run`：`if (g_running) { ForceClose(); join(); }` → `if (g_running) stop_session();`；
   删除引擎变量重置两行。
6. `cmd_set_engine`：`aslr` 取反 + 新增 `disable-randomization`；更新 usage。
7. `README.md`：D1/D2/D3/E1/E2 状态已更新（本文件）；`ISSUES.md` 已移除，ASLR 限制
   并入 README Known limitations。

## 3. 验收

- `tests/run_tests.py --no-build` 保持 35 全绿（含 4.5 stepi/disas、4.6 info regs、
  4.22 batch 退出码、4.23/4.24 bp 上下文）。
- 手动：
  - `start` 停止输出不再含 `registers:`；`info registers` 正常输出。
  - `--json` 下停止事件为 `{"ok":true,"type":"stopped",...}`，`bt` JSON 含 `symbol`。
  - `start` 后再 `run` 不再挂起。
  - `set engine aslr on` 后 `run` 正常启动（ASLR 保持）；`aslr off` /
    `disable-randomization on` 设置已持久化到引擎（旧代码会被 `run` 清零），但 TitanEngine
    的 ASLR 关闭实现不可靠，实测常致 `InitDebugW failed`（见 §1.4 限制）。
  - `set engine passexc on` 后 `run`，设置不再被清零。

## 4. 遗留 / 说明

- `g_quiet`（`-q`）与 `g_silent`（`--batch-silent`）分工不变；`--batch-silent` 仍压制
  全部输出。
- JSON `type` 字段仅事件类存在，命令结果无 `type`——消费方以 `type` 是否存在分发。
- D1 移除 JSON stop 寄存器后，需要寄存器的脚本改用 `info registers`（`{"ok":true,
  "result":{...}}`）。
- `UE_ENGINE_DISABLE_ASLR` 依赖 TitanEngine `HollowProcessWithoutASLR`，本机构建下不可靠
  （`aslr off` 常致进程创建失败）——属引擎遗留，见 §1.4。
