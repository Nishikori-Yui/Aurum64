#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate original, self-contained RDP/VI capture fixtures."""
import pathlib
import random
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


def texture_ia16_fixture() -> bytes:
    """A single IA16 tile with distinct intensity and alpha bytes."""
    width, height = 8, 4
    texels = bytearray()
    for y in range(height):
        for x in range(width):
            texels += bytes((0x20 + y * 8 + x, 0x80 + y * 8 + x))
    words = (set_texture_image(3, 2, width, TEXTURE_SRC) +
             set_tile(0, 3, 2, width // 4, 0) +
             load_tile(0, 0, 0, (width - 1) * 4, (height - 1) * 4) + TEXTURE_SYNC)
    return serialize([initial(8 << 20), memory(2, TEXTURE_SRC, bytes(texels)),
                      rdp(3, 0x100000, words), vi(4, 2, TEXTURE_FB, 64)])


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


def texture_copy_fixture(memory_bytes: int = 8 << 20, matrix: bool = False) -> bytes:
    """Original RGBA16 copy draws: no ROM data or upstream renderer code."""
    width, height = 32, 16
    source = b"".join(struct.pack(">H", ((x * 3 & 31) << 11) |
                                 ((y * 5 & 31) << 6) | ((x + y & 31) << 1) |
                                 ((x ^ y) & 1))
                      for y in range(height) for x in range(width))
    framebuffer = TEXTURE_FB + 2  # preserve a neighboring halfword
    stride = 63                  # adjacent rows can share one host word
    records = [initial(memory_bytes), memory(2, TEXTURE_SRC, source)]
    seed = b"\xa5\x5a" * ((8192 * 128 if matrix else 8192) // 2)
    # Each record also contains a four-byte address; stay below the 1 MiB
    # payload cap even when seeding an entire matrix of independent targets.
    for offset in range(0, len(seed), 1 << 19):
        records.append(memory(len(records) + 1, TEXTURE_FB + offset, seed[offset:offset + (1 << 19)]))
    sequence = len(records) + 1

    def submit(words):
        nonlocal sequence
        records.append(rdp(sequence, 0x100000 + (sequence - 4) * 0x100, words))
        sequence += 1

    base = 257 if matrix else 1
    submit(set_texture_image(0, 2, width, TEXTURE_SRC) +
           set_tile(0, 0, 2, width // 4, base) +
           load_tile(0, 0, 0, (width - 1) * 4, (height - 1) * 4) +
           set_image(2, framebuffer, stride) + scissor(0, 0, 62 * 4, 64 * 4))
    rng = random.Random(0xA64007)
    for index in range(128 if matrix else 1):
        flip = bool(index & 1)
        x0, y0 = (rng.randrange(3, 25), rng.randrange(3, 25)) if matrix else (1, 2)
        x1, y1 = x0 + rng.randrange(1, 34), y0 + rng.randrange(1, 20)
        clip_x0, clip_y0 = (x0 + index % 5, y0 + index % 4) if matrix else (0, 0)
        clip_x1, clip_y1 = min(61, x1 - index % 3), min(63, y1 + 1 - index % 4)
        modes = (5 << 4) | (4 << 14)  # S/T mask
        if index & 2:
            modes |= 1 << 8 | 1 << 18  # mirror
        if index & 4:
            modes |= 1 << 9 | 1 << 19  # clamp ignored in copy
        if matrix:
            modes |= rng.choice((0, 1, 2, 15)) | rng.choice((0, 1, 2, 15)) << 10
        tile = set_tile(0, 0, 2, width // 4, base)
        tile[1] |= modes
        sl, tl = (index % 3 * 4, index % 4 * 4) if matrix else (0, 0)
        s = rng.randrange(-2000, 2000) if matrix else 0
        t = rng.randrange(-1000, 1000) if matrix else 0
        ds, dt = (rng.choice((4096, 2048, 1024, -4096, 511, -33)),
                  rng.choice((1024, 512, -1024, 4096, 127, -31))) if matrix else (4096, 1024)
        destination = framebuffer + (index * 8192 if matrix else 0)
        submit(set_image(2, destination, stride) +
               tile + [0xf2000000 | sl << 12 | tl, (sl + 124) << 12 | (tl + 60)] +
               scissor(clip_x0 * 4, clip_y0 * 4, clip_x1 * 4, clip_y1 * 4) +
               [0xef200000, index & 1,
                (0xe5000000 if flip else 0xe4000000) | x1 * 4 << 12 | y1 * 4,
                x0 * 4 << 12 | y0 * 4, (s & 65535) << 16 | (t & 65535),
                (ds & 65535) << 16 | (dt & 65535)] + TEXTURE_SYNC)
    records.append(vi(sequence, 2, framebuffer, stride))
    return serialize(records)


def texture_copy_feedback_fixture() -> bytes:
    """A completed GPU draw becomes the next upload's RDRAM source."""
    pixels = b"".join(struct.pack(">H", 0x4001 + y * 0x100 + x * 2)
                       for y in range(8) for x in range(8))
    words = set_texture_image(0, 2, 8, TEXTURE_SRC) + set_tile(0, 0, 2, 2, 0)
    words += load_tile(0, 0, 0, 28, 28)
    words += set_image(2, TEXTURE_FB, 16) + scissor(0, 0, 64, 32) + [0xef200000, 0]
    words += [0xe4000000 | 28 << 12 | 28, 0, 0, 4096 << 16 | 1024]
    words += set_texture_image(0, 2, 16, TEXTURE_FB) + set_tile(1, 0, 2, 2, 32)
    words += load_tile(1, 0, 0, 28, 28)
    words += [0xe4000000 | 60 << 12 | 28, 1 << 24 | 32 << 12, 0, 4096 << 16 | 1024]
    words += TEXTURE_SYNC
    return serialize([initial(4 << 20), memory(2, TEXTURE_SRC, pixels),
                      rdp(3, 0x100000, words), vi(4, 2, TEXTURE_FB, 16)])


def texture_indexed_fixture(bits: int, matrix: bool = False, bank_probe: bool = False) -> bytes:
    """CI4/8 copy with a complete original RGBA16 palette in upper TMEM."""
    width, height = 32, 8
    palette_address = TEXTURE_SRC + 0x1000
    palette = b"".join(struct.pack(">H", ((i * 7 & 31) << 11) |
                                  ((i * 3 & 31) << 6) | ((i >> 3 & 31) << 1) |
                                  (i & 1)) for i in range(256))
    source = bytes((i * 13 + i // width) & 255 for i in range(width * height))
    packed_width = width if bits == 8 else width // 2
    if bits == 4:
        source = bytes((((i * 2) & 15) << 4) | ((i * 2 + 1) & 15)
                       for i in range(packed_width * height))
    records = [initial(4 << 20), memory(2, TEXTURE_SRC, source),
               memory(3, palette_address, palette),
               memory(4, TEXTURE_FB, b"\x6b\x5a" * (8192 * (64 if matrix else 1) // 2))]
    sequence = 5

    def submit(words):
        nonlocal sequence
        records.append(rdp(sequence, 0x100000 + (sequence - 5) * 0x100, words))
        sequence += 1

    # A size-0 load tile packs four replicated halfwords per palette entry.
    submit(set_image(2, TEXTURE_FB, 63) +
           set_texture_image(0, 2, 256, palette_address) + set_tile(7, 0, 0, 0, 256) +
           load_tlut(7, 0, 0, 255 * 4, 0) +
           set_texture_image(2, 1, packed_width, TEXTURE_SRC) +
           set_tile(0, 2, 1, packed_width // 8, 16) +
           load_tile(0, 0, 0, (packed_width - 1) * 4, (height - 1) * 4))
    if bank_probe:
        # Break palette replication deliberately. A copy qword must read the
        # corresponding palette port for each lane, not reuse a decoded color.
        submit(set_texture_image(0, 2, 16, palette_address) + set_tile(6, 0, 2, 4, 256) +
               load_tile(6, 0, 0, 15 * 4, 0))
    rng = random.Random(0x64004 + bits)
    for index in range(64 if matrix else 1):
        tile = set_tile(0, 2, 1 if bits == 8 else 0, packed_width // 8, 16)
        smode, tmode = (5 << 4), (3 << 4)
        if matrix:
            smode |= rng.choice((0, 1, 15)) | ((index & 1) << 8) | ((index & 2) << 8)
            tmode |= rng.choice((0, 1, 15)) | ((index & 2) << 7) | ((index & 1) << 9)
        tile[1] |= smode | tmode << 10 | (index & 15) << 20
        x0, y0 = 1 + index % 7, 2 + index % 5
        x1, y1 = x0 + 23, y0 + 7
        s, t = (rng.randrange(-1000, 1000), rng.randrange(-200, 200)) if matrix else (0, 0)
        words = set_image(2, TEXTURE_FB + index * 8192 + 2, 63) + tile
        words += [0xf2000000, 124 << 12 | 28]
        words += scissor((x0 + index % 3) * 4, (y0 + index % 2) * 4, (x1 - index % 2) * 4, (y1 + 1) * 4)
        words += [0xef208000, index & 1,
                  (0xe5000000 if index & 1 else 0xe4000000) | x1 * 4 << 12 | y1 * 4,
                  x0 * 4 << 12 | y0 * 4, (s & 65535) << 16 | (t & 65535),
                  (rng.choice((4096, 1024, -2048, 127)) & 65535) << 16 |
                  (rng.choice((1024, -512, 33)) & 65535)]
        submit(words + TEXTURE_SYNC)
    records.append(vi(sequence, 2, TEXTURE_FB + 2, 63))
    return serialize(records)


def texture_intensity_copy_fixture(fmt: int, size: int = 1,
                                   matrix: bool = False) -> bytes:
    """IA8/I8 or IA16 copy rectangle with distinguishable source texels."""
    width, height = ((32, 16) if matrix else (16, 4)) if size == 1 else \
                    ((16, 8) if matrix else (8, 4))
    if size == 1:
        source = bytes((0x13 + y * 0x31 + x * 0x17 + x * y * 3) & 255
                       for y in range(height) for x in range(width))
    else:
        source = b"".join(struct.pack(">H", 0x2180 + y * 0x100 + x * 0x11)
                          for y in range(height) for x in range(width))
    fb_width = 63 if matrix else width
    destination = TEXTURE_FB + (2 if matrix else 0)
    tile_line = width // (8 if size == 1 else 4)
    records = [initial(4 << 20), memory(2, TEXTURE_SRC, source)]
    if matrix:
        records.append(memory(3, destination, b"\x6b\x5a" * (8192 * 64 // 2)))
    words = (set_texture_image(fmt, size, width, TEXTURE_SRC) +
             set_tile(0, fmt, size, tile_line, 0) +
             load_tile(0, 0, 0, (width - 1) * 4, (height - 1) * 4) +
             TEXTURE_SYNC)
    rng = random.Random(0x1A110 + fmt * 17 + size)
    for index in range(64 if matrix else 1):
        fb = destination + (index * 8192 if matrix else 0)
        x0, y0 = (1 + index % 7, 2 + index % 5) if matrix else (0, 0)
        x1 = min(width - 1, x0 + (rng.randrange(8, 24) if matrix else width - 1))
        y1 = min(height - 1, y0 + (rng.randrange(3, 9) if matrix else height - 1))
        clip_x0 = x0 + (index % 3 if matrix else 0)
        clip_y0 = y0 + (index % 2 if matrix else 0)
        clip_x1 = min(width, x1 + 1 - (index % 2 if matrix else 0))
        clip_y1 = min(height, y1 + 1)
        shift_s = (index % 3) if matrix else 0
        shift_t = (index // 3 % 3) if matrix else 0
        mask_s = (index % 6) if matrix else 0
        mask_t = (index % 4) if matrix else 0
        modes = (shift_s | (mask_s << 4) | ((index & 1) << 8) |
                 (shift_t << 10) | (mask_t << 14) | (((index >> 1) & 1) << 18))
        tile = set_tile(0, fmt, size, tile_line, 0)
        tile[1] |= modes
        s = rng.randrange(0, 32 * 32) if matrix else 0
        t = rng.randrange(0, 16 * 32) if matrix else 0
        dsdx = rng.choice((4096, 2048, 1024, 511)) if matrix else 4096
        dtdy = rng.choice((1024, 512, 2048)) if matrix else 1024
        flip = bool(index & 1) if matrix else False
        words += (set_image(2, fb, fb_width) + tile +
                  [0xf2000000, ((width - 1) * 4 << 12) | ((height - 1) * 4)] +
                  scissor(clip_x0 * 4, clip_y0 * 4, clip_x1 * 4, clip_y1 * 4) +
                  [0xef200000, 0,
                   (0xe5000000 if flip else 0xe4000000) | x1 * 4 << 12 | y1 * 4,
                   x0 * 4 << 12 | y0 * 4, (s & 65535) << 16 | (t & 65535),
                   (dsdx & 65535) << 16 | (dtdy & 65535)] + TEXTURE_SYNC)
    records.extend([rdp(4, 0x100000, words), vi(5, 2, destination, fb_width)])
    return serialize(records)


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
        "texture_ia16.aurcap": texture_ia16_fixture(),
        "texture_8bit_offset.aurcap": texture_8bit_offset_fixture(),
        "sampler_rgba16.aurcap": sampler_rgba16_fixture(),
        "sampler_rgba32.aurcap": sampler_rgba32_fixture(),
        "texture_copy.aurcap": texture_copy_fixture(),
        "texture_copy_matrix4.aurcap": texture_copy_fixture(4 << 20, True),
        "texture_copy_matrix8.aurcap": texture_copy_fixture(8 << 20, True),
        "texture_copy_feedback.aurcap": texture_copy_feedback_fixture(),
        "texture_ci8_copy.aurcap": texture_indexed_fixture(8),
        "texture_ci4_copy.aurcap": texture_indexed_fixture(4),
        "texture_ci8_matrix.aurcap": texture_indexed_fixture(8, True),
        "texture_ci4_matrix.aurcap": texture_indexed_fixture(4, True),
        "texture_ci8_ports.aurcap": texture_indexed_fixture(8, bank_probe=True),
        "texture_ci4_ports.aurcap": texture_indexed_fixture(4, bank_probe=True),
        "texture_ia8_copy.aurcap": texture_intensity_copy_fixture(3),
        "texture_i8_copy.aurcap": texture_intensity_copy_fixture(4),
        "texture_ia16_copy.aurcap": texture_intensity_copy_fixture(3, 2),
        "texture_ia8_matrix.aurcap": texture_intensity_copy_fixture(3, matrix=True),
        "texture_i8_matrix.aurcap": texture_intensity_copy_fixture(4, matrix=True),
        "texture_ia16_matrix.aurcap": texture_intensity_copy_fixture(3, 2, matrix=True),
    }
    for name, data in fixtures.items():
        (destination / name).write_bytes(data)
        print(f"Generated {destination / name} ({len(data)} bytes)")


if __name__ == "__main__":
    write_all(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".build/tests/fixtures"))
