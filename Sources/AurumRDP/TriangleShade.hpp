// SPDX-License-Identifier: MIT
#pragma once
#include "ShadeMath.h"
#include "TriangleSpans.hpp"

namespace aurum::rdp {
inline ShadeParameters makeShadeParameters(const Triangle& triangle) {
    ShadeParameters result{};
    for (unsigned channel = 0; channel < 4; ++channel) {
        const auto& a = triangle.rgba[channel];
        result.rgba[channel] = {a.initial, a.dx, a.de, a.dy};
    }
    result.firstRow = floorDiv(triangle.yh, 4);
    result.xh = triangle.xh;
    result.dxhdy = triangle.dxhdy;
    result.leftMajor = triangle.leftMajor;
    result.offset = triangle.majorSlopeNegative == triangle.leftMajor;
    return result;
}

inline int32_t shadeScanStart(const Triangle& triangle, const TriangleSpan& span) {
    if (triangle.leftMajor) return span.left;
    int32_t start = INT32_MIN;
    for (unsigned row = 0; row < 4; ++row)
        if (span.coverage & (1u << row)) start = std::max(start, span.sampleRight[row] >> 16);
    return start;
}
} // namespace aurum::rdp
