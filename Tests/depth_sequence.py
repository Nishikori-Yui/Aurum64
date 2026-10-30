#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise persistent Z/coverage dependencies at each original draw prefix."""
import json
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "scripts"))
from make_reference_fixtures import initial, rdp, serialize, set_image, scissor

native = pathlib.Path(sys.argv[1]).resolve()


def run(args):
    result = subprocess.run([str(a) for a in args], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (args, result.stdout, result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix="aurum-depth-sequence-") as temp:
    directory = pathlib.Path(temp)
    capture, reference, actual = [directory / s for s in ("input.aurcap", "reference", "metal")]
    for size in (4 << 20, 8 << 20):
        for left_major in (0, 1):
            records = [initial(size)]
            # Z's exponent changes the delta tolerance. The final value is max Z.
            zs = (0x2000, 0x20000, 0x30000, 0x38000, 0x3c000, 0x3e000, 0x3f000, 0x3ffff)

            def batch(phase):
                words = []
                for case in range(64):
                    fb = 0x200002 + case * 256
                    zb = 0x300002 + case * 256
                    z = zs[(case // 8) % 8]
                    delta = (1, 2, 4, 8)[case % 4]
                    clip_y = 1 if phase == 0 else 2  # Grow edge coverage on later draws.
                    modes = 0x20 if phase == 0 else (0x10, 0x30, 0x50, 0x70)[case % 4]
                    if phase == 1:
                        z = min(0x3ffff, z + 16)  # Stored hidden delta can make this pass.
                    elif phase == 2:
                        z = max(0, z - 64)
                    elif phase == 3:
                        z = min(0x3ffff, z + 1024)
                    elif phase == 4:
                        z = max(0, z - 1)
                    color = (0xff0000ff, 0x00ff00ff, 0x0000ffff, 0xffff00ff, 0x00ffffff)[phase]
                    words += set_image(2, fb, 7) + scissor(0, 0, 28, clip_y)
                    words += [0xfe000000, zb, 0xef0000f0, modes,
                              0xfcffffff, 0xfffdf6fb, 0xfa000000, color]
                    xh, xm = (1, 5) if left_major else (5, 1)
                    # Constant interpolated Z with nonzero edge-independent delta.
                    # dx=0, dy/de=delta keeps row0 depth exact for this orientation.
                    words += [0xc9000004 | (left_major << 23), 0x00040000,
                              xm << 16, 0, xh << 16, 0, xm << 16, 0,
                              (z << 13) & 0xffffffff, 0, delta << 16, delta << 16]
                words += [0xe9000000, 0]
                records.append(rdp(len(records) + 1, 0x10000 + phase * 0x10000, words))

            for phase in range(5):
                batch(phase)
                capture.write_bytes(serialize(records))
                run([native / "aurum-angrylion-reference", capture, reference])
                stats = json.loads(run([native / "aurum-metal-replay", capture, actual,
                                        native / "AurumRDP.metallib"]))
                assert stats["unsupported"] == 0
                result = json.loads(run([native / "aurum-reference-diff", reference,
                                         actual, capture, "--require-hidden"]))
                assert "hidden" in result["checked"]["layers"]
            # Stride-one rows visit overlapping pixels: a nearer early row
            # must survive farther candidates, not be removed during planning.
            xh, xm = (0, 6) if left_major else (6, 0)
            words = set_image(2, 0x280000, 1) + scissor(0, 0, 24, 16)
            words += [0xfe000000, 0x380000, 0xef0000f0, 0x24,
                      0xfcffffff, 0xfffdf6fb, 0xfa000000, 0xff0000ff,
                      0xee000000, 0x7fff0001,
                      0xc8000010 | (left_major << 23), 0x00100000,
                      xm << 16, 0, xh << 16, 0, xm << 16, 0,
                      0xef0000f0, 0x30, 0xfa000000, 0x0000ffff,
                      0xc9000010 | (left_major << 23), 0x00100000,
                      xm << 16, 0, xh << 16, 0, xm << 16, 0,
                      0x10000000, 0, 0x08000000, 0x08000000, 0xe9000000, 0]
            records.append(rdp(len(records) + 1, 0x70000, words))
            capture.write_bytes(serialize(records))
            run([native / "aurum-angrylion-reference", capture, reference])
            stats = json.loads(run([native / "aurum-metal-replay", capture, actual,
                                    native / "AurumRDP.metallib"]))
            assert stats["unsupported"] == 0
            run([native / "aurum-reference-diff", reference, actual, capture, "--require-hidden"])
print("PASS: 4/8 MiB opaque compare prefixes, both edges, exponent/delta/coverage, update on/off")
