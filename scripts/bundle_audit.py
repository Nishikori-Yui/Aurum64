#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Audit an Aurum64 development bundle without launching it or loading its code."""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import re
import stat
import subprocess


LOCAL_LIBRARIES = (
    "libmupen64plus.dylib", "mupen64plus-rsp-cxd4.dylib",
    "libaurum-runtime.dylib", "libaurum-video.dylib", "libaurum-audio.dylib", "libaurum-input.dylib",
)
REQUIRED_LIBRARIES = set(LOCAL_LIBRARIES) | {"libSDL2-2.0.0.dylib", "libpng16.16.dylib"}
ALLOWED_LIBRARIES = REQUIRED_LIBRARIES | {"libSDL3.dylib"}
CORE_LICENSES = {
    "Aurum64-GPL-2.txt", "AurumRDP-MIT.txt", "Mupen64Plus-LICENSES.txt",
    "CXD4-CC0.txt", "CXD4-README.txt", "CXD4-OSAL-GPL.txt", "Core-minizip.txt",
    "Core-minizip-zip.txt", "Core-md5.txt", "Core-CIC.txt", "Third-Party-Notices.md",
    "Aurum-Core-Patch.txt",
}
LICENSE_NAMES = ("LICENSE", "LICENSE.txt", "COPYING", "NOTICE")
EXTERNAL_LICENSES = {f"{component}-{name}" for component in ("sdl2", "sdl2-compat", "sdl3", "libpng")
                     for name in LICENSE_NAMES}
RESOURCE_FILES = {"AurumRDP.metallib", "mupen64plus.ini"}
FIXED_FILES = {
    "Contents/Info.plist", "Contents/MacOS/Aurum64", "Contents/Resources/BuildManifest.json",
    "Contents/_CodeSignature/CodeResources",
} | {f"Contents/Resources/{name}" for name in RESOURCE_FILES}
DIRECTORIES = {"Contents", "Contents/MacOS", "Contents/Frameworks", "Contents/Resources",
               "Contents/Resources/Licenses", "Contents/_CodeSignature"}


class AuditError(Exception):
    pass


def version(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}", value):
        raise AuditError(f"Invalid macOS version: {value!r}")
    parts = tuple(map(int, value.split(".")))
    return parts + (0,) * (3 - len(parts))


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def is_hash(value, lengths=(64,)):
    return isinstance(value, str) and len(value) in lengths and re.fullmatch("[0-9a-f]+", value)


def load_json(path):
    if path.stat().st_size > 2 * 1024 * 1024:
        raise AuditError("Build manifest exceeds the 2 MiB limit")
    try:
        manifest = json.loads(path.read_bytes())
    except (ValueError, UnicodeError) as error:
        raise AuditError("Invalid BuildManifest.json") from error
    if not isinstance(manifest, dict) or manifest.get("formatVersion") != 1:
        raise AuditError("Unsupported build manifest; rebuild the bundle with current packaging tools")
    return manifest


def _records(value, key):
    if not isinstance(value, list):
        raise AuditError(f"Missing manifest {key} records")
    result = {}
    for item in value:
        if not isinstance(item, dict) or not isinstance(item.get(key), str) or item[key] in result:
            raise AuditError(f"Invalid or duplicate manifest {key} record")
        result[item[key]] = item
    return result


def audit_layout(app):
    app = Path(app).absolute()
    if app.is_symlink() or not app.is_dir():
        raise AuditError("Bundle must be a directory, not a symlink")
    allowed = (FIXED_FILES | {f"Contents/Frameworks/{name}" for name in ALLOWED_LIBRARIES}
               | {f"Contents/Resources/Licenses/{name}" for name in CORE_LICENSES | EXTERNAL_LICENSES})
    files = set()
    pending = [app]
    while pending:
        directory = pending.pop()
        for path in directory.iterdir():
            name = path.relative_to(app).as_posix()
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                raise AuditError(f"Bundle symlinks are not allowed: {name}")
            if stat.S_ISDIR(mode):
                if name not in DIRECTORIES:
                    raise AuditError(f"Unexpected bundle directory: {name}")
                pending.append(path)
            elif not stat.S_ISREG(mode) or name not in allowed:
                raise AuditError(f"Unexpected bundle file: {name}")
            else:
                if path.stat().st_size == 0:
                    raise AuditError(f"Empty bundle file: {name}")
                files.add(name)
    required = (FIXED_FILES | {f"Contents/Frameworks/{name}" for name in REQUIRED_LIBRARIES}
                | {f"Contents/Resources/Licenses/{name}" for name in CORE_LICENSES})
    if required - files:
        raise AuditError(f"Missing bundle file: {sorted(required - files)[0]}")
    if not (app / "Contents/MacOS/Aurum64").stat().st_mode & 0o111:
        raise AuditError("Aurum64 executable has no execute permission")
    try:
        info = plistlib.loads((app / "Contents/Info.plist").read_bytes())
    except (ValueError, plistlib.InvalidFileException) as error:
        raise AuditError("Invalid Info.plist") from error
    if not isinstance(info, dict) or any(info.get(key) != expected for key, expected in {
            "CFBundleExecutable": "Aurum64", "CFBundleIdentifier": "org.aurum64.Aurum64",
            "CFBundlePackageType": "APPL"}.items()):
        raise AuditError("Unexpected bundle identity or executable in Info.plist")
    minimum = info.get("LSMinimumSystemVersion")
    version(minimum)
    manifest = load_json(app / "Contents/Resources/BuildManifest.json")
    if version(manifest.get("minimumMacOS")) != version(minimum):
        raise AuditError("Info.plist and BuildManifest.json disagree about the minimum macOS version")
    pins = manifest.get("submodules")
    if (not isinstance(pins, dict) or set(pins) != {"mupen64plus-core", "mupen64plus-rsp-cxd4"}
            or not all(is_hash(pin, (40, 64)) for pin in pins.values())):
        raise AuditError("Invalid or missing production submodule pins")
    provenance = manifest.get("source")
    if not isinstance(provenance, dict) or not is_hash(provenance.get("commit"), (40, 64)):
        raise AuditError("Missing source provenance")
    if provenance.get("kind") == "source-snapshot":
        if not is_hash(provenance.get("manifestSHA256")):
            raise AuditError("Missing source snapshot manifest hash")
    elif provenance.get("kind") == "git-checkout":
        if type(provenance.get("checkoutModified")) is not bool:
            raise AuditError("Missing checkout modification status")
    else:
        raise AuditError("Unknown source provenance kind")
    libraries = _records(manifest.get("libraries"), "library")
    names = {PurePosixPath(name).name for name in files if name.startswith("Contents/Frameworks/")}
    if set(libraries) != names:
        raise AuditError("Bundle libraries do not match the build manifest")
    for name, entry in libraries.items():
        if (not is_hash(entry.get("sourceSHA256")) or not isinstance(entry.get("source"), str)
                or not entry["source"] or not is_hash(entry.get("bundledSHA256"))):
            raise AuditError(f"Incomplete library provenance: {name}")
        source = entry["source"]
        label, separator, location = source.partition(":")
        if (not separator or label not in {"repository", "homebrew"} or "\\" in location
                or any(part in {"", ".", ".."} for part in location.split("/"))
                or any(ord(c) < 32 for c in source)):
            raise AuditError(f"Invalid or absolute library source label: {name}")
        if digest(app / "Contents/Frameworks" / name) != entry["bundledSHA256"]:
            raise AuditError(f"Bundled library checksum differs: {name}")
    resources = _records(manifest.get("resources"), "path")
    paths = {name.removeprefix("Contents/Resources/") for name in files
             if name.startswith("Contents/Resources/") and not name.endswith("/BuildManifest.json")}
    if set(resources) != paths:
        raise AuditError("Bundle resources do not match the build manifest")
    for path, entry in resources.items():
        if not is_hash(entry.get("sha256")) or digest(app / "Contents/Resources" / path) != entry["sha256"]:
            raise AuditError(f"Bundled resource checksum differs: {path}")
    if manifest.get("corePatchSHA256") != resources["Licenses/Aurum-Core-Patch.txt"]["sha256"]:
        raise AuditError("Bundled core patch does not match build provenance")
    licenses = {PurePosixPath(path).name for path in paths if path.startswith("Licenses/")}

    def has_license(component):
        return any(f"{component}-{name}" in licenses for name in LICENSE_NAMES)

    if not has_license("libpng") or not (has_license("sdl2") or has_license("sdl2-compat")):
        raise AuditError("Missing SDL2 or libpng license notice")
    uses_compat = libraries["libSDL2-2.0.0.dylib"]["source"].startswith("homebrew:sdl2-compat/")
    if uses_compat and not has_license("sdl2-compat"):
        raise AuditError("Missing SDL2-compat license notice")
    if (uses_compat or has_license("sdl2-compat")) and "libSDL3.dylib" not in names:
        raise AuditError("SDL2-compat requires the bundled SDL3 dlopen dependency")
    if "libSDL3.dylib" in names and not has_license("sdl3"):
        raise AuditError("Missing SDL3 license notice")
    return {"status": "layout-only", "files": len(files), "libraries": len(names),
            "minimumMacOS": minimum, "sourceKind": provenance["kind"],
            "sourceCommit": provenance["commit"], "binaries": ["Contents/MacOS/Aurum64"]
            + [f"Contents/Frameworks/{name}" for name in sorted(names)]}


@dataclass
class MachO:
    dependencies: list
    rpaths: list
    identifiers: list
    minimum: str


def parse_load_commands(text):
    dependencies, rpaths, identifiers, minimums = [], [], [], []
    for block in re.split(r"(?m)^Load command [0-9]+\s*$", text)[1:]:
        command = re.search(r"(?m)^\s*cmd (LC_\w+)\s*$", block)
        if command is None:
            raise AuditError("Unrecognized Mach-O load command output")
        command = command[1]

        def field(name):
            match = re.search(rf"(?m)^\s*{name} (.+?)(?: \(offset [0-9]+\))?\s*$", block)
            if match is None:
                raise AuditError(f"Missing {name} in {command}")
            return match[1]

        if command == "LC_RPATH":
            rpaths.append(field("path"))
        elif command == "LC_ID_DYLIB":
            identifiers.append(field("name"))
        elif command in {"LC_LOAD_DYLIB", "LC_LOAD_WEAK_DYLIB", "LC_REEXPORT_DYLIB",
                         "LC_LOAD_UPWARD_DYLIB", "LC_LAZY_LOAD_DYLIB"}:
            dependencies.append(field("name"))
        elif command == "LC_BUILD_VERSION":
            if field("platform") not in {"1", "MACOS"}:
                raise AuditError("Mach-O targets a platform other than macOS")
            minimums.append(field("minos"))
        elif command == "LC_VERSION_MIN_MACOSX":
            minimums.append(field("version"))
        elif command.startswith("LC_VERSION_MIN_") or command == "LC_DYLD_ENVIRONMENT":
            raise AuditError(f"Unsupported Mach-O load command: {command}")
        elif command == "LC_LOAD_DYLINKER" and field("name") != "/usr/lib/dyld":
            raise AuditError("Unexpected dynamic linker")
    if len(minimums) != 1:
        raise AuditError("Expected exactly one macOS deployment load command")
    version(minimums[0])
    return MachO(dependencies, rpaths, identifiers, minimums[0])


def parse_filetype(text):
    lines = text.splitlines()
    for index, line in enumerate(lines[:-1]):
        fields = line.split()
        if "filetype" in fields:
            values = lines[index + 1].split()
            position = fields.index("filetype")
            if len(values) > position and values[position] in {"EXECUTE", "DYLIB", "BUNDLE"}:
                return values[position]
    raise AuditError("Unsupported or missing Mach-O file type")


def command(*args):
    try:
        result = subprocess.run(list(map(str, args)), text=True, capture_output=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise AuditError(f"Cannot run {args[0]}; use a macOS host with Xcode command-line tools") from error
    if result.returncode:
        raise AuditError(f"{args[0]} failed: {result.stderr.strip()}")
    return result.stdout.strip()


def audit_bundle(app, run=command):
    report = audit_layout(app)
    app = Path(app).absolute()
    frameworks = app / "Contents/Frameworks"
    executable = app / "Contents/MacOS/Aurum64"
    images = {}
    types = {}
    for relative in report["binaries"]:
        path = app / relative
        if run("lipo", "-archs", path).split() != ["arm64"]:
            raise AuditError(f"Expected an arm64-only binary: {relative}")
        filetype = parse_filetype(run("otool", "-hv", path))
        allowed_types = {"EXECUTE"} if path == executable else {"DYLIB"}
        if path.name == "mupen64plus-rsp-cxd4.dylib":
            allowed_types = {"BUNDLE"}
        if filetype not in allowed_types:
            raise AuditError(f"Unexpected Mach-O type in {relative}: {filetype}")
        types[path] = filetype
        image = parse_load_commands(run("otool", "-l", path))
        if version(image.minimum) > version(report["minimumMacOS"]):
            raise AuditError(f"{relative} requires macOS {image.minimum}, above the advertised minimum")
        expected_id = [f"@rpath/{path.name}"] if filetype == "DYLIB" else []
        if image.identifiers != expected_id:
            raise AuditError(f"Unexpected dylib identity: {relative}")
        for rpath in image.rpaths:
            expected = "@executable_path/../Frameworks" if path == executable else "@loader_path"
            if rpath != expected:
                raise AuditError(f"Unexpected runtime search path in {relative}: {rpath}")
        images[path] = image
    if "@executable_path/../Frameworks" not in images[executable].rpaths:
        raise AuditError("Main executable cannot resolve its bundled Frameworks directory")
    for path, image in images.items():
        for dependency in image.dependencies:
            if ("\\" in dependency or any(ord(c) < 32 for c in dependency)
                    or any(part in {"", ".", ".."} for part in dependency.lstrip("/").split("/"))):
                raise AuditError(f"Unsafe dependency in {path.name}: {dependency}")
            if dependency.startswith(("/System/Library/", "/usr/lib/")):
                continue
            if dependency.startswith("@rpath/"):
                target = frameworks / dependency.removeprefix("@rpath/")
            elif dependency.startswith("@loader_path/"):
                target = path.parent / dependency.removeprefix("@loader_path/")
            else:
                raise AuditError(f"External or unsupported dependency in {path.name}: {dependency}")
            if target.parent != frameworks or target not in images:
                raise AuditError(f"Missing bundled dependency in {path.name}: {dependency}")
            if types[target] != "DYLIB":
                raise AuditError(f"Dependency is not a dylib in {path.name}: {dependency}")
        run("codesign", "--verify", "--strict", path)
    run("codesign", "--verify", "--deep", "--strict", app)
    report["status"] = "passed"
    report["signature"] = "verified"
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("--layout-only", action="store_true",
                        help="Check files, manifests and hashes without macOS tools; not a full audit")
    args = parser.parse_args()
    try:
        report = audit_layout(args.app) if args.layout_only else audit_bundle(args.app)
    except (AuditError, OSError, ValueError) as error:
        parser.exit(1, f"Bundle audit failed: {error}\n")
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
