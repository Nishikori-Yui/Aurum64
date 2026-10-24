#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate original, self-contained RDP/VI capture fixtures."""
import pathlib
import struct
import sys

HEADER = b"AURCAP" + bytes([1, 0])


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def chunk(kind: int, sequence: int, payload: bytes) -> bytes:
    return struct.pack("<IIQI4x", kind, len(payload), sequence, fnv1a(payload)) + payload


def initial(memory: int) -> bytes:
    return chunk(1, 1, struct.pack("<II", memory, 0))


def rdp(sequence: int, base: int, words: list[int]) -> bytes:
    payload = struct.pack("<IIII", base, base + 4 * len(words), 0, 0)
    payload += b"".join(struct.pack(">I", word) for word in words)
    return chunk(3, sequence, payload)


def vi(sequence: int, kind: int, origin: int, width: int) -> bytes:
    registers = (kind, origin, width, 320, 480, 1024, 1024)
    return chunk(4, sequence, struct.pack("<7I", *registers))


def set_image(kind: int, address: int, width: int) -> list[int]:
    size = 2 if kind == 2 else 3
    return [0xff000000 | size << 19 | (width - 1), address]


def scissor(x0: int, y0: int, x1: int, y1: int) -> list[int]:
    return [0xed000000 | x0 << 12 | y0, x1 << 12 | y1]


def fill_rect(x0: int, y0: int, x1: int, y1: int) -> list[int]:
    return [0xf6000000 | x1 << 12 | y1, x0 << 12 | y0]


def serialize(records: list[bytes]) -> bytes:
    return HEADER + struct.pack("<HHIIQ4x", 1, 0, 0x01020304, 32, 64 << 20) + b"".join(records)


def matrix_fixture() -> bytes:
    records = [initial(8 << 20)]
    sequence = 2
    bases = iter(range(0x100000, 0x100000 + 0x100 * 8, 0x100))
    continuation = 0

    def submit(words):
        nonlocal sequence
        nonlocal continuation
        base = continuation if continuation else next(bases)
        continuation = 0 if words[-1] != 0xf7000000 else base + 4 * len(words)
        records.append(rdp(sequence, base, words))
        sequence += 1

    submit(set_image(2, 0x200000, 64) + scissor(0, 0, 256, 128) +
           [0xef300000, 0, 0xf7000000, 0xEB0BEB0B] +
           fill_rect(4, 4, 40, 20) + [0xe9000000, 0])
    submit(set_image(3, 0x300000, 64) + scissor(0, 0, 256, 128) +
           [0xef300000, 0, 0xf7000000, 0x12345678] +
           fill_rect(0, 0, 32, 16) + [0xe9000000, 0])
    submit(set_image(2, 0x240000, 64) + scissor(32, 16, 96, 48) +
           [0xef300000, 0, 0xe9000000, 0])
    submit(set_image(2, 0x280000, 64) + scissor(0, 0, 256, 128) +
           [0xef300000, 0, 0xe9000000, 0])
    submit([0xf7000000])
    submit([0x7C1F801, fill_rect(4, 4, 40, 20)[0], 0, 0xe9000000, 0])
    records.append(vi(sequence, 2, 0x200000, 64)); sequence += 1
    records.append(vi(sequence, 3, 0x300000, 64)); sequence += 1
    return serialize(records)


def boundary_fixture(memory: int) -> bytes:
    base = memory - 0x1000
    framebuffer = memory - 0x100
    records = [
        initial(memory),
        rdp(2, base, set_image(2, framebuffer, 64) + scissor(0, 0, 256, 8) +
            [0xef300000, 0, 0xf7000000, 0xF81C07C1] +
            fill_rect(0, 0, 256, 8) + [0xe9000000, 0]),
        vi(3, 2, framebuffer, 64),
    ]
    return serialize(records)


def write_all(destination: pathlib.Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    fixtures = {
        "matrix.aurcap": matrix_fixture(),
        "boundary4.aurcap": boundary_fixture(4 << 20),
        "boundary8.aurcap": boundary_fixture(8 << 20),
    }
    for name, data in fixtures.items():
        (destination / name).write_bytes(data)
        print(f"Generated {destination / name} ({len(data)} bytes)")


if __name__ == "__main__":
    write_all(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".build/tests/fixtures"))
