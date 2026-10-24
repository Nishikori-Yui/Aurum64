// SPDX-License-Identifier: MIT
#include "MemoryBus.hpp"
#include "TextureUpload.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        std::vector<uint8_t> rdram(0x2000, 0);
        const std::array<uint16_t, 4> source{0x1234, 0x5678, 0x9abc, 0xdef0};
        for (unsigned i = 0; i < source.size(); ++i)
            require(aurum::memory::writeHalfword(rdram.data(), rdram.size(),
                                                  0x1000 + i * 2, source[i]),
                    "RDRAM source setup");
        aurum::texture::Uploader uploader;
        uploader.setTextureImage(0, 2, 4, 0x1000);
        uploader.setTile(2, 7);
        require(uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT upload");
        for (unsigned i = 0; i < source.size(); ++i)
            require(uploader.store().read16(7 + i, 1) == source[i],
                    "TLUT bytes differ");
        require(!uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 14, 0),
                "unaligned TLUT bounds accepted");
        uploader.setTile(2, 510);
        require(!uploader.loadTlut(rdram.data(), rdram.size(), 2, 0, 0, 12, 0),
                "TLUT destination overflow accepted");
        std::array<uint16_t, 4> tileSource{0x0102, 0x0304, 0x0506, 0x0708};
        for (unsigned i = 0; i < tileSource.size(); ++i)
            require(aurum::memory::writeHalfword(rdram.data(), rdram.size(),
                                                  0x1200 + (i + (i / 2) * 2) * 2,
                                                  tileSource[i]),
                    "tile source setup");
        uploader.setTextureImage(0, 2, 4, 0x1200);
        uploader.setTile(1, 2, 1);
        require(uploader.loadTile(rdram.data(), rdram.size(), 1, 0, 0, 4, 4),
                "LoadTile upload");
        require(uploader.store().read16(2) == 0x0102 &&
                uploader.store().read16(3) == 0x0304 &&
                uploader.store().read16(6) == 0x0506 &&
                uploader.store().read16(7) == 0x0708,
                "LoadTile stride bytes differ");
        for (unsigned i = 0; i < tileSource.size(); ++i)
            require(aurum::memory::writeHalfword(rdram.data(), rdram.size(),
                                                  0x1210 + i * 2, tileSource[i]),
                    "block source setup");
        uploader.setTextureImage(0, 2, 4, 0x1210);
        uploader.setTile(1, 12);
        require(uploader.loadBlock(rdram.data(), rdram.size(), 1, 0, 0, 12, 0) &&
                uploader.store().read16(12) == 0x0102 &&
                uploader.store().read16(13) == 0x0304 &&
                uploader.store().read16(14) == 0x0506 &&
                uploader.store().read16(15) == 0x0708,
                "LoadBlock contiguous bytes differ");
        require(!uploader.loadBlock(rdram.data(), rdram.size(), 1, 0, 0, 12, 0x80),
                "Unsupported DXT mode accepted");
        uploader.setTextureImage(0, 2, 4, 0x1000);
        uploader.setTile(0, 20);
        uploader.setTile(1, 20);
        require(uploader.loadTlut(rdram.data(), rdram.size(), 0, 0, 0, 0, 0),
                "Aliased tile upload");
        require(uploader.store().read16(20, 1) == source[0],
                "Aliased tile destination mismatch");
        require(aurum::memory::writeHalfword(rdram.data(), rdram.size(), 0x1000, 0xbeef),
                "Repeated upload source setup");
        require(uploader.loadTlut(rdram.data(), rdram.size(), 1, 0, 0, 0, 0) &&
                uploader.store().read16(20, 1) == 0xbeef,
                "Repeated upload did not overwrite alias");
        uploader.setTile(3, 511, 128);
        require(uploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 0, 4),
                "TMEM upload ending at word 1024");
        uploader.setTile(3, 511, 129);
        require(!uploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 0, 4),
                "TMEM destination overflow accepted");
        uploader.setTile(3, 0, 1);
        uploader.setTextureImage(0, 2, 4, 0x1ffe);
        require(!uploader.loadTile(rdram.data(), rdram.size(), 3, 0, 0, 4, 0),
                "RDRAM source overflow accepted");
        std::puts("PASS: minimal SetTextureImage/SetTile/LoadTLUT byte upload");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
