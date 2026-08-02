# HANDOVER4_SOURCE_CHECKSUM.md — 源码/PDB 校验（第四阶段）

> 状态：**已实现**。本文件记录 aidbg 源码校验功能的设计、实现与实测结论。
> 新增：`set source-checksum on|off`、`show source-checksum`、`info source`、
> `list` / `break <file.c:NN>` 校验警告、缓存机制。测试：`tests/run_tests.py`
> 20 项全绿（含新增用例 4.14）。代码变更见 `aidbg.cpp`。

## 0. 背景与目标

MSVC 编译时会在 PDB 中记录每个源文件的哈希（`/ZH:` 控制算法）。若磁盘上的源码与
编译时不符，`list` 显示的行号/内容会误导调试。本阶段实现类似 VS 的"源码校验"：
通过 dbghelp 读取 PDB 中记录的源文件校验和，与本地文件哈希比对，不一致时告警。

定位：**可选特性**，默认关闭，不影响现有行为。

## 1. API 事实（微软文档 + 本地 SDK 确认）

- 签名（DbgHelp.h:2497，Windows SDK 10.0.26100）：

```c
BOOL SymGetSourceFileChecksumW(
    HANDLE hProcess, ULONG64 Base, PCWSTR FileSpec,
    DWORD *pCheckSumType, BYTE *pChecksum, DWORD checksumSize,
    DWORD *pActualBytesWritten);
```

- **`pCheckSumType` 无公开枚举**，本机实测映射见下节。
- `pChecksum` 可传 NULL 先取所需字节数（`pActualBytesWritten`）。
- `Base` 为模块基址（`SymGetModuleBase64`）。
- 需要 `SymInitialize` 过的进程句柄（aidbg 已具备 `g_sym_proc`）。

## 2. 算法映射（实测结论）

对同一源码分别用 `/ZH:MD5`、`/ZH:SHA1`、`/ZH:SHA-256`（正确拼写为 `/ZH:SHA_256`）
编译，probe 打印 `pCheckSumType` 并对比本地 BCrypt 哈希，实测确定：

| pCheckSumType | 算法 | 摘要长度 |
| :--- | :--- | :--- |
| 1 | MD5 | 16 |
| 2 | SHA-1 | 20 |
| 3 | SHA-256 | 32 |

- **MSVC 默认 `/ZH:SHA_256`（type=3）**，并非 MD5。
- `Base=0` 调用 `SymGetSourceFileChecksumW` 失败（err 126），**必须传模块基址**。
- 先以 `pChecksum=NULL` 查询所需字节数，再分配缓冲二次调用。
- 本地哈希用 BCrypt：`BCryptFinishHash` 的输出缓冲需等于摘要长度（传 64 会返回
  `0xc000000d`），MD5/SHA1/SHA256 与 PDB 校验和逐字节一致。
- 未知类型降级为 `unknown-algorithm(N)` 报告，不阻断其他功能。

## 3. 实现

### 3.1 哈希（bcrypt）

- `#include <bcrypt.h>` + `#pragma comment(lib, "bcrypt.lib")`。
- `local_file_checksum(path, type, out)`：按映射表取 BCrypt 算法句柄，读文件分块哈希。
  返回 0=成功 / -1=文件不可读 / -2=算法不支持。

### 3.2 校验核心

- `source_checksum_verify(hProc, base, file)`：
  1. 先以 `pChecksum=NULL` 查询所需字节数（PDB 无校验信息则 `no-checksum`）；
  2. 分配缓冲再取校验和；失败返回 `pdb-error`；
  3. 本地哈希；文件缺失 `file-not-found`，算法不支持 `unknown-algorithm(N)`；
  4. 字节比较 → `ok` / `mismatch`。

### 3.3 缓存

- `std::map<std::string, CheckSumCache>`，key=绝对路径；
  `CheckSumCache = { status, FILETIME mtime, ULONGLONG size }`。
- 命中且 mtime+size 未变直接复用；源文件改动自动失效。

### 3.4 命令

| 命令 | 说明 |
| :--- | :--- |
| `set source-checksum on\|off` | 开关（默认 off），仿 `set engine` |
| `show source-checksum` | 显示开关状态 |
| `info source` | 当前停止位置源文件路径 + 校验结果 + 算法 + 开关；**始终校验**（显式诊断）；JSON：`{file,checksum,algorithm,enabled}` |

### 3.5 `list` 集成

- `cmd_list` 获得 `file` 与 `addr` 后，`base = SymGetModuleBase64(g_sym_proc, addr)`；
- 仅开关开启时校验，非 `ok` 追加 `"!! Checksum mismatch: <file>"`（文本）与
  `"checksum"` 字段（JSON）。

### 3.6 `break <file.c:NN>` / `break <行号>` 集成

- 行号分支命中且开关开启时校验并告警（P1，随本阶段一并实现）。

## 4. 测试（tests/ 用例 4.14）

- 专用目标 `tests/src/test_checksum.c`，`/DEBUG:FULL`，默认 `/ZH`（MD5）。
- 断言：
  - `info source` 且源未改动 → `ok`；
  - 用不同内容覆盖 `.c` → `mismatch`（确定性，不污染其他用例）；
  - `set source-checksum on` + `list` → 输出 `!! Checksum`；
  - `info source` JSON 字段齐全；
  - `set source-checksum off` → `list` 无警告（默认不回归）。

## 5. 已知限制

- `pCheckSumType` 未公开枚举，映射表按实测固化；
- 本地哈希为文件原始字节（含 BOM/行尾），与编译器一致；
- 校验仅针对 dbghelp 能解析到校验和的 PDB（需 PDB 与 exe 同目录或符号路径可达）。
