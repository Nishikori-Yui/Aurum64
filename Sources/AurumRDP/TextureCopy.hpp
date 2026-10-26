// SPDX-License-Identifier: MIT
#pragma once
#include "TextureFetch.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace aurum::texture {
// Each GPU invocation owns both guest halfwords of one host word. The CPU
// resolves addresses only; TMEM values and alpha tests stay on the GPU.
struct CopyWord {
    uint32_t word = 0;
    uint32_t high = UINT32_MAX, low = UINT32_MAX;
};
static_assert(sizeof(CopyWord) == 12);

inline int32_t copyCoordinate(int32_t initial, uint32_t steps, int32_t derivative) {
    const uint32_t bits = (uint32_t(initial) << 16) +
                          steps * uint32_t(derivative) * 2048u;
    return signedCoordinate(bits >> 16);
}

inline bool makeCopyPlan(const aurum::rdp::TextureRect& rect, size_t memoryBytes,
                         std::vector<CopyWord>& words, std::string& error) {
    words.clear();
    const auto reject = [&](const char* reason) {
        words.clear();
        error = reason;
        return false;
    };
    const bool indexed = rect.tileFormat == 2 && rect.tileSize <= 1;
    const uint32_t expectedModes = (2u << 20) | (indexed ? (1u << 15) : 0);
    if (rect.cycleType != 2 || rect.otherModesHi != expectedModes ||
        (rect.otherModesLo & ~1u) || rect.scissorMode ||
        rect.fbFormat != 0 || rect.fbSize != 2 ||
        (!indexed && (rect.tileFormat != 0 || rect.tileSize != 2)))
        return reject("Unsupported texture copy format or mode.");
    if (((rect.xh | rect.yh | rect.xl | rect.yl | rect.clipXh | rect.clipYh |
          rect.clipXl | rect.clipYl) & 3u) ||
        std::max({rect.xh, rect.yh, rect.xl, rect.yl, rect.clipXh, rect.clipYh,
                  rect.clipXl, rect.clipYl}) > 4095)
        return reject("Fractional texture copy edges are not modeled.");
    if (!memoryBytes || memoryBytes % 4 || memoryBytes > (8u << 20) ||
        !rect.fbWidth || rect.fbWidth > 1024 || (rect.fbAddress & 1u) ||
        rect.fbAddress >= memoryBytes || rect.tileLine > 511 || rect.tileTmem > 511)
        return reject("Invalid texture copy memory layout.");

    if (rect.xh > rect.xl || rect.yh > rect.yl ||
        rect.clipXh >= rect.clipXl || rect.clipYh >= rect.clipYl ||
        rect.xh >= rect.clipXl || rect.xl < rect.clipXh ||
        rect.yh >= rect.clipYl || rect.yl < rect.clipYh)
        return true;
    const uint32_t x0 = std::max(rect.xh, rect.clipXh) >> 2;
    // Copy includes the rightmost scissored pixel; the bottom scissor excludes
    // its row. Left clipping starts a fresh qword without advancing S/T.
    const uint32_t xEnd = (std::min(rect.xl, rect.clipXl) >> 2) + 1;
    const uint32_t y0 = std::max(rect.yh, rect.clipYh) >> 2;
    const uint32_t yEnd = std::min((rect.yl >> 2) + 1, rect.clipYl >> 2);
    if (y0 >= yEnd) return true;
    if (xEnd > rect.fbWidth ||
        uint64_t(rect.fbAddress) + (uint64_t(yEnd - 1) * rect.fbWidth + xEnd) * 2 > memoryBytes)
        return reject("Texture copy would exceed the supported framebuffer.");

    const TileDescriptor tile = tileDescriptor(rect);
    for (uint32_t y = y0; y < yEnd; ++y) {
        const uint32_t row = y - (rect.yh >> 2);
        const int32_t rowS = copyCoordinate(rect.s, row, rect.flip ? rect.dsdx : 0);
        const int32_t rowT = copyCoordinate(rect.t, row, rect.flip ? 0 : rect.dtdy);
        // Retain the fractional accumulator across both row and group steps.
        for (uint32_t x = x0; x < xEnd; x += 4) {
            const uint32_t group = (x - x0) / 4;
            const int32_t s = rect.flip ? rowS : copyCoordinate(rect.s, group, rect.dsdx);
            const int32_t t = rect.flip ? copyCoordinate(rect.t, group, rect.dtdy) : rowT;
            const CopyCoordinates coordinates = mapCopyCoordinates(s, t, tile.s, tile.t);
            if (coordinates.t < 0 || coordinates.t > 255)
                return reject("Wrapped texture copy T addressing is not modeled.");
            const uint64_t base = uint64_t(tile.tmem) + uint64_t(tile.line) * coordinates.t;
            if (base >= 512) return reject("Wrapped texture copy row addressing is not modeled.");
            uint32_t indices[4];
            uint32_t bankWords[4];
            for (uint32_t lane = 0; lane < 4; ++lane) {
                if (coordinates.s[lane] < 0)
                    return reject("Wrapped texture copy S addressing is not modeled.");
                if (indexed) {
                    const uint32_t s = uint32_t(coordinates.s[lane]);
                    const uint64_t address = base * 8 + (rect.tileSize == 0 ? s / 2 : s);
                    if (address >= 2048)
                        return reject("Indexed texture copy exceeds the lower TMEM half.");
                    const uint32_t byte = uint32_t(address) ^ ((coordinates.t & 1) ? 7u : 3u);
                    bankWords[lane] = byte / 2;
                    // Palette reads have one port per qword lane; host layout
                    // swaps each halfword pair. Bit 14 selects the CI4 nibble.
                    indices[lane] = byte | (lane ^ 1u) << 12 | (s & 1u) << 14;
                } else {
                    if (base * 4 + coordinates.s[lane] >= 2048)
                        return reject("Wrapped texture copy S addressing is not modeled.");
                    indices[lane] = uint32_t(base * 4 + coordinates.s[lane]) ^
                                    ((coordinates.t & 1) ? 3u : 1u);
                    bankWords[lane] = indices[lane];
                }
                for (uint32_t earlier = 0; earlier < lane; ++earlier)
                    if ((bankWords[earlier] & 3u) == (bankWords[lane] & 3u) &&
                        (bankWords[earlier] & 1023u) != (bankWords[lane] & 1023u))
                        return reject("Texture copy bank conflicts are not modeled.");
            }
            for (uint32_t lane = 0; lane < 4 && x + lane < xEnd; ++lane) {
                const uint32_t address = rect.fbAddress + (y * rect.fbWidth + x + lane) * 2;
                const uint32_t word = address / 4;
                if (words.empty() || words.back().word != word) words.push_back({word});
                auto& destination = (address & 2u) ? words.back().low : words.back().high;
                destination = indices[lane];
            }
        }
    }
    return true;
}
} // namespace aurum::texture
