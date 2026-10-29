#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Original triangle command matrices; no external assets or renderer code."""
import pathlib
import random
import struct
import argparse

from make_reference_fixtures import (initial, memory, rdp, scissor, serialize, set_image,
                                     set_texture_image, set_tile, load_tile)


def pack_attributes(channels):
    words = [0] * 16
    for channel, fields in enumerate(channels):
        for group, field in zip((0, 2, 8, 10), fields):
            shift = 0 if channel & 1 else 16
            index = group + channel // 2
            words[index] |= ((field >> 16) & 65535) << shift
            words[index + 4] |= (field & 65535) << shift
    return words


def depth_compress(value):
    bucket = (value >> 11) & 0x7f
    if bucket <= 0x3f:
        return (value >> 4) & 0x1ffc
    if bucket <= 0x5f:
        return ((value >> 3) & 0x1ffc) | 0x2000
    if bucket <= 0x6f:
        return ((value >> 2) & 0x1ffc) | 0x4000
    if bucket <= 0x77:
        return ((value >> 1) & 0x1ffc) | 0x6000
    if bucket <= 0x7b:
        return (value & 0x1ffc) | 0x8000
    if bucket <= 0x7d:
        return ((value << 1) & 0x1ffc) | 0xa000
    if bucket == 0x7e:
        return ((value << 2) & 0x1ffc) | 0xc000
    return ((value << 2) & 0x1ffc) | 0xe000


def matrix(memory_bytes=4 << 20, seed=0x7A1A, shaded=False, textured=False,
           alpha_compare=False, depth=False):
    rng = random.Random(seed)
    records = [initial(memory_bytes), memory(2, 0x200000, b"\x6b\x5a" * (320 * 1024))]
    if depth:
        depth_seed = bytearray(320 * 2048)
        for case in range(320):
            z = 0x2000 + (case % 16) * 0x400
            stored = max(0, z - 0x100) if case % 2 == 0 else min(0x3ffff, z + 0x100)
            encoded = struct.pack(">H", depth_compress(stored))
            base = case * 2048
            for offset in range(0, 32 * 96 * 2, 2):
                depth_seed[base + offset:base + offset + 2] = encoded
        records.append(memory(3, 0x300000, bytes(depth_seed)))
    words = []
    if textured:
        source = b"".join((((x * 2 + 1) << 11) | ((y * 2) << 6) |
                           (((x + y) & 31) << 1) | ((x ^ y) & 1)).to_bytes(2, "big")
                          for y in range(16) for x in range(16))
        records.append(memory(3, 0x300000, source))
        # A nonzero tile and TMEM base ensure the triangle snapshot is used.
        words += set_texture_image(0, 2, 16, 0x300000)
        for base in (0, 128):
            words += set_tile(3, 0, 2, 4, base) + load_tile(3, 0, 0, 60, 60)
    for case in range(320):
        width = (17, 31, 32)[case % 3]
        destination = 0x200000 + case * 2048 + (case & 1) * 2
        # Fractional Y/clip bounds, negative/offscreen vertices, both major edges,
        # YM changes, positive/negative slopes, and low RGB/alpha bits all vary.
        yh = rng.randrange(-12, 48)
        ym = yh + rng.randrange(0, 24)
        yl = ym + rng.randrange(0, 25)
        xh = rng.randrange(-4 << 16, 32 << 16)
        xm = rng.randrange(-4 << 16, 32 << 16)
        xl = rng.randrange(-4 << 16, 32 << 16)
        slopes = [rng.randrange(-4 << 16, 4 << 16) for _ in range(3)]
        left_major = case & 1
        clip = (rng.randrange(0, 16), rng.randrange(0, 16), 128 - case % 4, 96 - case % 4)
        if case < 64:
            # Orthogonal controls isolate Y phase, X subpixel rounding and alpha.
            yh, ym, yl = case % 4, 8 + case % 4, 16 + case % 4
            left = (2 << 16) + (case // 4 % 8) * 8192 + (case // 32)
            right = 10 << 16
            xh, xm, xl = (left, right, right) if left_major else (right, left, left)
            slopes = [0, 0, 0]
            clip = (0, 0, width * 4 - case % 4, 96 - case % 4)
        elif case < 96:
            # Include the signed encoded X/slope limits and accumulator wrap.
            xh, xm, xl = [rng.choice((-0x8000000, 0x7ffffff, -1, 1, 10 << 16)) for _ in range(3)]
            slopes = [rng.choice((-0x20000000, 0x1fffffff, -1, 1)) for _ in range(3)]
        elif case < 104:
            clip = (0, 0, 0, 96) if case & 1 else (0, 0, 128, 0)
        elif case < 112:
            # Paired triangles share a sloped edge and exactly tile an 8x8 box.
            width = 32
            destination = 0x200000 + (case & ~1) * 2048
            yh, ym, yl = 8, 40, 40
            left_major = not (case & 1)
            xh, xm, xl = (2 if left_major else 10) << 16, 10 << 16, 2 << 16
            slopes = [0, 0, -65536]
            clip = (0, 0, 128, 96)
        depth_address = 0x300000 + (destination - 0x200000)
        color = rng.getrandbits(32)
        words += set_image(2, destination, width) + scissor(*clip)
        mode = 0xf0
        mode_lo = 0
        combine_hi = 0xffffff
        combine = 0xfffe793c if shaded else 0xfffdf6fb
        tile = 0
        depth_z = 0x2000 + (case % 16) * 0x400
        if depth:
            mode_lo = 0x20
            words += [0x3e000000, depth_address]
        if textured:
            mode = 0x8f0 | (((case // 2) & 1) << 19)
            if shaded:
                # One-cycle: TEXEL0 * SHADE for RGB and alpha. Unused
                # cycle-zero selectors are deliberately left at zero.
                combine_hi = (1 << 5) | 4
                combine = ((15 << 24) | (1 << 21) | (4 << 18) |
                           (7 << 6) | (7 << 3) | 7)
            else:
                combine = 0xfffcf279
            tile = case % 8
            descriptor = set_tile(tile, 0, 2, (0, 1, 4, 5)[case % 4], (0, 128)[case // 4 % 2])
            descriptor[1] |= ((case % 4) << 4) | (((case >> 2) & 1) << 8) | (((case >> 3) & 1) << 9)
            descriptor[1] |= (((case // 4) % 4) << 14) | (((case >> 3) & 1) << 18) | (((case >> 4) & 1) << 19)
            shifts = (0, 1, 3, 10, 11, 15)
            descriptor[1] |= shifts[case % 6] | (shifts[(case // 6) % 6] << 10)
            origin_s, origin_t = case % 5, case % 7
            words += descriptor + [0xf2000000 | (origin_s << 12) | origin_t,
                                    (tile << 24) | (60 << 12) | 60]
        if alpha_compare:
            threshold = (case * 29 + 37) & 0xff
            words += [0x39000000, threshold]
        words += [0xef000000 | mode, mode_lo | (1 if alpha_compare else 0),
                  0xfc000000 | combine_hi, combine,
                  0xfa000000, color]
        if depth:
            triangle_opcode = 0xc9000000
        elif textured and shaded:
            triangle_opcode = 0xce000000 | (tile << 16)
        elif textured:
            triangle_opcode = 0xca000000 | (tile << 16)
        elif shaded:
            triangle_opcode = 0xcc000000
        else:
            triangle_opcode = 0xc8000000
        words += [triangle_opcode | (left_major << 23) | (yl & 0x3fff),
                  ((ym & 0x3fff) << 16) | (yh & 0x3fff),
                  xl & 0xffffffff, slopes[0] & 0xffffffff,
                  xh & 0xffffffff, slopes[1] & 0xffffffff,
                  xm & 0xffffffff, slopes[2] & 0xffffffff]
        if shaded:
            channels = []
            for channel in range(4):
                if case < 16:
                    channels.append([((color >> (24 - channel * 8)) & 255) << 16, 0, 0, 0])
                elif case < 288:
                    channels.append([rng.randrange(-64 << 16, 320 << 16),
                                     rng.randrange(-8 << 16, 8 << 16),
                                     rng.randrange(-8 << 16, 8 << 16),
                                     rng.randrange(-8 << 16, 8 << 16)])
                else:
                    channels.append([rng.getrandbits(32) for _ in range(4)])
            words += pack_attributes(channels)
        if textured:
            if case < 16:
                stw = [[(case * 16) << 16, 32 << 16, 0, 0],
                       [0, 0, 32 << 16, 32 << 16], [0x40000000, 0, 0, 0]]
            elif case < 288:
                stw = [[rng.randrange(-128, 512) << 16, rng.randrange(-64, 64) << 16,
                        rng.randrange(-64, 64) << 16, rng.randrange(-64, 64) << 16] for _ in range(2)]
                stw.append([rng.choice((0, -1, 1, 8192, 16384, 32767)) << 16,
                            rng.randrange(-128, 128) << 16,
                            rng.randrange(-128, 128) << 16, rng.randrange(-128, 128) << 16])
            else:
                stw = [[rng.getrandbits(32) for _ in range(4)] for _ in range(3)]
            words += pack_attributes(stw)
        if depth:
            words += [depth_z << 13, 0, 0, 0]
    records.append(rdp(4 if textured or depth else 3, 0x10000, words + [0xe9000000, 0]))
    return serialize(records)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("destination", type=pathlib.Path)
    parser.add_argument("memory", type=int, nargs="?", default=4 << 20)
    parser.add_argument("seed", type=int, nargs="?", default=0x7A1A)
    parser.add_argument("--shaded", action="store_true")
    parser.add_argument("--textured", action="store_true")
    parser.add_argument("--alpha-compare", action="store_true")
    parser.add_argument("--depth", action="store_true")
    args = parser.parse_args()
    destination = args.destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    if (args.alpha_compare or args.depth) and (args.shaded or args.textured):
        parser.error("alpha-compare/depth are currently limited to primitive-color fixtures")
    destination.write_bytes(matrix(args.memory, args.seed, args.shaded, args.textured,
                                   args.alpha_compare, args.depth))
