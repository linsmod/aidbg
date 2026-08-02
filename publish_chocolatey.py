#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""publish_chocolatey.py - pack and push aidbg to chocolatey.org.

Flow:
    resolve the release (tag) -> get the x64 zip URL + SHA256 from the GitHub
    release -> stage the nuspec + tools -> choco pack -> choco push

The API key can be supplied three ways (in priority order):
    1. --api-key <key>
    2. the CHOCO_API_KEY environment variable
    3. interactive prompt

Usage:
    python publish_chocolatey.py                        # latest release, prompt for key
    python publish_chocolatey.py v0.1.0                 # specific tag
    python publish_chocolatey.py v0.1.0 --api-key xxxx  # tag + key
    python publish_chocolatey.py v0.1.0 --dry-run       # pack only, no push
    $env:CHOCO_API_KEY = "xxxx"; python publish_chocolatey.py v0.1.0

Exit code 0 on success.
"""

import argparse
import getpass
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

REPO = "linsmod/-vc-dev-debuging-tool-for-ai-agent"
HERE = os.path.dirname(os.path.abspath(__file__))
X64_ASSET_RE = re.compile(r"^aidbg-x64-.*\.zip$")


def log(msg):
    print("[choco-publish] %s" % msg, flush=True)


def fail(msg):
    log("ERROR: %s" % msg)
    sys.exit(1)


def github_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "aidbg-choco-publish"})
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        raise RuntimeError("GET %s -> %d: %s"
                           % (url, exc.code, exc.read().decode("utf-8", "replace"))) from exc


def download_sha256(url):
    hasher = __import__("hashlib").sha256()
    with urllib.request.urlopen(url, timeout=300) as resp:
        while True:
            chunk = resp.read(1 << 16)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


def resolve_release(tag):
    if tag:
        url = "https://api.github.com/repos/%s/releases/tags/%s" % (REPO, tag)
    else:
        url = "https://api.github.com/repos/%s/releases/latest" % REPO
    release = github_json(url)
    rtag = release["tag_name"]
    asset = next((a for a in release.get("assets", [])
                  if X64_ASSET_RE.match(a["name"])), None)
    if not asset:
        fail("no aidbg-x64 zip asset in release %s" % rtag)
    digest = asset.get("digest") or ""
    if digest.startswith("sha256:"):
        sha256 = digest[len("sha256:"):].lower()
    else:
        log("no digest on asset, downloading to compute SHA256 ...")
        sha256 = download_sha256(asset["browser_download_url"])
    return {
        "tag": rtag,
        "version": re.sub(r"^v", "", rtag),
        "url": asset["browser_download_url"],
        "sha256": sha256,
    }


def pack(rel):
    stage = tempfile.mkdtemp(prefix="aidbg-choco-")
    tools = os.path.join(stage, "tools")
    os.makedirs(tools)
    shutil.copy(os.path.join(HERE, "packaging", "chocolatey", "aidbg.nuspec"), stage)
    shutil.copy(os.path.join(HERE, "packaging", "chocolatey", "tools",
                             "chocolateyinstall.ps1"), tools)
    shutil.copy(os.path.join(HERE, "packaging", "chocolatey", "tools",
                             "chocolateyuninstall.ps1"), tools)
    install = os.path.join(tools, "chocolateyinstall.ps1")
    with open(install, "r", encoding="utf-8") as f:
        content = f.read()
    content = content.replace("__URL__", rel["url"]).replace("__SHA256__", rel["sha256"])
    with open(install, "w", encoding="utf-8", newline="") as f:
        f.write(content)

    nuspec = os.path.join(stage, "aidbg.nuspec")
    out_dir = os.path.dirname(stage)
    log("packing aidbg %s ..." % rel["version"])
    rc = subprocess.run(["choco", "pack", nuspec,
                         "--version", rel["version"], "--out", out_dir],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        fail("choco pack failed\n" + rc.stdout + rc.stderr)
    nupkg = os.path.join(out_dir, "aidbg.%s.nupkg" % rel["version"])
    if not os.path.isfile(nupkg):
        fail("nupkg not produced at %s" % nupkg)
    shutil.rmtree(stage, ignore_errors=True)
    return nupkg


def get_api_key(arg):
    if arg:
        return arg
    env = os.environ.get("CHOCO_API_KEY")
    if env:
        return env
    return getpass.getpass("Chocolatey.org API key: ")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag", nargs="?", default=None,
                    help="GitHub release tag (default: latest release)")
    ap.add_argument("--api-key", default=None,
                    help="chocolatey.org API key (default: $env:CHOCO_API_KEY or prompt)")
    ap.add_argument("--dry-run", action="store_true",
                    help="pack the nupkg but do not push")
    ap.add_argument("--source", default="https://push.chocolatey.org/",
                    help="chocolatey push source (default: community repo)")
    args = ap.parse_args()

    rel = resolve_release(args.tag)
    log("release %s -> version %s" % (rel["tag"], rel["version"]))
    log("url %s" % rel["url"])
    log("sha256 %s" % rel["sha256"])

    nupkg = pack(rel)
    log("nupkg: %s" % nupkg)

    if args.dry_run:
        log("DRY RUN - not pushing. Push with:")
        log('  choco push "%s" --source "%s" --api-key <key>' % (nupkg, args.source))
        return 0

    key = get_api_key(args.api_key)
    if not key:
        fail("no API key provided")
    log("pushing to %s ..." % args.source)
    rc = subprocess.run(["choco", "push", nupkg,
                         "--source", args.source, "--api-key", key],
                        capture_output=True, text=True)
    if rc.returncode != 0:
        fail("choco push failed\n" + rc.stdout + rc.stderr)
    print(rc.stdout, end="")
    log("done. https://community.chocolatey.org/packages/aidbg")
    return 0


if __name__ == "__main__":
    sys.exit(main())
