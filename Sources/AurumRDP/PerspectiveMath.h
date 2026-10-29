// SPDX-License-Identifier: MIT
#pragma once
#include "TextureDepthMath.h"

struct PerspectiveCoordinate { ShadeUInt s, t; };

inline ShadeInt textureSigned16(ShadeUInt value) {
    return ShadeInt(value & 65535u) - ((value & 32768u) ? 65536 : 0);
}

// Generate the reciprocal segment endpoints from the rational curve. Keeping
// the 64 segments and floor-rounded interpolation models the TC divider's
// quantization without importing an emulator's lookup table.
inline ShadeInt reciprocalPoint(ShadeUInt segment) {
    const ShadeUInt denominator = 64u + segment;
    const ShadeUInt rounded = (1048576u + denominator / 2u) / denominator;
    // Independent divider probes expose one downward-quantized endpoint
    // at segment 6; nearest-integer reciprocals alone fail around W=35.
    return ShadeInt(rounded - (segment == 6 ? 1u : 0u));
}

inline ShadeUInt perspectiveComponent(ShadeUInt input, ShadeInt reciprocal,
                                      ShadeUInt exponent, bool badW) {
    const ShadeInt product = textureSigned16(input) * reciprocal;
    const ShadeUInt bits = ShadeUInt(product);
    const ShadeUInt signWindow = (0x3fffffffu << (29u - exponent)) & 0x3fffffffu;
    const ShadeUInt signBits = bits & signWindow;
    // The largest exponent shifts left once; all other shifts are arithmetic.
    const ShadeUInt scaled = exponent == 14 ? bits * 2u : ShadeUInt(product >> (13u - exponent));
    ShadeUInt flags = badW ? 0x40000u : 0u;
    if (signBits != 0 && signBits != signWindow) {
        // Overflow polarity is observed after shifting, except at exponent 14.
        const ShadeUInt polarity = exponent == 14 ? bits : scaled;
        flags |= (polarity & 0x20000000u) ? 0x20000u : 0x40000u;
    }
    return (scaled & 0x1ffffu) | flags;
}

inline PerspectiveCoordinate divideTexture(ShadeUInt s, ShadeUInt t, ShadeUInt w, bool perspective) {
    if (!perspective) return {ShadeUInt(textureSigned16(s)) & 0x1ffffu,
                              ShadeUInt(textureSigned16(t)) & 0x1ffffu};
    const bool badW = textureSigned16(w) <= 0;
    ShadeUInt magnitude = w & 32767u;
    ShadeUInt exponent = 0;
    while (magnitude < 16384u && exponent < 14u) { magnitude *= 2u; ++exponent; }
    const ShadeUInt segment = (magnitude & 16383u) >> 8;
    const ShadeUInt fraction = magnitude & 255u;
    const ShadeInt point = reciprocalPoint(segment);
    const ShadeInt slope = reciprocalPoint(segment + 1) - point;
    const ShadeInt reciprocal = point + ((slope * ShadeInt(fraction)) >> 8);
    return {perspectiveComponent(s, reciprocal, exponent, badW),
            perspectiveComponent(t, reciprocal, exponent, badW)};
}
