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
        if (!rdram || tile >= tmemWords.size() || tiSize != 2 || !tiWidth ||
            (sl & 3) || (tl & 3) || (sh & 3) || (th & 3) || sh < sl || th < tl)
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        const uint32_t columns = (sh - sl) / 4 + 1;
        const uint32_t rows = (th - tl) / 4 + 1;
        const uint32_t lineWords = tileLines[tile] * 4;
        const uint32_t destination = tmemWords[tile];
        if (!lineWords || columns > lineWords || destination + (rows - 1) * lineWords +
                columns > 1024) return false;
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint32_t address = tiAddress +
                    ((y0 + row) * tiWidth + x0 + column) * 2;
                uint16_t value = 0;
                if (!aurum::memory::readHalfword(rdram, rdramBytes, address, value))
                    return false;
                tmem.write16(destination + row * lineWords + column, value);
            }
        }
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
        if (destination + count > 1024) return false;
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t address = tiAddress + ((y0 * tiWidth + x0 + i) * 2);
            uint16_t value = 0;
            if (!aurum::memory::readHalfword(rdram, rdramBytes, address, value))
                return false;
            tmem.write16(destination + i, value);
        }
        return true;
    }

    bool loadTlut(const uint8_t* rdram, size_t rdramBytes, uint32_t tile,
                  uint32_t sl, uint32_t tl, uint32_t sh, uint32_t th) {
        if (!rdram || tile >= tmemWords.size() || tiSize != 2 || !tiWidth ||
            (sl & 3) || (tl & 3) || (sh & 3) || (th & 3) || sh < sl || th < tl)
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        const uint32_t columns = (sh - sl) / 4 + 1;
        const uint32_t rows = (th - tl) / 4 + 1;
        const uint32_t count = columns * rows;
        const uint32_t destination = tmemWords[tile];
        if (count > 512 || destination > 512 - count) return false;
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                const uint32_t address = tiAddress +
                    ((y0 + row) * tiWidth + x0 + column) * 2;
                uint16_t value = 0;
                if (!aurum::memory::readHalfword(rdram, rdramBytes, address, value))
                    return false;
                tmem.write16(destination + row * columns + column, value, 1);
            }
        }
        return true;
    }

    const aurum::tmem::Store& store() const { return tmem; }

private:
    uint32_t tiFormat = 0, tiSize = 0, tiWidth = 0, tiAddress = 0;
    std::array<uint32_t, 8> tmemWords{};
    std::array<uint32_t, 8> tileLines{};
    aurum::tmem::Store tmem;
};
}
