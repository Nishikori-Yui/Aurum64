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
tests = pathlib.Path(os.environ.get("AURUM_TEST_OUTPUT_DIR", root / ".build/tests")).resolve()
tests.mkdir(parents=True, exist_ok=True)
def run(args, **kwargs):
    # The full Metal matrix includes ordered depth-sequence replay on hosted
    # runners; bound the whole command without making slow CI look hung.
    subprocess.run([str(x) for x in args], cwd=root, check=True, timeout=120, **kwargs)

run(["ctest", "--test-dir", root / ".build/native", "--output-on-failure"])
# The CTest fixture chain always runs the independent coordinate reference.
# Corrupt one mapped coordinate and one texel result to prove both comparison
# paths reject bad output, and reject a probe truncated between word bytes.
coordinate_dir = root / ".build/native/coordinate-tests"
with tempfile.TemporaryDirectory(prefix="aurum-coordinate-negative-") as temp:
    coordinate_cases = coordinate_dir / "cases.bin"
    coordinate_reference = coordinate_dir / "reference.bin"
    damaged = pathlib.Path(temp) / "reference.bin"
    for field in (0, 7):
        shutil.copyfile(coordinate_reference, damaged)
        with damaged.open("r+b") as output:
            for offset in range(8 + field * 4, min(damaged.stat().st_size, 8 + 44 * 1024), 44):
                output.seek(offset)
                value = struct.unpack("<I", output.read(4))[0]
                output.seek(offset)
                output.write(struct.pack("<I", value ^ 1))
                if field == 0:
                    break
        rejected = subprocess.run([str(root / ".build/native/aurum-texture-coordinate-tests"),
                                   "--compare", coordinate_cases, damaged],
                                  capture_output=True, text=True, timeout=30)
        assert rejected.returncode == 1 and f"field={field}" in rejected.stderr, rejected.stderr
    truncated_probe = pathlib.Path(temp) / "truncated.bin"
    truncated_probe.write_bytes(coordinate_cases.read_bytes()[:13])
    rejected = subprocess.run([str(root / ".build/native/aurum-angrylion-reference"),
                               "--texture-coordinates", truncated_probe, damaged],
                              capture_output=True, text=True, timeout=30)
    assert rejected.returncode == 1 and "Truncated coordinate probe" in rejected.stderr
print("PASS: independent coordinate/texel negative controls and truncated probe rejection")
# The CPU planner and the actual Metal kernel must each match the external
# renderer. Comparing Metal only to our own span implementation is circular.
for mode in ("flat", "shaded", "textured", "depth", "depth-gradient", "primitive-depth",
             "depth-compare", "depth-compare-gradient", "depth-compare-primitive"):
    for memory_size in (4 << 20, 8 << 20):
        prefix = tests / f"triangles-{memory_size}-{mode}"
        capture = pathlib.Path(str(prefix) + ".aurcap")
        expected, actual = str(prefix) + ".ref", str(prefix) + ".metal"
        options = {
            "flat": [], "depth-compare-gradient": ["--depth-compare", "--depth-gradient"],
            "depth-compare-primitive": ["--depth-compare", "--primitive-depth"],
        }.get(mode, [f"--{mode}"])
        run([sys.executable, root / "scripts/make_triangle_fixtures.py", capture, memory_size, *options])
        run([root / ".build/native/aurum-angrylion-reference", capture, expected])
        run([root / ".build/native/aurum-triangle-tests", capture, expected + ".rdram"])
        rendered = subprocess.run([str(root / ".build/native/aurum-metal-replay"), capture, actual,
                                   root / ".build/native/AurumRDP.metallib"],
                                  capture_output=True, text=True, timeout=30)
        assert rendered.returncode == 0, rendered.stderr
        assert json.loads(rendered.stdout)["unsupported"] == 0, rendered.stdout
        run([root / ".build/native/aurum-reference-diff", expected, actual, capture, "--require-hidden"])
        with tempfile.TemporaryDirectory(prefix="aurum-triangle-negative-") as temp:
            damaged = pathlib.Path(temp) / "reference.rdram"
            data = bytearray(pathlib.Path(expected + ".rdram").read_bytes())
            data[0x200000] ^= 1
            damaged.write_bytes(data)
            rejected = subprocess.run([str(root / ".build/native/aurum-triangle-tests"), capture, damaged],
                                      capture_output=True, text=True, timeout=30)
            assert rejected.returncode == 1 and "byte=2097152" in rejected.stderr, rejected.stderr
            data[0x200000] ^= 1
            damaged.write_bytes(data)
            hidden = bytearray(pathlib.Path(expected + ".hidden").read_bytes())
            # Choose an actually written pixel, not an untouched seed byte.
            address = next(a for a in range(0x200000, 0x2a0000, 2)
                           if data[a ^ 2:(a ^ 2) + 2] != b"\x5a\x6b")
            hidden[address // 2] ^= 1
            if mode == "textured":
                shutil.copyfile(expected + ".tmem", damaged.with_suffix(".tmem"))
            damaged.with_suffix(".hidden").write_bytes(hidden)
            rejected = subprocess.run([str(root / ".build/native/aurum-triangle-tests"), capture, damaged],
                                      capture_output=True, text=True, timeout=30)
            assert rejected.returncode == 1 and "hidden=" in rejected.stderr, rejected.stderr
            if mode == "textured":
                hidden[address // 2] ^= 1
                damaged.with_suffix(".hidden").write_bytes(hidden)
                corrupted = bytearray(pathlib.Path(expected + ".tmem").read_bytes())
                corrupted[128 * 8] ^= 1
                damaged.with_suffix(".tmem").write_bytes(corrupted)
                rejected = subprocess.run([str(root / ".build/native/aurum-triangle-tests"), capture, damaged],
                                          capture_output=True, text=True, timeout=30)
                assert rejected.returncode == 1 and "TMEM mismatch" in rejected.stderr, rejected.stderr
print("PASS: 4/8 MiB flat/shaded/textured/depth triangles match independent layers; negative controls reject corruption")
run([sys.executable, root / "scripts/make_test_rom.py", tests / "fill.z64"])
run([sys.executable, root / "scripts/make_test_rom.py", tests / "texture.z64", "--texture"])
run([sys.executable, root / "scripts/make_test_rom.py", tests / "texture-ci8.z64", "--ci-texture"])
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
    texture_env = {k: v for k, v in env.items() if k not in ("AURUM_AURCAP", "AURUM_RDP_TRACE")}
    run([root / ".build/native/aurum-smoke", tests / "texture.z64",
         relocated / "Contents/Frameworks", relocated / "Contents/Resources",
         tests / "texture-user", "--texture"], env=texture_env)
    run([root / ".build/native/aurum-smoke", tests / "texture-ci8.z64",
         relocated / "Contents/Frameworks", relocated / "Contents/Resources",
         tests / "texture-ci-user", "--ci-texture"], env=texture_env)
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

# Rebuild a validly checksummed capture whose RDP submission metadata claims
# more bytes than its payload contains. Both replay executables must reject it
# before indexing into the command payload.
capture_bytes = bytearray((tests / "fill.aurcap").read_bytes())
cursor = 32
malformed_chunks = []
while cursor < len(capture_bytes):
    chunk_type, chunk_length = struct.unpack_from("<II", capture_bytes, cursor)
    chunk = bytearray(capture_bytes[cursor:cursor + 24 + chunk_length])
    payload = bytearray(chunk[24:])
    if chunk_type == 3:
        payload = payload[:-4]
        struct.pack_into("<I", chunk, 4, len(payload))
        checksum = 2166136261
        for value in payload:
            checksum = ((checksum ^ value) * 16777619) & 0xffffffff
        struct.pack_into("<I", chunk, 16, checksum)
        chunk = chunk[:24] + payload
        malformed_chunks.append(chunk)
    else:
        malformed_chunks.append(chunk)
    cursor += 24 + chunk_length
assert malformed_chunks and len(malformed_chunks[0])
malformed_capture = tests / "bad-rdp-range.aurcap"
malformed_capture.write_bytes(capture_bytes[:32] + b"".join(malformed_chunks))
for executable in ("aurum-aurcap-replay", "aurum-angrylion-reference"):
    rejected = subprocess.run([str(root / ".build/native" / executable), malformed_capture],
                              capture_output=True, text=True, timeout=30)
    assert rejected.returncode != 0, f"{executable} accepted mismatched RDP payload length"

# Move the known-good RDP words to an XBUS interval crossing the 4 KiB DMEM
# boundary. The reference backend must apply the documented 0xfff wrap.
xbus_chunks = []
capture_bytes = bytearray((tests / "fill.aurcap").read_bytes())
cursor = 32
while cursor < len(capture_bytes):
    _, chunk_length = struct.unpack_from("<II", capture_bytes, cursor)
    chunk = bytearray(capture_bytes[cursor:cursor + 24 + chunk_length])
    if struct.unpack_from("<I", chunk)[0] == 3:
        struct.pack_into("<I", chunk, 24, 0xFD8)
        struct.pack_into("<I", chunk, 28, 0x1008)
        struct.pack_into("<I", chunk, 36, 1)
        payload = chunk[24:]
        checksum = 2166136261
        for value in payload:
            checksum = ((checksum ^ value) * 16777619) & 0xffffffff
        struct.pack_into("<I", chunk, 16, checksum)
        xbus_chunks.append(chunk)
    else:
        xbus_chunks.append(chunk)
    cursor += 24 + chunk_length
xbus_capture = tests / "xbus-wrap.aurcap"
xbus_capture.write_bytes(capture_bytes[:32] + b"".join(xbus_chunks))
xbus_reference = tests / "xbus-reference/fill"
xbus_actual = tests / "xbus-actual/fill"
xbus_reference.parent.mkdir(exist_ok=True)
xbus_actual.parent.mkdir(exist_ok=True)
for executable, prefix in (("aurum-angrylion-reference", xbus_reference),
                           ("aurum-aurcap-replay", xbus_actual)):
    run([root / ".build/native" / executable, xbus_capture, prefix])
run([root / ".build/native/aurum-reference-diff", xbus_reference, xbus_actual, xbus_capture])

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
with tempfile.TemporaryDirectory(prefix="aurum-checkpoint-metadata-") as temp:
    changed = pathlib.Path(temp) / "actual"
    changed.mkdir()
    for artifact in reference_dir.glob("fill.*"):
        shutil.copy2(artifact, changed)
    checkpoints = bytearray((changed / "fill.checkpoints").read_bytes())
    struct.pack_into("<Q", checkpoints, 16, struct.unpack_from("<Q", checkpoints, 16)[0] + 1)
    (changed / "fill.checkpoints").write_bytes(checkpoints)
    mismatch = subprocess.run([str(root / ".build/native/aurum-reference-diff"),
                               reference_dir / "fill", changed / "fill", tests / "fill.aurcap"],
                              capture_output=True, text=True, timeout=30)
    assert mismatch.returncode == 1 and '"field":"submission"' in mismatch.stdout, \
        "Reference diff ignored checkpoint event metadata"
with tempfile.TemporaryDirectory(prefix="aurum-missing-layer-") as temp:
    missing = pathlib.Path(temp) / "actual"
    missing.mkdir()
    for artifact in reference_dir.glob("fill.*"):
        shutil.copy2(artifact, missing)
    (missing / "fill.rdram").unlink()
    absent = subprocess.run([str(root / ".build/native/aurum-reference-diff"),
                             reference_dir / "fill", missing / "fill", tests / "fill.aurcap"],
                            capture_output=True, text=True, timeout=30)
    assert absent.returncode == 1 and '"firstDifference":"layer-presence"' in absent.stdout, \
        "Reference diff accepted a missing required layer"
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
# IA16 is intentionally reference-only here: the production/scalar upload
# path has not implemented that format yet.
ia16_reference = fixtures / "texture_ia16.ref"
ia16_reference.mkdir(exist_ok=True)
run([root / ".build/native/aurum-angrylion-reference",
     fixtures / "texture_ia16.aurcap", ia16_reference / "texture_ia16"])
run([root / ".build/native/aurum-texture-fetch-tests", fixtures])

# Exercise the same Renderer used by the production plugin, in a process
# separate from the pinned software reference. Scalar replay has no draw
# handler for these textures and must still report them as unsupported.
for fixture in ("texture_copy", "texture_copy_matrix4", "texture_copy_matrix8",
                "texture_copy_feedback", "texture_ci8_copy", "texture_ci4_copy",
                "texture_ci8_matrix", "texture_ci4_matrix", "texture_ci8_ports",
                "texture_ci4_ports", "texture_ia8_copy", "texture_i8_copy",
                "texture_ia16_copy", "texture_ia8_matrix", "texture_i8_matrix",
                "texture_ia16_matrix"):
    capture = fixtures / f"{fixture}.aurcap"
    expected, actual = fixtures / f"{fixture}.ref", fixtures / f"{fixture}.metal"
    run([root / ".build/native/aurum-angrylion-reference", capture, expected])
    rendered = subprocess.run([str(root / ".build/native/aurum-metal-replay"), capture, actual,
                               root / ".build/native/AurumRDP.metallib"],
                              capture_output=True, text=True, timeout=30)
    assert rendered.returncode == 0, rendered.stderr
    unsupported = json.loads(rendered.stdout)["unsupported"]
    if fixture in ("texture_ia8_matrix", "texture_i8_matrix"):
        assert unsupported == 64, rendered.stdout
        # Addressing variants that diverge must fail closed before any draw writes.
        framebuffer = pathlib.Path(str(actual) + ".rdram").read_bytes()
        base = 0x300000
        length = 8192 * 64
        expected = bytearray(length)
        for index in range(length):
            guest_address = base + index
            if guest_address >= base + 2:
                source_offset = guest_address - (base + 2)
                if source_offset < length:
                    expected[(guest_address ^ 3) - base] = (0x6b, 0x5a)[source_offset & 1]
        assert framebuffer[base:base + length] == expected, fixture
        continue
    assert unsupported == 0, rendered.stdout
    assert "engine=aurum-metal-replay" in pathlib.Path(str(actual) + ".manifest").read_text()
    run([root / ".build/native/aurum-reference-diff", expected, actual, capture])
    if fixture == "texture_copy":
        scalar = subprocess.run([str(root / ".build/native/aurum-aurcap-replay"), capture],
                                capture_output=True, text=True, timeout=30)
        assert scalar.returncode == 0 and json.loads(scalar.stdout)["unsupported"] == 1
print("PASS: original RGBA16/CI4/CI8/IA8/I8/IA16 supported Metal texture draws match independent reference layers; unverified IA/I matrices fail closed")

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
    byte_capture = tests / "byte-capped.aurcap"
    capped_env.pop("AURUM_AURCAP_MAX_VI")
    capped_env.update({"AURUM_AURCAP": str(byte_capture), "AURUM_AURCAP_MAX_BYTES": "200"})
    byte_result = subprocess.run([str(root / ".build/native/aurum-smoke"), tests / "fill.z64",
         capped / "Contents/Frameworks", capped / "Contents/Resources", pathlib.Path(temp) / "byte-user"],
         timeout=30, text=True, capture_output=True, env=capped_env)
    assert byte_result.returncode == 0, byte_result.stderr
    byte_data = byte_capture.read_bytes()
    assert len(byte_data) <= 200 and byte_data.endswith(b"byte limit"), \
        "Capture exceeded its byte budget or omitted the terminal reason"
    run([root / ".build/native/aurum-aurcap-replay", byte_capture])
    print("PASS: capture byte budget includes the terminal record")
truncated = tests / "truncated.rdp"
truncated.write_bytes((tests / "fill.rdp").read_bytes()[:-4])
result = subprocess.run([str(root / ".build/native/aurum-rdp-tests"), str(root / ".build/native/AurumRDP.metallib"), str(truncated)],
                        capture_output=True, text=True, timeout=30)
assert result.returncode == 1 and "Incomplete replay command" in result.stderr, "Truncated replay was not rejected"
print("PASS: relocated self-contained app libraries, signed bundle, RDP capture/replay")
