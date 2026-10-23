#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Patch an ignored build copy, never the tracked upstream checkout."""
import hashlib
import pathlib
import shutil
import subprocess

root = pathlib.Path(__file__).resolve().parent.parent
source = root / "third_party/mupen64plus-core"
target = root / ".build/sources/mupen64plus-core"
patch = root / "patches/mupen64plus-core/0001-interpreter-host-poll.patch"
pin = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
stamp = pin + ":" + hashlib.sha256(patch.read_bytes()).hexdigest()
marker = target / ".aurum-patch-stamp"
if not marker.exists() or marker.read_text() != stamp:
    if target.exists():
        shutil.rmtree(target)  # Only our generated source copy, never the upstream submodule.
    target.mkdir(parents=True)
    archive = subprocess.Popen(["git", "-C", str(source), "archive", pin], stdout=subprocess.PIPE)
    subprocess.run(["tar", "-x", "-C", str(target)], stdin=archive.stdout, check=True)
    archive.stdout.close()
    if archive.wait() != 0:
        raise RuntimeError("Cannot export pinned core source")
    subprocess.run(["git", "apply", "--check", str(patch)], cwd=target, check=True)
    subprocess.run(["git", "apply", str(patch)], cwd=target, check=True)
    marker.write_text(stamp)
print(target)
