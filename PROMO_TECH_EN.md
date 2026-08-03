# Linux has gdb. Windows has no decent command-line debugger suited for AI tools. How do you get AI to happily develop and debug Win32 programs on Windows? aidbg can be your new choice!

> **Project URL:** https://github.com/linsmod/aidbg.git

## Problem Background

In the Linux ecosystem, gdb's batch mode (`gdb -batch -ex`) has long been the de facto
standard for AI tooling and automation to drive native debugging: machine-readable,
process-level invocation, stable command semantics. On Windows, the picture is different:

- Visual Studio's debugger is GUI-shaped and cannot be consumed by AI/CLI automation;
- cdb/WinDbg have their own command family, verbose output, and a mental model far from
  GDB's, making the learning cost for AI high;
- MinGW gdb has poor support for MSVC-produced binaries, PDB symbols, Win32 exceptions,
  and system API breakpoints.

**The gap**: a Windows-native debugger with a GDB-aligned command set, machine-parseable
output, and process-level one-shot invocation. aidbg was designed exactly for this.

## Positioning & Implementation

- **Implementation**: single-file C++17 (MSVC), links `TitanEngine.dll`, ~500KB x64 Release.
- **Architecture**: a REPL main thread and a DebugLoop worker thread synchronized via a
  condition_variable; stopping callbacks block and wait for the main thread's commands,
  exit callbacks only set events; symbols go through dbghelp (`SymInitializeW` +
  `fInvadeProcess=TRUE`, which natively handles ASLR runtime bases).
- **Docs**: `handleover.md` / `handover2.md` / `handover3.md` record design, implementation,
  and pitfalls.

## Core Capabilities

### 1. GDB Command Set Alignment (including semantics)

```
run / start [func] / continue(c) / stepi(si) / nexti(ni) / finish / bt
break / hbreak / mbreak / watch / rwatch / awatch / condition / ignore
registers / set / x / dump / disas(u) / search / strings / list
info break / threads / modules / proc / files / locals / args
attach / detach / delete / disable / enable / thread / set engine / file
```

Key semantics match GDB (handover3.md, all verified by test):

| Behavior | Alignment |
| :--- | :--- |
| `finish` | Stops at the instruction after the caller's return (`main+0x21`), not at the end of the callee |
| `start` / `start <func>` | Temporary breakpoint at entry; prints `Temporary breakpoint 1, main () at file.c:line` |
| Symbol breakpoint before run | `break main` recorded as pending, applied at actual base after `run`/`attach` (A1) |
| `break <lineno>` / `break file.c:NN` | Line-to-address resolution; out-of-range lines report `No line N` (A2) |
| `x/<n>i` | Instruction disassembly (format charset `xduicsfi`; `i` no longer falls into the size set) |
| `thread <id>` | Exact OS TID match first, then 1-based internal numbering |
| `disable`/`enable` with no args | Acts on all breakpoints (GDB semantics) |
| `info files` | Symbol file + entry point (PE `AddressOfEntryPoint`) + loaded file list |
| `--command <file>` | If the argument is an existing file, execute as a GDB command file |

### 2. JSON Lines Machine Interface (`--json`)

All output is single-line JSON, with event/result/error clearly separated — one parse
consumes everything:

```
{"ok":true,"result":{"breakpoint":{"id":1}}}
{"type":"stopped","reason":"breakpoint","thread":27828,"rip":"0x..","registers":{...}}
{"type":"stopped","reason":"exception","exception":{"code":"0xc0000005","address":"0x0"}}
{"type":"running"} / {"type":"exited","code":42} / {"type":"detached","pid":1234}
```

### 3. Process-Level One-Shot Commands

```
aidbg.exe --json --command "break kernel32!Sleep" target.exe
aidbg.exe --json --command "x/4gx $rsp" target.exe
aidbg.exe --json --command "bt" target.exe
```

One process, one command, one line of output, exit code 0/1; use `--commands <file>` or a
stdin pipe for multi-command sessions.

### 4. Win32 Scenario Capabilities

- **Breakpoints**: software / hardware (DR0~DR3) / memory (guard-page) / API
  (`dll!api`, auto-appends `.dll`);
- **Symbols**: PDB sideloaded via dbghelp; `info locals`/`info args` use
  `SymSetContext`+`SymEnumSymbols`+`RtlVirtualUnwind`; `bt` uses `StackWalk64`
  (independent of -O2/omit-frame-pointer);
- **Exceptions**: `0xc0000094` (divide by zero), `0xc0000005` (access violation), etc.
  stop precisely with details;
- **Multithreading**: `info threads` marks current thread with `*`; `thread` switches
  context, then cross-thread `bt`;
- **Attach**: `attach <pid>` / `detach` (target keeps running after detach);
- **Memory**: `x/<n><fmt>` (b/w/g + x/d/u/i/s/c/f), `search` (`?` wildcards), `strings`.

## Known Boundaries (source-level, see README.md)

- `print <bare identifier>`, `condition` with local variables, and source-level
  `step`/`next` are not implemented (`stepi`/`nexti` at instruction level work);
- 32-bit (WOW64) targets supported: software/hardware breakpoints, single-step,
  bt (cases 4.16–4.18).

## Verification Status

`tests/` suite: 6 test targets (basic/memory/exception/threads/symbols/resident process)
+ 19 automated cases, all green, covering breakpoints, condition/ignore, memory watches,
exceptions, single-stepping, variable enumeration, thread switching, attach/detach,
search, line breakpoints, and GDB compatibility. Run with:

```
tests\build.cmd
tests\run_tests.cmd        :: returns 0 when all green, CI-ready
```
