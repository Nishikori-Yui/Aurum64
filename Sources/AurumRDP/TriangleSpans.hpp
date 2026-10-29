// SPDX-License-Identifier: MIT
#pragma once

#include "RDP.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <vector>

namespace aurum::rdp {
struct TriangleSpan {
    int32_t y = 0;
    int32_t left = 0;
    int32_t right = 0; // Exclusive bounding pixel, not a coverage decision.
    uint8_t coverage = 0; // Valid Y subrows, distinct from the pixel sample mask.
    bool valid = false;
    std::array<int32_t, 4> sampleLeft{}, sampleRight{}; // 16.16 edges
};

inline int32_t floorDiv(int32_t value, int32_t divisor) {
    return value / divisor - (value % divisor < 0);
}

inline int32_t triangleEdge(int32_t origin, int32_t slope, int32_t subrows) {
    const int64_t step = (slope >> 2) & ~1;
    const uint32_t bits = uint32_t(int64_t(origin & ~1) + step * subrows) & 0x0fffffffu;
    return bits & 0x08000000u ? int32_t(int64_t(bits) - 0x10000000ll) : int32_t(bits);
}

inline void updateTriangleBounds(TriangleSpan& span) {
    span.left = INT32_MAX;
    span.right = INT32_MIN;
    for (unsigned row = 0; row < 4; ++row) {
        if (!(span.coverage & (1u << row))) continue;
        span.left = std::min(span.left, span.sampleLeft[row] >> 16);
        span.right = std::max(span.right, int32_t((int64_t(span.sampleRight[row]) + 65535) >> 16));
    }
    span.valid = span.coverage && span.left < span.right;
}

// XH/XM are anchored at the start of YH's integer row; XL is anchored at YM.
// Evaluate edges independently instead of intersecting all subrows of a pixel.
inline std::vector<TriangleSpan> generateTriangleSpans(const Triangle& triangle) {
    std::vector<TriangleSpan> spans;
    if (triangle.yh < -8192 || triangle.yl > 8191 || triangle.yh >= triangle.yl)
        return spans;
    const int32_t anchor = floorDiv(triangle.yh, 4) * 4;
    for (int32_t row = floorDiv(triangle.yh, 4); row * 4 < triangle.yl; ++row) {
        TriangleSpan span;
        span.y = row;
        for (int32_t subrow = 0; subrow < 4; ++subrow) {
            const int32_t y = row * 4 + subrow;
            if (y < triangle.yh || y >= triangle.yl) continue;
            const int32_t major = triangleEdge(triangle.xh, triangle.dxhdy, y - anchor);
            const int32_t minor = y >= triangle.ym
                ? triangleEdge(triangle.xl, triangle.dxldy, y - triangle.ym)
                : triangleEdge(triangle.xm, triangle.dxmdy, y - anchor);
            const int32_t left = triangle.leftMajor ? major : minor;
            const int32_t right = triangle.leftMajor ? minor : major;
            span.sampleLeft[subrow] = left;
            span.sampleRight[subrow] = right;
            // Edge-crossing detection uses quarter-pixel precision.
            if ((left >> 14) <= (right >> 14)) span.coverage |= uint8_t(1u << subrow);
        }
        updateTriangleBounds(span);
        spans.push_back(span);
    }
    return spans;
}

// Raw scissor coordinates are 10.2, including fractional boundaries.
inline void clipTriangleSpans(std::vector<TriangleSpan>& spans,
                              int32_t clipXh, int32_t clipYh,
                              int32_t clipXl, int32_t clipYl) {
    for (auto& span : spans) {
        for (int32_t row = 0; row < 4; ++row) {
            if (span.y * 4 + row < clipYh || span.y * 4 + row >= clipYl) {
                span.coverage &= uint8_t(~(1u << row));
                continue;
            }
            span.sampleLeft[row] = std::clamp(span.sampleLeft[row], clipXh * 16384, clipXl * 16384);
            span.sampleRight[row] = std::clamp(span.sampleRight[row], clipXh * 16384, clipXl * 16384);
        }
        updateTriangleBounds(span);
    }
}

inline uint8_t trianglePixelCoverage(const TriangleSpan& span, int32_t x) {
    uint8_t mask = 0;
    // Eight staggered samples: even subrows at X=0,1/2; odd at X=1/4,3/4.
    for (unsigned row = 0; row < 4; ++row) {
        if (!(span.coverage & (1u << row))) continue;
        for (unsigned sample = 0; sample < 2; ++sample) {
            const int64_t position = int64_t(x) * 65536 + (row & 1u) * 16384 + sample * 32768;
            if (position >= span.sampleLeft[row] && position < span.sampleRight[row])
                mask |= uint8_t(1u << (7 - row * 2 - sample));
        }
    }
    return mask;
}

inline uint16_t triangleCoverageColor(uint16_t rgb, uint8_t mask) {
    // RGBA16's low framebuffer bit stores coverage, not primitive alpha.
    const unsigned count = std::popcount(mask);
    return uint16_t((rgb & 0xfffeu) | (count ? ((count - 1) >> 2) : 0));
}
} // namespace aurum::rdp
