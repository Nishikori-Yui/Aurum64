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

        // Size-2 TLUT load descriptors leave 16-word gaps between entries.
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
        // Uploads can target either half. This size-2 descriptor places the
        // final entry at 4*499 + 3*16 = 2044, exactly the last four words.
        uploader.setTile(2, 499);
        require(uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT ending at the last TMEM word rejected");
        uploader.setTile(2, 500);
        require(!uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT exceeding TMEM accepted");
        uploader.setTile(2, 511);
        require(!uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT destination overflow accepted");
        Uploader palette;
        for (uint32_t entry = 0; entry < 256; ++entry)
            require(aurum::memory::writeHalfword(rdram.data(), rdram.size(),
                                                0x1400 + entry * 2, 0x8100 + entry),
                    "Palette source setup");
        aurum::rdp::TextureLoad load;
        load.opcode = 0x30; load.tile = 7; load.tileSize = 0; load.tileTmem = 256;
        load.tiSize = 2; load.tiWidth = 256; load.tiAddress = 0x1400; load.sh = 255 * 4;
        require(palette.load(rdram.data(), rdram.size(), load),
                "Conventional size-0 descriptor / size-2 source palette rejected");
        for (uint32_t entry = 0; entry < 256; ++entry)
            for (uint32_t bank = 0; bank < 4; ++bank)
                require(palette.store().readHwWord(1024 + entry * 4 + bank) == 0x8100 + entry,
                        "Full upper-half palette replication differs");
        std::array<uint8_t, 4096> beforePalette{}, afterPalette{};
        palette.store().copyBytes(beforePalette);
        load.tileTmem = 257;
        require(!palette.load(rdram.data(), rdram.size(), load), "Wrapped palette accepted");
        palette.store().copyBytes(afterPalette);
        require(beforePalette == afterPalette, "Rejected palette upload was not atomic");

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
        // The hardware word index is 11 bits wide, so a single row starting at
        // the last tmem unit ends exactly at word 2048. A second row pushes
        // the row base past the 9-bit unit field and is rejected.
        boundsUploader.setTile(3, 511, 1);
        require(boundsUploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 12, 0),
                "TMEM upload ending at the last hardware word");
        require(!boundsUploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 12, 4),
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

        // RGBA32 splits each texel across the banks: the high halfword goes to
        // bank 0 and the low halfword to bank 1 at the same word index.
        std::vector<uint8_t> source32(0x2000, 0);
        const std::array<uint32_t, 4> words32{0x10008000, 0x10018001,
                                              0x10028002, 0x10038003};
        for (unsigned i = 0; i < words32.size(); ++i)
            require(aurum::memory::writeWord(source32.data(), source32.size(),
                                              0x1000 + i * 4, words32[i]),
                    "RGBA32 source setup");
        Uploader splitUploader;
        splitUploader.setTextureImage(0, 3, 4, 0x1000);
        splitUploader.setTile(0, 0, 1);
        require(splitUploader.loadTile(source32.data(), source32.size(), 0, 0, 0,
                                       12, 0),
                "RGBA32 LoadTile upload");
        for (unsigned i = 0; i < words32.size(); ++i) {
            const uint32_t index = Uploader::tileWordIndex(0, 1, 0, i);
            require(splitUploader.store().readHwWord(index) ==
                        uint16_t(words32[i] >> 16),
                    "RGBA32 high halfword placement differs");
            require(splitUploader.store().readHwWord(index + 1024) ==
                        uint16_t(words32[i] & 0xffff),
                    "RGBA32 low halfword placement differs");
        }

        // 8-bit texels use the same shape at byte granularity: one byte per
        // texel, even rows reverse the bytes of each aligned word and odd rows
        // additionally swap the two halves of the 8-byte group.
        // The uploader reads guest byte addresses, which live at host address
        // A ^ 3, so the source has to be laid out in guest order.
        std::vector<uint8_t> source8(0x2000, 0);
        for (unsigned i = 0; i < 16; ++i)
            source8[(0x1000 + i) ^ 3] = uint8_t(0x10 + i);
        Uploader eightBit;
        eightBit.setTextureImage(2, 1, 8, 0x1000);
        eightBit.setTile(0, 0, 2);
        require(eightBit.loadTile(source8.data(), source8.size(), 0, 0, 0, 28, 4),
                "8-bit LoadTile upload");
        // Row 0 is even, row 1 is odd. Asserted as literal byte indices so the
        // placement is pinned independently of the helper.
        require(eightBit.store().readByte(3) == 0x10 &&
                eightBit.store().readByte(2) == 0x11 &&
                eightBit.store().readByte(7) == 0x14 &&
                eightBit.store().readByte(6) == 0x15,
                "8-bit row-0 byte placement differs");
        require(eightBit.store().readByte(23) == 0x18 &&
                eightBit.store().readByte(22) == 0x19 &&
                eightBit.store().readByte(19) == 0x1c &&
                eightBit.store().readByte(18) == 0x1d,
                "8-bit row-1 byte placement differs");
        require(Uploader::tileByteIndex(0, 2, 0, 0) == 3 &&
                Uploader::tileByteIndex(0, 2, 0, 1) == 2 &&
                Uploader::tileByteIndex(0, 2, 1, 0) == 23 &&
                Uploader::tileByteIndex(0, 2, 1, 1) == 22,
                "tileByteIndex disagrees with the reference layout");
        // An 8-bit tile that is wider than the line stride is rejected. With
        // line 1 a row holds eight bytes, and sh 32 asks for nine columns.
        eightBit.setTile(0, 0, 1);
        require(!eightBit.loadTile(source8.data(), source8.size(), 0, 0, 0, 32, 0),
                "8-bit tile wider than the line stride accepted");

        // Only the verified halves of the address space are accepted. RGBA16
        // LoadTile may reach bank 1; RGBA32 LoadTile, LoadBlock and any
        // LoadBlock into bank 1 are reported unsupported instead.
        Uploader bankUploader;
        bankUploader.setTextureImage(0, 2, 4, 0x1000);
        bankUploader.setTile(0, 256, 2);
        require(bankUploader.loadTile(source32.data(), source32.size(), 0, 0, 0,
                                      12, 12),
                "RGBA16 LoadTile into bank 1 rejected");
        Uploader rejectUploader;
        rejectUploader.setTextureImage(0, 3, 4, 0x1000);
        rejectUploader.setTile(0, 256, 2);
        require(!rejectUploader.loadTile(source32.data(), source32.size(), 0, 0,
                                         0, 12, 12),
                "RGBA32 LoadTile into bank 1 accepted");
        rejectUploader.setTile(0, 0, 2);
        require(!rejectUploader.loadBlock(source32.data(), source32.size(), 0, 0,
                                          0, 4, 0),
                "RGBA32 LoadBlock accepted");
        Uploader blockBankUploader;
        blockBankUploader.setTextureImage(0, 2, 4, 0x1000);
        blockBankUploader.setTile(0, 256);
        require(!blockBankUploader.loadBlock(source32.data(), source32.size(), 0,
                                             0, 0, 4, 0),
                "LoadBlock into bank 1 accepted");

        std::puts("PASS: RGBA16 and RGBA32 TMEM uploads match the reference layout");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
