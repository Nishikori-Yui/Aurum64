#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Original cross-submission hidden state regression against an offline oracle."""
import json
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "scripts"))
from make_reference_fixtures import (initial, memory, rdp, serialize, set_image,
                                     scissor, fill_rect, set_texture_image, set_tile, load_tile)

native = pathlib.Path(sys.argv[1]).resolve()


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix="aurum-hidden-sequence-") as temp:
    directory = pathlib.Path(temp)
    capture, expected, actual = [directory / name for name in ("input.aurcap", "reference", "metal")]
    for size in (4 << 20, 8 << 20):
        records = [initial(size)]

        def submit(words):
            records.append(rdp(len(records) + 1, 0x10000 + len(records) * 0x100,
                               words + [0xe9000000, 0]))

        def compare():
            capture.write_bytes(serialize(records))
            run([native / "aurum-angrylion-reference", capture, expected])
            stats = json.loads(run([native / "aurum-metal-replay", capture, actual,
                                    native / "AurumRDP.metallib"]))
            assert stats["unsupported"] == 0
            result = json.loads(run([native / "aurum-reference-diff", expected, actual,
                                     capture, "--require-hidden"]))
            assert "hidden" in result["checked"]["layers"]

        submit(set_image(3, 0x200000, 8) + scissor(0, 0, 32, 16) +
               [0xef300000, 0, 0xf7000000, 0x01000101] + fill_rect(0, 0, 28, 12))
        compare()
        submit(set_image(2, 0x200002, 7) + scissor(0, 0, 28, 16) +
               [0xf7000000, 0xaaaa5555] + fill_rect(4, 0, 20, 12))
        compare()
        # Partial Y coverage and odd framebuffer base; next draw replaces the
        # color hidden bits but must preserve earlier Z hidden bits.
        triangle = [0xc8800010, 0x00100000, 5 << 16, 0, 1 << 16, 0, 5 << 16, 0]
        submit(set_image(2, 0x200002, 7) + scissor(0, 0, 28, 13) +
               [0xfe000000, 0x300002, 0xef0000f0, 0x24, 0xee000000, 0x23450008,
                0xfcffffff, 0xfffdf6fb, 0xfa000000, 0x781254ff] + triangle)
        compare()
        records.append(memory(len(records) + 1, 0x200003, b"\x00\x01\xff\xfe"))
        records.append(memory(len(records) + 1, 0x300004, b"\x00\x00"))
        compare()
        submit([0xef0000f0, 1, 0xf9000000, 128, 0xfa000000, 0xeeeeee00] + triangle)
        compare()  # Alpha rejects; neither hidden nor color may be overwritten.
        submit(scissor(0, 0, 28, 10) + [0xef0000f0, 0, 0xfa000000, 0x888888ff] + triangle)
        compare()
        # Same output prefix now contains an unmodeled copy hidden writer. An
        # earlier .hidden file must not survive and falsely claim validity.
        records.append(memory(len(records) + 1, 0x380000, b"\xff\xff" * 8))
        submit(set_texture_image(0, 2, 8, 0x380000) + set_tile(0, 0, 2, 2, 0) +
               load_tile(0, 0, 0, 28, 0) + set_image(2, 0x200000, 8) +
               scissor(0, 0, 32, 4) + [0xef200000, 0, 0xe400c000, 0, 0, 0x10000400])
        capture.write_bytes(serialize(records))
        stats = json.loads(run([native / "aurum-metal-replay", capture, actual,
                                native / "AurumRDP.metallib"]))
        assert stats["unsupported"] == 0
        assert not pathlib.Path(str(actual) + ".hidden").exists()
print("PASS: 4/8 MiB mixed fill/depth/color/CPU-store submissions preserve hidden state; stale output removed")
