// SPDX-License-Identifier: MIT
#pragma once

#include "ShadeMath.h"
#include "CombinerMath.h"
#include "TriangleTextureMath.h"

struct TriangleColorParameters {
    TriangleTextureParameters texture;
    ShadeUInt combineHi = 0, combineLo = 0, primitiveColor = 0, flags = 0;
};
static_assert(sizeof(TriangleColorParameters) == 208);

inline CombinerColor primitiveCombinerColor(ShadeUInt value) {
    return {(value >> 24) & 0xffu, (value >> 16) & 0xffu,
            (value >> 8) & 0xffu, value & 0xffu};
}

inline CombinerColor shadeCombinerColor(ShadeParameters setup, ShadeInt x, ShadeInt y,
                                        ShadeInt start) {
    return {shadeChannel(setup.rgba[0], setup, x, y, start),
            shadeChannel(setup.rgba[1], setup, x, y, start),
            shadeChannel(setup.rgba[2], setup, x, y, start),
            shadeChannel(setup.rgba[3], setup, x, y, start)};
}
