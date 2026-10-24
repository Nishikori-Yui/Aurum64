// SPDX-License-Identifier: MIT
#pragma once
#include "MemoryBus.hpp"
#include "Tmem.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace aurum::texture {
// RGBA16 texture uploads into TMEM.
//
// The hardware word indices below were derived from Angrylion's
// get_tmem_idx() and verified byte-for-byte against TMEM exported by
// aurum-angrylion-reference: the SetTile "tmem" and "line" fields count
// 8-byte units, so a halfword index is (tmem + line * row) << 2; odd rows
// additionally swap the dword pair; and every index carries the host
// word swap.
//
// Deliberate divergence from the reference: an out-of-range source address is
// rejected with false instead of being masked into RDRAM and read as zero the
// way rdram_read_idx32() does. The plan requires invalid combinations to fail
// explicitly rather than return plausible data, so uploads that would read
// outside RDRAM are reported as unsupported.
class Uploader {
public:
    static constexpr uint32_t kTmemUnits = 512;      // 9-bit tmem field
    static constexpr uint32_t kTmemWords16 = 2048;   // 4 KiB of halfwords

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
        uint32_t columns = 0, rows = 0;
        if (!decodeRect(rdram, tile, sl, tl, sh, th, columns, rows))
            return false;
        const uint32_t line = tileLines[tile];
        const uint32_t destination = tmemWords[tile];
        // 64-bit: a wide extent must not wrap past the TMEM window.
        const uint64_t lastRow = uint64_t(destination) + line * (rows - 1);
        if (!line || columns > line * 4 || lastRow >= kTmemUnits ||
            (lastRow << 2) + columns > kTmemWords16)
            return false;
        if (!sourceReadsValid(columns, rows, sl >> 2, tl >> 2, rdramBytes))
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        for (uint32_t row = 0; row < rows; ++row)
            for (uint32_t column = 0; column < columns; ++column)
                tmem.writeHwWord(tileWordIndex(destination, line, row, column),
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
        // Each edge-walker step spans four RGBA16 halfwords, so the texel
        // count is four times the number of steps. LoadBlock writes one
        // contiguous run with no odd-row swap.
        const uint32_t count = ((sh - sl) / 4 + 1) * 4;
        const uint32_t destination = tmemWords[tile];
        if (uint64_t(destination << 2) + count > kTmemWords16)
            return false;
        if (!sourceReadsValid(count, 1, x0, y0, rdramBytes))
            return false;
        for (uint32_t i = 0; i < count; ++i)
            tmem.writeHwWord(((destination << 2) + i) ^ 1u,
                             readSource(rdram, rdramBytes, x0 + i, y0));
        return true;
    }

    bool loadTlut(const uint8_t* rdram, size_t rdramBytes, uint32_t tile,
                  uint32_t sl, uint32_t tl, uint32_t sh, uint32_t th) {
        uint32_t columns = 0, rows = 0;
        if (!decodeRect(rdram, tile, sl, tl, sh, th, columns, rows))
            return false;
        // A TLUT is a single row of palette entries; anything else is not a
        // defined upload rather than something to approximate.
        if (rows != 1) return false;
        const uint32_t destination = tmemWords[tile];
        // Each entry occupies four hardware words (eight bytes).
        if (uint64_t(destination << 2) + uint64_t(columns) * 16 > kTmemWords16)
            return false;
        if (!sourceReadsValid(columns, 1, sl >> 2, tl >> 2, rdramBytes))
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        for (uint32_t entry = 0; entry < columns; ++entry) {
            const uint16_t value = readSource(rdram, rdramBytes, x0 + entry, y0);
            for (uint32_t copy = 0; copy < 4; ++copy)
                tmem.writeHwWord((destination << 2) + entry * 16 + copy, value);
        }
        return true;
    }

    const aurum::tmem::Store& store() const { return tmem; }
    aurum::tmem::Store& store() { return tmem; }

    static uint32_t tileWordIndex(uint32_t tmem, uint32_t line, uint32_t row,
                                  uint32_t column) {
        const uint32_t base = ((tmem + line * row) & 0x1ff) << 2;
        return (base + column) ^ ((row & 1) ? 3u : 1u);
    }

private:
    bool decodeRect(const uint8_t* rdram, uint32_t tile, uint32_t sl,
                    uint32_t tl, uint32_t sh, uint32_t th, uint32_t& columns,
                    uint32_t& rows) const {
        if (!rdram || tile >= tmemWords.size() || tiSize != 2 || !tiWidth ||
            (sl & 3) || (tl & 3) || (sh & 3) || (th & 3) || sh < sl || th < tl)
            return false;
        columns = (sh - sl) / 4 + 1;
        rows = (th - tl) / 4 + 1;
        return true;
    }

    // Every read is validated before TMEM is touched, so a rejected upload
    // cannot leave the destination partially written.
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
