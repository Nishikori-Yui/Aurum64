// SPDX-License-Identifier: MIT
#pragma once

#ifndef __METAL_VERSION__
#include <cstdint>
using ShadeUInt = uint32_t;
using ShadeInt = int32_t;
#else
using ShadeUInt = uint;
using ShadeInt = int;
#endif

// Shared fixed-point arithmetic for the CPU diagnostic and Metal shade path.
// Row setup is quantized before the X increment, not after interpolation.
struct ShadeChannel { ShadeInt initial, dx, de, dy; };
struct ShadeParameters {
    ShadeChannel rgba[4];
    ShadeInt firstRow, xh, dxhdy;
    ShadeUInt leftMajor, offset;
};

inline ShadeUInt attributeEdgeBits(ShadeParameters setup, ShadeInt y) {
    const ShadeInt rows = y - setup.firstRow;
    const ShadeUInt steps = ShadeUInt(rows) * 4u + (setup.offset ? 3u : 0u);
    const ShadeInt edgeStep = (setup.dxhdy >> 2) & ~1;
    return ShadeUInt(setup.xh & ~1) + ShadeUInt(edgeStep) * steps;
}

inline ShadeInt attributeAnchorX(ShadeParameters setup, ShadeInt y) {
    const ShadeUInt edge = attributeEdgeBits(setup, y);
    const ShadeUInt integerX = (edge >> 16) & 4095u;
    return integerX & 2048u ? ShadeInt(integerX) - 4096 : ShadeInt(integerX);
}

inline ShadeUInt attributeRow(ShadeChannel attribute, ShadeParameters setup, ShadeInt y) {
    const ShadeInt rows = y - setup.firstRow;
    const ShadeUInt edge = attributeEdgeBits(setup, y);
    const ShadeUInt fractionX = (edge >> 8) & 255u;
    ShadeUInt value = (ShadeUInt(attribute.initial) + ShadeUInt(attribute.de) * ShadeUInt(rows)) & ~511u;
    if (setup.offset) {
        // Three-quarter-row edge correction relative to the Y derivative.
        const ShadeInt along = attribute.de & ~511;
        const ShadeInt vertical = attribute.dy & ~511;
        value += ShadeUInt(along) - ShadeUInt(along >> 2);
        value -= ShadeUInt(vertical) - ShadeUInt(vertical >> 2);
    }
    value -= fractionX * ShadeUInt((attribute.dx >> 8) & ~1);
    return value & ~1023u;
}

inline ShadeUInt attributePixel(ShadeChannel attribute, ShadeParameters setup,
                                ShadeInt x, ShadeInt y, ShadeInt scanStart, ShadeUInt dxMask) {
    ShadeUInt value = attributeRow(attribute, setup, y);
    const ShadeInt anchorX = attributeAnchorX(setup, y);
    const ShadeInt direction = setup.leftMajor ? 1 : -1;
    const ShadeUInt clippedSteps = ShadeUInt(direction * (scanStart - anchorX)) & 4095u;
    const ShadeUInt dx = ShadeUInt(attribute.dx) & dxMask;
    value += dx * (ShadeUInt(direction) * clippedSteps + ShadeUInt(x - scanStart));
    return value;
}

inline ShadeUInt shadeChannel(ShadeChannel attribute, ShadeParameters setup,
                              ShadeInt x, ShadeInt y, ShadeInt scanStart) {
    // Non-AA accepts only pixels covering (0,0), so centroid offsets are zero.
    const ShadeUInt value = attributePixel(attribute, setup, x, y, scanStart, ~31u);
    const ShadeUInt channel = (value >> 16) & 511u;
    // RDP nine-bit clamp: 0..255 pass, 256..383 saturate, 384..511 underflow.
    return channel < 256u ? channel : channel < 384u ? 255u : 0u;
}

inline ShadeUInt shadeRGB16(ShadeParameters setup, ShadeInt x, ShadeInt y, ShadeInt scanStart) {
    const ShadeUInt r = shadeChannel(setup.rgba[0], setup, x, y, scanStart);
    const ShadeUInt g = shadeChannel(setup.rgba[1], setup, x, y, scanStart);
    const ShadeUInt b = shadeChannel(setup.rgba[2], setup, x, y, scanStart);
    return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1);
}
