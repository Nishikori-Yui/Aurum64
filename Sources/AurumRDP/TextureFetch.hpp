// SPDX-License-Identifier: MIT
#pragma once
#include "RDP.hpp"
#include "TextureUpload.hpp"
#include "TextureCoordinates.hpp"
#include "Tmem.hpp"
#include <cstdint>

namespace aurum::texture {
// A tile descriptor as SetTile stores it: format, size and the two TMEM
// addressing fields, both counting 8-byte units.
struct TileDescriptor {
    uint32_t format = 0, size = 0, line = 0, tmem = 0;
    AxisCoordinates s{}, t{};
};

inline TileDescriptor tileDescriptor(const aurum::rdp::TextureRect& rect) {
    const uint32_t modes = rect.tileModes;
    TileDescriptor tile{rect.tileFormat, rect.tileSize, rect.tileLine, rect.tileTmem};
    tile.s = {rect.tileSl, rect.tileSh, modes & 15u, (modes >> 4) & 15u,
              bool(modes & (1u << 8)), bool(modes & (1u << 9))};
    tile.t = {rect.tileTl, rect.tileTh, (modes >> 10) & 15u, (modes >> 14) & 15u,
              bool(modes & (1u << 18)), bool(modes & (1u << 19))};
    return tile;
}

struct Texel {
    uint32_t r = 0, g = 0, b = 0, a = 0;
};

// Explicit point-sampling fetch for debugging and for the copy paths that
// follow. This is deliberately not a filter: bilinear and three-point
// filtering and LOD are separate work. fetchFixedPoint maps signed coordinates
// for a point cycle; copy cycles must use mapCopyCoordinates instead.
class Sampler {
public:
    // Fetches the texel at integer coordinates (s, t) of a tile. Returns
    // false for combinations that are not modeled rather than approximating.
    bool fetch(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
               uint32_t s, uint32_t t, Texel& out) const {
        if (tile.format == 3 && tile.size == 1)
            return fetchIA8(tmem, tile, s, t, out);
        if (tile.format == 4 && tile.size == 1)
            return fetchI8(tmem, tile, s, t, out);
        if (tile.format == 3 && tile.size == 2)
            return fetchIA16(tmem, tile, s, t, out);
        if (tile.format != 0) return false;
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

    bool fetchFixedPoint(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
                         int32_t s, int32_t t, Texel& out) const {
        const int32_t mappedS = mapPointCoordinate(s, tile.s);
        const int32_t mappedT = mapPointCoordinate(t, tile.t);
        // Wrapped/negative TMEM addressing is not part of this fetch subset.
        if (mappedS < 0 || mappedT < 0) return false;
        return fetch(tmem, tile, uint32_t(mappedS), uint32_t(mappedT), out);
    }

private:
    bool fetchIA16(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
                   uint32_t s, uint32_t t, Texel& out) const {
        uint32_t index = 0;
        if (!wordIndex(tile, s, t, index)) return false;
        const uint32_t value = tmem.readHwWord(index);
        out = {value >> 8, value >> 8, value >> 8, value & 0xffu};
        return true;
    }

    bool fetchIA8(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
                  uint32_t s, uint32_t t, Texel& out) const {
        uint32_t address = 0;
        if (!byteAddress(tile, s, t, address)) return false;
        const uint32_t value = tmem.readByte(address);
        const uint32_t intensity = ((value >> 4) & 15u) * 17u;
        out = {intensity, intensity, intensity, (value & 15u) * 17u};
        return true;
    }

    bool fetchI8(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
                 uint32_t s, uint32_t t, Texel& out) const {
        uint32_t address = 0;
        if (!byteAddress(tile, s, t, address)) return false;
        const uint32_t intensity = tmem.readByte(address);
        out = {intensity, intensity, intensity, 255};
        return true;
    }

    bool byteAddress(const TileDescriptor& tile, uint32_t s, uint32_t t,
                     uint32_t& address) const {
        if (tile.line > 511 || tile.tmem > 511 || t > 255) return false;
        const uint64_t row = uint64_t(tile.tmem) * 8 + uint64_t(tile.line) * 8 * t;
        if (row >= aurum::tmem::Store::kBytes) return false;
        const uint64_t index = row + (s ^ ((t & 1u) ? 7u : 3u));
        if (index >= aurum::tmem::Store::kBytes) return false;
        address = uint32_t(index);
        return true;
    }

    bool fetchRGBA16(const aurum::tmem::Store& tmem, const TileDescriptor& tile,
                     uint32_t s, uint32_t t, Texel& out) const {
        uint32_t index = 0;
        if (!wordIndex(tile, s, t, index)) return false;
        const uint32_t value = tmem.readHwWord(index);
        const auto expand = [](uint32_t channel) { return (channel << 3) | (channel >> 2); };
        out.r = expand((value >> 11) & 31);
        out.g = expand((value >> 6) & 31);
        out.b = expand((value >> 1) & 31);
        out.a = (value & 1) ? 255 : 0;
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
        if (tile.line > 511 || tile.tmem > 511 || t > 255) return false;
        const uint64_t row = uint64_t(tile.tmem) + uint64_t(tile.line) * t;
        if (row >= Uploader::kTmemUnits) return false;
        const uint64_t word = row * 4 + s;
        if (word >= Uploader::kTmemWords16) return false;
        index = uint32_t(word) ^ ((t & 1) ? 3u : 1u);
        return index < Uploader::kTmemWords16;
    }
};
}
