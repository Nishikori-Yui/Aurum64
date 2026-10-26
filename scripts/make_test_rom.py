#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate our own tiny IPL3 replacement; contains no Nintendo boot code/assets."""
import pathlib
import struct
import sys

words = []
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
rom[0x20:0x34] = b"AURUM OPEN TEST ROM".ljust(20, b" ")
rom[0x3E] = ord("E")
for i, word in enumerate(words):
    struct.pack_into(">I", rom, 0x40 + i * 4, word)
destination = pathlib.Path(sys.argv[1])
destination.parent.mkdir(parents=True, exist_ok=True)
destination.write_bytes(rom)
print(f"Generated {destination} ({len(words)} MIPS instructions; no proprietary IPL3)")
