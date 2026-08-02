#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make_zip.py <version> - create aidbg-x64-v<version>.zip from build/bin.

The archive has root-level entries (aidbg.exe + TitanEngine.dll) matching the
existing release convention.

Usage:
    python make_zip.py 0.1.1     # writes aidbg-x64-v0.1.1.zip in the repo root
"""

import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD_BIN = os.path.join(HERE, "build", "bin")
VERSION_RE = re.compile(r"^v?\d+\.\d+\.\d+$")


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: python make_zip.py 0.1.1")
    version = sys.argv[1]
    if not VERSION_RE.match(version):
        sys.exit("bad version: %s" % version)
    tag = version if version.startswith("v") else "v" + version
    out = os.path.join(HERE, "aidbg-x64-%s.zip" % tag)

    files = [os.path.join(BUILD_BIN, "aidbg.exe"),
             os.path.join(BUILD_BIN, "TitanEngine.dll")]
    for f in files:
        if not os.path.isfile(f):
            sys.exit("missing %s" % f)

    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(f, arcname=os.path.basename(f))
    print("[make_zip] wrote %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
