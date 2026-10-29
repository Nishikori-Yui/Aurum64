// SPDX-License-Identifier: MIT
#pragma once

#include "TriangleColorMath.h"

struct TriangleDepthParameters {
    TriangleColorParameters color;
    ShadeUInt zbAddress = 0, zbWidth = 0, zValue = 0, dzPixel = 1, dzEncoded = 0;
};
static_assert(sizeof(TriangleDepthParameters) == 232);
