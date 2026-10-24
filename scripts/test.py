#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parent.parent
tests = root / ".build/tests"
tests.mkdir(parents=True, exist_ok=True)
def run(args, **kwargs):
    subprocess.run([str(x) for x in args], cwd=root, check=True, timeout=45, **kwargs)

run(["ctest", "--test-dir", root / ".build/native", "--output-on-failure"])
run([sys.executable, root / "scripts/make_test_rom.py", tests / "fill.z64"])
with tempfile.TemporaryDirectory(prefix="aurum-relocated-") as temp:
    relocated = pathlib.Path(temp) / "Aurum64.app"
    shutil.copytree(root / "dist/Aurum64.app", relocated)
    # Move the bundle outside the repository; check actual dyld loads, not just otool.
    env = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_")}
    env.update({"DYLD_PRINT_LIBRARIES": "1", "AURUM_RDP_TRACE": str(tests / "fill.rdp"),
                "AURUM_AURCAP": str(tests / "fill.aurcap")})
    output = subprocess.run([str(root / ".build/native/aurum-smoke"), str(tests / "fill.z64"),
         str(relocated / "Contents/Frameworks"), str(relocated / "Contents/Resources"), str(tests / "user")],
         timeout=30, text=True, capture_output=True, env=env)
    (tests / "dyld.log").write_text(output.stdout + output.stderr)
    print(output.stdout, end="")
    if output.returncode:
        print(output.stderr)
        raise RuntimeError(f"ROM smoke test exited {output.returncode}")
    loaded = [line for line in output.stderr.splitlines() if line.startswith("dyld[")]
    assert not any("/opt/homebrew/" in line or "libaurum-" in line and str(root) in line for line in loaded), "Unbundled library loaded"
    assert any("libSDL3" in line for line in loaded) or not any("sdl2-compat" in line for line in loaded)
    run([sys.executable, root / "Tests/lifecycle.py", tests, relocated])
    run(["codesign", "--verify", "--deep", "--strict", relocated])
    # Use the same bundle path resolution and transient Swift-to-C strings as the UI.
    # Modify only this throwaway test copy, then sign it before execution.
    swift_runner = relocated / "Contents/MacOS/Aurum64"
    shutil.copy2(root / ".build/native/aurum-swift-startup-tests", swift_runner)
    run(["codesign", "--force", "--sign", "-", relocated])
    run([swift_runner, tests / "fill.z64", pathlib.Path(temp) / "Swift Startup Tests"])
run([root / ".build/native/aurum-rdp-tests", root / ".build/native/AurumRDP.metallib",
     tests / "fill.rdp", tests / "fill.aurcap"])
replay = subprocess.run([str(root / ".build/native/aurum-aurcap-replay"), tests / "fill.aurcap"],
                        capture_output=True, text=True, timeout=30)
assert replay.returncode == 0 and '"submissions":1' in replay.stdout and \
       '"commands":6' in replay.stdout and '"unsupported":0' in replay.stdout, replay.stderr
print(replay.stdout, end="")
reference = subprocess.run([str(root / ".build/native/aurum-angrylion-reference"),
                            tests / "fill.aurcap"], capture_output=True, text=True, timeout=30)
if reference.returncode:
    raise RuntimeError(reference.stderr)
reference_stats = json.loads(reference.stdout)
assert reference_stats["engine"] == "angrylion-rdp-plus"
assert reference_stats["commit"] == "9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc"
assert reference_stats["frames"] == 8 and reference_stats["invalidFrames"] == 0
assert reference_stats["width"] > 0 and reference_stats["height"] > 0
assert reference_stats["nonzero16"] > 0
print(reference.stdout, end="")
with tempfile.TemporaryDirectory(prefix="aurum-capped-") as temp:
    capped = pathlib.Path(temp) / "Aurum64.app"
    shutil.copytree(root / "dist/Aurum64.app", capped)
    capped_capture = tests / "capped.aurcap"
    capped_env = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_")}
    capped_env.update({"AURUM_AURCAP": str(capped_capture), "AURUM_AURCAP_MAX_VI": "1"})
    capped_result = subprocess.run([str(root / ".build/native/aurum-smoke"), tests / "fill.z64",
         capped / "Contents/Frameworks", capped / "Contents/Resources", pathlib.Path(temp) / "user"],
         timeout=30, text=True, capture_output=True, env=capped_env)
    if capped_result.returncode:
        print(capped_result.stderr)
        raise RuntimeError(f"Capped capture smoke exited {capped_result.returncode}")
    run([root / ".build/native/aurum-rdp-tests", root / ".build/native/AurumRDP.metallib",
         tests / "fill.rdp", tests / "fill.aurcap", capped_capture])
truncated = tests / "truncated.rdp"
truncated.write_bytes((tests / "fill.rdp").read_bytes()[:-4])
result = subprocess.run([str(root / ".build/native/aurum-rdp-tests"), str(root / ".build/native/AurumRDP.metallib"), str(truncated)],
                        capture_output=True, text=True, timeout=30)
assert result.returncode == 1 and "Incomplete replay command" in result.stderr, "Truncated replay was not rejected"
print("PASS: relocated self-contained app libraries, signed bundle, RDP capture/replay")
