#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline source-packaging tests; no emulator build, Metal, or external data."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/source_archive.py"
spec = importlib.util.spec_from_file_location("source_archive", SCRIPT)
source_archive = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = source_archive
spec.loader.exec_module(source_archive)


def git(root, *args):
    environment = dict(os.environ, GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull,
                       GIT_AUTHOR_DATE="2026-01-01T00:00:00+00:00",
                       GIT_COMMITTER_DATE="2026-01-01T00:00:00+00:00",
                       GIT_AUTHOR_NAME="Archive Test", GIT_AUTHOR_EMAIL="test@example.invalid",
                       GIT_COMMITTER_NAME="Archive Test", GIT_COMMITTER_EMAIL="test@example.invalid")
    return subprocess.check_output(
        ["git", "-C", str(root), *args], stderr=subprocess.PIPE, env=environment,
        text=True).strip()


def commit(root, message="fixture"):
    git(root, "add", "--all")
    git(root, "-c", "commit.gpgsign=false", "commit", "-m", message)
    return git(root, "rev-parse", "HEAD")


class SourceArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="aurum-source-test-")
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "repository"
        self.root.mkdir()
        git(self.root, "init", "--quiet")
        (self.root / "LICENSE").write_text("Original test license\n")
        (self.root / "source.txt").write_text("Committed source\n")
        (self.root / ".gitignore").write_text("PrivateDocs/\n.build/\ndist/\n*.z64\n")
        self.head = commit(self.root)

    def archive(self, filename="source.tar.gz", root=None, revision="HEAD"):
        output = self.base / filename
        digest = source_archive.create_archive(root or self.root, revision, output)
        self.assertEqual(digest, hashlib.sha256(output.read_bytes()).hexdigest())
        return output

    def contents(self, output):
        with tarfile.open(output, "r:gz") as archive:
            prefix = archive.getnames()[0].split("/")[0] + "/"
            return {member.name.removeprefix(prefix): archive.extractfile(member).read()
                    for member in archive.getmembers() if member.isfile()}

    def add_submodule(self, parent=None, name="dependency"):
        upstream = self.base / name
        upstream.mkdir()
        git(upstream, "init", "--quiet")
        (upstream / "COPYING").write_text(f"{name} license\n")
        (upstream / "dependency.c").write_text("/* pinned upstream */\n")
        pin = commit(upstream)
        parent = parent or self.root
        path = f"third_party/{name}"
        git(parent, "-c", "protocol.file.allow=always", "submodule", "add", "--quiet",
            "--name", f"vendor-{name}", str(upstream), path)
        commit(parent, f"pin {name}")
        return parent / path, pin

    def test_deterministic_ignores_checkout_and_index_contents(self):
        (self.root / "source.txt").write_text("Staged source, not committed\n")
        git(self.root, "add", "source.txt")
        (self.root / "source.txt").write_text("Unstaged source\n")
        (self.root / "untracked.txt").write_text("Not committed\n")
        for path in ("PrivateDocs/plan.md", ".build/memory.bin", "dist/app", "game.z64"):
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("Synthetic private fixture\n")
        before = git(self.root, "status", "--porcelain=v1", "--untracked-files=all")
        first = self.archive()
        second = self.archive("different-name.tar.gz")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        files = self.contents(first)
        self.assertEqual(files["source.txt"], b"Committed source\n")
        self.assertEqual(set(files), {"LICENSE", "source.txt", ".gitignore", "SOURCE_MANIFEST.json"})
        self.assertEqual(before, git(self.root, "status", "--porcelain=v1", "--untracked-files=all"))

    def test_manifest_hashes_and_normalized_metadata(self):
        output = self.archive()
        files = self.contents(output)
        manifest = json.loads(files["SOURCE_MANIFEST.json"])
        self.assertEqual(manifest["commit"], self.head)
        self.assertEqual(manifest["submodules"], [])
        self.assertNotIn(str(self.base), files["SOURCE_MANIFEST.json"].decode())
        self.assertEqual(len(manifest["files"]), len(files) - 1)
        for entry in manifest["files"]:
            self.assertEqual(entry["sha256"], hashlib.sha256(files[entry["path"]]).hexdigest())
            self.assertEqual(entry["gitObject"], git(self.root, "rev-parse", f"HEAD:{entry['path']}"))
        with tarfile.open(output) as archive:
            for member in archive:
                self.assertEqual((member.uid, member.gid, member.uname, member.gname), (0, 0, "", ""))
                self.assertEqual(member.mtime, manifest["commitTimestamp"])

    def test_uses_pinned_submodule_objects_not_dirty_or_advanced_head(self):
        child, pin = self.add_submodule()
        (child / "dependency.c").write_text("/* newer commit */\n")
        commit(child, "advance checkout without changing parent pin")
        (child / "COPYING").write_text("Dirty license\n")
        (child / "game.z64").write_bytes(b"synthetic ignored cartridge")
        child_head = git(child, "rev-parse", "HEAD")
        files = self.contents(self.archive())
        self.assertEqual(files["third_party/dependency/dependency.c"], b"/* pinned upstream */\n")
        self.assertEqual(files["third_party/dependency/COPYING"], b"dependency license\n")
        self.assertNotIn("third_party/dependency/game.z64", files)
        self.assertEqual(json.loads(files["SOURCE_MANIFEST.json"])["submodules"],
                         [{"path": "third_party/dependency", "commit": pin}])
        self.assertEqual(git(child, "rev-parse", "HEAD"), child_head)
        self.assertEqual((child / "COPYING").read_text(), "Dirty license\n")

    def test_nested_submodules_and_linked_worktree_without_checkouts(self):
        child, _ = self.add_submodule()
        _, nested_pin = self.add_submodule(parent=child, name="nested")
        child_pin = git(child, "rev-parse", "HEAD")
        commit(self.root, "pin nested tree")
        worktree = self.base / "linked worktree"
        git(self.root, "worktree", "add", "--quiet", "--detach", str(worktree), "HEAD")
        self.assertFalse((worktree / "third_party/dependency/.git").exists())
        files = self.contents(self.archive(root=worktree))
        self.assertEqual(files["third_party/dependency/third_party/nested/COPYING"], b"nested license\n")
        self.assertEqual(json.loads(files["SOURCE_MANIFEST.json"])["submodules"],
                         [{"path": "third_party/dependency", "commit": child_pin},
                          {"path": "third_party/dependency/third_party/nested", "commit": nested_pin}])
        self.assertEqual(self.archive("primary.tar.gz").read_bytes(),
                         (self.base / "source.tar.gz").read_bytes())

    def test_preserves_executable_and_safe_symlink(self):
        (self.root / "run me.sh").write_text("#!/bin/sh\necho original\n")
        (self.root / "run me.sh").chmod(0o755)
        (self.root / "source-link").symlink_to("source.txt")
        commit(self.root)
        with tarfile.open(self.archive()) as archive:
            members = {m.name.split("/", 1)[1]: m for m in archive}
            self.assertEqual(members["run me.sh"].mode, 0o755)
            self.assertTrue(members["source-link"].issym())
            self.assertEqual(members["source-link"].linkname, "source.txt")

    def test_does_not_honor_export_ignore_or_export_subst(self):
        (self.root / ".gitattributes").write_text("LICENSE export-ignore\nsource.txt export-subst\n")
        (self.root / "source.txt").write_text("$Format:%H$\n")
        commit(self.root)
        files = self.contents(self.archive())
        self.assertIn("LICENSE", files)
        self.assertEqual(files["source.txt"], b"$Format:%H$\n")

    def test_explicit_revision(self):
        (self.root / "source.txt").write_text("Later revision\n")
        commit(self.root)
        files = self.contents(self.archive(revision=self.head))
        self.assertEqual(files["source.txt"], b"Committed source\n")
        self.assertEqual(json.loads(files["SOURCE_MANIFEST.json"])["commit"], self.head)

    def test_git_replace_cannot_change_archived_objects(self):
        (self.root / "source.txt").write_text("Replacement content\n")
        newer = commit(self.root)
        git(self.root, "replace", self.head, newer)
        files = self.contents(self.archive(revision=self.head))
        self.assertEqual(files["source.txt"], b"Committed source\n")

    def test_partial_clone_rejected_before_export(self):
        git(self.root, "config", "remote.origin.promisor", "true")
        with self.assertRaisesRegex(source_archive.ArchiveError, "Partial clones"):
            self.archive()
        self.assertFalse((self.base / "source.tar.gz").exists())

    def test_missing_submodule_mapping_fails(self):
        self.add_submodule()
        (self.root / ".gitmodules").write_text("")
        commit(self.root, "missing mapping")
        with self.assertRaisesRegex(source_archive.ArchiveError, "Missing .gitmodules entry"):
            self.archive()

    def test_unsafe_submodule_name_rejected(self):
        self.add_submodule()
        (self.root / ".gitmodules").write_text(
            '[submodule "../outside"]\n\tpath = third_party/dependency\n')
        commit(self.root, "unsafe name")
        with self.assertRaisesRegex(source_archive.ArchiveError, "Unsafe source path"):
            self.archive()

    def test_missing_submodule_fails_without_partial_archive(self):
        self.add_submodule()
        module = self.root / ".git/modules/vendor-dependency"
        module.rename(module.with_name("temporarily-unavailable"))
        with self.assertRaisesRegex(source_archive.ArchiveError, "Missing local submodule"):
            self.archive()
        self.assertFalse((self.base / "source.tar.gz").exists())

    def test_missing_pinned_commit_fails(self):
        self.add_submodule()
        git(self.root, "update-index", "--cacheinfo", "160000", "1" * 40, "third_party/dependency")
        git(self.root, "-c", "commit.gpgsign=false", "commit", "-m", "missing gitlink object")
        with self.assertRaisesRegex(source_archive.ArchiveError, "Missing pinned commit"):
            self.archive()
        self.assertFalse((self.base / "source.tar.gz").exists())

    def test_tracked_private_paths_are_rejected_not_silently_omitted(self):
        for path in ("PrivateDocs/plan.md", "game.Z64", "trace.aurcap", "game.st0", "trace.rdp",
                     ".env", ".env.local", "dist/app"):
            with self.subTest(path=path):
                target = self.root / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text("Synthetic forbidden fixture\n")
                git(self.root, "add", "--force", path)
                git(self.root, "-c", "commit.gpgsign=false", "commit", "-m", "private fixture")
                with self.assertRaisesRegex(source_archive.ArchiveError, "Private/generated"):
                    self.archive()
                git(self.root, "rm", "--", path)
                commit(self.root, "remove fixture")
        self.assertFalse((self.base / "source.tar.gz").exists())

    def test_reserved_manifest_rejected(self):
        (self.root / "SOURCE_MANIFEST.json").write_text("{}\n")
        commit(self.root)
        with self.assertRaisesRegex(source_archive.ArchiveError, "reserved"):
            self.archive()

    def test_unsafe_symlink_cleans_up_temporary_output(self):
        (self.root / "unsafe-link").symlink_to("../../outside")
        commit(self.root)
        with self.assertRaisesRegex(source_archive.ArchiveError, "Unsafe symlink"):
            self.archive()
        self.assertFalse((self.base / "source.tar.gz").exists())
        self.assertEqual(list(self.base.glob(".aurum-source-*")), [])

    def test_output_never_overwritten(self):
        output = self.base / "source.tar.gz"
        output.write_bytes(b"existing archive")
        with self.assertRaisesRegex(source_archive.ArchiveError, "Refusing to overwrite"):
            self.archive()
        self.assertEqual(output.read_bytes(), b"existing archive")

    def test_cli_invalid_ref_does_not_write_output(self):
        result = subprocess.run([sys.executable, str(SCRIPT), "--repo", str(self.root),
                                 "--ref", "does-not-exist", "--output", str(self.base / "missing.tar.gz")],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Source archive failed:", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertFalse((self.base / "missing.tar.gz").exists())


if __name__ == "__main__":
    unittest.main()
