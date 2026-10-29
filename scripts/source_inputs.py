#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Resolve build provenance from a checkout or a verified source snapshot."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess

from source_archive import ArchiveError, MANIFEST, relative_path, validate_archive_paths


MODULES = ("mupen64plus-core", "mupen64plus-rsp-cxd4", "angrylion-rdp-plus")
GENERATED_ROOTS = {".build", "build", "dist"}


def git(root, *args, allowed_codes=(0,)):
    result = subprocess.run(["git", "--no-replace-objects", "-C", str(root), *args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=dict(os.environ, GIT_OPTIONAL_LOCKS="0"))
    if result.returncode not in allowed_codes:
        raise ArchiveError(result.stderr.decode("utf-8", errors="replace").strip()
                           or "Cannot inspect source checkout")
    return result.stdout.decode("utf-8").strip()


def hex_digest(value, lengths):
    return isinstance(value, str) and len(value) in lengths and re.fullmatch("[0-9a-f]+", value)


class SourceInputs:
    def __init__(self, root):
        self.root = Path(root).resolve()
        self.files = {}
        self.pins = {}
        self.manifest = None
        # Do not let Git discover a parent checkout around an extracted snapshot.
        if (self.root / ".git").exists():
            self.commit = git(self.root, "rev-parse", "HEAD")
            for record in git(self.root, "ls-tree", "-r", self.commit).splitlines():
                metadata, path = record.split("\t", 1)
                mode, kind, pin = metadata.split()
                if mode == "160000" and kind == "commit":
                    self.pins[path] = pin
        else:
            self._load_manifest()

    def _load_manifest(self):
        path = self.root / MANIFEST
        if not path.is_file() or path.is_symlink():
            raise ArchiveError(f"Missing {MANIFEST}; use an initialized checkout or a source snapshot")
        if path.stat().st_size > 16 * 1024 * 1024:
            raise ArchiveError("Source manifest exceeds the 16 MiB limit")
        raw = path.read_bytes()
        try:
            manifest = json.loads(raw)
        except (UnicodeError, ValueError) as error:
            raise ArchiveError("Invalid source manifest JSON") from error
        if (not isinstance(manifest, dict) or manifest.get("formatVersion") != 1
                or manifest.get("kind") != "source-snapshot"
                or not hex_digest(manifest.get("commit"), (40, 64))
                or not isinstance(manifest.get("files"), list)
                or not isinstance(manifest.get("submodules"), list)):
            raise ArchiveError("Unsupported or incomplete source manifest")
        for entry in manifest["files"]:
            if (not isinstance(entry, dict) or not isinstance(entry.get("path"), str)
                    or entry.get("mode") not in ("100644", "100755", "120000")
                    or not hex_digest(entry.get("gitObject"), (40, 64))
                    or not hex_digest(entry.get("sha256"), (64,))):
                raise ArchiveError("Invalid file record in source manifest")
            name = relative_path(entry["path"])
            if name in self.files or name.split("/")[0] in GENERATED_ROOTS:
                raise ArchiveError(f"Duplicate or generated manifest path: {name}")
            self.files[name] = entry
        validate_archive_paths([*self.files, MANIFEST])
        for module in manifest["submodules"]:
            if (not isinstance(module, dict) or not isinstance(module.get("path"), str)
                    or not hex_digest(module.get("commit"), (40, 64))):
                raise ArchiveError("Invalid submodule record in source manifest")
            name = relative_path(module["path"])
            if name in self.pins:
                raise ArchiveError(f"Duplicate submodule record: {name}")
            self.pins[name] = module["commit"]
        self.commit = manifest["commit"]
        self.manifest = manifest
        self.manifest_sha256 = hashlib.sha256(raw).hexdigest()

    def pin(self, name):
        path = "third_party/" + relative_path(name)
        if path not in self.pins:
            raise ArchiveError(f"Missing source pin: {path}")
        if self.manifest is not None and not any(p.startswith(path + "/") for p in self.files):
            raise ArchiveError(f"Missing source files for pinned module: {path}")
        if self.manifest is None:
            source = self.root / path
            if not (source / ".git").exists():
                raise ArchiveError(f"Uninitialized submodule: {path}; run scripts/bootstrap.sh")
            if git(source, "rev-parse", "HEAD") != self.pins[path]:
                raise ArchiveError(f"Submodule does not match the committed pin: {path}; use a clean checkout")
            if git(source, "status", "--porcelain", "--untracked-files=all"):
                raise ArchiveError(f"Submodule has tracked changes or untracked files: {path}; "
                                   "use a clean checkout")
        return self.pins[path]

    def _read_file(self, name):
        path = self.root / name
        for parent in path.relative_to(self.root).parents:
            if (self.root / parent).is_symlink():
                raise ArchiveError(f"Source directory is a symlink: {parent}")
        entry = self.files[name]
        try:
            mode = path.lstat().st_mode
            if entry["mode"] == "120000":
                if not stat.S_ISLNK(mode):
                    raise ArchiveError(f"Expected source symlink: {name}")
                target = os.readlink(path)
                if (not target or target.startswith("/") or "\\" in target
                        or ".." in target.split("/") or any(ord(c) < 32 for c in target)):
                    raise ArchiveError(f"Unsafe source symlink: {name}")
                data = target.encode("utf-8")
            else:
                if not stat.S_ISREG(mode):
                    raise ArchiveError(f"Expected regular source file: {name}")
                if bool(mode & 0o111) != (entry["mode"] == "100755"):
                    raise ArchiveError(f"Source executable mode differs: {name}")
                data = path.read_bytes()
        except FileNotFoundError as error:
            raise ArchiveError(f"Missing source file: {name}") from error
        if hashlib.sha256(data).hexdigest() != entry["sha256"]:
            raise ArchiveError(f"Source checksum differs: {name}; extract a fresh snapshot")
        blob = b"blob " + str(len(data)).encode("ascii") + b"\0" + data
        digest = hashlib.sha1(blob) if len(entry["gitObject"]) == 40 else hashlib.sha256(blob)
        if digest.hexdigest() != entry["gitObject"]:
            raise ArchiveError(f"Source Git object differs: {name}")
        return data

    def verify(self):
        for name in MODULES:
            self.pin(name)
        if self.manifest is None:
            return
        for name in self.files:
            self._read_file(name)
        # Unlisted sources could enter wildcard-based upstream builds. Only the
        # build output roots and Python/Finder metadata may appear after export.
        for parent, directories, filenames in os.walk(self.root, followlinks=False):
            directory = Path(parent)
            for name in list(directories):
                path = directory / name
                if path.is_symlink():
                    filenames.append(name)
                    directories.remove(name)
                elif name == "__pycache__" or (directory == self.root and name in GENERATED_ROOTS):
                    directories.remove(name)
            for name in filenames:
                path = (directory / name).relative_to(self.root).as_posix()
                if path not in self.files and path != MANIFEST and name != ".DS_Store":
                    raise ArchiveError(f"Unlisted source file: {path}; extract a fresh snapshot")

    def copy_module(self, name, destination):
        """Copy only verified snapshot entries; never include local build outputs."""
        if self.manifest is None:
            raise ArchiveError("Snapshot copy requires a source manifest")
        self.pin(name)
        prefix = f"third_party/{name}/"
        for path, entry in self.files.items():
            if not path.startswith(prefix):
                continue
            data = self._read_file(path)
            target = Path(destination) / path[len(prefix):]
            target.parent.mkdir(parents=True, exist_ok=True)
            if entry["mode"] == "120000":
                target.symlink_to(data.decode("utf-8"))
            else:
                target.write_bytes(data)
                target.chmod(0o755 if entry["mode"] == "100755" else 0o644)

    def provenance(self):
        if self.manifest is not None:
            return {"kind": "source-snapshot", "commit": self.commit,
                    "manifestSHA256": self.manifest_sha256}
        return {"kind": "git-checkout", "commit": self.commit,
                "checkoutModified": bool(git(self.root, "status", "--porcelain", "--untracked-files=all"))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("command", choices=("verify", "pin"))
    parser.add_argument("module", nargs="?", choices=MODULES)
    args = parser.parse_args()
    if args.command == "pin" and args.module is None:
        parser.error("pin requires a module name")
    try:
        inputs = SourceInputs(args.root)
        inputs.verify()
        if args.command == "pin":
            print(inputs.pin(args.module))
        else:
            print(f"Verified source inputs: {inputs.provenance()['kind']} {inputs.commit}")
    except (ArchiveError, OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"Source verification failed: {error}\n")


if __name__ == "__main__":
    main()
