// SPDX-License-Identifier: MIT
#pragma once
#include "MemoryBus.hpp"
#include "Tmem.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace aurum::texture {
class Uploader {
public:
    void setTextureImage(uint32_t format, uint32_t size, uint32_t width,
                         uint32_t address) {
        tiFormat = format; tiSize = size; tiWidth = width; tiAddress = address;
    }

    void setTile(uint32_t tile, uint32_t tmemWord) {
        setTile(tile, tmemWord, 0);
    }
    void setTile(uint32_t tile, uint32_t tmemWord, uint32_t line) {
        if (tile < tmemWords.size()) {
            tmemWords[tile] = tmemWord & 0x1ff;
            tileLines[tile] = line & 0x1ff;
        }
    }

    bool loadTile(const uint8_t* rdram, size_t rdramBytes, uint32_t tile,
                  uint32_t sl, uint32_t tl, uint32_t sh, uint32_t th) {
        if (!commonTileValid(rdram, tile, sl, tl, sh, th) || th < tl)
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        const uint32_t columns = (sh - sl) / 4 + 1;
        const uint32_t rows = (th - tl) / 4 + 1;
        const uint32_t lineWords = tileLines[tile] * 4;
        const uint32_t destination = tmemWords[tile];
        // 64-bit so a wide tile extent cannot wrap past the 1024-word bound.
        if (!lineWords || columns > lineWords ||
            uint64_t(destination) + uint64_t(rows - 1) * lineWords +
                    columns > kTmemWords)
            return false;
        if (!sourceReadsValid(columns, rows, x0, y0, rdramBytes))
            return false;
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t column = 0; column < columns; ++column)
                tmem.write16(destination + row * lineWords + column,
                             readSource(rdram, rdramBytes, x0 + column,
                                        y0 + row));
        return true;
    }

    bool loadBlock(const uint8_t* rdram, size_t rdramBytes, uint32_t tile,
                   uint32_t sl, uint32_t tl, uint32_t sh, uint32_t dxt) {
        if (!rdram || tile >= tmemWords.size() || tiSize != 2 || !tiWidth ||
            dxt != 0 || (sl & 3) || (tl & 3) || (sh & 3) || sh < sl)
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        const uint32_t count = (sh - sl) / 4 + 1;
        const uint32_t destination = tmemWords[tile];
        if (uint64_t(destination) + count > kTmemWords) return false;
        if (!sourceReadsValid(count, 1, x0, y0, rdramBytes))
            return false;
        for (uint32_t i = 0; i < count; ++i)
            tmem.write16(destination + i,
                         readSource(rdram, rdramBytes, x0 + i, y0));
        return true;
    }

    bool loadTlut(const uint8_t* rdram, size_t rdramBytes, uint32_t tile,
                  uint32_t sl, uint32_t tl, uint32_t sh, uint32_t th) {
        if (!commonTileValid(rdram, tile, sl, tl, sh, th) || th < tl)
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        const uint32_t columns = (sh - sl) / 4 + 1;
        const uint32_t rows = (th - tl) / 4 + 1;
        const uint64_t count = uint64_t(columns) * rows;
        const uint32_t destination = tmemWords[tile];
        if (count > 512 || destination > 512 - uint32_t(count)) return false;
        if (!sourceReadsValid(columns, rows, x0, y0, rdramBytes))
            return false;
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t column = 0; column < columns; ++column)
                tmem.write16(destination + row * columns + column,
                             readSource(rdram, rdramBytes, x0 + column,
                                        y0 + row), 1);
        return true;
    }

    const aurum::tmem::Store& store() const { return tmem; }

private:
    static constexpr uint32_t kTmemWords = 1024;

    bool commonTileValid(const uint8_t* rdram, uint32_t tile, uint32_t sl,
                         uint32_t tl, uint32_t sh, uint32_t th) const {
        return rdram && tile < tmemWords.size() && tiSize == 2 && tiWidth &&
               !(sl & 3) && !(tl & 3) && !(sh & 3) && !(th & 3) && sh >= sl;
    }

    // Every read is validated before TMEM is touched, so a rejected upload
    // cannot leave a partially written destination behind.
    bool sourceReadsValid(uint32_t columns, uint32_t rows, uint32_t x0,
                          uint32_t y0, size_t rdramBytes) const {
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t column = 0; column < columns; ++column)
                if (!sourceFits(x0 + column, y0 + row, rdramBytes))
                    return false;
        return true;
    }

    // Guest address of an RGBA16 texel, or npos when it overflows 32 bits.
    size_t sourceAddress(uint32_t x, uint32_t y) const {
        const uint64_t address =
            uint64_t(tiAddress) + (uint64_t(y) * tiWidth + x) * 2;
        return address > UINT32_MAX ? npos : size_t(address);
    }

    bool sourceFits(uint32_t x, uint32_t y, size_t rdramBytes) const {
        const size_t address = sourceAddress(x, y);
        if (address == npos) return false;
        const size_t host =
            aurum::memory::hostHalfwordAddress(uint32_t(address));
        return host + sizeof(uint16_t) <= rdramBytes;
    }

    uint16_t readSource(const uint8_t* rdram, size_t rdramBytes, uint32_t x,
                        uint32_t y) const {
        uint16_t value = 0;
        // Validated by sourceReadsValid before any TMEM write.
        aurum::memory::readHalfword(rdram, rdramBytes,
                                    uint32_t(sourceAddress(x, y)), value);
        return value;
    }

    static constexpr size_t npos = SIZE_MAX;

    uint32_t tiFormat = 0, tiSize = 0, tiWidth = 0, tiAddress = 0;
    std::array<uint32_t, 8> tmemWords{};
    std::array<uint32_t, 8> tileLines{};
    aurum::tmem::Store tmem;
};
}
