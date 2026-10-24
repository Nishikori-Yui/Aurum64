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
        std::puts("PASS: minimal SetTextureImage/SetTile/LoadTLUT byte upload");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
