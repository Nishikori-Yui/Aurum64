// SPDX-License-Identifier: MIT
#pragma once

#include "TriangleSpans.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aurum::rdp {
struct TriangleWord {
    uint32_t word = 0;
    uint32_t high = UINT32_MAX;
    uint32_t low = UINT32_MAX;
};
static_assert(sizeof(TriangleWord) == 12);

inline bool makeFlatTrianglePlan(const Triangle& triangle, size_t memoryBytes,
                                 uint32_t framebufferAddress,
                                 uint32_t framebufferWidth,
                                 uint32_t framebufferSize,
                                 uint16_t color,
                                 int32_t clipXh, int32_t clipYh,
                                 int32_t clipXl, int32_t clipYl,
                                 std::vector<TriangleWord>& words,
                                 std::string& error) {
    words.clear();
    const auto reject = [&](const char* reason) {
        words.clear();
        error = reason;
        return false;
    };
    if (!memoryBytes || framebufferSize != 2 || !framebufferWidth ||
        framebufferWidth > 4096 || (framebufferAddress & 1u) ||
        framebufferAddress >= memoryBytes || clipXh < 0 || clipYh < 0 ||
        clipXh >= clipXl || clipYh >= clipYl)
        return reject("Invalid flat triangle plan target.");

    auto spans = generateTriangleSpans(triangle);
    clipTriangleSpans(spans, clipXh, clipYh, clipXl, clipYl);
    for (const auto& span : spans) {
        if (!span.valid) continue;
        for (int32_t x = span.left; x < span.right; ++x) {
            if (x < 0 || x >= static_cast<int32_t>(framebufferWidth)) continue;
            const uint64_t address = uint64_t(framebufferAddress) +
                (uint64_t(span.y) * framebufferWidth + uint32_t(x)) * 2;
            if (address + 2 > memoryBytes)
                return reject("Flat triangle plan exceeds memory.");
            const uint32_t word = uint32_t(address / 4);
            if (words.empty() || words.back().word != word)
                words.push_back({word});
            if (address & 2u) words.back().low = color;
            else words.back().high = color;
        }
    }
    return true;
}
} // namespace aurum::rdp
