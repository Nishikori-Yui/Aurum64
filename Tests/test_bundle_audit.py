#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Portable bundle policy tests; tool responses are synthetic, never GPU work."""
import copy
import json
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import bundle_audit as audit


def load_commands(dependencies=(), rpaths=(), identifier=None, minimum="26.0", platform="1"):
    blocks = [f"cmd LC_BUILD_VERSION\nplatform {platform}\nminos {minimum}\nsdk 27.0\n"]
    if identifier:
        blocks.append(f"cmd LC_ID_DYLIB\nname {identifier} (offset 24)\n")
    blocks.extend(f"cmd LC_LOAD_DYLIB\nname {name} (offset 24)\n" for name in dependencies)
    blocks.extend(f"cmd LC_RPATH\npath {name} (offset 12)\n" for name in rpaths)
    return "binary:\n" + "".join(f"Load command {index}\n{block}" for index, block in enumerate(blocks))


class BundleAuditTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="aurum-bundle-test-")
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.app = self.base / "Aurum64.app"
        for name in audit.DIRECTORIES:
            (self.app / name).mkdir(parents=True, exist_ok=True)
        licenses = audit.CORE_LICENSES | {"sdl2-compat-LICENSE.txt", "sdl3-LICENSE.txt", "libpng-LICENSE"}
        files = (audit.FIXED_FILES | {f"Contents/Frameworks/{name}" for name in audit.ALLOWED_LIBRARIES}
                 | {f"Contents/Resources/Licenses/{name}" for name in licenses})
        for name in files:
            (self.app / name).write_bytes(b"Synthetic bundle fixture: " + name.encode())
        (self.app / "Contents/MacOS/Aurum64").chmod(0o755)
        self.info = {"CFBundleExecutable": "Aurum64", "CFBundleIdentifier": "org.aurum64.Aurum64",
                     "CFBundlePackageType": "APPL", "LSMinimumSystemVersion": "26.0"}
        self.write_info()
        self.manifest = {
            "formatVersion": 1, "minimumMacOS": "26.0.0",
            "source": {"kind": "git-checkout", "commit": "1" * 40, "checkoutModified": False},
            "submodules": {"mupen64plus-core": "2" * 40, "mupen64plus-rsp-cxd4": "3" * 40},
            "libraries": [{"library": name, "source": "repository:.build/" + name,
                           "sourceSHA256": "4" * 64,
                           "bundledSHA256": audit.digest(self.app / "Contents/Frameworks" / name)}
                          for name in sorted(audit.ALLOWED_LIBRARIES)],
            "resources": [{"path": name.removeprefix("Contents/Resources/"),
                           "sha256": audit.digest(self.app / name)}
                          for name in sorted(files) if name.startswith("Contents/Resources/")
                          and not name.endswith("/BuildManifest.json")],
            "corePatchSHA256": audit.digest(self.app / "Contents/Resources/Licenses/Aurum-Core-Patch.txt"),
        }
        self.write_manifest()
        self.calls = []
        self.architecture = "arm64"
        self.overrides = {}
        self.type_overrides = {}
        self.signature_error = False

    def write_info(self):
        (self.app / "Contents/Info.plist").write_bytes(plistlib.dumps(self.info))

    def write_manifest(self):
        (self.app / "Contents/Resources/BuildManifest.json").write_text(json.dumps(self.manifest))

    def run_tool(self, *args):
        self.calls.append(args)
        if args[0] == "lipo":
            return self.architecture
        if args[0] == "codesign":
            if self.signature_error:
                raise audit.AuditError("codesign failed: synthetic invalid signature")
            return ""
        name = Path(args[-1]).name
        if args[:2] == ("otool", "-hv"):
            filetype = "EXECUTE" if name == "Aurum64" else "DYLIB"
            if name == "mupen64plus-rsp-cxd4.dylib":
                filetype = "BUNDLE"
            filetype = self.type_overrides.get(name, filetype)
            return f"Mach header\nmagic cputype cpusubtype caps filetype ncmds sizeofcmds flags\nMH_MAGIC_64 ARM64 ALL 0x00 {filetype} 15 1024 DYLDLINK\n"
        self.assertEqual(args[:2], ("otool", "-l"))
        if name in self.overrides:
            return self.overrides[name]
        if name == "Aurum64":
            return load_commands(["@rpath/libaurum-runtime.dylib", "/usr/lib/libSystem.B.dylib"],
                                 ["@executable_path/../Frameworks"])
        if name == "mupen64plus-rsp-cxd4.dylib":
            return load_commands(["/usr/lib/libSystem.B.dylib"])
        return load_commands(["/usr/lib/libSystem.B.dylib"], ["@loader_path"], f"@rpath/{name}")

    def assert_rejected(self, message, full=False):
        with self.assertRaisesRegex(audit.AuditError, message):
            audit.audit_bundle(self.app, self.run_tool) if full else audit.audit_layout(self.app)

    def test_full_audit_checks_each_binary_and_bundle_signature(self):
        report = audit.audit_bundle(self.app, self.run_tool)
        self.assertEqual(report["status"], "passed")
        self.assertEqual(report["libraries"], 9)
        self.assertEqual(report["signature"], "verified")
        self.assertEqual(sum(call[0] == "lipo" for call in self.calls), 10)
        self.assertEqual(sum(call[0] == "otool" for call in self.calls), 20)
        self.assertEqual(sum(call[0] == "codesign" for call in self.calls), 11)

    def test_layout_only_never_claims_signature_or_full_pass(self):
        report = audit.audit_layout(self.app)
        self.assertEqual(report["status"], "layout-only")
        self.assertNotIn("signature", report)

    def test_extra_private_files_rejected(self):
        for name in ("game.z64", "settings.sav", "trace.aurcap", ".env"):
            with self.subTest(name=name):
                path = self.app / "Contents/Resources" / name
                path.write_bytes(b"Synthetic private fixture")
                self.assert_rejected("Unexpected bundle file")
                path.unlink()

    def test_extra_directory_rejected_even_when_empty(self):
        (self.app / "Contents/PrivateDocs").mkdir()
        self.assert_rejected("Unexpected bundle directory")

    def test_unexpected_diagnostic_library_rejected(self):
        name = "libaurum-diagnostic.dylib"
        path = self.app / "Contents/Frameworks" / name
        path.write_bytes(b"development-only")
        self.manifest["libraries"].append({"library": name, "source": "x", "sourceSHA256": "5" * 64,
                                           "bundledSHA256": audit.digest(path)})
        self.write_manifest()
        self.assert_rejected("Unexpected bundle file")

    def test_missing_runtime_rejected(self):
        (self.app / "Contents/Frameworks/libaurum-runtime.dylib").unlink()
        self.assert_rejected("Missing bundle file")

    def test_empty_resource_rejected(self):
        (self.app / "Contents/Resources/AurumRDP.metallib").write_bytes(b"")
        self.assert_rejected("Empty bundle file")

    def test_file_symlink_rejected(self):
        path = self.app / "Contents/Resources/AurumRDP.metallib"
        path.unlink()
        path.symlink_to(self.base / "outside")
        self.assert_rejected("symlinks are not allowed")

    def test_directory_symlink_rejected_without_traversal(self):
        path = self.app / "Contents/Unexpected"
        path.symlink_to(self.base, target_is_directory=True)
        self.assert_rejected("symlinks are not allowed")

    def test_wrong_bundle_identity_rejected(self):
        self.info["CFBundleExecutable"] = "OtherApp"
        self.write_info()
        self.assert_rejected("Unexpected bundle identity")

    def test_main_executable_without_execute_mode_rejected(self):
        (self.app / "Contents/MacOS/Aurum64").chmod(0o644)
        self.assert_rejected("execute permission")

    def test_invalid_plist_rejected(self):
        (self.app / "Contents/Info.plist").write_bytes(b"invalid")
        self.assert_rejected("Invalid Info.plist")

    def test_macos_version_numeric_order_and_zero_padding(self):
        self.assertEqual(audit.version("26"), audit.version("26.0.0"))
        self.assertGreater(audit.version("26.10"), audit.version("26.9.9"))
        self.info["LSMinimumSystemVersion"] = "25.0"
        self.write_info()
        self.assert_rejected("disagree about the minimum")

    def test_unknown_manifest_version_rejected(self):
        self.manifest["formatVersion"] = 2
        self.write_manifest()
        self.assert_rejected("Unsupported build manifest")

    def test_missing_source_pin_rejected(self):
        self.manifest["submodules"].pop("mupen64plus-core")
        self.write_manifest()
        self.assert_rejected("submodule pins")

    def test_missing_checkout_provenance_rejected(self):
        self.manifest["source"].pop("checkoutModified")
        self.write_manifest()
        self.assert_rejected("modification status")

    def test_snapshot_provenance_requires_manifest_hash(self):
        self.manifest["source"] = {"kind": "source-snapshot", "commit": "1" * 40}
        self.write_manifest()
        self.assert_rejected("snapshot manifest hash")
        self.manifest["source"]["manifestSHA256"] = "2" * 64
        self.write_manifest()
        self.assertEqual(audit.audit_layout(self.app)["sourceKind"], "source-snapshot")

    def test_duplicate_library_record_rejected(self):
        self.manifest["libraries"].append(copy.deepcopy(self.manifest["libraries"][0]))
        self.write_manifest()
        self.assert_rejected("duplicate manifest library")

    def test_missing_library_record_rejected(self):
        self.manifest["libraries"].pop()
        self.write_manifest()
        self.assert_rejected("libraries do not match")

    def test_absolute_developer_source_path_rejected(self):
        self.manifest["libraries"][0]["source"] = "/Users/developer/secret-project/library.dylib"
        self.write_manifest()
        self.assert_rejected("absolute library source label")

    def test_compat_provenance_requires_matching_license(self):
        for entry in self.manifest["libraries"]:
            if entry["library"] == "libSDL2-2.0.0.dylib":
                entry["source"] = "homebrew:sdl2-compat/2.32.70/lib/libSDL2-2.0.0.dylib"
        before = self.app / "Contents/Resources/Licenses/sdl2-compat-LICENSE.txt"
        before.rename(before.with_name("sdl2-LICENSE.txt"))
        for resource in self.manifest["resources"]:
            if resource["path"] == "Licenses/sdl2-compat-LICENSE.txt":
                resource["path"] = "Licenses/sdl2-LICENSE.txt"
        self.write_manifest()
        self.assert_rejected("Missing SDL2-compat license")

    def test_modified_library_checksum_rejected(self):
        (self.app / "Contents/Frameworks/libSDL3.dylib").write_bytes(b"changed")
        self.assert_rejected("library checksum differs")

    def test_modified_resource_checksum_rejected(self):
        (self.app / "Contents/Resources/mupen64plus.ini").write_bytes(b"changed")
        self.assert_rejected("resource checksum differs")

    def test_unlisted_resource_record_rejected(self):
        self.manifest["resources"].append({"path": "../../private", "sha256": "5" * 64})
        self.write_manifest()
        self.assert_rejected("resources do not match")

    def test_patch_hash_mismatch_rejected(self):
        self.manifest["corePatchSHA256"] = "0" * 64
        self.write_manifest()
        self.assert_rejected("core patch does not match")

    def remove_file_and_record(self, path):
        (self.app / path).unlink()
        if path.startswith("Contents/Frameworks/"):
            self.manifest["libraries"] = [x for x in self.manifest["libraries"]
                                          if x["library"] != Path(path).name]
        else:
            self.manifest["resources"] = [x for x in self.manifest["resources"]
                                          if x["path"] != path.removeprefix("Contents/Resources/")]
        self.write_manifest()

    def test_missing_dynamic_sdl3_dependency_rejected(self):
        self.remove_file_and_record("Contents/Frameworks/libSDL3.dylib")
        self.assert_rejected("SDL2-compat requires")

    def test_missing_external_license_rejected(self):
        self.remove_file_and_record("Contents/Resources/Licenses/libpng-LICENSE")
        self.assert_rejected("Missing SDL2 or libpng license")

    def test_native_sdl2_does_not_require_sdl3(self):
        self.remove_file_and_record("Contents/Frameworks/libSDL3.dylib")
        self.remove_file_and_record("Contents/Resources/Licenses/sdl3-LICENSE.txt")
        before = self.app / "Contents/Resources/Licenses/sdl2-compat-LICENSE.txt"
        before.rename(before.with_name("sdl2-LICENSE.txt"))
        for resource in self.manifest["resources"]:
            if resource["path"] == "Licenses/sdl2-compat-LICENSE.txt":
                resource["path"] = "Licenses/sdl2-LICENSE.txt"
        self.write_manifest()
        self.assertEqual(audit.audit_layout(self.app)["libraries"], 8)

    def test_wrong_architecture_rejected(self):
        for architecture in ("x86_64", "arm64 x86_64", "arm64e"):
            with self.subTest(architecture=architecture):
                self.architecture = architecture
                self.assert_rejected("arm64-only binary", full=True)

    def test_main_executable_minimum_is_checked(self):
        self.overrides["Aurum64"] = load_commands(minimum="27.0")
        self.assert_rejected("above the advertised minimum", full=True)

    def test_wrong_platform_rejected(self):
        self.overrides["Aurum64"] = load_commands(platform="2")
        self.assert_rejected("other than macOS", full=True)

    def test_absolute_rpath_rejected(self):
        self.overrides["Aurum64"] = load_commands(rpaths=["/opt/homebrew/lib"])
        self.assert_rejected("runtime search path", full=True)

    def test_homebrew_relative_rpath_rejected(self):
        self.overrides["libSDL2-2.0.0.dylib"] = load_commands(
            rpaths=["@loader_path/../../../../opt/sdl3/lib"], identifier="@rpath/libSDL2-2.0.0.dylib")
        self.assert_rejected("runtime search path", full=True)

    def test_main_executable_requires_framework_rpath(self):
        self.overrides["Aurum64"] = load_commands()
        self.assert_rejected("cannot resolve", full=True)

    def test_missing_relative_dependency_rejected(self):
        self.overrides["Aurum64"] = load_commands(["@rpath/libmissing.dylib"],
                                                 ["@executable_path/../Frameworks"])
        self.assert_rejected("Missing bundled dependency", full=True)

    def test_absolute_dependency_rejected(self):
        self.overrides["Aurum64"] = load_commands(["/opt/homebrew/lib/local.dylib"],
                                                 ["@executable_path/../Frameworks"])
        self.assert_rejected("External or unsupported dependency", full=True)

    def test_dependency_parent_traversal_rejected(self):
        self.overrides["Aurum64"] = load_commands(["/usr/lib/../../outside.dylib"],
                                                 ["@executable_path/../Frameworks"])
        self.assert_rejected("Unsafe dependency", full=True)

    def test_loader_path_dependency_resolves(self):
        self.overrides["libaurum-video.dylib"] = load_commands(["@loader_path/libaurum-runtime.dylib"],
            ["@loader_path"], "@rpath/libaurum-video.dylib")
        self.assertEqual(audit.audit_bundle(self.app, self.run_tool)["status"], "passed")

    def test_bad_dylib_identity_rejected(self):
        self.overrides["libSDL3.dylib"] = load_commands(identifier="/opt/homebrew/lib/libSDL3.dylib")
        self.assert_rejected("Unexpected dylib identity", full=True)

    def test_wrong_macho_file_type_rejected(self):
        self.type_overrides["libSDL3.dylib"] = "BUNDLE"
        self.assert_rejected("Unexpected Mach-O type", full=True)

    def test_plugin_first_dependency_is_not_ignored(self):
        self.overrides["mupen64plus-rsp-cxd4.dylib"] = load_commands(["/tmp/missing.dylib"])
        self.assert_rejected("External or unsupported dependency", full=True)

    def test_mh_bundle_cannot_be_a_linked_dylib_dependency(self):
        self.overrides["libaurum-video.dylib"] = load_commands(
            ["@loader_path/mupen64plus-rsp-cxd4.dylib"], identifier="@rpath/libaurum-video.dylib")
        self.assert_rejected("Dependency is not a dylib", full=True)

    def test_signature_failure_is_not_reported_as_pass(self):
        self.signature_error = True
        self.assert_rejected("invalid signature", full=True)

    def test_parse_legacy_macos_version(self):
        image = audit.parse_load_commands("Load command 0\ncmd LC_VERSION_MIN_MACOSX\nversion 12.0\nsdk 13.0\n")
        self.assertEqual(image.minimum, "12.0")

    def test_parse_missing_macos_version_rejected(self):
        with self.assertRaisesRegex(audit.AuditError, "deployment load command"):
            audit.parse_load_commands("Load command 0\ncmd LC_SEGMENT_64\n")

    def test_weak_and_reexport_dependencies_included(self):
        text = load_commands(["@rpath/weak.dylib", "@rpath/reexport.dylib"])
        text = text.replace("LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", 1)
        text = text.replace("LC_LOAD_DYLIB", "LC_REEXPORT_DYLIB", 1)
        self.assertEqual(audit.parse_load_commands(text).dependencies,
                         ["@rpath/weak.dylib", "@rpath/reexport.dylib"])

    def test_dyld_environment_command_rejected(self):
        text = load_commands() + "Load command 1\ncmd LC_DYLD_ENVIRONMENT\n"
        with self.assertRaisesRegex(audit.AuditError, "Unsupported Mach-O"):
            audit.parse_load_commands(text)

    def test_cli_reports_failure_without_traceback(self):
        (self.app / "Contents/Resources/private.txt").write_text("synthetic")
        result = subprocess.run([sys.executable, "-B", audit.__file__, str(self.app), "--layout-only"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn("Bundle audit failed:", result.stderr)
        self.assertNotIn("Traceback", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()
