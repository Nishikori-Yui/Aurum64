#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Negative controls for the independent attribute and perspective protocols."""
import pathlib
import struct
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[1])


def reject(args, message):
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True, timeout=30)
    assert result.returncode == 1 and message in result.stderr, (args, result.stdout, result.stderr)


with tempfile.TemporaryDirectory(prefix="aurum-stage-negative-") as temp:
    temp = pathlib.Path(temp)
    for name, option, request_words, response_words in (
        ("attribute", "--triangle-attributes", 47, 25),
        ("perspective", "--perspective", 4, 2),
    ):
        directory = root / f"{name}-tests"
        cases = directory / "cases.bin"
        reference = directory / "reference.bin"
        verifier = root / f"aurum-{name}-tests"
        raw_input = cases.read_bytes()
        raw_output = reference.read_bytes()
        index = 0
        if name == "attribute":
            index = next(i for i in range((len(raw_output) - 8) // (response_words * 4))
                         if struct.unpack_from("<I", raw_output, 8 + i * response_words * 4)[0])
        # Distinct mutations prove row, derivative, TC/depth, and division
        # fields are checked rather than merely counting reference records.
        fields = (3, 7, 10, 15, 18, 19, 22, 24) if name == "attribute" else (0, 1)
        for field in fields:
            corrupted = bytearray(raw_output)
            corrupted[8 + (index * response_words + field) * 4] ^= 1
            damaged = temp / "damaged.bin"
            damaged.write_bytes(corrupted)
            reject([verifier, "--compare", cases, damaged], "mismatch")
        extra = temp / "extra.bin"
        extra.write_bytes(raw_output + raw_output[8:8 + response_words * 4])
        reject([verifier, "--compare", cases, extra], "Trailing")
        truncated = temp / "truncated.bin"
        truncated.write_bytes(raw_input[:11])
        reject([root / "aurum-angrylion-reference", option, truncated, temp / "unused.bin"], "Truncated")
        reject([verifier, "--compare", truncated, reference], "Truncated")
        truncated.write_bytes(raw_output[:-1])
        reject([verifier, "--compare", cases, truncated], "Truncated")
        invalid = bytearray(raw_input[:8 + request_words * 4])
        if name == "attribute":
            struct.pack_into("<I", invalid, 8 + 46 * 4, 1024)
        else:
            struct.pack_into("<I", invalid, 8, 2)
        bad = temp / "invalid.bin"
        bad.write_bytes(invalid)
        reject([root / "aurum-angrylion-reference", option, bad, temp / "unused.bin"], "Invalid")
        reject([verifier, "--compare", bad, reference], "Invalid")
        print(f"PASS: {name} corrupted fields, truncation, invalid inputs and trailing reference rejected")
