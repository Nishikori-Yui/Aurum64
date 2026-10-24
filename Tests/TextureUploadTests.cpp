// SPDX-License-Identifier: MIT
#include "MemoryBus.hpp"
#include "TextureUpload.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

namespace {
using aurum::texture::Uploader;

uint16_t tmemWord(const Uploader& uploader, uint32_t tmem, uint32_t line,
                  uint32_t row, uint32_t column) {
    return uploader.store().readHwWord(
        Uploader::tileWordIndex(tmem, line, row, column));
}
}

int main() {
    try {
        std::vector<uint8_t> rdram(0x2000, 0);
        const std::array<uint16_t, 4> source{0x1234, 0x5678, 0x9abc, 0xdef0};
        for (unsigned i = 0; i < source.size(); ++i)
            require(aurum::memory::writeHalfword(rdram.data(), rdram.size(),
                                                  0x1000 + i * 2, source[i]),
                    "RDRAM source setup");

        // LoadTLUT places each entry in bank 0, repeated across the four
        // hardware words of an 8-byte unit, matching the reference engine.
        Uploader uploader;
        uploader.setTextureImage(0, 2, 4, 0x1000);
        uploader.setTile(2, 7);
        require(uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT upload");
        for (unsigned i = 0; i < source.size(); ++i)
            for (unsigned copy = 0; copy < 4; ++copy)
                require(uploader.store().readHwWord((7u << 2) + i * 16 + copy) ==
                            source[i],
                        "TLUT entry or copy differs");
        require(!uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 14, 0),
                "unaligned TLUT bounds accepted");
        require(!uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 4),
                "multi-row TLUT accepted");
        // Four entries span 64 hardware words, so a 9-bit tmem base of 511
        // (word 2044) is the smallest start that runs past the last word.
        uploader.setTile(2, 511);
        require(!uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT destination overflow accepted");
        // 496 << 2 is word 1984; four entries of 16 words end exactly at 2048.
        uploader.setTile(2, 496);
        require(uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT ending at the final TMEM word rejected");
        uploader.setTile(2, 497);
        require(!uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT one word past the end accepted");

        // LoadTile: tmem and line count 8-byte units, odd rows swap the dword
        // pair, and every index carries the host word swap.
        // Stride is 4 texels, so row 1 starts eight bytes after row 0.
        const std::array<std::pair<uint32_t, uint16_t>, 4> tileSource{{
            {0x1200, 0x0102}, {0x1202, 0x0304}, {0x1208, 0x0506}, {0x120a, 0x0708}}};
        for (const auto& [address, value] : tileSource)
            require(aurum::memory::writeHalfword(rdram.data(), rdram.size(),
                                                  address, value),
                    "tile source setup");
        Uploader tileUploader;
        tileUploader.setTextureImage(0, 2, 4, 0x1200);
        tileUploader.setTile(1, 1, 2);
        require(tileUploader.loadTile(rdram.data(), rdram.size(), 1, 0, 0, 4, 4),
                "LoadTile upload");
        // tmem=1, line=2: row 0 has base word 4, row 1 base word 12. The host
        // word swap puts column 1 first in row 0, and the odd-row dword swap
        // additionally exchanges the pair in row 1. Spelled out as literal
        // indices so the placement is pinned independently of the helper.
        require(tileUploader.store().readHwWord(5) == 0x0102 &&
                tileUploader.store().readHwWord(4) == 0x0304 &&
                tileUploader.store().readHwWord(15) == 0x0506 &&
                tileUploader.store().readHwWord(14) == 0x0708,
                "LoadTile hardware word placement differs");
        require(tmemWord(tileUploader, 1, 2, 0, 0) == 0x0102 &&
                tmemWord(tileUploader, 1, 2, 0, 1) == 0x0304 &&
                tmemWord(tileUploader, 1, 2, 1, 0) == 0x0506 &&
                tmemWord(tileUploader, 1, 2, 1, 1) == 0x0708,
                "LoadTile tileWordIndex disagrees with the reference layout");

        // LoadBlock: one contiguous 64-bit-per-step run, four halfwords each.
        std::array<uint16_t, 8> blockSource{0x1111, 0x2222, 0x3333, 0x4444,
                                            0x5555, 0x6666, 0x7777, 0x8888};
        for (unsigned i = 0; i < blockSource.size(); ++i)
            require(aurum::memory::writeHalfword(rdram.data(), rdram.size(),
                                                  0x1210 + i * 2,
                                                  blockSource[i]),
                    "block source setup");
        Uploader blockUploader;
        blockUploader.setTextureImage(0, 2, 8, 0x1210);
        blockUploader.setTile(1, 3);
        require(blockUploader.loadBlock(rdram.data(), rdram.size(), 1, 0, 0, 4, 0),
                "LoadBlock upload");
        for (unsigned i = 0; i < blockSource.size(); ++i)
            require(blockUploader.store().readHwWord(((3u << 2) + i) ^ 1u) ==
                        blockSource[i],
                    "LoadBlock contiguous words differ");
        require(!blockUploader.loadBlock(rdram.data(), rdram.size(), 1, 0, 0, 4, 0x80),
                "Unsupported DXT mode accepted");

        // Shared and aliased destinations: two tiles writing the same TMEM
        // unit must observe each other's data, and a repeat must overwrite it.
        Uploader aliasUploader;
        aliasUploader.setTextureImage(0, 2, 4, 0x1000);
        aliasUploader.setTile(0, 20);
        aliasUploader.setTile(1, 20);
        require(aliasUploader.loadTlut(rdram.data(), rdram.size(), 0, 0, 0, 0, 0),
                "Aliased tile upload");
        require(aliasUploader.store().readHwWord(20u << 2) == source[0],
                "Aliased tile destination mismatch");
        require(aurum::memory::writeHalfword(rdram.data(), rdram.size(), 0x1000,
                                             0xbeef),
                "Repeated upload source setup");
        require(aliasUploader.loadTlut(rdram.data(), rdram.size(), 1, 0, 0, 0, 0) &&
                aliasUploader.store().readHwWord(20u << 2) == 0xbeef,
                "Repeated upload did not overwrite alias");

        // Destination overflow comes from the line stride, since the tmem
        // field itself is only 9 bits wide.
        Uploader boundsUploader;
        boundsUploader.setTextureImage(0, 2, 4, 0x1000);
        // With two rows, tmem=255 and line=256 the last row base is unit 511,
        // whose words end exactly at the last of the 2048 hardware halfwords.
        boundsUploader.setTile(3, 255, 256);
        require(boundsUploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 0, 4),
                "TMEM upload ending at the last hardware word");
        boundsUploader.setTile(3, 255, 257);
        require(!boundsUploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 0, 4),
                "TMEM destination overflow accepted");
        // A single row whose columns exceed the line stride is rejected too.
        boundsUploader.setTile(3, 0, 1);
        require(!boundsUploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 32, 0),
                "LoadTile wider than the line stride accepted");

        // A source that runs past the end of RDRAM is rejected outright and
        // leaves TMEM untouched.
        Uploader sourceUploader;
        sourceUploader.setTextureImage(0, 2, 4, 0x1ffe);
        sourceUploader.setTile(3, 0, 1);
        require(!sourceUploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 4, 0),
                "RDRAM source overflow accepted");
        for (uint32_t word = 0; word < 16; ++word)
            require(sourceUploader.store().read16(word) == 0 &&
                    sourceUploader.store().read16(word, 1) == 0,
                    "Rejected upload modified TMEM");

        std::puts("PASS: RGBA16 TMEM uploads match the reference hardware layout");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
