#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""run_tests.py - automated test runner for the aidbg test suite.

Builds the test targets (if needed) and executes the test cases defined in
TestGuid.md section 4 using aidbg's GDB-compatible batch mode.

Design notes (these are the realities the suite is built around):
  * Symbol breakpoints (`break main`) require the PDB to be loaded, which only
    happens once the target has started -> cases use `start` (which stops at
    the internal initial breakpoint) and only then `break <symbol>`.
  * In batch mode a bare `run` continues past the initial breakpoint and runs
    to completion, so the exception cases (4.4/4.4b) use `run` to reach the
    fault and are asserted on the *output text*, not the exit code (the exit
    code after `quit` is subject to a stop/quit race in aidbg).
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
ROOT = os.path.dirname(HERE)          # repo root: aidbg.exe lives here
CASES_DIR = os.path.join(HERE, "cases")
SRC_DIR = os.path.join(HERE, "src")
AIDBG = os.path.join(ROOT, "aidbg.exe")
BUILD_CMD = os.path.join(HERE, "build.cmd")
PYTHON = sys.executable

TARGETS = ["test_basic", "test_memory", "test_exception", "test_threads",
           "test_symbols", "test_attach", "test_checksum", "test_debugbreak"]

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


def ensure_targets():
    """(Re)build the test targets via build.cmd if the exe is missing."""
    missing = [t for t in TARGETS if not os.path.exists(os.path.join(ROOT, t + ".exe"))]
    if not missing:
        return True
    print("[runner] missing targets: %s -> running build.cmd" % ", ".join(missing))
    rc, out = run(["cmd", "/c", BUILD_CMD], timeout=300)
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
    """Case 4.15 - program-generated DebugBreak()/int 3.

    A program with DebugBreak() in the middle writes dbgbrk_before.flag and
    dbgbrk_after.flag into its working directory. With the TitanEngine fix the
    stray int 3 is consumed (DBG_CONTINUE, like VS): aidbg stops ONCE at the
    int 3 and `continue` resumes past it, so the after-flag must exist and the
    process must exit cleanly. A regression (double-stop / NOT_HANDLED) leaves
    the process stuck or crashing and the after-flag missing.
    """
    results = []
    exe = os.path.join(ROOT, "test_debugbreak.exe")
    before = os.path.join(ROOT, "dbgbrk_before.flag")
    after = os.path.join(ROOT, "dbgbrk_after.flag")

    script = os.path.join(HERE, "_case_4_15_dbgbrk.txt")
    with open(script, "w", encoding="utf-8") as f:
        f.write("\n".join([
            "file %s" % exe,
            "start",
            "continue",
            "continue",
            "quit",
        ]) + "\n")
    try:
        p = subprocess.run([AIDBG, "--batch", "-ex", "set engine console on",
                            "-x", script], cwd=ROOT,
                           capture_output=True, text=True, timeout=90,
                           encoding="utf-8", errors="replace",
                           creationflags=CREATE_NO_WINDOW)
        out = p.stdout + p.stderr
        stops = out.count("Stopped:")
        results.append(("program ran before DebugBreak",
                        os.path.exists(before), out))
        results.append(("single stop at the int 3 (no double-stop)",
                        stops <= 2, out))
        results.append(("resumed past DebugBreak (after flag written)",
                        os.path.exists(after), out))
    finally:
        os.unlink(script)
        for f in (before, after):
            if os.path.exists(f):
                os.unlink(f)
    return results


# ------------------------------------------------------------- case table ---

CASES = [
    {
        "id": "4.1",
        "name": "start stops at main / backtrace",
        "script": "case_4_1_basic_break.txt",
        "expect": ["Temporary breakpoint 1, main", "Stopped: breakpoint", "in main"],
    },
    {
        "id": "4.1b",
        "name": "pending break before run (GDB-style)",
        "script": "case_4_1b_pending_break.txt",
        "expect": ["pending", "Breakpoint 1, main () at test_basic.c:29", "in main"],
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
    },
    {
        "id": "4.4b",
        "name": "exception access violation (0xc0000005)",
        "script": "case_4_4b_exception_av.txt",
        "expect": ["Stopped: exception", "0xc0000005"],
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
        "expect": ["Breakpoint 2, add", "test_symbols.c", "in add"],
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
        "name": "DebugBreak() continuation (TitanEngine fix)",
        "script": None,
        "expect": None,
        "dynamic": "debugbreak",
    },
]


def run_case(case):
    """Run one static case, return list of (label, passed, output)."""
    script = os.path.join(CASES_DIR, case["script"])
    rc, out = aidbg_run(script)
    results = [("script exits", rc == 0, out)]
    if case["expect"]:
        missing = expect(out, case["expect"])
        results.append(("output contains %s" % ", ".join(case["expect"]),
                        not missing, "missing: %s" % missing if missing else out))
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--case", default=None)
    args = ap.parse_args()

    if not os.path.exists(AIDBG):
        print("error: %s not found" % AIDBG)
        return 1
    if not args.no_build and not ensure_targets():
        print("error: failed to build targets")
        return 1

    # (id, name, results)
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
        else:
            results = run_case(case)
        all_cases.append((case["id"], case["name"], results))

    if args.case in (None, "4.7b"):
        all_cases.append(("4.7b", "thread switching + bt", case_thread_switch()))
    if args.case in (None, "4.8"):
        all_cases.append(("4.8", "attach / detach", case_attach_detach()))
    # report
    print("\n%-6s  %-40s  %-6s  %s" % ("CASE", "NAME", "STATUS", "DETAIL"))
    print("-" * 100)
    total = failed = 0
    for cid, name, results in all_cases:
        ok = all(p for _, p, _ in results)
        total += 1
        if not ok:
            failed += 1
        print("%-6s  %-40s  %-6s  %s" % (
            cid, name, "PASS" if ok else "FAIL", ""))
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
