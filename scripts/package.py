#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bundle all non-system dylibs, including SDL2-compat's dlopen dependency."""
import hashlib
import json
import os
import pathlib
import plistlib
import shutil
import subprocess

from source_inputs import SourceInputs

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE_INPUTS = SourceInputs(ROOT)
SOURCE_INPUTS.verify()
APP = ROOT / "dist/Aurum64.app"
STAGING = ROOT / ".build/package/Aurum64.app"
FINAL_APP = APP
if STAGING.exists():
    shutil.rmtree(STAGING)
APP = STAGING
CONTENTS = APP / "Contents"
LIBS = CONTENTS / "Frameworks"
RES = CONTENTS / "Resources"
for directory in [LIBS, RES / "Licenses", CONTENTS / "MacOS"]:
    directory.mkdir(parents=True, exist_ok=True)

def run(*args):
    return subprocess.check_output(args, text=True).strip()

def copy(source, destination):
    shutil.copy2(source, destination)
    destination.chmod(destination.stat().st_mode | 0o200)

def dependencies(path):
    return [line.strip().split(" (compatibility")[0] for line in run("otool", "-L", str(path)).splitlines()[1:]]

copied = {}
def bundle(source):
    source = pathlib.Path(source)
    name = source.name
    if name in copied:
        if source.resolve() != copied[name].resolve():
            raise RuntimeError(f"Dependency filename collision: {source}")
        return
    if not source.is_file():
        raise RuntimeError(f"Missing library: {source}")
    copied[name] = source
    destination = LIBS / name
    copy(source, destination)
    run("install_name_tool", "-id", f"@rpath/{name}", str(destination))
    # The packaged ID replaces an upstream build-path ID; it is not a dependency.
    source_dependencies = dependencies(source)
    for dependency in source_dependencies[1:]:
        if dependency.startswith(("/System/", "/usr/lib/")):
            continue
        if dependency in [str(source), f"@rpath/{name}"]:
            continue
        if dependency.startswith("@rpath/") or dependency.startswith("@loader_path/"):
            candidate = source.parent / pathlib.Path(dependency).name
        else:
            candidate = pathlib.Path(dependency)
        if candidate.resolve() == source.resolve():
            continue
        bundle(candidate)
        run("install_name_tool", "-change", dependency, f"@loader_path/{candidate.name}", str(destination))

for library in [ROOT / ".build/libmupen64plus.dylib", ROOT / ".build/mupen64plus-rsp-cxd4.dylib",
                *sorted((ROOT / ".build/native").glob("libaurum-*.dylib"))]:
    bundle(library)

# SDL2-compat loads SDL3 dynamically; otool alone cannot discover it.
if any("sdl2-compat" in str(path.resolve()) for path in copied.values()):
    bundle(pathlib.Path(run("brew", "--prefix", "sdl3")) / "lib/libSDL3.dylib")

copy(ROOT / ".build/app/Aurum64", CONTENTS / "MacOS/Aurum64")
copy(ROOT / ".build/native/AurumRDP.metallib", RES / "AurumRDP.metallib")
copy(ROOT / "third_party/mupen64plus-core/data/mupen64plus.ini", RES / "mupen64plus.ini")
license_files = {
    "Aurum64-GPL-2.txt": ROOT / "LICENSE",
    "AurumRDP-MIT.txt": ROOT / "Sources/AurumRDP/LICENSE",
    "Mupen64Plus-LICENSES.txt": ROOT / "third_party/mupen64plus-core/LICENSES",
    "CXD4-CC0.txt": ROOT / "third_party/mupen64plus-rsp-cxd4/COPYING",
    "CXD4-README.txt": ROOT / "third_party/mupen64plus-rsp-cxd4/README.md",
    "CXD4-OSAL-GPL.txt": ROOT / "third_party/mupen64plus-rsp-cxd4/osal_dynamiclib_unix.c",
    "Core-minizip.txt": ROOT / "third_party/mupen64plus-core/subprojects/minizip/unzip.h",
    "Core-minizip-zip.txt": ROOT / "third_party/mupen64plus-core/subprojects/minizip/zip.h",
    "Core-md5.txt": ROOT / "third_party/mupen64plus-core/subprojects/md5/md5.h",
    "Core-CIC.txt": ROOT / "third_party/mupen64plus-core/src/device/pif/n64_cic_nus_6105.c",
    "Third-Party-Notices.md": ROOT / "docs/DEPENDENCIES.md",
    "Aurum-Core-Patch.txt": ROOT / "patches/mupen64plus-core/0001-interpreter-host-poll.patch",
}
for source in list(copied.values()):
    real = source.resolve()
    if "/Cellar/" in str(real):
        prefix = real.parent.parent
        for filename in ["LICENSE", "LICENSE.txt", "COPYING", "NOTICE"]:
            path = prefix / filename
            if path.is_file():
                license_files[f"{prefix.parent.name}-{filename}"] = path
for name, source in license_files.items():
    copy(source, RES / "Licenses" / name)

minimum = os.environ.get("MACOSX_DEPLOYMENT_TARGET", "26.0")
def version(value):
    return tuple(int(i) for i in value.split("."))
manifest = []
for name, source in copied.items():
    commands = run("otool", "-l", str(LIBS / name)).splitlines()
    for index, line in enumerate(commands):
        if line.strip() == "cmd LC_RPATH":
            path = commands[index + 2].strip().split(" (offset")[0].removeprefix("path ")
            if path.startswith("/"):
                run("install_name_tool", "-delete_rpath", path, str(LIBS / name))
    output = run("vtool", "-show-build", str(LIBS / name))
    for line in output.splitlines():
        fields = line.split()
        if fields and fields[0] == "minos" and version(fields[1]) > version(minimum):
            raise RuntimeError(f"{name} requires macOS {fields[1]}, above deployment target {minimum}")
    for dependency in dependencies(LIBS / name):
        if not dependency.startswith(("@loader_path/", "@rpath/", "/System/", "/usr/lib/")):
            raise RuntimeError(f"Unbundled dependency in {name}: {dependency}")
    manifest.append({"library": name, "source": str(source.resolve()),
                     "sourceSHA256": hashlib.sha256(source.read_bytes()).hexdigest()})
    run("codesign", "--force", "--sign", "-", str(LIBS / name))

pins = {name: SOURCE_INPUTS.pin(name)
        for name in ["mupen64plus-core", "mupen64plus-rsp-cxd4"]}
(RES / "BuildManifest.json").write_text(json.dumps({"minimumMacOS": minimum, "submodules": pins, "libraries": manifest,
    "source": SOURCE_INPUTS.provenance(),
    "corePatchSHA256": hashlib.sha256((ROOT / "patches/mupen64plus-core/0001-interpreter-host-poll.patch").read_bytes()).hexdigest()}, indent=2) + "\n")
plist = {"CFBundleExecutable": "Aurum64", "CFBundleIdentifier": "org.aurum64.Aurum64",
         "CFBundleName": "Aurum64", "CFBundlePackageType": "APPL", "CFBundleShortVersionString": "0.1.0",
         "CFBundleVersion": "1", "LSMinimumSystemVersion": minimum, "NSHighResolutionCapable": True,
         "NSPrincipalClass": "NSApplication", "NSHumanReadableCopyright": "Aurum64 contributors. GPL-2.0-or-later.",
         "CFBundleDocumentTypes": [{"CFBundleTypeName": "Nintendo 64 ROM", "CFBundleTypeRole": "Viewer",
             "LSHandlerRank": "Alternate", "CFBundleTypeExtensions": ["z64", "n64", "v64"]}]}
with (CONTENTS / "Info.plist").open("wb") as file:
    plistlib.dump(plist, file)
run("codesign", "--force", "--sign", "-", str(APP))
run("codesign", "--verify", "--deep", "--strict", str(APP))
FINAL_APP.parent.mkdir(parents=True, exist_ok=True)
if FINAL_APP.exists():
    shutil.rmtree(FINAL_APP)
shutil.move(APP, FINAL_APP)
print(f"Packaged {FINAL_APP}; {len(copied)} bundled libraries; macOS {minimum}+")
