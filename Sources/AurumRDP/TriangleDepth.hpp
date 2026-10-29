// SPDX-License-Identifier: MIT
#pragma once

#include "DepthMath.h"
#include "MemoryBus.hpp"
#include "TriangleColor.hpp"
#include "TriangleDepthMath.h"

namespace aurum::rdp {
inline TriangleDepthParameters makeTriangleDepthParameters(const Triangle& triangle) {
    TriangleDepthParameters result{};
    result.color = makeTriangleColorParameters(triangle);
    result.zbAddress = triangle.zbAddress;
    result.zbWidth = triangle.fbWidth;
    result.zValue = nonAADepth(ShadeUInt(triangle.depth.initial));
    return result;
}

inline bool makeOpaqueDepthPlan(const Triangle& triangle, size_t memoryBytes,
                                std::vector<TriangleWord>& words, std::string& error) {
    words.clear();
    if (!isDepthUpdateTriangle(triangle)) {
        error = "Unsupported depth-update triangle.";
        return false;
    }
    if (((triangle.zbAddress ^ triangle.fbAddress) & 3u) != 0) {
        error = "Opaque depth/color word ownership is not aligned.";
        return false;
    }
    if (!makeFlatTrianglePlan(triangle, memoryBytes, triangle.fbAddress, triangle.fbWidth,
                              triangle.fbSize, 0, triangle.clipXh, triangle.clipYh,
                              triangle.clipXl, triangle.clipYl, words, error))
        return false;
    for (const auto& word : words) {
        const auto check = [&](int32_t x, int32_t y) {
            const uint64_t address = uint64_t(triangle.zbAddress) +
                (uint64_t(y) * triangle.fbWidth + uint32_t(x)) * 2;
            return address + 2 <= memoryBytes;
        };
        if ((word.high != UINT32_MAX && !check(word.highX, word.highY)) ||
            (word.low != UINT32_MAX && !check(word.lowX, word.lowY))) {
            words.clear();
            error = "Opaque depth plan exceeds memory.";
            return false;
        }
    }
    return true;
}

inline bool opaqueDepthLane(uint8_t* memory, size_t memoryBytes,
                            const TriangleDepthParameters& parameters,
                            int32_t x, int32_t y, uint16_t color, uint8_t coverage,
                            uint32_t colorAddress, bool& wrote) {
    const uint64_t zAddress64 = uint64_t(parameters.zbAddress) +
        (uint64_t(y) * parameters.zbWidth + uint32_t(x)) * 2;
    if (zAddress64 + 2 > memoryBytes) return false;
    const uint32_t zAddress = uint32_t(zAddress64);
    uint16_t stored = 0;
    if (!aurum::memory::readHalfword(memory, memoryBytes, zAddress, stored)) return false;
    const uint16_t output = uint16_t((color & 0xfffeu) | coverage);
    if (!aurum::memory::writeHalfword(memory, memoryBytes, colorAddress, output) ||
        !aurum::memory::writeHalfword(memory, memoryBytes, zAddress,
                                      uint16_t(depthCompress(parameters.zValue) |
                                               (parameters.dzEncoded >> 2))))
        return false;
    wrote = true;
    return true;
}

inline bool opaqueDepthWord(uint8_t* memory, size_t memoryBytes,
                            const TriangleDepthParameters& parameters,
                            const TriangleWord& word, uint32_t& writeMask) {
    writeMask = 0;
    const uint16_t color = primitiveColor16(parameters.color.primitiveColor);
    if (word.high != UINT32_MAX) {
        bool wrote = false;
        if (!opaqueDepthLane(memory, memoryBytes, parameters, word.highX, word.highY,
                             color, uint8_t(word.high & 1u), word.word * 4, wrote))
            return false;
        if (wrote) writeMask |= 1u;
    }
    if (word.low != UINT32_MAX) {
        bool wrote = false;
        if (!opaqueDepthLane(memory, memoryBytes, parameters, word.lowX, word.lowY,
                             color, uint8_t(word.low & 1u), word.word * 4 + 2, wrote))
            return false;
        if (wrote) writeMask |= 2u;
    }
    return true;
}
} // namespace aurum::rdp
