// SPDX-License-Identifier: MIT
#include "MemoryBus.hpp"
#include "TextureFetch.hpp"
#include "TextureUpload.hpp"
#include "Tmem.hpp"
#include <array>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

namespace {
using aurum::texture::Sampler;
using aurum::texture::Texel;
using aurum::texture::TileDescriptor;

// Loads a raw 4 KiB hardware TMEM image so a sampler can be checked against
// bytes the reference engine produced, rather than against our own uploader.
aurum::tmem::Store loadReferenceTmem(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open reference TMEM layer.");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
    require(bytes.size() == aurum::tmem::Store::kBytes,
            "Reference TMEM layer is not 4 KiB.");
    aurum::tmem::Store store;
    for (size_t i = 0; i < bytes.size(); ++i) store.writeByte(uint32_t(i), bytes[i]);
    return store;
}
}

int main(int argc, char** argv) {
    try {
        // An optional directory of reference layers produced by
        // aurum-angrylion-reference for the committed fixtures. scripts/test.py
        // passes it so the fetch is checked against hardware bytes; without it
        // the self-contained round-trip and rejection cases still run.
        const std::string dir = argc >= 2 ? argv[1] : std::string{};
        Sampler sampler;

        if (!dir.empty()) {
            // The sampler_rgba16 fixture uploads an 8x8 RGBA16 tile from a
            // 16-wide source whose texel at (y, x) is 0x1000 + y * 0x100 + x,
            // into tmem unit 0 with line 2.
            const auto store = loadReferenceTmem(
                dir + "/sampler_rgba16.ref/sampler_rgba16.tmem");
            TileDescriptor tile{0, 2, 2, 0};
            for (uint32_t t = 0; t < 8; ++t) {
                for (uint32_t s = 0; s < 8; ++s) {
                    Texel texel;
                    require(sampler.fetch(store, tile, s, t, texel),
                            "RGBA16 fetch of reference TMEM failed");
                    const uint32_t value = 0x1000 + t * 0x100 + s;
                    const auto expand = [](uint32_t c) { return (c << 3) | (c >> 2); };
                    require(texel.r == expand((value >> 11) & 31) &&
                            texel.g == expand((value >> 6) & 31) &&
                            texel.b == expand((value >> 1) & 31) &&
                            texel.a == ((value & 1) ? 255u : 0u),
                            "RGBA16 channel decode differs");
                }
            }

            // The sampler_rgba32 fixture uploads an 8x8 RGBA32 tile whose
            // texel at (y, x) is 0x1000 + y*16 + x in the high halfword and
            // 0x8000 + y*16 + x in the low one, into tmem unit 0 with line 2.
            const auto store32 = loadReferenceTmem(
                dir + "/sampler_rgba32.ref/sampler_rgba32.tmem");
            TileDescriptor tile32{0, 3, 2, 0};
            for (uint32_t t = 0; t < 8; ++t) {
                for (uint32_t s = 0; s < 8; ++s) {
                    Texel texel;
                    require(sampler.fetch(store32, tile32, s, t, texel),
                            "RGBA32 fetch of reference TMEM failed");
                    const uint32_t value = ((0x1000 + t * 16 + s) << 16) |
                                           (0x8000 + t * 16 + s);
                    require(texel.r == ((value >> 24) & 0xff) &&
                            texel.g == ((value >> 16) & 0xff) &&
                            texel.b == ((value >> 8) & 0xff) &&
                            texel.a == (value & 0xff),
                            "RGBA32 channel decode differs");
                }
            }

            const auto check8Bit = [&](const char* fixture, uint32_t format) {
                const auto store = loadReferenceTmem(
                    dir + "/texture_" + fixture + ".ref/texture_" + fixture + ".tmem");
                TileDescriptor tile{format, 1, 2, 0};
                for (uint32_t t = 0; t < 4; ++t) {
                    for (uint32_t s = 0; s < 16; ++s) {
                        Texel texel;
                        require(sampler.fetch(store, tile, s, t, texel),
                                "8-bit reference TMEM fetch failed");
                        const uint32_t packed = 0x10 + t * 0x10 + s;
                        const uint32_t intensity = format == 3
                            ? ((packed >> 4) & 15u) * 17u : packed;
                        const uint32_t alpha = format == 3
                            ? (packed & 15u) * 17u : 255u;
                        require(texel.r == intensity && texel.g == intensity &&
                                texel.b == intensity && texel.a == alpha,
                                "8-bit reference TMEM channel decode differs");
                    }
                }
            };
            check8Bit("ia8", 3);
            check8Bit("i8", 4);

            const auto storeIA16 = loadReferenceTmem(
                dir + "/texture_ia16.ref/texture_ia16.tmem");
            TileDescriptor ia16{3, 2, 2, 0};
            for (uint32_t t = 0; t < 4; ++t) {
                for (uint32_t s = 0; s < 8; ++s) {
                    Texel texel;
                    require(sampler.fetch(storeIA16, ia16, s, t, texel),
                            "IA16 reference TMEM fetch failed");
                    require(texel.r == 0x20 + t * 8 + s &&
                            texel.g == 0x20 + t * 8 + s &&
                            texel.b == 0x20 + t * 8 + s &&
                            texel.a == 0x80 + t * 8 + s,
                            "IA16 reference TMEM channel decode differs");
                }
            }
        }

        // A fetch has to agree with the uploader that produced the TMEM, so the
        // two paths cannot drift apart silently.
        {
            std::vector<uint8_t> rdram(0x2000, 0);
            const uint16_t value = 0xffff;   // opaque white in RGBA5551
            for (unsigned i = 0; i < 4; ++i)
                require(aurum::memory::writeHalfword(rdram.data(), rdram.size(),
                                                      0x1000 + i * 2, value),
                        "RGBA16 source setup");
            aurum::texture::Uploader uploader;
            uploader.setTextureImage(0, 2, 4, 0x1000);
            uploader.setTile(0, 0, 1);
            require(uploader.loadTile(rdram.data(), rdram.size(), 0, 0, 0, 12, 0),
                    "RGBA16 round-trip upload");
            Texel texel;
            require(sampler.fetch(uploader.store(), TileDescriptor{0, 2, 1, 0}, 2,
                                  0, texel),
                    "RGBA16 round-trip fetch");
            require(texel.r == 255 && texel.g == 255 && texel.b == 255 &&
                    texel.a == 255,
                    "RGBA16 round-trip color differs");
        }

        // Unmodeled combinations must fail rather than approximate.
        {
            aurum::tmem::Store store;
            Texel texel;
            require(!sampler.fetch(store, TileDescriptor{2, 1, 2, 0}, 0, 0, texel),
                    "8-bit fetch accepted");
            require(!sampler.fetch(store, TileDescriptor{2, 0, 1, 0}, 0, 0, texel),
                    "4-bit fetch accepted");
            require(!sampler.fetch(store, TileDescriptor{3, 3, 1, 0}, 0, 0, texel),
                    "Unsupported IA32 format accepted");
            require(!sampler.fetch(store, TileDescriptor{0, 2, 2, 0}, UINT32_MAX, 0, texel),
                    "Overflowing S aliased TMEM");
            require(!sampler.fetch(store, TileDescriptor{0, 2, 2, 0}, 0, 1u << 31, texel),
                    "Overflowing T aliased TMEM");
            require(!sampler.fetch(store, TileDescriptor{3, 1, 0, 0}, 4096, 0, texel),
                    "IA8 fetch wrapped beyond TMEM");
            // A row base outside the 9-bit unit field is rejected: tmem 511
            // with line 1 puts row 1 at unit 512.
            require(sampler.fetch(store, TileDescriptor{0, 2, 1, 511}, 0, 0,
                                  texel),
                    "Last in-range row base rejected");
            require(!sampler.fetch(store, TileDescriptor{0, 2, 1, 511}, 0, 1,
                                   texel),
                    "Out-of-range row base accepted");
        }

        std::puts("PASS: explicit RGBA16/RGBA32 point fetch against reference TMEM");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
