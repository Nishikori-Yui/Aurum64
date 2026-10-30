// SPDX-License-Identifier: MIT
#pragma once

#include "TriangleSpans.hpp"
#include "TriangleShade.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aurum::rdp {
struct TriangleWord {
    uint32_t word = 0;
    uint32_t high = UINT32_MAX;
    uint32_t low = UINT32_MAX;
    int32_t highX = 0, highY = 0, highStart = 0;
    int32_t lowX = 0, lowY = 0, lowStart = 0;
    uint32_t highHidden = 0, lowHidden = 0;
};
static_assert(sizeof(TriangleWord) == 44);
static_assert(sizeof(ShadeParameters) == 84);

inline bool makeFlatTrianglePlan(const Triangle& triangle, size_t memoryBytes,
                                 uint32_t framebufferAddress,
                                 uint32_t framebufferWidth,
                                 uint32_t framebufferSize,
                                 uint16_t color,
                                 int32_t clipXh, int32_t clipYh,
                                 int32_t clipXl, int32_t clipYl,
                                 std::vector<TriangleWord>& words,
                                 std::string& error, bool preserveOrder = false) {
    words.clear();
    const auto reject = [&](const char* reason) {
        words.clear();
        error = reason;
        return false;
    };
    if (!memoryBytes || memoryBytes % 4 || memoryBytes > 8 * 1024 * 1024 ||
        framebufferSize != 2 || !framebufferWidth ||
        framebufferWidth > 4096 || (framebufferAddress & 1u) ||
        framebufferAddress >= memoryBytes || clipXh < 0 || clipYh < 0 ||
        clipXh > clipXl || clipYh > clipYl || clipXl > 4095 || clipYl > 4095)
        return reject("Invalid flat triangle plan target.");

    auto spans = generateTriangleSpans(triangle);
    clipTriangleSpans(spans, clipXh, clipYh, clipXl, clipYl);
    // Scissor can extend past the stride, so different rows may share words
    // or pixels. Resolve writes in issue order before assigning GPU ownership.
    std::map<uint32_t, TriangleWord> destinations;
    for (const auto& span : spans) {
        if (!span.valid) continue;
        for (int32_t x = span.left; x < span.right; ++x) {
            const uint8_t mask = trianglePixelCoverage(span, x);
            // With antialiasing disabled the first sample controls the write.
            if (!(mask & 0x80)) continue;
            const uint64_t address = uint64_t(framebufferAddress) +
                (uint64_t(span.y) * framebufferWidth + uint32_t(x)) * 2;
            if (address + 2 > memoryBytes)
                return reject("Flat triangle plan exceeds memory.");
            const uint32_t word = uint32_t(address / 4);
            auto& target = destinations.try_emplace(word, TriangleWord{word}).first->second;
            const uint16_t value = triangleCoverageColor(color, mask);
            const int32_t start = shadeScanStart(triangle, span);
            if (address & 2u) {
                target.low = value;
                target.lowHidden = (std::popcount(mask) - 1) & 3u;
                target.lowX = x; target.lowY = span.y; target.lowStart = start;
            } else {
                target.high = value;
                target.highHidden = (std::popcount(mask) - 1) & 3u;
                target.highX = x; target.highY = span.y; target.highStart = start;
            }
        }
        if (preserveOrder) {
            if (triangle.leftMajor) {
                for (const auto& [index, word] : destinations) words.push_back(word);
            } else {
                for (auto i = destinations.rbegin(); i != destinations.rend(); ++i)
                    words.push_back(i->second);
            }
            destinations.clear();
        }
    }
    for (const auto& [index, word] : destinations) words.push_back(word);
    return true;
}
} // namespace aurum::rdp
