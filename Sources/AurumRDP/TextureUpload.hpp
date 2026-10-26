// SPDX-License-Identifier: MIT
#pragma once
#include "MemoryBus.hpp"
#include "Tmem.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace aurum::texture {
// RGBA16 and RGBA32 texture uploads into TMEM.
//
// The hardware word indices below were derived from Angrylion's
// get_tmem_idx() and verified byte-for-byte against TMEM exported by
// aurum-angrylion-reference: the SetTile "tmem" and "line" fields count
// 8-byte units, so a halfword index is (tmem + line * row) << 2; odd rows
// additionally swap the dword pair; and every index carries the host
// word swap. RGBA32 splits each texel across the banks, keeping the high
// halfword in bank 0 and the low halfword in bank 1 at the same word index.
//
// 8-bit texels use the same shape at byte granularity: a byte index is
// (tmem + line * row) << 3, odd rows swap the whole 8-byte group and even
// rows reverse the bytes within each aligned word. That layout is shared by
// CI8, IA8 and I8, which were each verified against the reference, because
// the upload path depends on the texture image size rather than its format.
//
// Deliberate divergence from the reference: an out-of-range or misaligned
// source address is rejected with false instead of being masked into RDRAM
// and read as zero the way rdram_read_idx32() does. The plan requires invalid
// combinations to fail explicitly rather than return plausible data, so such
// uploads are reported as unsupported.
class Uploader {
public:
    static constexpr uint32_t kTmemUnits = 512;      // 9-bit tmem field
    static constexpr uint32_t kTmemWords16 = 2048;   // 4 KiB of halfwords
    static constexpr uint32_t kBankWords16 = 1024;
    static constexpr uint32_t kTmemBytes = 4096;

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
        // The hardware word index is 11 bits wide and spans both banks, so
        // RGBA16 may reach the last of 2048 words. RGBA32 also writes the
        // same index plus one bank, which has to stay inside that window.
        // 8-bit texels use byte indices over the same 4 KiB, so the row base
        // is measured in 8-byte units instead of words.
        // 64-bit: a wide extent must not wrap past the TMEM window.
        const uint64_t lastRow = uint64_t(destination) + line * (rows - 1);
        if (!line || lastRow >= kTmemUnits) return false;
        if (tiSize == 1) {
            if (columns > line * 8 || (lastRow << 3) + columns > kTmemBytes)
                return false;
        } else {
            const uint64_t lastWord = (lastRow << 2) + columns;
            const uint64_t limit = tiSize == 3 ? kBankWords16 : kTmemWords16;
            if (columns > line * 4 || lastWord > limit) return false;
        }
        if (!sourceReadsValid(columns, rows, sl >> 2, tl >> 2, rdramBytes))
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        for (uint32_t row = 0; row < rows; ++row) {
            for (uint32_t column = 0; column < columns; ++column) {
                if (tiSize == 1) {
                    tmem.writeByte(tileByteIndex(destination, line, row, column),
                                   readSourceByte(rdram, rdramBytes,
                                                  x0 + column, y0 + row));
                    continue;
                }
                const uint32_t index =
                    tileWordIndex(destination, line, row, column);
                if (tiSize == 3) {
                    const uint32_t value =
                        readSourceWord(rdram, rdramBytes, x0 + column, y0 + row);
                    tmem.writeHwWord(index, uint16_t(value >> 16));
                    tmem.writeHwWord(index + kBankWords16,
                                     uint16_t(value & 0xffff));
                } else {
                    tmem.writeHwWord(index, readSourceHalfword(
                                                rdram, rdramBytes, x0 + column,
                                                y0 + row));
                }
            }
        }
        return true;
    }

    bool loadBlock(const uint8_t* rdram, size_t rdramBytes, uint32_t tile,
                   uint32_t sl, uint32_t tl, uint32_t sh, uint32_t dxt) {
        // RGBA32 LoadBlock is deliberately not modeled: the reference writes
        // steps * 4 - 2 texels rather than a whole number of texels per step,
        // and that layout is not yet verified, so it is reported unsupported.
        if (!rdram || tile >= tmemWords.size() || tiSize != 2 || !tiWidth ||
            dxt != 0 || (sl & 3) || (tl & 3) || (sh & 3) || sh < sl)
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        // Each edge-walker step spans 64 bits, so four RGBA16 halfwords are
        // written per step with no odd-row swap.
        const uint32_t count = ((sh - sl) / 4 + 1) * 4;
        const uint32_t destination = tmemWords[tile];
        // Only bank 0 is verified for LoadBlock, so the run must stay there.
        if (uint64_t(destination << 2) + count > kBankWords16)
            return false;
        if (!sourceReadsValid(count, 1, x0, y0, rdramBytes))
            return false;
        for (uint32_t i = 0; i < count; ++i)
            tmem.writeHwWord(((destination << 2) + i) ^ 1u,
                             readSourceHalfword(rdram, rdramBytes, x0 + i, y0));
        return true;
    }

    bool loadTlut(const uint8_t* rdram, size_t rdramBytes, uint32_t tile,
                  uint32_t sl, uint32_t tl, uint32_t sh, uint32_t th) {
        uint32_t columns = 0, rows = 0;
        if (!decodeRect(rdram, tile, sl, tl, sh, th, columns, rows))
            return false;
        // A TLUT is a single row of 16-bit palette entries regardless of the
        // texture image size; anything else is not a defined upload rather
        // than something to approximate.
        if (rows != 1 || tiSize != 2) return false;
        const uint32_t destination = tmemWords[tile];
        // Each entry occupies four hardware words (eight bytes).
        if (uint64_t(destination << 2) + uint64_t(columns) * 16 > kBankWords16)
            return false;
        if (!sourceReadsValid(columns, 1, sl >> 2, tl >> 2, rdramBytes))
            return false;
        const uint32_t x0 = sl >> 2, y0 = tl >> 2;
        for (uint32_t entry = 0; entry < columns; ++entry) {
            const uint16_t value =
                readSourceHalfword(rdram, rdramBytes, x0 + entry, y0);
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

    // The same placement at byte granularity, for 8-bit texels.
    static uint32_t tileByteIndex(uint32_t tmem, uint32_t line, uint32_t row,
                                  uint32_t column) {
        const uint32_t base = ((tmem + line * row) & 0x1ff) << 3;
        return (base + column) ^ ((row & 1) ? 7u : 3u);
    }

private:
    bool supportedSize() const { return tiSize == 1 || tiSize == 2 || tiSize == 3; }

    uint32_t texelBytes() const { return tiSize == 3 ? 4 : (tiSize == 1 ? 1 : 2); }

    bool decodeRect(const uint8_t* rdram, uint32_t tile, uint32_t sl,
                    uint32_t tl, uint32_t sh, uint32_t th, uint32_t& columns,
                    uint32_t& rows) const {
        if (!rdram || tile >= tmemWords.size() || !supportedSize() ||
            !tiWidth || (sl & 3) || (tl & 3) || (sh & 3) || (th & 3) ||
            sh < sl || th < tl)
            return false;
        // RGBA32 texels are whole words, so the image must start aligned.
        if (tiSize == 3 && (tiAddress & 3)) return false;
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

    // Guest address of one texel, or npos when it overflows 32 bits.
    size_t sourceAddress(uint32_t x, uint32_t y) const {
        const uint64_t address = uint64_t(tiAddress) +
            (uint64_t(y) * tiWidth + x) * texelBytes();
        return address > UINT32_MAX ? npos : size_t(address);
    }

    bool sourceFits(uint32_t x, uint32_t y, size_t rdramBytes) const {
        const size_t address = sourceAddress(x, y);
        if (address == npos) return false;
        if (tiSize == 3) {
            const size_t host = aurum::memory::hostWordAddress(uint32_t(address));
            return host + sizeof(uint32_t) <= rdramBytes;
        }
        if (tiSize == 1) {
            const size_t host = aurum::memory::hostByteAddress(uint32_t(address));
            return host < rdramBytes;
        }
        const size_t host =
            aurum::memory::hostHalfwordAddress(uint32_t(address));
        return host + sizeof(uint16_t) <= rdramBytes;
    }

    uint16_t readSourceHalfword(const uint8_t* rdram, size_t rdramBytes,
                                uint32_t x, uint32_t y) const {
        uint16_t value = 0;
        // Validated by sourceReadsValid before any TMEM write.
        aurum::memory::readHalfword(rdram, rdramBytes,
                                    uint32_t(sourceAddress(x, y)), value);
        return value;
    }

    uint8_t readSourceByte(const uint8_t* rdram, size_t rdramBytes,
                           uint32_t x, uint32_t y) const {
        return rdram[aurum::memory::hostByteAddress(
            uint32_t(sourceAddress(x, y)))];
    }

    uint32_t readSourceWord(const uint8_t* rdram, size_t rdramBytes,
                            uint32_t x, uint32_t y) const {
        uint32_t value = 0;
        aurum::memory::readWord(rdram, rdramBytes,
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
