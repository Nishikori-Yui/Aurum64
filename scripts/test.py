#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import json
import os
import pathlib
import shutil
import struct
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
replay_dir = tests / "replay"
replay_dir.mkdir(exist_ok=True)
run([root / ".build/native/aurum-aurcap-replay", tests / "fill.aurcap",
     replay_dir / "fill"])
scalar_counts = struct.unpack_from("<3Q", (replay_dir / "fill.counts").read_bytes())
assert scalar_counts[0] == 205 * 240, scalar_counts
assert (replay_dir / "fill.rdram").stat().st_size == 8 * 1024 * 1024
assert (replay_dir / "fill.vi-color-7.rgba8").stat().st_size == 205 * 240 * 4
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
assert reference_stats["layers"] is False
print(reference.stdout, end="")
reference_dir = tests / "reference"
reference_dir.mkdir(exist_ok=True)
layer = subprocess.run([str(root / ".build/native/aurum-angrylion-reference"),
                        tests / "fill.aurcap", reference_dir / "fill"],
                       capture_output=True, text=True, timeout=30)
if layer.returncode:
    raise RuntimeError(layer.stderr)
layer_stats = json.loads(layer.stdout)
assert layer_stats["layers"] is True
manifest = (reference_dir / "fill.manifest").read_text()
assert "format=aurum-reference-layers/1" in manifest
assert (reference_dir / "fill.rdram").stat().st_size == 8 * 1024 * 1024
assert (reference_dir / "fill.hidden").stat().st_size == 4 * 1024 * 1024
assert (reference_dir / "fill.tmem").stat().st_size == 4096
counts = (reference_dir / "fill.counts").read_bytes()
layer_counts = struct.unpack_from("<24Q", counts)
assert all(value == 205 * 240 for value in layer_counts), layer_counts
assert (reference_dir / "fill.vi-color-7.rgba8").stat().st_size == 205 * 240 * 4
assert (reference_dir / "fill.vi-depth-7.rgba8").stat().st_size == 205 * 240 * 4
assert (reference_dir / "fill.vi-coverage-7.rgba8").stat().st_size == 205 * 240 * 4
print(layer.stdout, end="")
run([root / ".build/native/aurum-reference-diff", reference_dir / "fill",
     replay_dir / "fill", tests / "fill.aurcap"])
fixtures = tests / "fixtures"
run([sys.executable, root / "scripts/make_reference_fixtures.py", fixtures])
for fixture in ("matrix", "boundary4", "boundary8", "texture", "texture_stride",
                "texture_block", "texture_tlut", "texture_rgba32",
                "texture_rgba32_offset", "texture_memory_record",
                "texture_memory_record_fill", "texture_ci8", "texture_ia8",
                "texture_i8", "texture_8bit_offset",
                "sampler_rgba16", "sampler_rgba32"):
    fixture_reference = fixtures / f"{fixture}.ref"
    fixture_actual = fixtures / f"{fixture}.actual"
    fixture_reference.mkdir(exist_ok=True)
    fixture_actual.mkdir(exist_ok=True)
    run([root / ".build/native/aurum-angrylion-reference",
         fixtures / f"{fixture}.aurcap", fixture_reference / fixture])
    run([root / ".build/native/aurum-aurcap-replay",
         fixtures / f"{fixture}.aurcap", fixture_actual / fixture])
    diff = subprocess.run([str(root / ".build/native/aurum-reference-diff"),
                           fixture_reference / fixture, fixture_actual / fixture,
                           fixtures / f"{fixture}.aurcap"], capture_output=True, text=True,
                          timeout=30)
    if diff.returncode:
        raise RuntimeError(f"{fixture} differential failed: {diff.stdout}{diff.stderr}")
    print(diff.stdout, end="")
# The fetch test also runs against the reference TMEM layers generated above,
# so the sampler is checked against hardware bytes and not only against the
# uploader that wrote them.
run([root / ".build/native/aurum-texture-fetch-tests", fixtures])

# Pin the memory record's byte mapping to the independently validated fill
# path: both fixtures write the same texels, so the reference engine must
# produce the same RDRAM image for the region either way.
record_ref = fixtures / "texture_memory_record.ref" / "texture_memory_record.rdram"
fill_ref = fixtures / "texture_memory_record_fill.ref" / "texture_memory_record_fill.rdram"
record_region = record_ref.read_bytes()[0x200000:0x200000 + 32]
fill_region = fill_ref.read_bytes()[0x200000:0x200000 + 32]
assert record_region == fill_region and record_region.count(0) < 32, \
    "memory record byte mapping disagrees with the fill path"
print("memory record RDRAM mapping matches the fill path")

actual_dir = reference_dir / "actual"
actual_dir.mkdir(exist_ok=True)
run([root / ".build/native/aurum-angrylion-reference", tests / "fill.aurcap",
     actual_dir / "fill"])
run([root / ".build/native/aurum-reference-diff", reference_dir / "fill",
     actual_dir / "fill", tests / "fill.aurcap"])
state = bytearray((actual_dir / "fill.states").read_bytes())
state[4] ^= 1
(actual_dir / "fill.states").write_bytes(state)
checkpoint_bytes = bytearray((actual_dir / "fill.checkpoints").read_bytes())
checkpoint_bytes[40] ^= 1
(actual_dir / "fill.checkpoints").write_bytes(checkpoint_bytes)
state_diff = subprocess.run([str(root / ".build/native/aurum-reference-diff"),
                             reference_dir / "fill", actual_dir / "fill",
                             tests / "fill.aurcap"], capture_output=True, text=True, timeout=30)
assert state_diff.returncode == 1, state_diff.stderr
state_difference = json.loads(state_diff.stdout)
assert state_difference["firstDifference"] == "checkpoint"
assert state_difference["checkpointIndex"] == 0 and state_difference["submission"] == 1
assert state_difference["rdpState"]["field"] == "fb_size"
assert state_difference["rdpState"]["expected"] == 2
assert state_difference["rdpState"]["actual"] == 3
print(state_diff.stdout, end="")
run([root / ".build/native/aurum-angrylion-reference", tests / "fill.aurcap",
     actual_dir / "fill"])
mutated = bytearray((actual_dir / "fill.rdram").read_bytes())
mutated[1234] ^= 1
(actual_dir / "fill.rdram").write_bytes(mutated)
diff = subprocess.run([str(root / ".build/native/aurum-reference-diff"),
                       reference_dir / "fill", actual_dir / "fill", tests / "fill.aurcap"],
                      capture_output=True, text=True, timeout=30)
assert diff.returncode == 1, diff.stderr
difference = json.loads(diff.stdout)
assert difference["status"] == "differ"
assert difference["firstDifference"] == "byte" and difference["layer"] == "rdram"
assert difference["offset"] == 1234 and difference["expected"] == 0 and difference["actual"] == 1
print(diff.stdout, end="")
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
