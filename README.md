# aidbg — A GDB-style command-line debugger for Windows, built for AI

> **English**: this file · **中文**: [README_CN.md](README_CN.md)

> Linux has gdb, but Windows has no decent command-line debugger suited to AI tools.
> **aidbg** lets AI debug Win32 programs on Windows as comfortably as it does with gdb on Linux.

aidbg is a GDB-style Windows native debugger built on [TitanEngine](https://github.com/x64dbg/TitanEngine):
a single-file C++17 implementation, ~500KB x64 Release, no install required.
Its command set and semantics align with GDB, and it ships a built-in **JSON Lines
machine interface** and **process-level one-shot commands**, designed for AI coding
assistants, CI automation and scripted debugging.

## Table of Contents

- [Features](#features)
- [Installation](#installation)
- [Quick Start](#quick-start)
- [Building from source](#building-from-source)
- [AI usage examples](#ai-usage-examples)
- [Command overview](#command-overview)
- [GDB compatibility](#gdb-compatibility)
- [Testing](#testing)
- [Roadmap](#roadmap)
- [Documentation](#documentation)
- [Known limitations](#known-limitations)
- [Credits & License](#credits--license)

## Features

- **GDB-aligned command set**: `run / start / continue / stepi / nexti / finish / bt /
  break / hbreak / mbreak / watch / condition / ignore / registers / set / x / dump /
  disas / search / strings / info ... / attach / detach / thread`, with key behaviors
  verified against real GDB (`finish` stops after the caller returns, `start` stops at
  `main`, pre-run symbol breakpoints resolve as pending, `break <line>`, `x/<n>i`
  disassembly, internal thread numbering, bare `disable`, etc.).
- **JSON Lines machine interface (`--json`)**: every output is a single-line JSON object
  with distinct event / result / error types, consumable in one parse pass; `rip` carries
  PDB symbol names.
- **Process-level one-shot commands**: `--command` for a single command, `--commands` for
  a script, stdin piping — each invocation is independent, clean and concurrent.
- **Three breakpoint kinds + API breakpoints**: software, hardware (DR0~DR3), memory
  (guard-page), plus `dll!api` system API breakpoints (`.dll` appended automatically).
- **Source/PDB validation**: `info source` verifies the current source file against the
  PDB checksum; with `set source-checksum on`, `list` / `break <file.c:NN>` warn on
  mismatch (default off).
- **Deep symbols**: PDB loaded via dbghelp; `info locals` / `info args` enumerate local
  variables and parameters; `bt` uses `StackWalk64` (stable even on optimized builds).
- **Exception interception**: divide-by-zero `0xc0000094`, access violation `0xc0000005`,
  etc. stop precisely and are reported.
- **Self `int3` (DebugBreak)**: stops once, then `continue` skips past it; aidbg consumes
  the exception explicitly via TitanEngine's continue-status API, without changing the
  engine's default exception semantics.
- **Multi-thread / attach**: thread enumeration, context switching, cross-thread
  backtraces; `attach <pid>` / `detach`.

## Installation

### winget

```powershell
winget install linsmod.aidbg
```

### Chocolatey

```powershell
choco install aidbg
```

With either package manager the `aidbg` command is added to PATH automatically
(`TitanEngine.dll` is deployed into the same directory). Verify the install:

```powershell
aidbg --version
```

### Manual download (GitHub Releases)

Download `aidbg-x64-v<version>.zip` from [Releases](../../releases), unzip it and put
`aidbg.exe` and `TitanEngine.dll` in the same directory — no install, no admin rights.

## Quick Start

Assuming `aidbg` is on PATH (see [Installation](#installation) if not).

Just like gdb, pass commands on the command line and read the plain text output:

```powershell
aidbg.exe --batch -ex "start" -ex "break kernel32!Sleep" -ex "continue" -ex "bt" target.exe
```

Output is plain GDB text:

```
Temporary breakpoint 1, main () at target.c:29
Stopped: breakpoint
Breakpoint 2 at 0x00007fff... (kernel32!Sleep)
Stopped: breakpoint
#0  0x00007fff... in kernel32!Sleep ()
#1  0x00000001400011b2 in target!main () at target.c:31
#2  0x0000000140001450 in target!mainCRTStartup ()
```

A single command can also be passed with `--command`:

```powershell
aidbg.exe --command "bt" target.exe
```

Interactive use (REPL): run `aidbg.exe target.exe` and use `help` to list commands.

### Building from source

Dependencies: Visual Studio 2022 (MSVC, C++17) and CMake. TitanEngine is pinned as a Git
submodule on the official `x64dbg` branch at `ccac889`, same as x64dbg; the repo no longer
commits prebuilt DLLs.

```cmd
git clone --recurse-submodules https://github.com/linsmod/-vc-dev-debuging-tool-for-ai-agent aidbg
cd aidbg
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build output is in `build\bin\`, containing a directly runnable `aidbg.exe` and
`TitanEngine.dll`. For an existing clone, run
`git submodule update --init --recursive` to fetch the dependency.

Release-related scripts: `release.py` (build + test), `make_zip.py` (zip),
`winget_publish.py` (submit a winget manifest PR), `publish_chocolatey.py`
(pack and push to Chocolatey); the one-shot flow is `publish.py`.

## AI usage examples

AI reads plain text best, so the typical loop is: run one `-ex` command line, read the
text output, decide the next step.

```powershell
aidbg.exe --batch -ex "start" -ex "bt" target.exe
```

```
Temporary breakpoint 1, main () at target.c:29
Stopped: breakpoint
#0  0x0000000140001450 in target!main () at target.c:29
#1  0x0000000140009a10 in target!mainCRTStartup ()
```

Then break, continue and inspect in the same style, plain GDB text throughout:

```powershell
aidbg.exe --batch -ex "break kernel32!Sleep" -ex "continue" -ex "bt" target.exe
```

`--json` remains available when a structured stream is preferred, but for most AI and
scripting use the plain GDB text above is all you need.

## Command overview

```
file / run(r) / start [func] / attach / detach / continue(c,cont)
stepi(si) / nexti(ni) / finish
break(b,br) / hbreak(hb) / mbreak(mb) / watch / rwatch / awatch
condition / ignore / delete / disable / enable
registers(regs) / set / x / dump / disas(u) / bt / search / strings / list
info break / threads / modules / proc / files / locals / args / events / source / registers
set engine <aslr|console|passexc> on/off
set source-checksum on|off / show source-checksum
set pagination on|off / show pagination
echo / help / quit(q)
```

## GDB compatibility

The table below shows how each implemented command aligns with GDB (grouped by the GDB
manual). Legend: **✅** = semantics verified against real GDB; **⚠️** = partial (differences
noted under "Known limitations" or inline); **ⓘ** = aidbg extension (no GDB equivalent).
The "Case" column points to the automated checks in `tests/run_tests.py` (4.21 is an
end-to-end walkthrough of the man-page core commands, 4.22 checks CLI batch semantics).

### Running / Stopping

| Command | GDB semantics | aidbg behavior | Compat | Case |
| :--- | :--- | :--- | :--- | :--- |
| `run [args]` | Start the program | Same (in batch, continues to exit/crash) | ✅ | 4.1b |
| `start [func]` | Start and stop at the entry function | Stops at main/WinMain (or a given function) | ✅ | 4.1 |
| `continue` / `c` | Continue running | Same | ✅ | 4.1 |
| `step` / `s [n]` | Source-line step, **into** calls | Same (falls back to stepi with a note when no line info) | ✅ | 4.19/4.21 |
| `next` / `n [n]` | Source-line step, **over** calls | Same (falls back to nexti with a note when no line info) | ✅ | 4.20/4.21 |
| `stepi` / `si [n]` | Instruction step | Same | ✅ | 4.5 |
| `nexti` / `ni [n]` | Instruction step (over call) | Same | ✅ | 4.5 |
| `finish` | Run until the current frame returns | Stops at the instruction after the caller returns | ✅ | 4.12/4.21 |
| `kill` | Terminate the run | Same | ✅ | — |
| `attach <pid>` | Attach to a process | Same | ✅ | 4.8 |
| `detach` | Detach | Same (target keeps running) | ✅ | 4.8 |

### Breakpoints

| `break <symbol>` | Break by symbol | Same (PDB resolution, pending before run) | ✅ | 4.9/4.1b |
| `break <line>` / `<file.c:line>` | Break by source line | Same (out-of-range reports `No line N`) | ✅ | 4.11 |
| `break <addr>` / `*addr` | Break by address | Same | ✅ | 4.1 |
| `hbreak <addr\|sym> [r/w/x]` | Hardware breakpoint | Same | ✅ | 4.17/4.18 |
| `watch` / `rwatch` / `awatch` | Data watchpoints | Same (TitanEngine guard-page, page granularity) | ⚠️ | 4.3 |
| `condition <id> [expr]` | Breakpoint condition | Same (supports locals; stops conservatively on eval failure) | ✅ | 4.2b/4.27 |
| `ignore <id> <count>` | Ignore the first N hits | Same | ✅ | 4.2 |
| `delete` / `disable` / `enable` | Delete/disable/enable breakpoints | Same (no args = all) | ✅ | 4.12 |
| `mbreak <addr> <size>` | — | aidbg extension (memory-range breakpoint) | ⓘ | 4.3 |

### Stack / Threads

| `bt` / `where` | Backtrace | Same (StackWalk64; frames shown as `module!func+off (file:line)`; works on optimized builds and on WOW64 32-bit targets) | ✅ | 4.1/4.21 |
| `info locals` / `info args` | Local variables / function arguments | Same (needs a full PDB, see Known limitations) | ✅ | 4.6 |
| `thread <id>` | Switch threads | Same (internal number or OS TID) | ✅ | 4.7b |
| `info threads` | List threads | Same (`*` marks the current thread) | ✅ | 4.7 |
| `frame <N>` / `up` / `down` | Frame navigation | Same (`info locals`/`print`/`registers` apply to the selected frame) | ✅ | 4.29 |

### Source / Symbols

| `list` / `l [func\|line]` | Show source near the current line | Same (±5 lines, `>` marks the current line) | ✅ | 4.9/4.21 |
| `info source` | Current source-file info | Same (also checks the PDB checksum) | ✅ | 4.14 |
| `info files` | Symbol files / entry point / loaded files | Same | ✅ | 4.13 |

### Data

| `print` / `p` | Print an expression | `$reg` / literals / `*addr` / **locals** / global symbols (functions print addresses) / arithmetic expressions; **no array subscripts / members / `&`** | ✅ | 4.6/4.25/4.26 |
| `x/<n><fmt> <addr>` | Examine memory | Same (b/h/w/g + x/d/u/i/s/c/f, symbols supported) | ✅ | 4.12/4.25 |
| `set $reg = <val>` | Write a register | Same (RHS supports expressions) | ✅ | 4.6/4.28 |
| `set *addr = <val>` | Write memory | Same (symbol addresses supported) | ✅ | 4.6/4.25 |
| `set <local> = <val>` | Write a local variable | Same | ✅ | 4.28 |
| `registers` / `regs` | Shorthand for `info registers` | Same | ✅ | — |

### Disassembly / Misc

| `disas` / `disassemble [start,end]` | Disassemble | Same (GDB range syntax, symbols supported) | ✅ | 4.5/4.25 |
| `set pagination on\|off` / `show pagination` | Toggle GDB-style paging | Same (interactive REPL only; never pages in batch / `--command` / JSON / piped modes) | ✅ | — |
| `help` / `quit` / `echo` | Help / quit / output | Same | ✅ | — |
| `dump` / `search` / `strings` / `info modules\|events\|proc` | — | aidbg extensions | ⓘ | 4.10 |

### Invocation (see gdb_quick_reference.txt OPTIONS)

| `--batch` | Batch; nonzero exit on command errors | Same (checked in case 4.22) | ✅ | 4.22 |
| `-ex "cmd"` / `-x file` | Execute a command / command file | Same | ✅ | 4.22 |
| `-e file` (`--exec=file`) | Select the executable | Same | ✅ | 4.22 |
| `--args prog arg...` | Set target and arguments | Same (a bare `run` uses those args) | ✅ | — |
| `-q` / `--quiet` | Suppress the startup banner | Same | ✅ | — |
| `--command` | Run a single command | Same (a file path is executed as a command file) | ✅ | 4.12 |
| `--json` | — | aidbg's JSON Lines machine interface | ⓘ | — |

## Testing

The `tests/` suite has 7 test targets (basic / memory / exception / multi-thread /
symbols / source-step / resident process) plus automated cases covering breakpoints,
conditions / ignore, memory watchpoints, exceptions, single-stepping, source-level
`step`/`next`, variable enumeration, thread switching, attach/detach, search, line
breakpoints, post-breakpoint contextual commands and GDB compatibility (including the
4.21 command walkthrough, 4.22 batch exit codes, 4.25 symbol resolution, and
4.26–4.29 locals / expressions / frame navigation). All pass (35 cases):

```cmd
set _NT_SYMBOL_PATH=%CD%\..
tests\build.cmd
tests\run_tests.cmd        :: green, returns 0, CI-ready
```

## Roadmap

- [x] **Locals + expression evaluation** (done, handover7): `print`/`condition`/`set`
  support local variables and arithmetic expressions; `frame`/`up`/`down` navigation
  (cases 4.26–4.29)
- [ ] **AI interface enhancements**: long-lived `--host/--port` socket protocol (multiple
  commands per session, no per-command process spawn); richer JSON (dump/x byte arrays,
  fuller breakpoint fields)
- [ ] **Engine stability & edges**: WOW64 32-bit unwinding for `info locals`/`thread`
  (`bt` done — StackWalk64 on a packed x86 context, cases 4.16/4.18);
  `list` with non-ASCII source paths (wide-char open); `set scheduler-locking on|off`
- [ ] **Advanced breakpoints**: `commands <id>` breakpoint command lists (auto-continue,
  etc.); `until` / `advance <loc>` run-to-location

## Documentation

| Document | Description |
| :--- | :--- |
| `TestGuid.md` | Guidance for designing test programs and test logic |
| `tests/README.md` | Test suite documentation |
| `archive/README.md` | Historical implementation notes and overview (handover 1–5 index) |

## Known limitations

- `print`/`condition`/`set` support locals and expressions (handover7); **array
  subscripts / member access / `&` are not yet supported** (the evaluator is value-based;
  use `print *ptr` to dereference instead).
- `print` supports `$reg` / literals / `*addr` / global symbols (data prints its value,
  functions print their address).
- `disas`/`x`/`set`/`print` support PDB symbols (`parse_addr` symbol fallback, handover6);
  `break`/`list`/`hbreak`/`watch`/`mbreak`/`condition` do too.
- Source-level `step`/`next` are implemented (`archive/handover5.md`); without PDB line
  info they fall back to instruction stepping (`stepi`/`nexti`) with a notice.
- 32-bit (WOW64) targets are supported (software/hardware breakpoints, single-step;
  cases 4.16–4.18). `bt` unwinds them via `StackWalk64` fed an x86-layout
  `WOW64_CONTEXT` (previously the AMD64-layout mismatch stopped the walk after frame 0).
- `bt` reports only frames the unwinder derives from metadata (x64 `.pdata` / x86 FPO +
  EBP chain); it does not raw-scan the stack, so no guessed frames are shown. Where the
  unwind metadata runs out (e.g. re-entrant dispatch inside a WoW64 message pump) the
  list simply ends there.
- `info locals` variable enumeration on `/DEBUG:FASTLINK` or `/O2` builds is limited by a
  dbghelp limitation.
- **ASLR disabling is unreliable (TitanEngine engine limitation)**: `set engine aslr off`
  (or the GDB-aligned alias `set engine disable-randomization on`) maps to
  `UE_ENGINE_DISABLE_ASLR`, which relies on TitanEngine's `HollowProcessWithoutASLR`
  (create-suspended + image re-map + re-attach). In this build that path is unreliable and
  often fails with `InitDebugW failed to create the target process`. The setting itself
  persists across `run` (GDB-like); only the underlying engine implementation is broken.
  `set engine aslr on` (the default) works normally.

## Credits & License

The debugging engine is built on [x64dbg/TitanEngine](https://github.com/x64dbg/TitanEngine)
(enhanced v2.0.3) and used under the TitanEngine open-source license. aidbg's own code is a
single-file C++17 implementation, freely usable, modifiable and redistributable.
