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
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
AIDBG_CPP = os.path.join(HERE, "aidbg.cpp")
AIDBG_EXE = os.path.join(HERE, "aidbg.exe")
TITAN_DLL = os.path.join(HERE, "TitanEngine.dll")
BUILD_DIR = os.path.join(HERE, "build")
BUILD_BIN = os.path.join(BUILD_DIR, "bin")
TEST_BUILD = os.path.join(HERE, "tests", "build.cmd")
TEST_RUN = os.path.join(HERE, "tests", "run_tests.py")

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


def build_aidbg():
    log("initializing TitanEngine submodule ...")
    rc, out = run(["git", "submodule", "update", "--init", "--recursive"])
    if rc != 0:
        fail("git submodule update failed\n" + out[-2000:])

    log("configuring CMake ...")
    rc, out = run(["cmake", "-S", HERE, "-B", BUILD_DIR, "-A", "x64"])
    if rc != 0:
        fail("CMake configure failed\n" + out[-2000:])

    log("building aidbg.exe and TitanEngine.dll ...")
    rc, out = run(["cmake", "--build", BUILD_DIR, "--config", "Release"], timeout=1200)
    if rc != 0:
        fail("CMake build failed\n" + out[-2000:])

    for name, destination in (("aidbg.exe", AIDBG_EXE),
                              ("TitanEngine.dll", TITAN_DLL)):
        source = os.path.join(BUILD_BIN, name)
        if not os.path.isfile(source):
            fail("CMake output missing: %s" % source)
        shutil.copy2(source, destination)
    log("aidbg.exe and TitanEngine.dll built")


def build_tests(vcvars):
    log("building test targets ...")
    build_cmd = os.path.join(HERE, "_build_tests.cmd")
    with open(build_cmd, "w", encoding="utf-8") as f:
        f.write('@echo off\r\n')
        f.write('call "%s" >nul 2>&1\r\n' % vcvars)
        f.write('call "%s"\r\n' % TEST_BUILD)
    try:
        rc, out = run(["cmd", "/c", build_cmd], timeout=600)
        if rc != 0:
            fail("tests/build.cmd failed\n" + out[-2000:])
    finally:
        if os.path.exists(build_cmd):
            os.unlink(build_cmd)


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
        build_aidbg()
        build_tests(vcvars)
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
