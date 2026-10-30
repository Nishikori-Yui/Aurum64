// SPDX-License-Identifier: MIT
#pragma once

#include "TriangleColorMath.h"
#include "DepthMath.h"
#include "HiddenMath.h"

struct TriangleDepthParameters {
    TriangleColorParameters color;
    ShadeChannel depth;
    ShadeUInt zbAddress = 0, zbWidth = 0, primitiveZ = 0, dzPixel = 1, dzEncoded = 0;
    ShadeUInt usePrimitiveZ = 0;
    ShadeUInt modes = 0;
};
static_assert(sizeof(TriangleDepthParameters) == 256);

inline ShadeUInt triangleDepthValue(TriangleDepthParameters parameters,
                                    ShadeInt x, ShadeInt y, ShadeInt start) {
    if (parameters.usePrimitiveZ) return nonAADepth(parameters.primitiveZ);
    return nonAADepth(interpolateDepth(parameters.depth, parameters.color.texture.setup,
                                      x, y, start));
}

inline bool triangleDepthPass(TriangleDepthParameters parameters, ShadeUInt incoming,
                              ShadeUInt stored, ShadeUInt zHidden, ShadeUInt color,
                              ShadeUInt colorHidden, ShadeUInt coverage) {
    if (!(parameters.modes & 0x10u)) return true;
    const ShadeUInt memoryCoverage = (parameters.modes & 0x40u)
        ? ((color & 1u) << 2) | hiddenRead(colorHidden, color) : 7u;
    const ShadeUInt delta = ((stored & 3u) << 2) | hiddenRead(zHidden, stored);
    return depthOpaquePass(incoming, stored, delta, parameters.dzPixel,
                           ((memoryCoverage + coverage) & 8u) != 0);
}
