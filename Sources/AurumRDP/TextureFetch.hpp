// SPDX-License-Identifier: MIT
#pragma once
#include "TextureUpload.hpp"
#include "Tmem.hpp"
#include <cstdint>

namespace aurum::texture {
// A tile descriptor as SetTile stores it: format, size and the two TMEM
// addressing fields, both counting 8-byte units.
struct TileDescriptor {
    uint32_t format = 0, size = 0, line = 0, tmem = 0;
};

struct Texel {
    uint32_t r = 0, g = 0, b = 0, a = 0;
};

// Explicit point-sampling fetch for debugging and for the copy paths that
// follow. This is deliberately not a filter: bilinear and three-point
// filtering, LOD, mask/mirror/clamp and signed coordinates are separate work,
// and a caller that needs them must not silently receive a nearest sample.
//
// Channel decoding uses the same rules as the reference engine's RGBA5551 and
// RGBA8888 helpers, which the VI scanout already applies and which the VI
// conversion tests cover.
class Sampler {
public:
    // Fetches the texel at integer coordinates (s, t) of a tile. Returns
    // false for combinations that are not modeled rather than approximating.
    bool fetch(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
               uint32_t s, uint32_t t, Texel& out) const {
        switch (tile.size) {
        case 2: return fetchRGBA16(tmem, tile, s, t, out);
        case 3: return fetchRGBA32(tmem, tile, s, t, out);
        default:
            // 8-bit and 4-bit texels carry an index or a packed channel pair,
            // and turning that into a color needs TLUT lookup and channel
            // layout rules that are not verified yet.
            return false;
        }
    }

private:
    bool fetchRGBA16(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
                     uint32_t s, uint32_t t, Texel& out) const {
        uint32_t index = 0;
        if (!wordIndex(tile, s, t, index)) return false;
        const uint32_t value = tmem.readHwWord(index);
        out.r = (value >> 8) & 0xf8;
        out.g = (value & 0x7c0) >> 3;
        out.b = (value & 0x3e) << 2;
        out.a = value & 1;
        return true;
    }

    bool fetchRGBA32(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
                     uint32_t s, uint32_t t, Texel& out) const {
        uint32_t index = 0;
        if (!wordIndex(tile, s, t, index)) return false;
        // RGBA32 keeps the high halfword in bank 0 and the low one in bank 1.
        if (index + Uploader::kBankWords16 >= Uploader::kTmemWords16)
            return false;
        const uint32_t value = (uint32_t(tmem.readHwWord(index)) << 16) |
                               tmem.readHwWord(index + Uploader::kBankWords16);
        out.r = (value >> 24) & 0xff;
        out.g = (value >> 16) & 0xff;
        out.b = (value >> 8) & 0xff;
        out.a = value & 0xff;
        return true;
    }

    bool wordIndex(const TileDescriptor& tile, uint32_t s, uint32_t t,
                   uint32_t& index) const {
        // The row base must stay inside the 9-bit unit field the hardware has.
        const uint64_t row = uint64_t(tile.tmem) + tile.line * t;
        if (row >= Uploader::kTmemUnits) return false;
        index = Uploader::tileWordIndex(tile.tmem, tile.line, t, s);
        return index < Uploader::kTmemWords16;
    }
};
}
