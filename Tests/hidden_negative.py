#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove required hidden-layer comparisons cannot pass by omitting output."""
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

native = pathlib.Path(sys.argv[1]).resolve()
source = native / "triangle-tests/primitive-depth-metal"
reference = native / "triangle-tests/primitive-depth-reference"
capture = native / "triangle-tests/primitive-depth.aurcap"
with tempfile.TemporaryDirectory(prefix="aurum-hidden-negative-") as temp:
    actual = pathlib.Path(temp) / "actual"
    for suffix in (".rdram", ".checkpoints", ".states", ".tmem", ".hidden"):
        shutil.copyfile(str(source) + suffix, str(actual) + suffix)
    hidden_file = pathlib.Path(str(actual) + ".hidden")
    hidden = hidden_file.read_bytes()

    def compare():
        return subprocess.run([native / "aurum-reference-diff", reference, actual,
                               capture, "--require-hidden"], capture_output=True,
                              text=True, timeout=30)

    assert compare().returncode == 0
    for offset in (0, next(i for i in range(0x180000, len(hidden)) if hidden[i] != 4)):
        broken = bytearray(hidden)
        broken[offset] ^= 1
        hidden_file.write_bytes(broken)
        result = compare()
        assert result.returncode == 1 and json.loads(result.stdout)["layer"] == "hidden", result
    hidden_file.write_bytes(hidden[:-1])
    result = compare()
    assert result.returncode == 1 and json.loads(result.stdout)["firstDifference"] == "layer-size"
    hidden_file.unlink()
    result = compare()
    assert result.returncode == 1 and json.loads(result.stdout)["firstDifference"] == "required-layer-missing"
print("PASS: hidden untouched/depth corruption, truncated layer and missing layer rejected")
