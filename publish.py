#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""publish.py - fully manual, locally controlled aidbg release.

No CI automation. Everything runs on this machine, step by step, and each
step asks for confirmation (unless --yes). You decide when and whether each
step happens.

Steps (in order):
    [commit]  stage + commit + push working tree changes (if any)
    [tag]     create annotated git tag v<version> and push it
    [build]   build + test        -> python release.py --skip-commit
    [zip]     create aidbg-x64-v<tag>.zip from build/bin
    [release] GitHub release bound to the tag + upload the zip
              (gh release create v<version> <zip>)
    [winget]  open/update the winget-pkgs PR   -> python winget_publish.py
    [choco]   pack + push to chocolatey.org    -> python publish_chocolatey.py

The release is bound to a real git tag so the tag's commit is always exact.
Build runs after tag so the embedded commit matches the tagged commit and the
binary is not marked -dirty.

Usage:
    python publish.py 0.1.1                # full manual flow (prompts each step)
    python publish.py 0.1.1 --skip-build   # reuse existing build/bin
    python publish.py 0.1.1 --skip-release # build/zip only, no GitHub release
    python publish.py 0.1.1 --skip-commit  --skip-winget --skip-choco
    python publish.py 0.1.1 --yes          # no confirmation prompts
    python publish.py 0.1.1 --dry-run      # print the plan, do nothing

Exit code 0 on success, 1 on any failure or declined step.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD_BIN = os.path.join(HERE, "build", "bin")
CMAKELISTS = os.path.join(HERE, "CMakeLists.txt")
REPO = "linsmod/aidbg"
TAG_RE = re.compile(r"^v?\d+\.\d+\.\d+$")


def log(msg):
    print("\n[release] %s" % msg, flush=True)


def fail(msg):
    log("ERROR: %s" % msg)
    sys.exit(1)


def run(args, cwd=HERE):
    print("    $ %s" % " ".join(args))
    p = subprocess.run(args, cwd=cwd)
    if p.returncode != 0:
        fail("%s exited with code %d" % (args[0], p.returncode))


def git(args, capture=False, cwd=HERE):
    p = subprocess.run(["git"] + args, cwd=cwd, capture_output=True,
                       text=True)
    return p.returncode, p.stdout.strip(), p.stderr.strip()


def confirm(prompt):
    while True:
        answer = input("%s [Y/n] " % prompt).strip().lower()
        if answer in ("", "y", "yes"):
            return True
        if answer in ("n", "no"):
            return False
        print("    (please answer Y or n)")


def read_cmake_version():
    with open(CMAKELISTS, encoding="utf-8") as f:
        m = re.search(r"project\(\s*aidbg\s+VERSION\s+([\d.]+)", f.read())
    return m.group(1) if m else None


def git_clean():
    rc, out, _ = git(["status", "--porcelain"])
    return not out


def git_branch():
    rc, out, _ = git(["rev-parse", "--abbrev-ref", "HEAD"])
    return out


def tag_exists(tag):
    rc, _, _ = git(["rev-parse", "-q", "--verify", "refs/tags/%s" % tag])
    return rc == 0


def do_step(label, desc, fn, args):
    if args.yes or confirm("Run step '%s'?" % label):
        log("STEP: %s" % label)
        fn()
    else:
        log("skipped %s (declined)" % label)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("version", help="version to release, e.g. 0.1.1")
    ap.add_argument("--skip-commit", action="store_true")
    ap.add_argument("--skip-tag", action="store_true")
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-release", action="store_true")
    ap.add_argument("--skip-winget", action="store_true")
    ap.add_argument("--skip-choco", action="store_true")
    ap.add_argument("--yes", action="store_true", help="no confirmation prompts")
    ap.add_argument("--dry-run", action="store_true", help="print the plan only")
    ap.add_argument("--notes", default=None,
                    help="GitHub release notes (default: generate from commits)")
    args = ap.parse_args()

    version = args.version
    if not TAG_RE.match(version):
        fail("version must look like 0.1.1, got '%s'" % version)
    tag = version if version.startswith("v") else "v" + version
    zip_name = "aidbg-x64-%s.zip" % tag
    zip_path = os.path.join(HERE, zip_name)

    cmake_ver = read_cmake_version()
    if cmake_ver != version:
        log("WARNING: CMakeLists.txt project version is %s, releasing %s. "
            "Update it first if this is not intended."
            % (cmake_ver or "unknown", version))
    if not git_clean():
        log("NOTE: working tree has uncommitted changes - the built binary "
            "will be marked -dirty unless you run the commit step first.")

    # ---- collect the plan: each step is (label, description, fn) ----
    steps = []

    if not args.skip_commit and not git_clean():
        branch = git_branch()
        steps.append((
            "commit",
            "git add -A; git commit -m 'Release %s'; git push origin %s"
            % (tag, branch),
            lambda: (run(["git", "add", "-A"]),
                     run(["git", "commit", "-m", "Release %s" % tag]),
                     run(["git", "push", "origin", branch])),
        ))
    elif not args.skip_commit:
        log("working tree clean, nothing to commit")

    if not args.skip_tag:
        if tag_exists(tag):
            log("tag %s already exists locally, skipping creation" % tag)
        else:
            steps.append((
                "tag",
                "git tag -a %s -m 'Release %s'; git push origin %s"
                % (tag, tag, tag),
                lambda: (run(["git", "tag", "-a", tag,
                              "-m", "Release %s" % tag]),
                         run(["git", "push", "origin", tag])),
            ))

    if not args.skip_build:
        steps.append((
            "build",
            "python release.py --skip-commit",
            lambda: run(["python", os.path.join(HERE, "release.py"),
                         "--skip-commit"]),
        ))
    else:
        log("--skip-build: reusing existing build/bin")
        if not os.path.isfile(os.path.join(BUILD_BIN, "aidbg.exe")):
            fail("build/bin/aidbg.exe missing; drop --skip-build")

    if not args.skip_release:
        steps.append((
            "zip",
            "python make_zip.py %s" % version,
            lambda: run(["python", os.path.join(HERE, "make_zip.py"),
                         version]),
        ))
        notes_args = (["--generate-notes"] if not args.notes
                      else ["--notes", args.notes])
        steps.append((
            "release",
            "gh release create %s %s --title 'aidbg %s' --repo %s"
            % (tag, zip_name, tag, REPO),
            lambda: run(["gh", "release", "create", tag, zip_path,
                         "--title", "aidbg %s" % tag, "--repo", REPO]
                        + notes_args),
        ))

    if not args.skip_winget:
        steps.append((
            "winget",
            "python winget_publish.py %s" % tag,
            lambda: run(["python", os.path.join(HERE, "winget_publish.py"),
                         tag]),
        ))

    if not args.skip_choco:
        steps.append((
            "choco",
            "python publish_chocolatey.py %s" % version,
            lambda: run(["python",
                         os.path.join(HERE, "publish_chocolatey.py"),
                         version]),
        ))

    if args.dry_run:
        log("PLAN (--dry-run):")
        for label, desc, _ in steps:
            print("    %-9s %s" % (label, desc))
        return 0

    # ---- execute ----
    for label, _, fn in steps:
        if label == "release" and not os.path.isfile(zip_path):
            fail("missing %s - run the zip step first" % zip_path)
        do_step(label, None, fn, args)

    log("done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
