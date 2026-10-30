// SPDX-License-Identifier: MIT
#pragma once

#include "DepthMath.h"
#include "MemoryBus.hpp"
#include "TriangleColor.hpp"
#include "TriangleDepthMath.h"
#include <set>

namespace aurum::rdp {
inline TriangleDepthParameters makeTriangleDepthParameters(const Triangle& triangle) {
    TriangleDepthParameters result{};
    result.color = makeTriangleColorParameters(triangle);
    result.zbAddress = triangle.zbAddress;
    result.zbWidth = triangle.fbWidth;
    const auto depth = triangle.depth;
    result.depth = {depth.initial, depth.dx, depth.de, depth.dy};
    result.usePrimitiveZ = (triangle.otherModesLo >> 2) & 1u;
    result.primitiveZ = triangle.primitiveDepth & 0x7fff0000u;
    result.dzPixel = result.usePrimitiveZ ? triangle.primitiveDepth & 0xffffu :
        depthPixelDelta(depth.dx, depth.dy);
    result.dzEncoded = depthDeltaCompress(result.dzPixel);
    result.modes = triangle.otherModesLo;
    return result;
}

inline bool makeDepthPlan(const Triangle& triangle, size_t memoryBytes,
                                std::vector<TriangleWord>& words, std::string& error) {
    words.clear();
    if (!isDepthTriangle(triangle)) {
        error = "Unsupported depth triangle.";
        return false;
    }
    if (((triangle.zbAddress ^ triangle.fbAddress) & 3u) != 0) {
        error = "Opaque depth/color word ownership is not aligned.";
        return false;
    }
    if (!makeFlatTrianglePlan(triangle, memoryBytes, triangle.fbAddress, triangle.fbWidth,
                              triangle.fbSize, 0, triangle.clipXh, triangle.clipYh,
                              triangle.clipXl, triangle.clipYl, words, error,
                              (triangle.otherModesLo & 0x10u) != 0))
        return false;
    std::set<uint32_t> colorWords;
    for (const auto& word : words) colorWords.insert(word.word);
    for (const auto& word : words) {
        const auto check = [&](int32_t x, int32_t y) {
            const uint64_t address = uint64_t(triangle.zbAddress) +
                (uint64_t(y) * triangle.fbWidth + uint32_t(x)) * 2;
            return address + 2 <= memoryBytes && !colorWords.contains(uint32_t(address / 4));
        };
        if ((word.high != UINT32_MAX && !check(word.highX, word.highY)) ||
            (word.low != UINT32_MAX && !check(word.lowX, word.lowY))) {
            words.clear();
            error = "Depth plan exceeds memory or aliases a color word.";
            return false;
        }
    }
    return true;
}

inline bool depthLane(uint8_t* memory, size_t memoryBytes, std::span<uint8_t> hidden,
                            const TriangleDepthParameters& parameters,
                            int32_t x, int32_t y, int32_t start, uint16_t color, uint8_t coverage,
                            uint32_t colorAddress, uint8_t coverageHidden, bool& wrote) {
    wrote = false;
    const uint64_t zAddress64 = uint64_t(parameters.zbAddress) +
        (uint64_t(y) * parameters.zbWidth + uint32_t(x)) * 2;
    if (zAddress64 + 2 > memoryBytes || hidden.size() < memoryBytes / 2) return false;
    const uint32_t zAddress = uint32_t(zAddress64);
    uint16_t stored = 0;
    if (!aurum::memory::readHalfword(memory, memoryBytes, zAddress, stored)) return false;
    uint16_t oldColor = 0;
    if (!aurum::memory::readHalfword(memory, memoryBytes, colorAddress, oldColor)) return false;
    const uint32_t incoming = triangleDepthValue(parameters, x, y, start);
    const uint32_t count = (uint32_t(coverage) << 2) + coverageHidden + 1;
    if (!triangleDepthPass(parameters, incoming, stored, hidden[zAddress / 2],
                           oldColor, hidden[colorAddress / 2], count)) return true;
    const uint16_t output = uint16_t((color & 0xfffeu) | coverage);
    if (!aurum::memory::writeHalfword(memory, memoryBytes, colorAddress, output))
        return false;
    hidden[colorAddress / 2] = coverageHidden;
    if (parameters.modes & 0x20u) {
        if (!aurum::memory::writeHalfword(memory, memoryBytes, zAddress,
                uint16_t(depthCompress(incoming) | (parameters.dzEncoded >> 2)))) return false;
        hidden[zAddress / 2] = uint8_t(parameters.dzEncoded & 3u);
    }
    wrote = true;
    return true;
}

inline bool depthWord(uint8_t* memory, size_t memoryBytes, std::span<uint8_t> hidden,
                            const TriangleDepthParameters& parameters,
                            const TriangleWord& word, uint32_t& writeMask) {
    writeMask = 0;
    const uint16_t color = primitiveColor16(parameters.color.primitiveColor);
    if (word.high != UINT32_MAX) {
        bool wrote = false;
        if (!depthLane(memory, memoryBytes, hidden, parameters, word.highX, word.highY, word.highStart,
                             color, uint8_t(word.high & 1u), word.word * 4, word.highHidden, wrote))
            return false;
        if (wrote) writeMask |= 1u;
    }
    if (word.low != UINT32_MAX) {
        bool wrote = false;
        if (!depthLane(memory, memoryBytes, hidden, parameters, word.lowX, word.lowY, word.lowStart,
                             color, uint8_t(word.low & 1u), word.word * 4 + 2, word.lowHidden, wrote))
            return false;
        if (wrote) writeMask |= 2u;
    }
    return true;
}
} // namespace aurum::rdp
