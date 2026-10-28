// SPDX-License-Identifier: MIT
#pragma once

#include "MemoryBus.hpp"
#include "TriangleSpans.hpp"
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
    if (!memory || !memoryBytes || framebufferSize != 2 ||
        !framebufferWidth || framebufferWidth > 4096 ||
        (framebufferAddress & 1u) || framebufferAddress >= memoryBytes ||
        clipXh < 0 || clipYh < 0 || clipXh >= clipXl || clipYh >= clipYl)
        return reject("Invalid flat triangle reference target.");

    auto spans = generateTriangleSpans(triangle);
    clipTriangleSpans(spans, clipXh, clipYh, clipXl, clipYl);
    std::vector<uint32_t> addresses;
    for (const auto& span : spans) {
        if (!span.valid) continue;
        for (int32_t x = span.left; x < span.right; ++x) {
            if (x < 0 || x >= static_cast<int32_t>(framebufferWidth)) continue;
            const uint64_t address = uint64_t(framebufferAddress) +
                (uint64_t(span.y) * framebufferWidth + uint32_t(x)) * 2;
            if (address + 2 > memoryBytes)
                return reject("Flat triangle reference write exceeds memory.");
            addresses.push_back(static_cast<uint32_t>(address));
        }
    }
    for (const uint32_t address : addresses)
        if (!aurum::memory::writeHalfword(memory, memoryBytes, address, color))
            return reject("Flat triangle reference write failed.");
    return true;
}
} // namespace aurum::rdp
