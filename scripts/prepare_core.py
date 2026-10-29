#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Patch an ignored build copy, never the tracked upstream checkout."""
import hashlib
import pathlib
import subprocess
import tempfile

from source_inputs import ArchiveError, SourceInputs


def prepare(root):
    root = pathlib.Path(root).resolve()
    inputs = SourceInputs(root)
    inputs.verify()
    source = root / "third_party/mupen64plus-core"
    target = root / ".build/sources/mupen64plus-core"
    patch = root / "patches/mupen64plus-core/0001-interpreter-host-poll.patch"
    pin = inputs.pin("mupen64plus-core")
    stamp = pin + ":" + hashlib.sha256(patch.read_bytes()).hexdigest()
    marker = target / ".aurum-patch-stamp"
    if marker.is_file() and marker.read_text() == stamp:
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".core-prepare-", dir=target.parent) as temporary:
        staging = pathlib.Path(temporary) / "source"
        staging.mkdir()
        if inputs.manifest is not None:
            inputs.copy_module("mupen64plus-core", staging)
        else:
            with tempfile.TemporaryFile() as archive:
                subprocess.run(["git", "--no-replace-objects", "-C", str(source), "archive", pin],
                               stdout=archive, check=True)
                archive.seek(0)
                subprocess.run(["tar", "-x", "-C", str(staging)], stdin=archive, check=True)
        subprocess.run(["git", "apply", "--no-index", "--check", str(patch)], cwd=staging, check=True)
        subprocess.run(["git", "apply", "--no-index", str(patch)], cwd=staging, check=True)
        (staging / ".aurum-patch-stamp").write_text(stamp)
        # A failed patch must leave the last usable build copy intact.
        backup = pathlib.Path(temporary) / "previous"
        if target.exists():
            target.rename(backup)
        try:
            staging.rename(target)
        except OSError:
            if backup.exists():
                backup.rename(target)
            raise
    return target


if __name__ == "__main__":
    try:
        print(prepare(pathlib.Path(__file__).resolve().parent.parent))
    except (ArchiveError, OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Core preparation failed: {error}")
