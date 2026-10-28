#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Source snapshot build-input checks using only disposable repositories."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import prepare_core
import source_inputs
import test_source_archive as fixtures


PATCH = """--- a/dependency.c
+++ b/dependency.c
@@ -1 +1 @@
-/* pinned upstream */
+/* patched build copy */
"""


class SourceInputsTests(unittest.TestCase):
    def setUp(self):
        self.fixture = fixtures.SourceArchiveTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        self.base = self.fixture.base
        self.root = self.fixture.root
        self.pins = {}
        for name in source_inputs.MODULES:
            _, self.pins[name] = self.fixture.add_submodule(name=name)
        self.patch = self.root / "patches/mupen64plus-core/0001-interpreter-host-poll.patch"
        self.patch.parent.mkdir(parents=True)
        self.patch.write_text(PATCH)
        fixtures.commit(self.root)

    def snapshot(self):
        archive = self.fixture.archive()
        destination = self.base / "extracted"
        destination.mkdir()
        with tarfile.open(archive) as source:
            options = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
            source.extractall(destination, **options)
        result, = destination.iterdir()
        return result

    def rewrite_manifest(self, root, change):
        path = root / source_inputs.MANIFEST
        data = json.loads(path.read_text())
        change(data)
        path.write_text(json.dumps(data))

    def test_git_and_snapshot_pins_agree(self):
        checkout = source_inputs.SourceInputs(self.root)
        checkout.verify()
        snapshot = source_inputs.SourceInputs(self.snapshot())
        snapshot.verify()
        for name, pin in self.pins.items():
            self.assertEqual(checkout.pin(name), pin)
            self.assertEqual(snapshot.pin(name), pin)
        self.assertEqual(checkout.provenance()["kind"], "git-checkout")
        self.assertFalse(checkout.provenance()["checkoutModified"])
        self.assertEqual(snapshot.provenance()["kind"], "source-snapshot")
        self.assertEqual(checkout.commit, snapshot.commit)

    def test_modified_root_checkout_is_identified_not_misrepresented(self):
        (self.root / "source.txt").write_text("Local app edit\n")
        checkout = source_inputs.SourceInputs(self.root)
        checkout.verify()
        self.assertTrue(checkout.provenance()["checkoutModified"])

    def test_advanced_submodule_checkout_rejected(self):
        child = self.root / "third_party" / source_inputs.MODULES[0]
        (child / "dependency.c").write_text("/* different upstream */\n")
        fixtures.commit(child)
        with self.assertRaisesRegex(source_inputs.ArchiveError, "committed pin"):
            source_inputs.SourceInputs(self.root).verify()

    def test_dirty_submodule_checkout_rejected(self):
        (self.root / "third_party/mupen64plus-core/dependency.c").write_text("/* dirty */\n")
        with self.assertRaisesRegex(source_inputs.ArchiveError, "tracked changes"):
            source_inputs.SourceInputs(self.root).verify()

    def test_untracked_submodule_source_rejected(self):
        (self.root / "third_party/mupen64plus-core/injected.c").write_text("/* untracked */\n")
        with self.assertRaisesRegex(source_inputs.ArchiveError, "untracked files"):
            source_inputs.SourceInputs(self.root).verify()

    def test_uninitialized_submodule_does_not_fall_back_to_parent_head(self):
        worktree = self.base / "linked"
        fixtures.git(self.root, "worktree", "add", "--quiet", "--detach", str(worktree), "HEAD")
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Uninitialized submodule"):
            source_inputs.SourceInputs(worktree).verify()

    def test_snapshot_inside_another_checkout_uses_own_manifest(self):
        root = self.snapshot()
        moved = self.root / "nested-source-snapshot"
        root.rename(moved)
        source = source_inputs.SourceInputs(moved)
        source.verify()
        self.assertEqual(source.provenance()["kind"], "source-snapshot")

    def test_checksum_mismatch_rejected(self):
        root = self.snapshot()
        (root / "source.txt").write_text("Modified snapshot\n")
        with self.assertRaisesRegex(source_inputs.ArchiveError, "checksum differs"):
            source_inputs.SourceInputs(root).verify()

    def test_git_object_mismatch_rejected(self):
        root = self.snapshot()
        self.rewrite_manifest(root, lambda m: m["files"][0].update(gitObject="0" * 40))
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Git object differs"):
            source_inputs.SourceInputs(root).verify()

    def test_unlisted_source_file_rejected(self):
        root = self.snapshot()
        (root / "third_party/mupen64plus-core/injected.c").write_text("/* unlisted */\n")
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Unlisted source file"):
            source_inputs.SourceInputs(root).verify()

    def test_missing_source_file_rejected(self):
        root = self.snapshot()
        (root / "source.txt").unlink()
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Missing source file"):
            source_inputs.SourceInputs(root).verify()

    def test_changed_mode_rejected(self):
        root = self.snapshot()
        (root / "source.txt").chmod(0o755)
        with self.assertRaisesRegex(source_inputs.ArchiveError, "executable mode differs"):
            source_inputs.SourceInputs(root).verify()

    def test_symlink_source_directory_rejected(self):
        root = self.snapshot()
        child = root / "third_party/mupen64plus-core"
        child.rename(self.base / "elsewhere")
        child.symlink_to(self.base / "elsewhere", target_is_directory=True)
        with self.assertRaisesRegex(source_inputs.ArchiveError, "directory is a symlink"):
            source_inputs.SourceInputs(root).verify()

    def test_generated_outputs_and_python_caches_allowed(self):
        root = self.snapshot()
        for name in (".build/native/file.o", "dist/Aurum64.app/binary", "scripts/__pycache__/module.pyc"):
            path = root / name
            path.parent.mkdir(parents=True)
            path.write_bytes(b"generated output")
        source_inputs.SourceInputs(root).verify()

    def test_unknown_manifest_version_rejected(self):
        root = self.snapshot()
        self.rewrite_manifest(root, lambda m: m.update(formatVersion=2))
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Unsupported"):
            source_inputs.SourceInputs(root)

    def test_duplicate_manifest_file_rejected(self):
        root = self.snapshot()
        self.rewrite_manifest(root, lambda m: m["files"].append(m["files"][0]))
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Duplicate"):
            source_inputs.SourceInputs(root)

    def test_unsafe_manifest_path_rejected(self):
        root = self.snapshot()
        self.rewrite_manifest(root, lambda m: m["files"][0].update(path="../outside"))
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Unsafe source path"):
            source_inputs.SourceInputs(root)

    def test_invalid_manifest_file_record_rejected(self):
        root = self.snapshot()
        self.rewrite_manifest(root, lambda m: m["files"][0].update(mode=[]))
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Invalid file record"):
            source_inputs.SourceInputs(root)

    def test_missing_module_pin_rejected(self):
        root = self.snapshot()
        self.rewrite_manifest(root, lambda m: m["submodules"].clear())
        with self.assertRaisesRegex(source_inputs.ArchiveError, "Missing source pin"):
            source_inputs.SourceInputs(root).verify()

    def test_missing_manifest_has_actionable_failure(self):
        root = self.snapshot()
        (root / source_inputs.MANIFEST).unlink()
        with self.assertRaisesRegex(source_inputs.ArchiveError, "initialized checkout or a source snapshot"):
            source_inputs.SourceInputs(root)

    def test_prepare_snapshot_patches_only_generated_copy_and_reuses_it(self):
        root = self.snapshot()
        before = (root / "third_party/mupen64plus-core/dependency.c").read_bytes()
        target = prepare_core.prepare(root)
        self.assertEqual((target / "dependency.c").read_text(), "/* patched build copy */\n")
        self.assertEqual((root / "third_party/mupen64plus-core/dependency.c").read_bytes(), before)
        stamp = target / ".aurum-patch-stamp"
        previous = stamp.stat().st_mtime_ns
        self.assertEqual(prepare_core.prepare(root), target)
        self.assertEqual(stamp.stat().st_mtime_ns, previous)
        self.assertEqual(stamp.read_text(), self.pins["mupen64plus-core"] + ":"
                         + hashlib.sha256(self.patch.read_bytes()).hexdigest())
        source_inputs.SourceInputs(root).verify()

    def test_prepare_git_and_snapshot_match(self):
        snapshot = self.snapshot()
        git_target = prepare_core.prepare(self.root)
        snapshot_target = prepare_core.prepare(snapshot)
        self.assertEqual((git_target / "dependency.c").read_bytes(),
                         (snapshot_target / "dependency.c").read_bytes())
        self.assertEqual((git_target / ".aurum-patch-stamp").read_bytes(),
                         (snapshot_target / ".aurum-patch-stamp").read_bytes())

    def test_failed_patch_keeps_previous_generated_source(self):
        target = prepare_core.prepare(self.root)
        previous = (target / "dependency.c").read_bytes()
        stamp = (target / ".aurum-patch-stamp").read_bytes()
        self.patch.write_text(PATCH.replace("pinned upstream", "missing source line"))
        with self.assertRaises(subprocess.CalledProcessError):
            prepare_core.prepare(self.root)
        self.assertEqual((target / "dependency.c").read_bytes(), previous)
        self.assertEqual((target / ".aurum-patch-stamp").read_bytes(), stamp)
        self.assertEqual(list(target.parent.glob(".core-prepare-*")), [])

    def test_cli_verifies_before_printing_pin(self):
        root = self.snapshot()
        command = [sys.executable, "-B", str(Path(source_inputs.__file__).resolve()), "--root", str(root),
                   "pin", "mupen64plus-core"]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), self.pins["mupen64plus-core"])
        (root / "source.txt").write_text("Damaged snapshot\n")
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("Source verification failed", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
