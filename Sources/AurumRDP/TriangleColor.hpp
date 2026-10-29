// SPDX-License-Identifier: MIT
#pragma once

#include "TextureFetch.hpp"
#include "TriangleColorMath.h"
#include "TriangleMode.hpp"
#include "TrianglePlan.hpp"

namespace aurum::rdp {
inline TriangleColorParameters makeTriangleColorParameters(const Triangle& triangle) {
    TriangleColorParameters result{};
    result.texture.setup = makeShadeParameters(triangle);
    for (unsigned i = 0; i < 3; ++i) {
        const auto a = triangle.stw[i];
        result.texture.stw[i] = {a.initial, a.dx, a.de, a.dy};
    }
    const uint32_t modes = triangle.tileModes;
    result.texture.s = {triangle.tileSl, triangle.tileSh, modes & 15u, (modes >> 4) & 15u,
                        (modes >> 8) & 1u, (modes >> 9) & 1u};
    result.texture.t = {triangle.tileTl, triangle.tileTh, (modes >> 10) & 15u,
                        (modes >> 14) & 15u, (modes >> 18) & 1u, (modes >> 19) & 1u};
    result.texture.line = triangle.tileLine;
    result.texture.tmem = triangle.tileTmem;
    result.texture.perspective = (triangle.otherModesHi >> 19) & 1u;
    result.combineHi = triangle.combineHi;
    result.combineLo = triangle.combineLo;
    result.primitiveColor = triangle.primitiveColor;
    result.blendColor = triangle.blendColor;
    result.flags = (triangle.shade ? 1u : 0u) | (triangle.texture ? 2u : 0u) |
        ((triangle.otherModesLo & 1u) ? 4u : 0u);
    return result;
}

inline bool makeTriangleColorPlan(const Triangle& triangle, size_t memoryBytes,
                                  std::vector<TriangleWord>& words, std::string& error) {
    words.clear();
    if (!isSimpleCombinerTriangle(triangle)) {
        error = "Unsupported one-cycle triangle combiner.";
        return false;
    }
    if (!makeFlatTrianglePlan(triangle, memoryBytes, triangle.fbAddress, triangle.fbWidth,
                              triangle.fbSize, 0, triangle.clipXh, triangle.clipYh,
                              triangle.clipXl, triangle.clipYl, words, error))
        return false;
    if (!triangle.texture) return true;
    const auto parameters = makeTriangleColorParameters(triangle);
    for (const auto& word : words) {
        if ((word.high != UINT32_MAX &&
             triangleTextureIndex(parameters.texture, word.highX, word.highY, word.highStart) == UINT32_MAX) ||
            (word.low != UINT32_MAX &&
             triangleTextureIndex(parameters.texture, word.lowX, word.lowY, word.lowStart) == UINT32_MAX)) {
            words.clear();
            error = "One-cycle combiner triangle would wrap unsupported TMEM addressing.";
            return false;
        }
    }
    return true;
}

inline bool sampleTriangleTexel(const TriangleColorParameters& parameters,
                                const aurum::tmem::Store& tmem,
                                int32_t x, int32_t y, int32_t start,
                                CombinerColor& result) {
    const uint32_t source = triangleTextureIndex(parameters.texture, x, y, start);
    if (source == UINT32_MAX) return false;
    const uint16_t value = tmem.readHwWord(source);
    const auto expand = [](uint32_t channel) { return (channel << 3) | (channel >> 2); };
    result = {expand((value >> 11) & 31u), expand((value >> 6) & 31u),
              expand((value >> 1) & 31u), (value & 1u) ? 255u : 0u};
    return true;
}

inline bool triangleColorWord(const TriangleColorParameters& parameters,
                              const aurum::tmem::Store* tmem,
                              const TriangleWord& word, uint16_t& high, uint16_t& low,
                              uint32_t& writeMask) {
    writeMask = 0;
    const CombinerColor primitive = primitiveCombinerColor(parameters.primitiveColor);
    const auto write = [&](int32_t x, int32_t y, int32_t start,
                           uint16_t& destination, uint32_t bit) {
        const CombinerColor shade = (parameters.flags & 1u)
            ? shadeCombinerColor(parameters.texture.setup, x, y, start) : CombinerColor{};
        CombinerColor texel{};
        if (parameters.flags & 2u) {
            if (!tmem || !sampleTriangleTexel(parameters, *tmem, x, y, start, texel)) return false;
        }
        const CombinerColor color = simpleCombinerColor(parameters.combineHi, parameters.combineLo,
                                                        texel, shade, primitive);
        if ((parameters.flags & 4u) && color.a < (parameters.blendColor & 0xffu)) return true;
        destination = uint16_t(((color.r >> 3) << 11) | ((color.g >> 3) << 6) |
                               ((color.b >> 3) << 1));
        writeMask |= bit;
        return true;
    };
    if (word.high != UINT32_MAX && !write(word.highX, word.highY, word.highStart, high, 1u)) return false;
    if (word.low != UINT32_MAX && !write(word.lowX, word.lowY, word.lowStart, low, 2u)) return false;
    return true;
}

inline bool triangleColorWord(const TriangleColorParameters& parameters,
                              const aurum::tmem::Store* tmem,
                              const TriangleWord& word, uint16_t& high, uint16_t& low) {
    uint32_t writeMask = 0;
    return triangleColorWord(parameters, tmem, word, high, low, writeMask);
}
} // namespace aurum::rdp
