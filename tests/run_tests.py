#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""run_tests.py - automated test runner for the aidbg test suite.

Builds the test targets (if needed) and executes the test cases defined in
TestGuid.md section 4 using aidbg's GDB-compatible batch mode.

Design notes (these are the realities the suite is built around):
  * Symbol breakpoints (`break main`) require the PDB to be loaded, which only
    happens once the target has started -> cases use `start` (which stops at
    the internal initial breakpoint) and only then `break <symbol>`.
  * GDB batch exit codes are now reliable: `quit` ends command processing
    without discarding the exit code, so 0 = success and nonzero = a command
    error or a crash/exception stop. The exception cases (4.4/4.4b) and the
    out-of-range line case (4.11c) assert exit 1; everything else asserts 0.
  * Dynamic values (thread ids, PIDs, search hit addresses) cannot live in a
    static script, so those cases are driven here programmatically.

Usage:
  python run_tests.py [--no-build] [--keep] [--case N]
      --no-build   do not (re)build the targets
      --keep       do not kill a leftover test_attach.exe after the attach case
      --case N     run only case 4.N (e.g. --case 4.2)
"""

import argparse
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # repo root
CASES_DIR = os.path.join(HERE, "cases")
SRC_DIR = os.path.join(HERE, "src")
BUILD_BIN = os.path.join(ROOT, "build", "bin")
AIDBG = os.path.join(BUILD_BIN, "aidbg.exe")
BUILD_CMD = os.path.join(HERE, "build.cmd")
PYTHON = sys.executable

TARGETS = ["test_basic", "test_memory", "test_exception", "test_threads",
           "test_symbols", "test_attach", "test_checksum", "test_debugbreak",
           "test_source_step", "test_vars", "test_string"]

# x86 (WoW64) targets are built with a separate script; aidbg (x64) debugs them
# cross-architecture, which exercises the STATUS_WX86_BREAKPOINT path.
X86_TARGETS = ["test_wow64"]
BUILD_CMD_X86 = os.path.join(HERE, "build_x86.bat")

# Never let the spawned aidbg / test targets pop their own console windows.
# (The debuggee itself is additionally hidden via `set engine console on`,
# which makes TitanEngine create it with CREATE_NO_WINDOW.)
CREATE_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0) if os.name == "nt" else 0


# ------------------------------------------------------------------- helpers ---

def run(cmd, timeout=120):
    """Run a command, capture stdout, return (exit_code, output)."""
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                       encoding="utf-8", errors="replace",
                       creationflags=CREATE_NO_WINDOW)
    return p.returncode, p.stdout + p.stderr


def aidbg_run(script):
    """Run `aidbg --batch -x <script>` and return (exit_code, output).

    `-ex "set engine console on"` is injected before the script so every
    debuggee is created with CREATE_NO_WINDOW (no console window pops up).
    """
    return run([AIDBG, "--batch", "-ex", "set engine console on", "-x", script],
               timeout=90)


def ensure_aidbg():
    """Ensure build/bin/aidbg.exe exists (and its TitanEngine.dll sibling).

    aidbg is built by CMake (`cmake -S . -B build -A x64` +
    `cmake --build build --config Release`), whose outputs live in build/bin.
    The tests always run that freshly built binary. If it is missing the
    runner configures and builds it here so `run_tests.py` is self-contained.
    """
    if os.path.isfile(AIDBG) and os.path.isfile(os.path.join(BUILD_BIN, "TitanEngine.dll")):
        return
    print("[runner] build/bin/aidbg.exe not found -> running CMake build")
    rc, out = run(["cmake", "-S", ROOT, "-B", os.path.join(ROOT, "build"),
                   "-A", "x64"], timeout=600)
    if rc != 0:
        print(out)
        sys.exit("error: CMake configure failed")
    rc, out = run(["cmake", "--build", os.path.join(ROOT, "build"),
                   "--config", "Release"], timeout=1200)
    if rc != 0:
        print(out)
        sys.exit("error: CMake build failed")
    if not os.path.isfile(AIDBG):
        sys.exit("error: aidbg.exe missing after build: %s" % AIDBG)


def aidbg_session(cmds, marker="===AIDBG_MARKER===", timeout=90):
    """Run aidbg in stdin-batch mode, feeding `cmds` one at a time.

    Returns (process, output_until_marker).  The `marker` echo lets the caller
    read the output produced by the commands issued *before* the marker even
    though the process stays alive (used to parse dynamic values such as
    thread ids in the same session).  The caller must eventually write "quit"
    to `proc.stdin` and wait.
    """
    p = subprocess.Popen([AIDBG], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True,
                         encoding="utf-8", errors="replace",
                         creationflags=CREATE_NO_WINDOW)
    assert p.stdin and p.stdout
    p.stdin.write("\n".join(["set engine console on"] + list(cmds)) + "\n")
    p.stdin.flush()
    # read incrementally until the marker shows up
    collected = ""
    deadline = time.time() + timeout
    while marker not in collected and time.time() < deadline:
        line = p.stdout.readline()
        if line == "":
            break
        collected += line
    return p, collected


def aidbg_session_finish(proc, extra_cmds, timeout=90):
    """Send remaining commands (incl. 'quit') to a live aidbg_session and
    return the full stdout produced from the marker point onward."""
    assert proc.stdin and proc.stdout
    proc.stdin.write("\n".join(extra_cmds) + "\n")
    proc.stdin.flush()
    rest = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = proc.stdout.readline()
        if line == "":
            break
        rest += line
    proc.wait(timeout=timeout)
    return rest


def target_outdated(name):
    """True when a test target's exe is missing or its source is newer."""
    exe = os.path.join(ROOT, name + ".exe")
    src = os.path.join(SRC_DIR, name + ".c")
    if not os.path.exists(exe):
        return True
    return os.path.getmtime(src) > os.path.getmtime(exe)


def ensure_targets(force=False):
    """(Re)build the test targets if they are stale.

    A target is stale when the exe is missing or the .c source is newer than
    the exe (so editing a test program recompiles it automatically). `force`
    rebuilds everything regardless of timestamps. x64 targets use build.cmd;
    x86 (WoW64) targets use build_x86.bat.
    """
    stale_x64 = TARGETS if force else [t for t in TARGETS if target_outdated(t)]
    if stale_x64:
        print("[runner] building x64 targets: %s" % ", ".join(stale_x64))
        rc, out = run(["cmd", "/c", BUILD_CMD], timeout=300)
        if rc != 0:
            print(out)
            return False
    stale_x86 = X86_TARGETS if force else [t for t in X86_TARGETS if target_outdated(t)]
    if stale_x86:
        print("[runner] building x86 targets: %s" % ", ".join(stale_x86))
        rc, out = run(["cmd", "/c", BUILD_CMD_X86], timeout=300)
        if rc != 0:
            print(out)
            return False
    return True


def expect(output, needles):
    """Return list of needles missing from output."""
    return [n for n in needles if n not in output]


def parse_thread_ids(output):
    """Parse `info threads` output.  Format (after GDB-style numbering):
       '* 1  27828  start=...' / '  2  29184  start=...'
    Returns the list of OS thread ids (the second number on each line)."""
    ids = []
    for line in output.splitlines():
        m = re.match(r"^\s*\*?\s*\d+\s+(\d+)\s+start=", line)
        if m:
            ids.append(int(m.group(1)))
    return ids


def parse_search_address(output):
    """Extract the first address line from `search` output (0x...)."""
    for line in output.splitlines():
        m = re.match(r"^\s*(0x[0-9a-fA-F]+)\s*$", line)
        if m:
            return m.group(1)
    return None


# ------------------------------------------------------------ dynamic cases ---

def case_thread_switch():
    """Case 4.7b - switch to another thread and backtrace it.

    Thread ids are per-run, so everything runs in ONE interactive aidbg
    session: break on thread_func, enumerate threads, parse a non-current
    thread id, then `thread <id>` + `bt` in the same session.
    """
    results = []
    marker = "===THREADMARK==="
    cmds = [
        "file test_threads.exe",
        "start",
        "break thread_func",
        "continue",
        "info threads",
        "echo %s" % marker,
    ]
    out, _unused = None, None
    proc, out = aidbg_session(cmds, marker=marker)
    results.append(("stops at thread_func",
                    "Stopped: breakpoint" in out and "thread_func" in out, out))
    ids = parse_thread_ids(out)
    if len(ids) < 2:
        results.append((">=2 threads listed", False,
                        "expected >=2 threads, got %r" % ids))
        aidbg_session_finish(proc, ["quit"])
        return results

    # find the current thread's INTERNAL number (line prefixed with '*'),
    # then switch to another thread by internal number (GDB-style `thread <id>`).
    current_num = None
    for line in out.splitlines():
        if re.match(r"^\s*\*\s*\d+", line):
            m = re.search(r"^\s*\*\s*(\d+)", line)
            current_num = int(m.group(1))
            break
    target_num = None
    for n in range(1, len(ids) + 1):
        if n != current_num:
            target_num = n
            break
    if target_num is None:
        results.append(("non-current thread found", False, ids))
        aidbg_session_finish(proc, ["quit"])
        return results

    # continue the same session: switch thread by GDB internal number, backtrace, quit
    out2 = aidbg_session_finish(proc, ["thread %d" % target_num, "bt", "quit"])
    results.append(("thread switch to internal #%d" % target_num,
                    "Switching to thread" in out2, out2))
    results.append(("bt on switched thread", "#0" in out2, out2))
    return results


def case_attach_detach():
    """Case 4.8 - attach to a running target, break, detach."""
    results = []
    attach_exe = os.path.join(ROOT, "test_attach.exe")
    p = subprocess.Popen([attach_exe], cwd=ROOT, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, creationflags=CREATE_NO_WINDOW)
    try:
        time.sleep(1.0)   # let it spin up and print its PID
        pid = p.pid
        script = os.path.join(HERE, "_case_4_8_attach.txt")
        with open(script, "w") as f:
            f.write("\n".join([
                "attach %d" % pid,
                "break tick_func",
                "continue",
                "bt",
                "detach",
                "quit",
            ]) + "\n")
        rc, out = aidbg_run(script)
        os.unlink(script)
        results.append(("attach to pid %d" % pid, "Stopped: attach" in out, out))
        results.append(("break tick_func",
                        "Breakpoint 1, tick_func" in out, out))
        results.append(("bt shows tick_func", "tick_func" in out and "#0" in out, out))
        results.append(("detach leaves target running",
                        "Detached from process" in out, out))
        # target should still be alive after detach
        time.sleep(0.5)
        alive = subprocess.run(
            ["tasklist", "/NH", "/FI", "PID eq %d" % pid],
            capture_output=True, text=True).stdout
        results.append(("target still alive after detach",
                        str(pid) in alive, alive))
        return results
    finally:
        p.kill()
        p.wait()


def case_search_strings():
    """Case 4.10b - after search finds "Hello", run strings to confirm."""
    results = []
    script = os.path.join(CASES_DIR, "case_4_10_search.txt")
    rc, out = aidbg_run(script)
    addr = parse_search_address(out)
    if addr is None:
        results.append(("search finds a match", False, out))
        return results
    results.append(("search finds a match", True, addr))
    follow = [
        "file test_basic.exe",
        "start",
        "strings %s 0x1000" % addr,
        "quit",
    ]
    follow_script = os.path.join(HERE, "_case_4_10b_follow.txt")
    with open(follow_script, "w") as f:
        f.write("\n".join(follow) + "\n")
    rc2, out2 = aidbg_run(follow_script)
    os.unlink(follow_script)
    results.append(("strings at hit shows Hello, aidbg!",
                    "Hello, aidbg!" in out2, out2))
    return results


def case_source_checksum():
    """Case 4.14 - source-file / PDB checksum verification.

    Builds test_checksum.exe, verifies `info source` reports ok while the
    source matches, tampers the .c file so the PDB checksum no longer matches,
    then asserts `info source` -> mismatch and that `list` warns (only while
    `set source-checksum on`).  The .c file is restored in `finally`.
    """
    results = []
    src = os.path.join(SRC_DIR, "test_checksum.c")
    exe = os.path.join(ROOT, "test_checksum.exe")
    with open(src, "rb") as f:
        original = f.read()

    def run_script(lines):
        script = os.path.join(HERE, "_case_4_14_tmp.txt")
        with open(script, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        rc, out = aidbg_run(script)
        os.unlink(script)
        return out

    try:
        base_cmds = ["file %s" % exe, "start"]
        out = run_script(base_cmds + ["info source", "quit"])
        results.append(("info source ok while source matches",
                        "Checksum: ok" in out, out))

        # tamper in binary so the PDB checksum (taken at build time over the
        # original bytes) no longer matches; restore must also be byte-exact.
        with open(src, "wb") as f:
            f.write(original + b"\n// TAMPERED for checksum test\n")

        out = run_script(base_cmds + ["info source", "quit"])
        results.append(("info source mismatch after tamper",
                        "Checksum: mismatch" in out, out))

        out = run_script(["file %s" % exe, "set source-checksum on", "start",
                          "list", "quit"])
        results.append(("list warns on mismatch (toggle on)",
                        "!! Checksum mismatch" in out, out))

        out = run_script(base_cmds + ["list", "quit"])
        results.append(("list silent when toggle off",
                        "!! Checksum mismatch" not in out, out))
    finally:
        with open(src, "wb") as f:
            f.write(original)
    return results


def case_debugbreak():
    """Case 4.15 - breakpoint disposition belongs to aidbg, not TitanEngine.

    `continue` consumes an executed short int3 so DebugBreak resumes normally.
    RaiseException(STATUS_BREAKPOINT) remains NOT_HANDLED and reaches the
    debuggee's SEH handler. Both scenarios must stop only once after `start`.
    """
    results = []
    exe = os.path.join(ROOT, "test_debugbreak.exe")
    script = os.path.join(HERE, "_case_4_15_dbgbrk.txt")
    flags = [
        "dbgbrk_before.flag", "dbgbrk_after.flag",
        "raise_before.flag", "raise_handler.flag", "raise_after.flag",
    ]

    def run_scenario(commands):
        with open(script, "w", encoding="utf-8") as f:
            f.write("\n".join(["file %s" % exe] + commands + ["quit"]) + "\n")
        p = subprocess.run([AIDBG, "--batch", "-ex", "set engine console on",
                            "-x", script], cwd=ROOT,
                           capture_output=True, text=True, timeout=90,
                           encoding="utf-8", errors="replace",
                           creationflags=CREATE_NO_WINDOW)
        return p.stdout + p.stderr

    try:
        out = run_scenario(["start", "continue", "continue"])
        results.append(("program ran before DebugBreak",
                        os.path.exists(os.path.join(ROOT, "dbgbrk_before.flag")), out))
        results.append(("single stop at the int 3 (no duplicate callback)",
                        out.count("Stopped:") == 2 and "Stopped: breakpoint" in out, out))
        results.append(("resumed past DebugBreak (after flag written)",
                        os.path.exists(os.path.join(ROOT, "dbgbrk_after.flag")), out))

        out = run_scenario(["set args raise", "start", "continue", "continue"])
        results.append(("RaiseException stops as an exception",
                        out.count("Stopped:") == 2 and "exception 0x80000003" in out, out))
        results.append(("RaiseException reaches the debuggee SEH handler",
                        os.path.exists(os.path.join(ROOT, "raise_handler.flag")), out))
        results.append(("debuggee resumes after handling RaiseException",
                        os.path.exists(os.path.join(ROOT, "raise_after.flag")), out))
    finally:
        if os.path.exists(script):
            os.unlink(script)
        for name in flags:
            path = os.path.join(ROOT, name)
            if os.path.exists(path):
                os.unlink(path)
    return results


def case_addr_expr():
    """Case 4.30b - address-of (&) and halfword examine regression.

    Verifies the three fixes from handover8:
      * `print &<local>` / `print &<global>` resolve addresses (the unary &
        operator was missing, so `print &file` failed with
        "cannot parse expression").
      * `x/Nh $reg-0xNN` / `$reg+0xNN` parse as address expressions; a broken
        address silently fell back to dumping the instruction pointer (which is
        why a halfword request could look like a disassembly of the code).
      * `x/Nh` dumps halfwords as unit-sized hex values; only `x/Ni`
        disassembles; a bad register address now reports an error instead of
        the CIP fallback.

    Stack addresses vary per run, so the dump addresses are parsed from the
    output and their offsets are checked arithmetically.
    """
    results = []
    script = os.path.join(HERE, "_case_4_30b_addr.txt")
    lines = [
        "file test_vars.exe",
        "start",
        "break 21",
        "continue",
        "print local_sum",
        "print &local_sum",
        "print *&local_sum",
        "print &g_vtick",
        "x/4h $rsp",
        "x/4h $rsp-0x10",
        "x/4h $rsp+0x10",
        "x/4h 0x1400076c0",
        "x/4i 0x1400076c0",
        "x/4h $nosuchreg-0x10",
        "quit",
    ]
    with open(script, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    try:
        rc, out = aidbg_run(script)
    finally:
        if os.path.exists(script):
            os.unlink(script)

    # `print` value lines, in command order: local_sum, &local_sum,
    # *&local_sum, &g_vtick.
    values = [int(m, 16) for m in re.findall(r"value = 0x([0-9a-fA-F]+)", out)]
    # `x/Nh` dump lines carry the address right after the leading space-less
    # start of the line followed by a colon (disassembly lines have no colon).
    dumps = [int(m, 16) for m in re.findall(r"^(0x[0-9a-fA-F]+):", out, re.M)]

    results.append(("local_sum evaluates to 36",
                    len(values) >= 4 and values[0] == 0x24, out))
    results.append(("&local_sum resolves (print does not error)",
                    len(values) >= 2 and values[1] != 0, out))
    results.append(("*(&local_sum) == local_sum (address round-trip)",
                    len(values) >= 3 and (values[2] & 0xFFFFFFFF) == 0x24, out))
    results.append(("&g_vtick is a fixed image address",
                    len(values) >= 4 and values[3] == 0x14011A280, out))
    results.append(("x/4h $rsp dumps on the stack (not the image)",
                    len(dumps) >= 1 and dumps[0] < 0x100000000, out))
    results.append(("x/4h $rsp-0x10 is 0x10 below $rsp",
                    len(dumps) >= 2 and dumps[1] == dumps[0] - 0x10, out))
    results.append(("x/4h $rsp+0x10 is 0x10 above $rsp",
                    len(dumps) >= 3 and dumps[2] == dumps[0] + 0x10, out))
    results.append(("x/4h at main dumps halfword values, not disassembly",
                    len(dumps) >= 4 and dumps[3] == 0x1400076C0 and
                    "0x8348 0x38ec" in out, out))
    results.append(("x/4i is the disassembly form",
                    "SUB RSP, 0x38" in out, out))
    results.append(("bad register reports bad address",
                    "error: bad address: $nosuchreg-0x10" in out, out))
    return results


def case_bp_commands():
    """Case 4.32 - GDB breakpoint command lists (commands/silent/end).

    The command list attached to `break show` runs automatically on the hit:
    `silent` suppresses the stop banner (so `start`'s temporary breakpoint is
    the only "Stopped: breakpoint" in the output), `x/hs pw` prints the string
    the pointer param points to, and the embedded `continue` resumes execution.
    """
    results = []
    script = os.path.join(HERE, "_case_4_32_cmds.txt")
    lines = [
        "file test_string.exe",
        "start",
        "break show",
        "commands",
        "silent",
        "x/hs pw",
        "continue",
        "end",
        "continue",
        "quit",
    ]
    with open(script, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    try:
        rc, out = aidbg_run(script)
    finally:
        if os.path.exists(script):
            os.unlink(script)
    results.append(("breakpoint command list ran automatically",
                    '"Hello, wide aidbg!"' in out, out))
    results.append(("silent suppressed the show stop banner",
                    out.count("Stopped: breakpoint") == 1, out))
    results.append(("embedded continue resumed execution to exit",
                    "Process exited with code 1" in out, out))
    return results


def case_batch_compat():
    """Case 4.22 - GDB invocation compatibility (gdb_quick_reference.txt OPTIONS).

    Verifies the man-page documented CLI semantics:
      - `--batch` exits 0 after processing a command file with no errors, and
        exits nonzero when a command in the file errors.
      - `-e <file>` (--exec=file) selects the executable.
    """
    results = []
    ok_script = os.path.join(HERE, "_case_4_22_ok.txt")
    err_script = os.path.join(HERE, "_case_4_22_err.txt")
    run_script = os.path.join(HERE, "_case_4_22_run.txt")
    try:
        with open(ok_script, "w", encoding="utf-8") as f:
            f.write("file exit_test.exe\nquit\n")
        with open(err_script, "w", encoding="utf-8") as f:
            f.write("file exit_test.exe\nbreak nosuchsymbol\n")
        with open(run_script, "w", encoding="utf-8") as f:
            f.write("run\nquit\n")
        p = subprocess.run([AIDBG, "--batch", "-x", ok_script], cwd=ROOT,
                           capture_output=True, text=True, timeout=90,
                           encoding="utf-8", errors="replace",
                           creationflags=CREATE_NO_WINDOW)
        results.append(("--batch success exits 0",
                        p.returncode == 0, p.stdout + p.stderr))
        p = subprocess.run([AIDBG, "--batch", "-x", err_script], cwd=ROOT,
                           capture_output=True, text=True, timeout=90,
                           encoding="utf-8", errors="replace",
                           creationflags=CREATE_NO_WINDOW)
        results.append(("--batch command error exits nonzero",
                        p.returncode != 0, p.stdout + p.stderr))
        p = subprocess.run([AIDBG, "--batch", "-e", os.path.join(ROOT, "exit_test.exe"),
                            "-x", run_script], cwd=ROOT,
                           capture_output=True, text=True, timeout=90,
                           encoding="utf-8", errors="replace",
                           creationflags=CREATE_NO_WINDOW)
        results.append(("-e <file> selects the executable (--exec)",
                        "Process exited with code 42" in (p.stdout + p.stderr),
                        p.stdout + p.stderr))
    finally:
        for f in (ok_script, err_script, run_script):
            if os.path.exists(f):
                os.unlink(f)
    return results


# ------------------------------------------------------------- case table ---

CASES = [
    {
        "id": "4.1",
        "name": "start stops at main / backtrace",
        "script": "case_4_1_basic_break.txt",
        "expect": ["Temporary breakpoint 1, main", "Stopped: breakpoint", "test_basic.exe!main"],
    },
    {
        "id": "4.1b",
        "name": "pending break before run (GDB-style)",
        "script": "case_4_1b_pending_break.txt",
        "expect": ["pending", "Breakpoint 1, main () at test_basic.c:29", "test_basic.exe!main"],
    },
    {
        "id": "4.2",
        "name": "ignore count (func1 hit 3)",
        "script": "case_4_2_ignore_count.txt",
        "expect": ["hit 3", "already hit 3 times"],
    },
    {
        "id": "4.2b",
        "name": "condition g_loop_index == 2",
        "script": "case_4_2b_condition.txt",
        "expect": ["hit 3", "g_loop_index == 2"],
    },
    {
        "id": "4.3",
        "name": "memory watchpoint",
        "script": "case_4_3_memory_watch.txt",
        "expect": ["Stopped: memory", "mem/watch"],
    },
    {
        "id": "4.4",
        "name": "exception div-zero (0xc0000094)",
        "script": "case_4_4_exception_divzero.txt",
        "expect": ["Stopped: exception", "0xc0000094", "imagebase"],
        "exit": 1,
    },
    {
        "id": "4.4b",
        "name": "exception access violation (0xc0000005)",
        "script": "case_4_4b_exception_av.txt",
        "expect": ["Stopped: exception", "0xc0000005"],
        "exit": 1,
    },
    {
        "id": "4.5",
        "name": "stepi / disas",
        "script": "case_4_5_stepi_disas.txt",
        "expect": ["Stopped: step", "0x0000000140"],
    },
    {
        "id": "4.6",
        "name": "info args / info locals / set $rax",
        "script": "case_4_6_locals_args.txt",
        "expect": ["func2", "= 0x0000000000000005"],
    },
    {
        "id": "4.7",
        "name": "thread enumeration",
        "script": "case_4_7_threads.txt",
        "expect": ["Stopped: breakpoint", "thread_func"],
    },
    {
        "id": "4.9",
        "name": "symbol break / list / bt",
        "script": "case_4_9_symbols_list.txt",
        "expect": ["Breakpoint 2, add", "test_symbols.c", "test_symbols.exe!add"],
    },
    {
        "id": "4.10",
        "name": "memory search for Hello",
        "script": "case_4_10_search.txt",
        "expect": None,   # handled by case_search_strings()
        "dynamic": "search",
    },
    {
        "id": "4.11",
        "name": "source-line break (file.c:NN)",
        "script": "case_4_11_line_break.txt",
        "expect": ["Breakpoint 2 at test_symbols.c:13", "add () at test_symbols.c:13"],
    },
    {
        "id": "4.11b",
        "name": "source-line break before run (pending)",
        "script": "case_4_11b_pending_line.txt",
        "expect": ["pending", "add () at test_symbols.c:15"],
    },
    {
        "id": "4.11c",
        "name": "out-of-range source line errors",
        "script": "case_4_11c_line_oob.txt",
        "expect": ["No line 999"],
        "exit": 1,
    },
    {
        "id": "4.12",
        "name": "GDB compat: finish/x-i/disable",
        "script": "case_4_12_gdb_compat.txt",
        "expect": ["main+0x21", "MOV", "disabled all breakpoints"],
    },
    {
        "id": "4.13",
        "name": "info files (symbol/exec files)",
        "script": "case_4_13_info_files.txt",
        "expect": ["Symbols from \"test_basic.exe\"", "Local exec file:", "Entry point: 0x"],
    },
    {
        "id": "4.14",
        "name": "source/PDB checksum verification",
        "script": None,
        "expect": None,
        "dynamic": "checksum",
    },
    {
        "id": "4.15",
        "name": "breakpoint continuation policy",
        "script": None,
        "expect": None,
        "dynamic": "debugbreak",
    },
    {
        "id": "4.16",
        "name": "32-bit (WoW64) breakpoint continuation",
        "script": "case_4_16_wow64_bp.txt",
        "expect": ["Stopped: breakpoint", "wow_target", "hit 3", "Stopped: step"],
    },
    {
        "id": "4.17",
        "name": "hardware breakpoint (x64)",
        "script": "case_4_17_hwbreak_x64.txt",
        "expect": ["Hardware breakpoint 2", "hbreak", "Stopped: hardware"],
    },
    {
        "id": "4.18",
        "name": "hardware breakpoint (WoW64)",
        "script": "case_4_18_hwbreak_wow64.txt",
        "expect": ["Hardware breakpoint 2", "hbreak", "Stopped: hardware"],
    },
    {
        "id": "4.19",
        "name": "source-level step (enters callee)",
        "script": "case_4_19_source_step.txt",
        "expect": ["Stopped: step", "callee (test_source_step.c:15)", "test_source_step.c:17"],
    },
    {
        "id": "4.20",
        "name": "source-level next (skips calls/loops)",
        "script": "case_4_20_source_next.txt",
        "expect": ["Stopped: step", "test_source_step.c:30", "test_source_step.c:33"],
    },
    {
        "id": "4.21",
        "name": "GDB command compatibility (start/step/next/bt/finish/list)",
        "script": "case_4_21_gdb_compat2.txt",
        "expect": ["Temporary breakpoint 1, main", "Breakpoint 2, main () at test_source_step.c:29",
                   "callee (test_source_step.c:15)", "test_source_step.exe!callee", "main+0x1b", ">   29"],
    },
    {
        "id": "4.22",
        "name": "GDB invocation compat (--batch exit code, -e)",
        "script": None,
        "expect": None,
        "dynamic": "batchcompat",
    },
    {
        "id": "4.23",
        "name": "bp-hit contextual inspect commands",
        "script": "case_4_23_context_cmds.txt",
        "expect": ["Stopped: breakpoint", "value = 0x0000000000000000",
                   "value = 0x0000000300905a4d", "Wrote 0x0000000000001234",
                   "test_basic.exe", "loaddll", "args = (none)",
                   "source-checksum = off", "bp-hit-context", "func2+0x4"],
    },
    {
        "id": "4.24",
        "name": "bp-hit breakpoint mgmt + watch variants",
        "script": "case_4_24_bp_ops.txt",
        "expect": ["keep n", "keep y", "Memory breakpoint 5", "mem/watch", "deleted 6"],
    },
    {
        "id": "4.25",
        "name": "symbol resolution: disas/x/set/print",
        "script": "case_4_25_sym_addr.txt",
        "expect": ["MOV", "value = 0x000000000000002a", "value = 0x00000001400075e0",
                   "Wrote 0x0000000000000005", "value = 0x0000000000000005"],
    },
    {
        "id": "4.26",
        "name": "print local vars + expressions",
        "script": "case_4_26_print_expr.txt",
        "expect": ["Stopped: breakpoint", "value = 0x0000000000000024",
                   "value = 0x00000000000000b4", "value = 0x00000000000000d8",
                   "value = 0x0000000000001950"],
    },
    {
        "id": "4.27",
        "name": "condition with a local variable",
        "script": "case_4_27_condition_local.txt",
        "expect": ["Breakpoint 2 condition: local_x2 == 6", "level1 () at test_vars.c:34"],
    },
    {
        "id": "4.28",
        "name": "set local variable + expr RHS",
        "script": "case_4_28_set_local.txt",
        "expect": ["Wrote 0x0000000000000032 to local_sum", "value = 0x0000000000000032",
                   "Wrote 0x0000000000000037 to local_prod", "value = 0x0000000000000037"],
    },
    {
        "id": "4.29",
        "name": "frame navigation + per-frame info locals",
        "script": "case_4_29_frame.txt",
        "expect": ["level1+0x6c (test_vars.c:34)", "local_x2", "0x0000000000000006",
                   "main+0x16 (test_vars.c:42)", "v                       int"],
    },
    {
        "id": "4.30",
        "name": "address-of (&) + GDB examine formats (x/Nfu)",
        "script": "case_4_30_addr_expr.txt",
        "expect": ["value = 0x0000000000000024", "value = 0x000000014011a280",
                   "0x8348 0x38ec", "33608 14572", "-31928 14572",
                   "1000001101001000", "0101510", "72 'H'",
                   "0x00000001400076c2:  0x38ec 0x44c7",
                   "SUB RSP, 0x38", "error: bad address"],
        "exit": 1,
    },
    {
        "id": "4.30b",
        "name": "address-of round-trip + $reg±offset dump addresses",
        "script": None,
        "expect": None,
        "dynamic": "addrexpr",
    },
    {
        "id": "4.31",
        "name": "examine strings + local-var addresses (x/s, x/hs)",
        "script": "case_4_31_strings.txt",
        "expect": ["\"Hello, aidbg!\"", "\"Hello, wide aidbg!\"",
                   "48 00 65 00 6c 00", ": \"H\""],
    },
    {
        "id": "4.32",
        "name": "breakpoint command lists (commands/silent/end)",
        "script": None,
        "expect": None,
        "dynamic": "bpcmds",
    },
]


def run_case(case):
    """Run one static case, return list of (label, passed, output).

    Cases marked `xfail` document a known limitation: the assertions are still
    evaluated, but failure is reported as XFAIL (expected) instead of FAIL, so
    the suite stays green while the limitation is visible.
    """
    script = os.path.join(CASES_DIR, case["script"])
    rc, out = aidbg_run(script)
    # GDB batch exit code: 0 on success, nonzero on a command error or a
    # crash/exception stop (quit preserves this now, matching GDB).
    want = case.get("exit", 0)
    results = [("script exits %d" % want, rc == want, out)]
    if case["expect"]:
        missing = expect(out, case["expect"])
        results.append(("output contains %s" % ", ".join(case["expect"]),
                        not missing, "missing: %s" % missing if missing else out))
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--force-build", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--case", default=None)
    args = ap.parse_args()

    ensure_aidbg()
    if not args.no_build and not ensure_targets(force=args.force_build):
        print("error: failed to build targets")
        return 1

    # (id, name, results, xfail_reason)
    all_cases = []
    for case in CASES:
        if args.case and case["id"] != args.case:
            continue
        if case.get("dynamic") == "search":
            results = case_search_strings()
        elif case.get("dynamic") == "checksum":
            results = case_source_checksum()
        elif case.get("dynamic") == "debugbreak":
            results = case_debugbreak()
        elif case.get("dynamic") == "batchcompat":
            results = case_batch_compat()
        elif case.get("dynamic") == "addrexpr":
            results = case_addr_expr()
        elif case.get("dynamic") == "bpcmds":
            results = case_bp_commands()
        else:
            results = run_case(case)
        all_cases.append((case["id"], case["name"], results, case.get("xfail")))

    if args.case in (None, "4.7b"):
        all_cases.append(("4.7b", "thread switching + bt", case_thread_switch(), None))
    if args.case in (None, "4.8"):
        all_cases.append(("4.8", "attach / detach", case_attach_detach(), None))
    # report
    print("\n%-6s  %-40s  %-6s  %s" % ("CASE", "NAME", "STATUS", "DETAIL"))
    print("-" * 100)
    total = failed = 0
    for cid, name, results, xfail in all_cases:
        ok = all(p for _, p, _ in results)
        total += 1
        status = "PASS"
        if xfail:
            status = "XFAIL" if not ok else "XPASS"
        elif not ok:
            status = "FAIL"
            failed += 1
        print("%-6s  %-40s  %-6s  %s" % (cid, name, status,
              (": " + xfail) if xfail else ""))
        for label, passed, out in results:
            mark = "  [ok] " if passed else "  [!!] "
            print("  %s%s" % (mark, label))
            if not passed:
                snippet = out.strip().splitlines()
                tail = "\n".join(snippet[-12:])
                print("        -- output tail --\n%s" % tail)
    print("-" * 100)
    print("summary: %d passed, %d failed" % (total - failed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
