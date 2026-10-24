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


def memory(sequence: int, address: int, data: bytes) -> bytes:
    """A record that places guest-order bytes directly into RDRAM.

    Needed for texel formats a fill cannot express exactly, such as 8-bit
    indices. The payload is a little-endian N64 byte address followed by that
    many guest-order bytes.
    """
    return chunk(2, sequence, struct.pack("<I", address) + data)


def rdp(sequence: int, base: int, words: list[int]) -> bytes:
    payload = struct.pack("<IIII", base, base + 4 * len(words), 0, 0)
    payload += b"".join(struct.pack(">I", word) for word in words)
    return chunk(3, sequence, payload)


def vi(sequence: int, kind: int, origin: int, width: int) -> bytes:
    registers = (kind, origin, width, 320, 480, 1024, 1024)
    return chunk(4, sequence, struct.pack("<7I", *registers))


def set_texture_image(fmt: int, size: int, width: int, address: int) -> list[int]:
    """Opcode 0x3d. Width is stored as width-1."""
    return [0xfd000000 | fmt << 21 | size << 19 | (width - 1), address]


def set_tile(tile: int, fmt: int, size: int, line: int, tmem: int) -> list[int]:
    """Opcode 0x35. line and tmem count 8-byte TMEM units."""
    return [0xf5000000 | fmt << 21 | size << 19 | line << 9 | tmem, tile << 24]


def load_tile(tile: int, sl: int, tl: int, sh: int, th: int) -> list[int]:
    """Opcode 0x34. Coordinates are 10.2 fixed point, inclusive."""
    return [0xf4000000 | sl << 12 | tl, tile << 24 | sh << 12 | th]


def load_block(tile: int, sl: int, tl: int, sh: int, dxt: int) -> list[int]:
    """Opcode 0x33."""
    return [0xf3000000 | sl << 12 | tl, tile << 24 | sh << 12 | dxt]


def load_tlut(tile: int, sl: int, tl: int, sh: int, th: int) -> list[int]:
    """Opcode 0x30."""
    return [0xf0000000 | sl << 12 | tl, tile << 24 | sh << 12 | th]


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


TEXTURE_SRC = 0x200000
TEXTURE_FB = 0x300000
TEXTURE_SYNC = [0xe9000000, 0]


def _texture_seed(width: int, height: int) -> list[int]:
    """Fill the texture source so every texel holds a distinct RGBA16 value.

    A distinct value per texel is what makes an addressing error show up as a
    differing byte instead of a plausible-looking tile.
    """
    words = scissor(0, 0, width * 4, height * 4) + [0xef300000, 0]
    for y in range(height):
        for x in range(width):
            value = 0x1000 + y * 0x100 + x
            words += set_image(2, TEXTURE_SRC, width) + [0xf7000000, value << 16 | value]
            words += [0xf6000000 | (x * 4 + 3) << 12 | (y * 4 + 3),
                      (x * 4) << 12 | (y * 4)]
    return words + TEXTURE_SYNC


def _texture_capture(width: int, height: int, uploads: list[list[int]]) -> bytes:
    """Build a capture that seeds the source, then applies each upload."""
    records = [initial(8 << 20)]
    sequence = 2
    bases = iter(range(0x100000, 0x100000 + 0x100 * 16, 0x100))

    def submit(words: list[int]) -> None:
        nonlocal sequence
        records.append(rdp(sequence, next(bases), words))
        sequence += 1

    submit(_texture_seed(width, height))
    submit(set_texture_image(0, 2, width, TEXTURE_SRC) + uploads[0])
    for upload in uploads[1:]:
        submit(upload)

    records.append(vi(sequence, 2, TEXTURE_FB, 64))
    sequence += 1
    return serialize(records)


def _texture_seed32(width: int, height: int, source: int = TEXTURE_SRC) -> list[int]:
    """Fill the RGBA32 source so both halfwords of every texel are distinct.

    RGBA32 splits a texel across the two TMEM banks, so the high and low
    halfword must each vary with x and y for the column order in both banks
    to be observable.
    """
    words = scissor(0, 0, width * 4, height * 4) + [0xef300000, 0]
    for y in range(height):
        for x in range(width):
            value = ((0x1000 + y * 16 + x) << 16) | (0x8000 + y * 16 + x)
            words += set_image(3, source, width) + [0xf7000000, value]
            words += [0xf6000000 | (x * 4 + 3) << 12 | (y * 4 + 3),
                      (x * 4) << 12 | (y * 4)]
    return words + TEXTURE_SYNC


def _texture_capture32(width: int, height: int,
                       uploads: list[list[int]],
                       source: int = TEXTURE_SRC) -> bytes:
    records = [initial(8 << 20)]
    sequence = 2
    bases = iter(range(0x100000, 0x100000 + 0x100 * 16, 0x100))

    def submit(words: list[int]) -> None:
        nonlocal sequence
        records.append(rdp(sequence, next(bases), words))
        sequence += 1

    submit(_texture_seed32(width, height, source))
    submit(set_texture_image(0, 3, width, source) + uploads[0])
    for upload in uploads[1:]:
        submit(upload)

    records.append(vi(sequence, 2, TEXTURE_FB, 64))
    sequence += 1
    return serialize(records)


def texture_fixture() -> bytes:
    """RGBA16 uploads exercising the TMEM addressing rules.

    Covers LoadTile with a non-zero tmem base and the odd-row dword swap,
    LoadTile reaching TMEM bank 1, LoadBlock with dxt=0, and LoadTLUT.
    """
    return _texture_capture(16, 16, [
        set_tile(0, 0, 2, 2, 1) + load_tile(0, 0, 0, 7 << 2, 7 << 2) + TEXTURE_SYNC,
        set_tile(1, 0, 2, 2, 256) + load_tile(1, 0, 0, 3 << 2, 3 << 2) + TEXTURE_SYNC,
        set_tile(2, 0, 2, 2, 8) + load_block(2, 0, 0, 12, 0) + TEXTURE_SYNC,
        set_tile(3, 0, 2, 2, 0) + load_tlut(3, 0, 0, 20, 0) + TEXTURE_SYNC,
    ])


def texture_rgba32_fixture() -> bytes:
    """RGBA32 LoadTile, which splits each texel across both TMEM banks.

    Covers a non-zero tmem base and line strides of 2 and 4. RGBA32 LoadBlock
    is not covered because it is reported as unsupported rather than modeled.
    """
    return _texture_capture32(8, 8, [
        set_tile(0, 0, 3, 2, 0) + load_tile(0, 0, 0, 7 << 2, 7 << 2) + TEXTURE_SYNC,
        set_tile(1, 0, 3, 4, 0) + load_tile(1, 0, 0, 7 << 2, 7 << 2) + TEXTURE_SYNC,
        set_tile(2, 0, 3, 2, 1) + load_tile(2, 0, 0, 7 << 2, 7 << 2) + TEXTURE_SYNC,
    ])


def texture_rgba32_offset_fixture() -> bytes:
    """RGBA32 LoadTile from a word-aligned but not 8-byte-aligned address.

    Guards the alignment policy: the uploader accepts an offset of 4 because
    each texel is a whole word, and this fixture confirms the result still
    matches the reference byte-for-byte. Offsets that are not word aligned are
    rejected and are not part of the committed suite.
    """
    return _texture_capture32(4, 4, [
        set_tile(0, 0, 3, 2, 0) + load_tile(0, 0, 0, 3 << 2, 3 << 2) + TEXTURE_SYNC,
    ], source=TEXTURE_SRC + 4)


TEXTURE_RECORD_WIDTH = 4
TEXTURE_RECORD_HEIGHT = 4


def _texture_record_texel(y: int, x: int) -> int:
    return 0x1000 + y * 0x10 + x


def texture_memory_record_fixture() -> bytes:
    """Place RGBA16 texels with a memory record, then upload with LoadTile.

    The texel bytes are written directly instead of through fills, which is the
    path 8-bit texel formats need. The resulting TMEM still has to match the
    reference byte-for-byte.
    """
    width = TEXTURE_RECORD_WIDTH
    height = TEXTURE_RECORD_HEIGHT
    texels = bytearray()
    for y in range(height):
        for x in range(width):
            texels += struct.pack(">H", _texture_record_texel(y, x))

    records = [
        initial(8 << 20),
        memory(2, TEXTURE_SRC, bytes(texels)),
        rdp(3, 0x100000, scissor(0, 0, width * 4, height * 4) +
            set_texture_image(0, 2, width, TEXTURE_SRC) +
            set_tile(0, 0, 2, 2, 0) +
            load_tile(0, 0, 0, (width - 1) << 2, (height - 1) << 2) + TEXTURE_SYNC),
        vi(4, 2, TEXTURE_FB, 64),
    ]
    return serialize(records)


def texture_memory_record_fill_fixture() -> bytes:
    """The same texels as texture_memory_record_fixture, written with fills.

    The fill path was validated against the reference long before memory
    records existed, so comparing the two fixtures' reference RDRAM images
    pins the record's byte mapping to independent ground truth instead of to
    whatever the replay and the reference happen to agree on.
    """
    width = TEXTURE_RECORD_WIDTH
    height = TEXTURE_RECORD_HEIGHT
    words = scissor(0, 0, width * 4, height * 4) + [0xef300000, 0]
    for y in range(height):
        for x in range(width):
            value = _texture_record_texel(y, x)
            words += set_image(2, TEXTURE_SRC, width) + [0xf7000000, value << 16 | value]
            words += [0xf6000000 | (x * 4 + 3) << 12 | (y * 4 + 3),
                      (x * 4) << 12 | (y * 4)]
    words += TEXTURE_SYNC
    return serialize([initial(8 << 20), rdp(2, 0x100000, words),
                      vi(3, 2, TEXTURE_FB, 64)])


def texture_8bit_fixture(fmt: int) -> bytes:
    """An 8-bit texel format uploaded with LoadTile from a memory record.

    Fills cannot place an 8-bit texel exactly, so the source is written with a
    memory record and every index is distinct. The upload layout is shared by
    CI8, IA8 and I8 because it depends on the texture image size, not the
    format, and each of those three is covered here.
    """
    width = 16
    height = 4
    indices = bytearray()
    for y in range(height):
        for x in range(width):
            indices.append(0x10 + y * 0x10 + x)

    records = [
        initial(8 << 20),
        memory(2, TEXTURE_SRC, bytes(indices)),
        rdp(3, 0x100000, scissor(0, 0, width * 4, height * 4) +
            set_texture_image(fmt, 1, width, TEXTURE_SRC) +
            set_tile(0, fmt, 1, 2, 0) +
            load_tile(0, 0, 0, (width - 1) << 2, (height - 1) << 2) + TEXTURE_SYNC),
        vi(4, 2, TEXTURE_FB, 64),
    ]
    return serialize(records)


def texture_8bit_offset_fixture() -> bytes:
    """CI8 with a narrower image and a non-zero tmem base, to move the row base."""
    width = 8
    height = 4
    indices = bytearray()
    for y in range(height):
        for x in range(width):
            indices.append(0x10 + y * 0x10 + x)

    records = [
        initial(8 << 20),
        memory(2, TEXTURE_SRC, bytes(indices)),
        rdp(3, 0x100000, scissor(0, 0, width * 4, height * 4) +
            set_texture_image(2, 1, width, TEXTURE_SRC) +
            set_tile(0, 2, 1, 1, 3) +
            load_tile(0, 0, 0, (width - 1) << 2, (height - 1) << 2) + TEXTURE_SYNC),
        vi(4, 2, TEXTURE_FB, 64),
    ]
    return serialize(records)


def sampler_rgba16_fixture() -> bytes:
    """One clean RGBA16 upload, for checking the fetch path in isolation.

    The combined texture fixtures stack several uploads that overwrite each
    other's TMEM, so a sampler check against them cannot attribute a texel to
    one upload. This fixture performs exactly one 8x8 LoadTile from a 16-wide
    source into tmem unit 0 with line 2, leaving the whole tile intact.
    """
    width = height = 8
    return _texture_capture(16, 16, [
        set_tile(0, 0, 2, 2, 0) +
        load_tile(0, 0, 0, (width - 1) << 2, (height - 1) << 2) + TEXTURE_SYNC,
    ])


def sampler_rgba32_fixture() -> bytes:
    """One clean RGBA32 upload, for checking the fetch path in isolation."""
    width = height = 8
    return _texture_capture32(8, 8, [
        set_tile(0, 0, 3, 2, 0) +
        load_tile(0, 0, 0, (width - 1) << 2, (height - 1) << 2) + TEXTURE_SYNC,
    ])


def texture_stride_fixture() -> bytes:
    """Line strides of 1 and 4, and a non-zero tmem base, for LoadTile."""
    return _texture_capture(16, 16, [
        set_tile(0, 0, 2, 1, 0) + load_tile(0, 0, 0, 3 << 2, 7 << 2) + TEXTURE_SYNC,
        set_tile(1, 0, 2, 4, 0) + load_tile(1, 0, 0, 3 << 2, 7 << 2) + TEXTURE_SYNC,
        set_tile(2, 0, 2, 2, 3) + load_tile(2, 0, 0, 3 << 2, 7 << 2) + TEXTURE_SYNC,
    ])


def texture_block_fixture() -> bytes:
    """LoadBlock with dxt=0 at several sh values, one 64-bit step each four."""
    return _texture_capture(16, 16, [
        set_tile(0, 0, 2, 2, 0) + load_block(0, 0, 0, 4, 0) + TEXTURE_SYNC,
        set_tile(1, 0, 2, 2, 0) + load_block(1, 0, 0, 44, 0) + TEXTURE_SYNC,
        set_tile(2, 0, 2, 2, 1) + load_block(2, 0, 0, 12, 0) + TEXTURE_SYNC,
    ])


def texture_tlut_fixture() -> bytes:
    """Single-row LoadTLUT at 4 and 7 entries, plus a non-zero tmem base."""
    return _texture_capture(16, 16, [
        set_tile(0, 0, 2, 2, 0) + load_tlut(0, 0, 0, 12, 0) + TEXTURE_SYNC,
        set_tile(1, 0, 2, 2, 0) + load_tlut(1, 0, 0, 28, 0) + TEXTURE_SYNC,
        set_tile(2, 0, 2, 2, 5) + load_tlut(2, 0, 0, 12, 0) + TEXTURE_SYNC,
    ])


def write_all(destination: pathlib.Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    fixtures = {
        "matrix.aurcap": matrix_fixture(),
        "boundary4.aurcap": boundary_fixture(4 << 20),
        "boundary8.aurcap": boundary_fixture(8 << 20),
        "texture.aurcap": texture_fixture(),
        "texture_stride.aurcap": texture_stride_fixture(),
        "texture_block.aurcap": texture_block_fixture(),
        "texture_tlut.aurcap": texture_tlut_fixture(),
        "texture_rgba32.aurcap": texture_rgba32_fixture(),
        "texture_rgba32_offset.aurcap": texture_rgba32_offset_fixture(),
        "texture_memory_record.aurcap": texture_memory_record_fixture(),
        "texture_memory_record_fill.aurcap": texture_memory_record_fill_fixture(),
        "texture_ci8.aurcap": texture_8bit_fixture(2),
        "texture_ia8.aurcap": texture_8bit_fixture(3),
        "texture_i8.aurcap": texture_8bit_fixture(4),
        "texture_8bit_offset.aurcap": texture_8bit_offset_fixture(),
        "sampler_rgba16.aurcap": sampler_rgba16_fixture(),
        "sampler_rgba32.aurcap": sampler_rgba32_fixture(),
    }
    for name, data in fixtures.items():
        (destination / name).write_bytes(data)
        print(f"Generated {destination / name} ({len(data)} bytes)")


if __name__ == "__main__":
    write_all(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".build/tests/fixtures"))
