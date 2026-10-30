// SPDX-License-Identifier: MIT
#pragma once
#include "PerspectiveMath.h"
#include "TriangleShade.hpp"

namespace aurum::rdp {
struct TextureDepthSample {
    std::array<uint32_t, 3> stw{}; // Un-divided, interpolated 32-bit accumulators.
    uint32_t z = 0, correctedZ = 0;
    PerspectiveCoordinate coordinate{};
};

// Diagnostic stage for future triangle sampling/depth. It does not fetch
// texels or write a framebuffer, and does not enable a production mode.
inline TextureDepthSample interpolateTextureDepth(const Triangle& triangle,
                                                  const TriangleSpan& row,
                                                  int32_t x, bool perspective) {
    TextureDepthSample sample;
    const auto setup = makeShadeParameters(triangle);
    const int32_t start = shadeScanStart(triangle, row);
    for (unsigned channel = 0; channel < 3; ++channel) {
        const auto a = triangle.stw[channel];
        sample.stw[channel] = interpolateTexture({a.initial, a.dx, a.de, a.dy}, setup, x, row.y, start);
    }
    const auto z = triangle.depth;
    sample.z = interpolateDepth({z.initial, z.dx, z.de, z.dy}, setup, x, row.y, start);
    sample.correctedZ = nonAADepth(sample.z);
    sample.coordinate = divideTexture(sample.stw[0] >> 16, sample.stw[1] >> 16,
                                       sample.stw[2] >> 16, perspective);
    return sample;
}
} // namespace aurum::rdp
