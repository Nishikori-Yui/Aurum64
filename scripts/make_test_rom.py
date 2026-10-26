#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate our own tiny IPL3 replacement; contains no Nintendo boot code/assets."""
import pathlib
import struct
import sys

words = []
textured = "--texture" in sys.argv[2:] or "--ci-texture" in sys.argv[2:]
ci_textured = "--ci-texture" in sys.argv[2:]
def emit(word):
    words.append(word)
def li(reg, value):
    emit(0x3C000000 | reg << 16 | ((value >> 16) & 65535))
    emit(0x34000000 | reg << 21 | reg << 16 | (value & 65535))
def store(base, offset, value):
    li(9, value)
    emit(0xAC000000 | base << 21 | 9 << 16 | offset)

# Execute directly in SP DMEM at A4000040 after the core's PIF HLE.
# Interrupts disabled: our test polls nothing and never needs an exception handler.
emit(0x40806000)  # mtc0 zero, Status
li(8, 0xA0100000)
rdp = [
    0xFF10013F, 0x00020000,                  # RGBA16, width 320
    0xED000000, (1280 << 12) | 960,         # scissor 320x240
    0xEF300000, 0,                         # fill cycle
    0xF7000000, 0xEB0BEB0B,                # orange RGBA5551
    0xF6000000 | (1276 << 12) | 956, 0,     # inclusive rectangle
    0xE9000000, 0,                         # full sync
]
if textured:
    if ci_textured:
        # Original 8x8 CI8 checker plus a complete 256-entry RGBA16 palette.
        li(8, 0xA0130000)
        indices = [(x + y) & 1 for y in range(8) for x in range(8)]
        for index in range(0, len(indices), 4):
            value = sum(indices[index + lane] << (24 - lane * 8) for lane in range(4))
            store(8, index, value)
        li(8, 0xA0140000)
        palette = [0x003e if index == 0 else 0xf801 if index == 1 else
                   (((index * 7 & 31) << 11) | ((index * 3 & 31) << 6) |
                    ((index >> 3 & 31) << 1)) for index in range(256)]
        for index in range(0, len(palette), 2):
            store(8, index * 2, palette[index] << 16 | palette[index + 1])
    else:
        # Original RGBA16 checker with one-bit alpha coverage.
        li(8, 0xA0130000)
        texels = [((0xf800 if x < 4 else 0x07c0) if y < 4 else
                   (0x003e if x < 4 else 0xfffe)) | ((x + y) & 1)
                  for y in range(8) for x in range(8)]
        for index in range(0, len(texels), 2):
            store(8, index * 2, texels[index] << 16 | texels[index + 1])
    li(8, 0xA0100000)
    texture_commands = []
    if ci_textured:
        texture_commands += [
            0xFD1000FF, 0x00140000,        # RGBA16 palette image, 256 entries
            0xF5000100, 7 << 24,           # palette tile, packed entry layout
            0xF0000000, (7 << 24) | (1020 << 12), # LoadTLUT to upper TMEM
            0xFD480007, 0x00130000,        # CI8 source, stride 8
            0xF5480200, (3 << 4) | (3 << 14), # CI8 tile, line 1, wrap 8x8
            0xF4000000, (28 << 12) | 28,   # LoadTile 8x8 indices
            0xF2000000, (28 << 12) | 28,   # tile bounds
        ]
    else:
        texture_commands += [
            0xFD100007, 0x00130000,        # RGBA16 source, stride 8
            0xF5100400, (3 << 4) | (3 << 14), # tile line 2, mask 3 on S/T
            0xF4000000, (28 << 12) | 28,   # LoadTile 8x8
        ]
    texture_commands += [
        (0xEF208000 if ci_textured else 0xEF200000), 1, # copy; TLUT only for CI, alpha compare
        0xE4000000 | (1244 << 12) | 924, (32 << 12) | 32,
        0, (4096 << 16) | 1024,
    ]
    rdp[-2:-2] = texture_commands
for offset, word in enumerate(rdp):
    store(8, offset * 4, word)
li(8, 0xA4100000)
store(8, 0x0C, 0x15)   # clear XBUS, freeze and flush
store(8, 0x00, 0x00100000)
store(8, 0x04, 0x00100000 + len(rdp) * 4)

# A tiny, original RSP program writes a green marker into DMEM, then BREAKs.
# CPU verifies the marker; a broken RSP leaves the orange framebuffer unchanged.
# These scalar instructions exercise the real LLE RSP, not an HLE display list.
li(8, 0xA4001000)
for offset, instruction in enumerate([0x3C0807C1, 0x350807C1, 0xAC080300, 0x0000000D]):
    store(8, offset * 4, instruction)
li(8, 0xA4000000)
store(8, 0xFC0, 1)         # graphics task, deliberately handled by the LLE RSP
store(8, 0xFF0, 0x100000)  # non-null task data so HLE misconfiguration is observable
li(8, 0xA4080000)
store(8, 0, 0)
li(8, 0xA4040000)
store(8, 0x10, 5)  # clear HALT and BROKE
li(8, 0xA4000300)
emit(0x8D090000)   # lw t1, 0(t0)
emit(0)            # load delay
li(10, 0x07C107C1)
emit(0x152A0004)   # bne t1, t2, skip marker write
emit(0)
li(8, 0xA0020000)
emit(0xAD090000)   # sw t1, 0(t0)

# Silent AI DMA validates audio plumbing without playing an unsolicited tone.
li(8, 0xA4500000)
for offset, value in [(0x10, 1520), (0x14, 15), (8, 1), (0, 0x120000), (4, 4096)]:
    store(8, offset, value)
li(8, 0xA4400000)
for offset, value in [(0, 2), (4, 0x20000), (8, 320), (0x24, 320),
                      (0x28, 480), (0x30, 1024), (0x34, 1024),
                      (0x0C, 2), (0x18, 525)]:
    store(8, offset, value)
emit(0x1000FFFF)  # beq zero, zero, self
emit(0)
rom = bytearray(4096)
struct.pack_into(">4I", rom, 0, 0x80371240, 0xF, 0xA4000040, 0)
rom[0x20:0x34] = (b"AURUM CI8 TEST" if ci_textured else
                   b"AURUM TEXTURE TEST" if textured else
                   b"AURUM OPEN TEST ROM").ljust(20, b" ")
rom[0x3E] = ord("E")
for i, word in enumerate(words):
    struct.pack_into(">I", rom, 0x40 + i * 4, word)
destination = pathlib.Path(sys.argv[1])
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_bytes(rom)
print(f"Generated {destination} ({len(words)} MIPS instructions; no proprietary IPL3)")
