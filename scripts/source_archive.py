#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export a deterministic source snapshot from local Git objects, never checkouts."""
import argparse
from dataclasses import dataclass
import gzip
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile


MANIFEST = "SOURCE_MANIFEST.json"
PRIVATE_ROOTS = {"privatedocs", ".build", "build", "dist", ".codex", ".aws"}
PRIVATE_SUFFIXES = {".z64", ".v64", ".n64", ".sav", ".aurcap", ".pyc", ".rdp"}


class ArchiveError(Exception):
    pass


def git(*args, allowed_codes=(0,)):
    result = subprocess.run(["git", "--no-replace-objects", *map(str, args)], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if result.returncode not in allowed_codes:
        raise ArchiveError(result.stderr.decode("utf-8", errors="replace").strip()
                           or "Git command failed")
    return result.stdout


def relative_path(value):
    parts = value.split("/")
    if (not value or any(part in {"", ".", "..", ".git"} for part in parts)
            or "\\" in value or any(ord(c) < 32 or ord(c) == 127 for c in value)):
        raise ArchiveError(f"Unsafe source path: {value!r}")
    return value


@dataclass(frozen=True)
class Repository:
    git_dir: Path

    def run(self, *args, **kwargs):
        return git(f"--git-dir={self.git_dir}", *args, **kwargs)

    def common_dir(self):
        return Path(self.run("rev-parse", "--path-format=absolute", "--git-common-dir")
                    .decode().strip())

    def require_full_clone(self):
        promisor = self.run("config", "--get-regexp", r"^remote\..*\.promisor$",
                            allowed_codes=(0, 1))
        partial = self.run("config", "--get", "extensions.partialclone", allowed_codes=(0, 1))
        if promisor or partial:
            raise ArchiveError("Partial clones are not supported; use a full local clone "
                               "to avoid implicit network fetches")


@dataclass(frozen=True)
class Entry:
    repository: Repository
    path: str
    mode: str
    object_id: str


def collect(repository, commit, prefix="", ancestors=()):
    repository.require_full_clone()
    identity = (repository.git_dir.resolve(), commit)
    if identity in ancestors or len(ancestors) >= 32:
        raise ArchiveError("Recursive submodule graph exceeds the supported depth")
    entries, modules = [], []
    tree = repository.run("ls-tree", "-rz", "--full-tree", commit)
    records = []
    for record in tree.split(b"\0"):
        if not record:
            continue
        metadata, raw_path = record.split(b"\t", 1)
        mode, kind, object_id = metadata.decode("ascii").split()
        path = relative_path(raw_path.decode("utf-8"))
        parts = PurePosixPath(path).parts
        if (not prefix and (parts[0].lower() in PRIVATE_ROOTS or path == MANIFEST)):
            raise ArchiveError(f"Private/generated/reserved path is tracked: {path}")
        suffix = PurePosixPath(path).suffix.lower()
        if (suffix in PRIVATE_SUFFIXES or (suffix.startswith(".st") and suffix[3:].isdigit())
                or any(p.lower() in {".ds_store", "__pycache__", ".env"}
                       or p.lower().startswith(".env.") for p in parts)):
            raise ArchiveError(f"Private/generated path is tracked: {prefix}{path}")
        records.append((mode, kind, object_id, path))

    names = {}
    if any(mode == "160000" for mode, _, _, _ in records):
        config = repository.run("config", "--null", "--blob", f"{commit}:.gitmodules",
                                "--get-regexp", r"^submodule\..*\.path$",
                                allowed_codes=(0, 1))
        for record in config.split(b"\0"):
            if not record:
                continue
            key, path = record.decode("utf-8").split("\n", 1)
            name = relative_path(key[len("submodule."):-len(".path")])
            path = relative_path(path)
            if path in names:
                raise ArchiveError(f"Duplicate submodule path: {prefix}{path}")
            names[path] = name

    for mode, kind, object_id, path in records:
        if mode == "160000":
            if path not in names:
                raise ArchiveError(f"Missing .gitmodules entry: {prefix}{path}")
            locations = [repository.git_dir / "modules" / names[path],
                         repository.common_dir() / "modules" / names[path]]
            local_dir = next((p for p in locations if p.is_dir()), None)
            if local_dir is None:
                raise ArchiveError(f"Missing local submodule: {prefix}{path}; "
                                   "initialize submodules in a separate checkout first")
            child = Repository(local_dir)
            child.require_full_clone()
            try:
                child.run("cat-file", "-e", f"{object_id}^{{commit}}")
            except ArchiveError as error:
                raise ArchiveError(f"Missing pinned commit {object_id} for {prefix}{path}") from error
            child_entries, child_modules = collect(
                child, object_id, f"{prefix}{path}/", (*ancestors, identity))
            entries.extend(child_entries)
            modules.append({"path": prefix + path, "commit": object_id})
            modules.extend(child_modules)
        elif kind == "blob" and mode in {"100644", "100755", "120000"}:
            entries.append(Entry(repository, prefix + path, mode, object_id))
        else:
            raise ArchiveError(f"Unsupported Git entry: {prefix}{path} ({mode} {kind})")
    return entries, modules


class BlobReader:
    """One cat-file process per repository; no filters or export substitutions."""

    def __init__(self, repository):
        self.process = subprocess.Popen(
            ["git", "--no-replace-objects", f"--git-dir={repository.git_dir}", "cat-file", "--batch"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def read(self, object_id):
        self.process.stdin.write(object_id.encode("ascii") + b"\n")
        self.process.stdin.flush()
        header = self.process.stdout.readline().split()
        if len(header) != 3 or header[:2] != [object_id.encode("ascii"), b"blob"]:
            raise ArchiveError(f"Cannot read blob {object_id}")
        size = int(header[2])
        data = self.process.stdout.read(size)
        if len(data) != size or self.process.stdout.read(1) != b"\n":
            raise ArchiveError(f"Truncated blob {object_id}")
        return data

    def close(self):
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.wait()


def add_member(archive, path, data, timestamp, mode="100644"):
    member = tarfile.TarInfo(path)
    member.mtime = timestamp
    member.mode = 0o755 if mode == "100755" else 0o644
    if mode == "120000":
        target = data.decode("utf-8")
        # Reject parent traversal even when lexical normalization looks safe:
        # another symlink in the target may change what '..' resolves to.
        if (not target or PurePosixPath(target).is_absolute() or "\\" in target
                or ".." in target.split("/") or any(ord(c) < 32 for c in target)):
            raise ArchiveError(f"Unsafe symlink: {path}")
        member.type = tarfile.SYMTYPE
        member.mode = 0o777
        member.linkname = target
        archive.addfile(member)
    else:
        member.size = len(data)
        archive.addfile(member, io.BytesIO(data))


def create_archive(root, revision, output):
    root, output = Path(root).resolve(), Path(output).absolute()
    if output.exists() or output.is_symlink():
        raise ArchiveError(f"Refusing to overwrite: {output}; choose a new output path")
    repository = Repository(Path(git("-C", root, "rev-parse", "--absolute-git-dir")
                                 .decode().strip()))
    repository.require_full_clone()
    commit = repository.run("rev-parse", "--verify", "--end-of-options",
                            f"{revision}^{{commit}}").decode().strip()
    timestamp = int(repository.run("show", "-s", "--format=%ct", commit))
    entries, modules = collect(repository, commit)
    prefix = f"Aurum64-{commit[:12]}-source"
    manifest = {"formatVersion": 1, "kind": "source-snapshot", "commit": commit,
                "commitTimestamp": timestamp, "submodules": sorted(modules, key=lambda m: m["path"]),
                "files": []}
    output.parent.mkdir(parents=True, exist_ok=True)
    readers = {}
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=".aurum-source-",
                                         delete=False) as raw:
            temporary = Path(raw.name)
            with gzip.GzipFile(fileobj=raw, mode="wb", filename="", mtime=0) as compressed:
                with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as archive:
                    for entry in sorted(entries, key=lambda e: e.path):
                        if entry.repository not in readers:
                            readers[entry.repository] = BlobReader(entry.repository)
                        data = readers[entry.repository].read(entry.object_id)
                        add_member(archive, f"{prefix}/{entry.path}", data, timestamp, entry.mode)
                        manifest["files"].append({"path": entry.path, "mode": entry.mode,
                                                  "gitObject": entry.object_id,
                                                  "sha256": hashlib.sha256(data).hexdigest()})
                    data = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8")
                    add_member(archive, f"{prefix}/{MANIFEST}", data, timestamp)
        # Atomic, exclusive publication: a failed run never replaces an archive.
        temporary.chmod(0o644)
        os.link(temporary, output)
    finally:
        for reader in readers.values():
            reader.close()
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    digest = hashlib.sha256()
    with output.open("rb") as archive:
        for block in iter(lambda: archive.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--ref", default="HEAD", help="Committed revision to export (default: HEAD)")
    parser.add_argument("--output", type=Path, required=True, help="New .tar.gz file; never overwritten")
    args = parser.parse_args()
    try:
        digest = create_archive(args.repo, args.ref, args.output)
    except (ArchiveError, OSError, UnicodeError, ValueError) as error:
        parser.exit(1, f"Source archive failed: {error}\n")
    print(f"{digest}  {args.output}")


if __name__ == "__main__":
    main()
