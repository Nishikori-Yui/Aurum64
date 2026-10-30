// SPDX-License-Identifier: MIT
#pragma once
#include "TriangleTextureMath.h"
#include "TrianglePlan.hpp"
#include "TriangleMode.hpp"
#include "TextureFetch.hpp"

namespace aurum::rdp {
inline TriangleTextureParameters makeTriangleTextureParameters(const Triangle& triangle) {
    TriangleTextureParameters result{};
    result.setup = makeShadeParameters(triangle);
    for (unsigned i = 0; i < 3; ++i) {
        const auto a = triangle.stw[i];
        result.stw[i] = {a.initial, a.dx, a.de, a.dy};
    }
    const uint32_t modes = triangle.tileModes;
    result.s = {triangle.tileSl, triangle.tileSh, modes & 15u, (modes >> 4) & 15u,
                (modes >> 8) & 1u, (modes >> 9) & 1u};
    result.t = {triangle.tileTl, triangle.tileTh, (modes >> 10) & 15u, (modes >> 14) & 15u,
                (modes >> 18) & 1u, (modes >> 19) & 1u};
    result.line = triangle.tileLine;
    result.tmem = triangle.tileTmem;
    result.perspective = (triangle.otherModesHi >> 19) & 1u;
    return result;
}
static_assert(sizeof(TriangleTextureParameters) == 192);

inline bool makeTexturedTrianglePlan(const Triangle& triangle, size_t memoryBytes,
                                     std::vector<TriangleWord>& words, std::string& error) {
    words.clear();
    if (!isTexturedTriangle(triangle) || triangle.tileLine > 511 || triangle.tileTmem > 511 ||
        triangle.tileSl > 4095 || triangle.tileTl > 4095 || triangle.tileSh > 4095 || triangle.tileTh > 4095) {
        error = "Unsupported textured triangle format or mode."; return false;
    }
    if (!makeFlatTrianglePlan(triangle, memoryBytes, triangle.fbAddress, triangle.fbWidth,
            triangle.fbSize, 0, triangle.clipXh, triangle.clipYh, triangle.clipXl, triangle.clipYl,
            words, error)) return false;
    const auto parameters = makeTriangleTextureParameters(triangle);
    for (const auto& word : words) {
        if ((word.high != UINT32_MAX && triangleTextureIndex(parameters, word.highX, word.highY, word.highStart) == UINT32_MAX) ||
            (word.low != UINT32_MAX && triangleTextureIndex(parameters, word.lowX, word.lowY, word.lowStart) == UINT32_MAX)) {
            words.clear(); error = "Textured triangle would wrap unsupported TMEM addressing."; return false;
        }
    }
    return true;
}

inline aurum::texture::TileDescriptor triangleTextureDescriptor(const Triangle& triangle) {
    const auto p = makeTriangleTextureParameters(triangle);
    return {triangle.tileFormat, triangle.tileSize, p.line, p.tmem,
        {p.s.lower, p.s.upper, p.s.shift, p.s.mask, bool(p.s.mirror), bool(p.s.clamp)},
        {p.t.lower, p.t.upper, p.t.shift, p.t.mask, bool(p.t.mirror), bool(p.t.clamp)}};
}
} // namespace aurum::rdp
