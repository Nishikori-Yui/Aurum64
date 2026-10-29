// SPDX-License-Identifier: MIT
#pragma once
#include "PerspectiveMath.h"

struct TriangleTextureAxis { ShadeUInt lower, upper, shift, mask, mirror, clamp; };
struct TriangleTextureParameters {
    ShadeParameters setup;
    ShadeChannel stw[3];
    TriangleTextureAxis s, t;
    ShadeUInt line, tmem, perspective;
};

inline ShadeInt clampTextureBus(ShadeUInt coordinate) {
    if (coordinate & 0x40000u) return 32767;
    if (coordinate & 0x20000u) return -32768;
    const ShadeUInt range = coordinate & 0x18000u;
    if (range == 0x08000u) return 32767;
    if (range == 0x10000u) return -32768;
    return textureSigned16(coordinate);
}

inline ShadeInt triangleTextureAxis(ShadeInt coordinate, TriangleTextureAxis axis) {
    const ShadeInt shifted = axis.shift <= 10
        ? textureSigned16(ShadeUInt(coordinate)) >> axis.shift
        : textureSigned16(ShadeUInt(coordinate) << (16u - axis.shift));
    const ShadeInt relative = shifted - ShadeInt(axis.lower * 8u);
    ShadeInt texel = relative >> 5;
    if (axis.clamp || !axis.mask) {
        if ((shifted >> 3) >= ShadeInt(axis.upper))
            texel = ShadeInt(((axis.upper >> 2) - (axis.lower >> 2)) & 1023u);
        else if (relative & 65536) texel = 0;
    }
    if (axis.mask) {
        const ShadeUInt shift = axis.mask < 10u ? axis.mask : 10u;
        if (axis.mirror && ((texel >> shift) & 1)) texel = ~texel;
        texel &= ShadeInt((1u << shift) - 1u);
    }
    return texel;
}

inline ShadeUInt triangleTextureIndex(TriangleTextureParameters parameters,
                                      ShadeInt x, ShadeInt y, ShadeInt start) {
    const ShadeUInt s = interpolateTexture(parameters.stw[0], parameters.setup, x, y, start);
    const ShadeUInt t = interpolateTexture(parameters.stw[1], parameters.setup, x, y, start);
    const ShadeUInt w = interpolateTexture(parameters.stw[2], parameters.setup, x, y, start);
    const auto uv = divideTexture(s >> 16, t >> 16, w >> 16, parameters.perspective != 0);
    const ShadeInt mappedS = triangleTextureAxis(clampTextureBus(uv.s), parameters.s);
    const ShadeInt mappedT = triangleTextureAxis(clampTextureBus(uv.t), parameters.t);
    if (mappedS < 0 || mappedT < 0 || mappedT > 255) return 0xffffffffu;
    const ShadeUInt row = parameters.tmem + parameters.line * ShadeUInt(mappedT);
    if (row >= 512u || row * 4u + ShadeUInt(mappedS) >= 2048u) return 0xffffffffu;
    return (row * 4u + ShadeUInt(mappedS)) ^ ((mappedT & 1) ? 3u : 1u);
}
