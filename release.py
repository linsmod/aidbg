#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""release.py - build aidbg.exe, run the test suite, commit and push to GitHub.

Flow:
    build aidbg.exe -> build test targets -> run tests -> (pass?) commit -> push

Usage:
    python release.py                 build + test + commit + push (auto message)
    python release.py -m "feat: ..."  custom commit message
    python release.py --skip-tests    build only, then commit/push
    python release.py --skip-push     commit only
    python release.py --skip-commit   build + test only (no git ops)
    python release.py --skip-build    reuse the existing aidbg.exe

Exit code 0 on success, 1 on any failure. Intended to be run from the aidbg/
repo root (or anywhere; the script locates itself).
"""

import argparse
import datetime
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AIDBG_CPP = os.path.join(HERE, "aidbg.cpp")
AIDBG_EXE = os.path.join(HERE, "aidbg.exe")
TEST_BUILD = os.path.join(HERE, "tests", "build.cmd")
TEST_RUN = os.path.join(HERE, "tests", "run_tests.py")

# build flags documented at the top of aidbg.cpp
CL_FLAGS = ["/nologo", "/EHsc", "/std:c++17", "/O2", "/utf-8"]


def log(msg):
    print("[release] %s" % msg)


def fail(msg):
    log("ERROR: %s" % msg)
    sys.exit(1)


def run(cmd, timeout=600, cwd=HERE):
    """Run a command, return (exit_code, combined_output)."""
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       timeout=timeout, encoding="utf-8", errors="replace")
    return p.returncode, p.stdout + p.stderr


def find_vcvars():
    """Locate vcvars64.bat (VS2022 first, then vswhere discovery)."""
    candidates = [
        os.path.join(os.environ.get("ProgramFiles", ""),
                     "Microsoft Visual Studio", "2022", "Community",
                     "VC", "Auxiliary", "Build", "vcvars64.bat"),
        os.path.join(os.environ.get("ProgramFiles(x86)", ""),
                     "Microsoft Visual Studio", "2022", "BuildTools",
                     "VC", "Auxiliary", "Build", "vcvars64.bat"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", ""),
                           "Microsoft Visual Studio", "Installer",
                           "vswhere.exe")
    if os.path.isfile(vswhere):
        p = subprocess.run([vswhere, "-latest", "-products", "*",
                            "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                            "-property", "installationPath"],
                           capture_output=True, text=True)
        base = (p.stdout or "").strip()
        c = os.path.join(base, "VC", "Auxiliary", "Build", "vcvars64.bat")
        if os.path.isfile(c):
            return c
    return None


def build_aidbg(vcvars):
    log("building aidbg.exe ...")
    titan_lib = os.path.join(os.environ.get("USERPROFILE", os.path.expanduser("~")),
                             "TitanEngine", "build_x64", "Release")
    # write a temp .cmd so cmd.exe does not mangle the quotes in `cmd /c "..."`
    build_cmd = os.path.join(HERE, "_build_aidbg.cmd")
    with open(build_cmd, "w", encoding="utf-8") as f:
        f.write('@echo off\r\n')
        f.write('call "%s" >nul 2>&1\r\n' % vcvars)
        f.write('cl %s "%s" /Fe:"%s" /Fo:"%s.obj" /link /LIBPATH:"%s" TitanEngine.lib\r\n'
                % (" ".join(CL_FLAGS), AIDBG_CPP, AIDBG_EXE, AIDBG_EXE, titan_lib))
    try:
        rc, out = run(["cmd", "/c", build_cmd])
        if rc != 0:
            fail("cl failed\n" + out[-2000:])
    finally:
        if os.path.exists(build_cmd):
            os.unlink(build_cmd)
    log("aidbg.exe built")


def build_tests():
    log("building test targets ...")
    rc, out = run(["cmd", "/c", TEST_BUILD], timeout=600)
    if rc != 0:
        fail("tests/build.cmd failed\n" + out[-2000:])


def run_tests():
    log("running test suite ...")
    rc, out = run([sys.executable, TEST_RUN], timeout=1200)
    print(out, end="")
    if rc != 0:
        fail("test suite failed")
    log("all tests passed")


def git(args, check=True):
    rc, out = run(["git"] + args)
    if check and rc != 0:
        fail("git %s failed\n%s" % (" ".join(args), out[-1000:]))
    return rc, out


def commit_and_push(message, do_push):
    _, status = git(["status", "--porcelain"], check=False)
    if not status.strip():
        log("working tree clean, nothing to commit")
        return
    log("staging changes ...")
    git(["add", "-A"])
    _, staged = git(["diff", "--cached", "--name-only"])
    files = [f for f in staged.splitlines() if f.strip()]
    log("committing %d file(s)" % len(files))
    git(["commit", "-m", message])
    log("committed: %s" % message.splitlines()[0])
    if do_push:
        log("pushing to origin/master ...")
        git(["push", "origin", "master"])
        log("pushed")
    else:
        log("push skipped")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-m", "--message", default=None,
                    help="commit message (default: auto-generated)")
    ap.add_argument("--skip-build", action="store_true", help="reuse existing aidbg.exe")
    ap.add_argument("--skip-tests", action="store_true", help="skip the test suite")
    ap.add_argument("--skip-commit", action="store_true", help="no git operations")
    ap.add_argument("--skip-push", action="store_true", help="commit but do not push")
    args = ap.parse_args()

    if not os.path.isfile(AIDBG_CPP):
        fail("aidbg.cpp not found in %s" % HERE)

    if not args.skip_build:
        vcvars = find_vcvars()
        if not vcvars:
            fail("vcvars64.bat not found; install VS2022 or set the path in release.py")
        build_aidbg(vcvars)
        build_tests()
    else:
        log("--skip-build: using existing aidbg.exe")

    if not args.skip_tests:
        if not os.path.isfile(AIDBG_EXE):
            fail("aidbg.exe missing; run without --skip-build first")
        run_tests()

    if args.skip_commit:
        log("--skip-commit: no git operations")
        log("done")
        return 0

    message = args.message or (
        "aidbg: build and test pass (%s)"
        % datetime.datetime.now().strftime("%Y-%m-%d %H:%M"))
    commit_and_push(message, not args.skip_push)
    log("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
