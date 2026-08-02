#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""winget_publish.py - open/update a microsoft/winget-pkgs PR for aidbg.

The manual path for publishing aidbg to winget. The GitHub Actions workflow
(.github/workflows/publish.yml) automates subsequent versions with
winget-releaser, but that action refuses to do the very first submission, so
this script covers that case (and serves as a manual fallback).

The whole flow is driven by the GitHub REST API:

    resolve release tag -> fetch x64 zip asset + SHA256
    -> render the 3 manifests (version / installer / locale)
    -> ensure the winget-pkgs fork exists and is synced to upstream master
    -> create a branch + a single commit containing the manifests
    -> open (or find) the pull request

The manifests use the portable-zip pattern, so winget registers the `aidbg`
command (via PortableCommandAlias) into the WinGet Links directory, which is on
the user PATH by default - no manual environment setup needed.

Auth: uses `gh auth token`; override with the GITHUB_TOKEN env var.

Usage:
    python winget_publish.py                     # latest release
    python winget_publish.py v0.1.0              # specific tag
    python winget_publish.py --dry-run v0.1.0    # print manifests only
    python winget_publish.py --no-pr v0.1.0      # push branch, skip the PR
"""

import argparse
import base64
import hashlib
import http.client
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO = "linsmod/-vc-dev-debuging-tool-for-ai-agent"
UPSTREAM = "microsoft/winget-pkgs"
PACKAGE_ID = "linsmod.aidbg"
MANIFEST_VERSION = "1.6.0"
X64_ASSET_RE = re.compile(r"^aidbg-x64-.*\.zip$")
WINGET_PKGS_FORK = "winget-pkgs"


def log(msg):
    print("[winget_publish] %s" % msg, flush=True)


def fail(msg):
    log("ERROR: %s" % msg)
    sys.exit(1)


class ApiError(Exception):
    def __init__(self, code, detail):
        self.code = code
        super().__init__("GitHub API error %d: %s" % (code, detail))


def get_token():
    env = os.environ.get("GITHUB_TOKEN")
    if env:
        return env
    try:
        return subprocess.check_output(
            ["gh", "auth", "token"], text=True).strip()
    except Exception as exc:
        fail("could not obtain a GitHub token (gh auth token / GITHUB_TOKEN): %s"
             % exc)


def api(token, method, path, data=None):
    req = urllib.request.Request("https://api.github.com" + path, method=method)
    req.add_header("Authorization", "Bearer %s" % token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("User-Agent", "aidbg-winget-publish")
    if data is not None:
        req.data = json.dumps(data).encode("utf-8")
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req) as resp:
            raw = resp.read().decode("utf-8")
            return json.loads(raw) if raw.strip() else None
    except urllib.error.HTTPError as exc:
        raise ApiError(exc.code, exc.read().decode("utf-8", "replace"))
    except urllib.error.URLError as exc:
        raise RuntimeError("network error: %s" % exc)


def repo_exists(token, repo):
    try:
        api(token, "GET", "/repos/" + repo)
        return True
    except ApiError as exc:
        if exc.code == 404:
            return False
        raise


def raw_status(owner_repo, ref, path):
    url = "https://raw.githubusercontent.com/%s/%s/%s" % (owner_repo, ref, path)
    try:
        with urllib.request.urlopen(url, timeout=60) as resp:
            return resp.status
    except urllib.error.HTTPError as exc:
        return exc.code
    except (urllib.error.URLError, ConnectionResetError,
            TimeoutError, OSError, http.client.HTTPException):
        return 0


def download_sha256(url):
    hasher = hashlib.sha256()
    with urllib.request.urlopen(url, timeout=300) as resp:
        while True:
            chunk = resp.read(1 << 16)
            if not chunk:
                break
            hasher.update(chunk)
    return hasher.hexdigest()


def resolve_release(token, tag):
    path = "/repos/%s/releases/latest" % REPO
    if tag:
        path = "/repos/%s/releases/tags/%s" % (REPO, tag)
    release = api(token, "GET", path)
    rtag = release["tag_name"]
    version = re.sub(r"^v", "", rtag)
    asset = next((a for a in release.get("assets", [])
                  if X64_ASSET_RE.match(a["name"])), None)
    if not asset:
        fail("no x64 zip asset in release %s (found: %s)"
             % (rtag, [a["name"] for a in release.get("assets", [])]))
    digest = asset.get("digest") or ""
    if digest.startswith("sha256:"):
        sha256 = digest[len("sha256:"):].lower()
    else:
        log("no digest on asset, downloading to compute SHA256 ...")
        sha256 = download_sha256(asset["browser_download_url"])
    return {
        "tag": rtag,
        "version": version,
        "zip_name": asset["name"],
        "url": asset["browser_download_url"],
        "sha256": sha256,
    }


def render_manifests(rel):
    base = "https://github.com/%s" % REPO
    v, tag, url, sha = rel["version"], rel["tag"], rel["url"], rel["sha256"]
    prefix = "manifests/l/linsmod/aidbg/%s" % v
    desc = ("aidbg is a single-file C++17 Windows native debugger based on "
            "TitanEngine. It exposes a GDB-aligned command set (run / start / "
            "continue / stepi / nexti / finish / bt / break / hbreak / mbreak / "
            "watch / condition / ignore / registers / set / x / dump / disas / "
            "search / strings / info / attach / detach / thread) plus a JSON "
            "Lines machine interface (--json) for AI coding assistants, CI "
            "automation and scripted debugging. It supports software, hardware "
            "and memory breakpoints, dll!api breakpoints, deep PDB symbol "
            "support with source/PDB checksum validation, exception "
            "interception and multi-thread debugging.")

    files = {}
    files["%s/%s.yaml" % (prefix, PACKAGE_ID)] = (
        "PackageIdentifier: %s\n"
        "PackageVersion: %s\n"
        "DefaultLocale: en-US\n"
        "ManifestType: version\n"
        "ManifestVersion: %s\n"
    ) % (PACKAGE_ID, v, MANIFEST_VERSION)

    files["%s/%s.installer.yaml" % (prefix, PACKAGE_ID)] = (
        "PackageIdentifier: %s\n"
        "PackageVersion: %s\n"
        "Commands:\n"
        "  - aidbg\n"
        "Installers:\n"
        "  - Architecture: x64\n"
        "    InstallerType: zip\n"
        "    InstallerUrl: %s\n"
        "    InstallerSha256: %s\n"
        "    NestedInstallerType: portable\n"
        "    NestedInstallerFiles:\n"
        "      - RelativeFilePath: aidbg.exe\n"
        "        PortableCommandAlias: aidbg\n"
        "ManifestType: installer\n"
        "ManifestVersion: %s\n"
    ) % (PACKAGE_ID, v, url, sha.upper(), MANIFEST_VERSION)

    files["%s/%s.locale.en-US.yaml" % (prefix, PACKAGE_ID)] = (
        "PackageIdentifier: %s\n"
        "PackageVersion: %s\n"
        "PackageLocale: en-US\n"
        "Publisher: linsmod\n"
        "PublisherUrl: %s\n"
        "PublisherSupportUrl: %s/issues\n"
        "PackageName: aidbg\n"
        "PackageUrl: %s\n"
        "License: GNU Lesser General Public License v3.0\n"
        "LicenseUrl: https://www.gnu.org/licenses/lgpl-3.0.html\n"
        "ShortDescription: GDB-style Windows command-line debugger built for "
        "AI agents and automation.\n"
        "Description: >-\n"
        "  %s\n"
        "Moniker: aidbg\n"
        "Tags:\n"
        "  - ai\n"
        "  - automation\n"
        "  - cli\n"
        "  - debugger\n"
        "  - gdb\n"
        "  - json\n"
        "  - windows\n"
        "ManifestType: defaultLocale\n"
        "ManifestVersion: %s\n"
    ) % (PACKAGE_ID, v, base, base, base, desc, MANIFEST_VERSION)
    return files


def ensure_fork(token, fork_owner):
    fork = "%s/%s" % (fork_owner, WINGET_PKGS_FORK)
    if repo_exists(token, fork):
        log("fork %s already exists" % fork)
        return fork
    log("creating fork %s (winget-pkgs is large, this may take a while) ..."
        % fork)
    api(token, "POST", "/repos/%s/forks" % UPSTREAM)
    for _ in range(60):
        time.sleep(5)
        if repo_exists(token, fork):
            log("fork ready: %s" % fork)
            return fork
    fail("fork %s did not become available in time" % fork)


def sync_fork_master(token, fork, upstream_master):
    try:
        current = api(token, "GET", "/repos/%s/git/ref/heads/master" % fork)[
            "object"]["sha"]
    except ApiError as exc:
        if exc.code == 404:
            log("no master ref on fork yet; will be created by the first commit")
            return
        raise
    if current != upstream_master:
        log("syncing %s master to upstream (%s)" % (fork, upstream_master[:8]))
        api(token, "PATCH", "/repos/%s/git/refs/heads/master" % fork,
            {"sha": upstream_master, "force": True})
    else:
        log("fork master already up to date")


def create_branch_commit(token, fork, branch, version, master_sha, master_tree,
                         files):
    try:
        api(token, "DELETE", "/repos/%s/git/refs/heads/%s" % (fork, branch))
        log("removed stale branch %s" % branch)
    except ApiError as exc:
        if exc.code != 422:
            raise

    blobs = {}
    for path, content in files.items():
        payload = {
            "content": base64.b64encode(content.encode("utf-8")).decode(),
            "encoding": "base64",
        }
        blobs[path] = api(token, "POST", "/repos/%s/git/blobs" % fork,
                          payload)["sha"]

    tree = {
        "base_tree": master_tree,
        "tree": [
            {"path": path, "mode": "100644", "type": "blob", "sha": blobs[path]}
            for path in files
        ],
    }
    tree_sha = api(token, "POST", "/repos/%s/git/trees" % fork, tree)["sha"]

    commit = {
        "message": "New version: %s version %s" % (PACKAGE_ID, version),
        "tree": tree_sha,
        "parents": [master_sha],
    }
    commit_sha = api(token, "POST", "/repos/%s/git/commits" % fork, commit)[
        "sha"]

    api(token, "POST", "/repos/%s/git/refs" % fork,
        {"ref": "refs/heads/%s" % branch, "sha": commit_sha})
    return commit_sha


def pr_body(tag):
    return """- [x] Have you signed the Contributor License Agreement?
- [x] Have you checked that there aren't other open pull requests for the same manifest update/change?
- [x] This PR only modifies a single manifest

###### Validation Steps Performed

- Verified the release asset `aidbg-x64-%s.zip` (aidbg.exe + TitanEngine.dll) at
  https://github.com/%s/releases/tag/%s
- Installer type: zip with a nested portable installer; the `aidbg` command is
  exposed via PortableCommandAlias so it lands on PATH (WinGet Links dir).
""" % (tag, REPO, tag)


def open_or_find_pr(token, fork_owner, branch, title, body):
    pulls = api(token, "GET",
                "/repos/%s/pulls?state=open&head=%s:%s&per_page=10"
                % (UPSTREAM, fork_owner, branch))
    if pulls:
        return pulls[0]["html_url"]
    pr = api(token, "POST", "/repos/%s/pulls" % UPSTREAM, {
        "title": title,
        "head": "%s:%s" % (fork_owner, branch),
        "base": "master",
        "body": body,
    })
    return pr["html_url"]


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tag", nargs="?", default=None,
                    help="release tag (default: latest release)")
    ap.add_argument("--dry-run", action="store_true",
                    help="render manifests and exit without any API writes")
    ap.add_argument("--no-pr", action="store_true",
                    help="push the branch but do not open a PR")
    ap.add_argument("--fork-owner", default=None,
                    help="account that owns the winget-pkgs fork "
                         "(default: authenticated user)")
    args = ap.parse_args()

    token = get_token()
    me = api(token, "GET", "/user")["login"]
    fork_owner = args.fork_owner or me

    rel = resolve_release(token, args.tag)
    version, tag = rel["version"], rel["tag"]
    branch = "aidbg-%s" % version
    log("release %s -> version %s" % (tag, version))
    log("asset %s sha256=%s" % (rel["zip_name"], rel["sha256"]))

    files = render_manifests(rel)
    if args.dry_run:
        for path, content in files.items():
            print("===== %s =====" % path)
            print(content, end="")
        return 0

    # refuse if this version is already published upstream
    probe = "manifests/l/linsmod/aidbg/%s/%s.yaml" % (version, PACKAGE_ID)
    status = raw_status(UPSTREAM, "master", probe)
    if status == 200:
        fail("version %s already exists in %s" % (version, UPSTREAM))
    if status == 0:
        log("warning: could not check upstream for an existing version, "
            "continuing")

    fork = ensure_fork(token, fork_owner)

    upstream_master = api(token, "GET",
                          "/repos/%s/git/ref/heads/master" % UPSTREAM)[
        "object"]["sha"]
    upstream_tree = api(token, "GET",
                        "/repos/%s/git/commits/%s" % (UPSTREAM, upstream_master))[
        "tree"]["sha"]
    sync_fork_master(token, fork, upstream_master)

    commit_sha = create_branch_commit(
        token, fork, branch, version, upstream_master, upstream_tree, files)
    log("branch %s pushed (commit %s)" % (branch, commit_sha[:8]))

    # verification: raw file visible on the fork branch
    probe = list(files)[0]
    if raw_status(fork, branch, probe) != 200:
        fail("verification failed: %s not visible on %s" % (probe, fork))
    log("verification OK: %s visible on %s" % (probe, fork))

    if args.no_pr:
        log("branch pushed; PR not opened (--no-pr)")
        return 0

    url = open_or_find_pr(
        token, fork_owner, branch,
        "New version: %s version %s" % (PACKAGE_ID, version),
        pr_body(tag))
    log("PR: %s" % url)
    return 0


if __name__ == "__main__":
    sys.exit(main())
