// SPDX-License-Identifier: MIT
#pragma once

#include "RDP.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace aurum::rdp {
struct TriangleSpan {
    int32_t y = 0;
    int32_t left = 0;
    int32_t right = 0;
    bool valid = false;
};

inline int32_t floorDiv(int32_t value, int32_t divisor) {
    const int32_t quotient = value / divisor;
    const int32_t remainder = value % divisor;
    return remainder < 0 ? quotient - 1 : quotient;
}

inline int32_t edgePixel(int32_t x) {
    return x >> 16;
}

// CPU-only first pass. It retains four subscanline edge samples per output
// row so later coverage work can compare intermediate results before drawing.
inline std::vector<TriangleSpan> generateTriangleSpans(const Triangle& triangle) {
    std::vector<TriangleSpan> spans;
    const int32_t firstRow = floorDiv(triangle.yh + 3, 4);
    const int32_t lastRow = floorDiv(triangle.yl + 3, 4);
    if (lastRow <= firstRow || triangle.yh >= triangle.yl) return spans;

    int32_t major = triangle.xh;
    int32_t minor = triangle.xm;
    int32_t majorStep = triangle.dxhdy >> 2;
    int32_t minorStep = triangle.dxmdy >> 2;
    const int32_t startSubpixel = firstRow * 4;
    const int32_t endSubpixel = lastRow * 4;
    for (int32_t subpixelY = startSubpixel; subpixelY < endSubpixel; ++subpixelY) {
        if (subpixelY == triangle.ym) {
            minor = triangle.xl;
            minorStep = triangle.dxldy >> 2;
        }
        const int32_t row = floorDiv(subpixelY, 4);
        if (spans.empty() || spans.back().y != row)
            spans.push_back({row, INT32_MIN, INT32_MAX, true});
        auto& span = spans.back();
        span.left = std::max(span.left, edgePixel(minor));
        span.right = std::min(span.right, edgePixel(major));
        if (span.left >= span.right) span.valid = false;
        major += majorStep;
        minor += minorStep;
    }
    return spans;
}
} // namespace aurum::rdp
