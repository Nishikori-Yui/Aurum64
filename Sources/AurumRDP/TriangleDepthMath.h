// SPDX-License-Identifier: MIT
#pragma once

#include "TriangleColorMath.h"
#include "DepthMath.h"

struct TriangleDepthParameters {
    TriangleColorParameters color;
    ShadeChannel depth;
    ShadeUInt zbAddress = 0, zbWidth = 0, primitiveZ = 0, dzPixel = 1, dzEncoded = 0;
    ShadeUInt usePrimitiveZ = 0;
};
static_assert(sizeof(TriangleDepthParameters) == 252);

inline ShadeUInt triangleDepthValue(TriangleDepthParameters parameters,
                                    ShadeInt x, ShadeInt y, ShadeInt start) {
    if (parameters.usePrimitiveZ) return nonAADepth(parameters.primitiveZ);
    return nonAADepth(interpolateDepth(parameters.depth, parameters.color.texture.setup,
                                      x, y, start));
}
