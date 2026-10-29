// SPDX-License-Identifier: MIT
#pragma once

#include "MemoryBus.hpp"
#include "TriangleSpans.hpp"
#include "TriangleShade.hpp"
#include "TriangleMode.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aurum::rdp {
inline bool drawFlatTriangleReference(uint8_t* memory, size_t memoryBytes,
                                      uint32_t framebufferAddress,
                                      uint32_t framebufferWidth,
                                      uint32_t framebufferSize,
                                      uint16_t color,
                                      const Triangle& triangle,
                                      int32_t clipXh, int32_t clipYh,
                                      int32_t clipXl, int32_t clipYl,
                                      std::string& error) {
    const auto reject = [&](const char* reason) {
        error = reason;
        return false;
    };
    if (!memory || !memoryBytes || memoryBytes % 4 || memoryBytes > 8 * 1024 * 1024 || framebufferSize != 2 ||
        !framebufferWidth || framebufferWidth > 4096 ||
        (framebufferAddress & 1u) || framebufferAddress >= memoryBytes ||
        clipXh < 0 || clipYh < 0 || clipXh > clipXl || clipYh > clipYl ||
        clipXl > 4095 || clipYl > 4095)
        return reject("Invalid flat triangle reference target.");

    auto spans = generateTriangleSpans(triangle);
    clipTriangleSpans(spans, clipXh, clipYh, clipXl, clipYl);
    std::vector<std::pair<uint32_t, uint16_t>> writes;
    const auto shade = makeShadeParameters(triangle);
    for (const auto& span : spans) {
        if (!span.valid) continue;
        for (int32_t x = span.left; x < span.right; ++x) {
            const uint8_t mask = trianglePixelCoverage(span, x);
            if (!(mask & 0x80)) continue;
            const uint64_t address = uint64_t(framebufferAddress) +
                (uint64_t(span.y) * framebufferWidth + uint32_t(x)) * 2;
            if (address + 2 > memoryBytes)
                return reject("Flat triangle reference write exceeds memory.");
            const uint16_t rgb = isShadeTriangle(triangle)
                ? uint16_t(shadeRGB16(shade, x, span.y, shadeScanStart(triangle, span))) : color;
            writes.emplace_back(static_cast<uint32_t>(address), triangleCoverageColor(rgb, mask));
        }
    }
    for (const auto& [address, value] : writes)
        if (!aurum::memory::writeHalfword(memory, memoryBytes, address, value))
            return reject("Flat triangle reference write failed.");
    return true;
}
} // namespace aurum::rdp
