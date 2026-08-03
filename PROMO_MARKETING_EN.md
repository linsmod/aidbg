# Linux has gdb. Windows has no decent command-line debugger suited for AI tools. How do you get AI to happily develop and debug Win32 programs on Windows? aidbg can be your new choice!

> **Project URL:** https://github.com/linsmod/aidbg.git

## Why do AI developers on Windows always end up fighting their debugger?

On Linux, AI coding assistants debug C/C++ programs as naturally as breathing:
`gdb -batch -ex "bt"`, `-ex "x/8gx $rsp"` — one command, one result, readable by machines,
chained by scripts, and the whole locate → fix → verify loop runs end to end.

Switch to Windows, and the mood instantly sours:

- **Visual Studio's** debugger is as powerful as they come, but it's made for "people."
  AI can't click buttons, and automation scripts can't either;
- **cdb / WinDbg** are plenty capable at the command line, but their command language is
  its own thing and the output is verbose to the point of pain. AI has to learn a whole
  new dialect;
- Fall back to **MinGW gdb**? Its support for MSVC-compiled binaries, PDB symbols, Win32
  exceptions, and system API breakpoints is patchy at best.

The net result: **AI writes code on Windows with ease, then hits a collective wall the
moment it needs to "run and debug."** It's either printf hacking or begging a human to
click breakpoints in an IDE by hand. Half of what makes AI useful just gets amputated.

## aidbg: bringing an "AI-friendly gdb" back to Windows

**aidbg** is a Windows-native command-line debugger built for AI. Single file, ~500KB,
no installation. Its mission in one sentence: **let AI debug Win32 programs on Windows as
happily as it does with gdb on Linux.**

To do that, it nails three things that matter most to AI:

### Thing one: the command set is the GDB toolbox — zero learning curve for AI

AI doesn't have to learn a single new command. All of gdb's everyday operations are
supported, and the behavior matches — not just the names:

```
run / start / continue / stepi / nexti / finish / bt
break / hbreak / mbreak / watch / condition / ignore
registers / set / x / dump / disas / search / strings
info break / threads / modules / proc / locals / args
attach / detach / thread / file / list
```

Don't underestimate "behavior matches." It means:

- `finish` stops **after the caller returns**, not loitering inside the callee;
- `start` stops right at `main`, with a GDB-flavored banner:
  `Temporary breakpoint 1, main () at file.c:line`;
- `break main` works **before** `run` (a pending breakpoint that lands automatically at
  launch — ASLR-native);
- `x/3i` prints real disassembly, not hex bytes;
- `thread 2` switches by GDB internal numbering, and `info threads` looks the same;
- even `--command <file>` honors GDB's command-file semantics.

**Every habit AI picked up from the GDB manual works on aidbg verbatim. Not one line
needs changing.**

### Thing two: JSON Lines output — a language AI actually speaks

This is aidbg's most "AI-native" design decision: in `--json` mode, **every line of
output is a single JSON object**. No more hunting through twenty lines of register dump.

```json
{"ok":true,"result":{"breakpoint":{"id":1}}}
{"type":"stopped","reason":"breakpoint","thread":27828,"rip":"0x140001234"}
{"type":"stopped","reason":"exception","exception":{"code":"0xc0000005","address":"0x0"}}
{"type":"exited","code":42}
```

Events, results, and errors are three clearly separated message types — **one JSON parser
handles every scenario**. Even `rip` carries a symbol name, like
`boom+0x14 (symtest.c:2)`, so pinpointing the problem doesn't even require guessing.

### Thing three: one command, one result — AI's question-answer rhythm

AI debugging typically goes "inspect a step, look, then take the next step." aidbg is
built as a **process-level one-shot command** to match:

```powershell
aidbg.exe --json --command "run" target.exe
aidbg.exe --json --command "break kernel32!Sleep" target.exe
aidbg.exe --json --command "continue" target.exe
aidbg.exe --json --command "registers" target.exe
aidbg.exe --json --command "bt" target.exe
```

No long-running process, no state to manage, no session memory to babysit — **every call
is independent, clean, and parallelizable**. That's the ideal operating model for
automation and AI agents.

## Real chops: polished for real Win32 scenarios

- **Breakpoint trinity + API breakpoints**: software, hardware (DR0~DR3), and memory
  breakpoints — plus `break kernel32!Sleep` to stop right on a system call;
- **Deep symbols**: `info locals` / `info args` enumerate local variables and arguments;
  `bt` backtraces hold up even on Release-optimized builds;
- **Exceptions, caught**: divide-by-zero `0xc0000094`, access violation `0xc0000005` —
  stopped precisely, reported clearly;
- **Multithreading, no sweat**: thread enumeration, context switching, cross-thread
  backtraces — one `thread 2` away;
- **Attach & detach**: `attach <pid>` onto a running process; the target keeps running
  after `detach`;
- **Memory arsenal**: `dump` / `x/<n><fmt>` / `search` (`?` wildcards) / `strings`, all present;
- **Engineering details**: `set engine aslr off` pins the base for regression tests;
  exit codes reflect crashes; CI-ready out of the box.

## The receipts: 19 automated test cases, all green

aidbg is not a concept demo. The `tests/` suite ships 6 test target programs and
**19 automated cases** — breakpoints, conditions, memory watches, exceptions,
single-stepping, variables, multithreading, attach/detach, right up to GDB compatibility.
**All pass.** After any code change, one `tests\run_tests.cmd` is all the confidence you need.

## Closing

Linux has gdb, and AI flies with it. Windows has been missing this one weapon.

aidbg fuses **GDB's command habits + a JSON machine interface + dead-simple process-level
invocation** into one tool, so AI debugging Win32 programs on Windows stops being a
compromise and becomes a pleasure.

If you build AI coding assistants or agents,
if you need to give automation native debugging superpowers on Windows,
if you're done with printf and hand-clicked breakpoints —

**aidbg can be your new choice!**

> The repo's `handleover.md` / `handover2.md` / `handover3.md` document the full design
> and the pitfalls hit along the way; `TestGuid.md` is a systematic testing guide.
> Try it, file issues, and feel free to contribute.
