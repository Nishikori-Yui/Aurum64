// SPDX-License-Identifier: MIT
#pragma once
#include "ShadeMath.h"

inline ShadeUInt affineTextureCoordinate(ShadeUInt value) {
    // Affine TC consumes the signed high half and exposes a 17-bit bus.
    const ShadeUInt high = value >> 16;
    return high | ((high & 0x8000u) << 1);
}

inline ShadeUInt nonAADepth(ShadeUInt value) {
    // Z enters the correction stage as 22 bits; accepted non-AA samples need
    // no centroid offset. Clamp the resulting 19-bit depth before Z encoding.
    const ShadeUInt z = value >> 13;
    switch (z >> 17) {
        case 2: return 0x3ffffu;
        case 3: return 0;
        default: return z & 0x3ffffu;
    }
}

inline ShadeUInt interpolateTexture(ShadeChannel attribute, ShadeParameters setup,
                                    ShadeInt x, ShadeInt y, ShadeInt scanStart) {
    return attributePixel(attribute, setup, x, y, scanStart, ~31u);
}

inline ShadeUInt interpolateDepth(ShadeChannel attribute, ShadeParameters setup,
                                  ShadeInt x, ShadeInt y, ShadeInt scanStart) {
    // Unlike RGBA/STW, the depth X derivative retains its low five bits.
    return attributePixel(attribute, setup, x, y, scanStart, ~0u);
}
