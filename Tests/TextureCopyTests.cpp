// SPDX-License-Identifier: MIT
#include "Renderer.hpp"
#include "TextureCopy.hpp"
#include <cstdio>
#include <stdexcept>

using namespace aurum::rdp;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        Renderer renderer;
        std::string error;
        require(renderer.initialize(argv[1], error), error.c_str());
        std::vector<uint32_t> memory((4u << 20) / 4, 0x12345678);
        aurum::tmem::Store tmem;
        for (uint32_t s = 0; s < 4; ++s) tmem.writeHwWord(s ^ 1u, 0x8800u + s);
        TextureRect rect;
        rect.cycleType = 2; rect.otherModesHi = 2u << 20;
        rect.fbSize = rect.tileSize = 2; rect.fbWidth = 7; rect.fbAddress = 0x1002;
        rect.tileLine = 1; rect.xl = 12; rect.yl = 4;
        rect.clipXl = 28; rect.clipYl = 8; rect.dsdx = 4096;
        rect.otherModesLo = 1;
        require(renderer.textureCopy(reinterpret_cast<uint8_t*>(memory.data()),
                                     memory.size() * 4, rect, tmem, error), error.c_str());
        for (uint32_t y = 0; y < 2; ++y) {
            for (uint32_t x = 0; x < 4; ++x) {
                uint16_t value = 0;
                const uint32_t address = rect.fbAddress + (y * rect.fbWidth + x) * 2;
                require(aurum::memory::readHalfword(reinterpret_cast<uint8_t*>(memory.data()),
                                                   memory.size() * 4, address, value),
                        "Read back texture pixel");
                const uint16_t expected = (x & 1) ? 0x8800 + x :
                    ((address & 2) ? 0x5678 : 0x1234);
                require(value == expected, "Copy alpha test or odd-stride write mismatch");
            }
        }
        require((memory[0x1000 / 4] >> 16) == 0x1234,
                "Copy changed a neighboring halfword");
        const auto before = memory;
        auto intensity = rect;
        intensity.tileFormat = 3;
        intensity.tileSize = 1;
        intensity.tileLine = 2;
        intensity.otherModesLo = 0;
        intensity.dsdx = 4096;
        intensity.dtdy = 1024;
        require(renderer.textureCopy(reinterpret_cast<uint8_t*>(memory.data()),
                                     memory.size() * 4, intensity, tmem, error),
                "Verified IA8 copy envelope was rejected");
        intensity.tileFormat = 4;
        require(renderer.textureCopy(reinterpret_cast<uint8_t*>(memory.data()),
                                     memory.size() * 4, intensity, tmem, error),
                "Verified I8 copy envelope was rejected");
        const auto afterI8 = memory;
        using Mutation = void (*)(TextureRect&);
        const Mutation intensityMutations[] = {
            +[](TextureRect& r) { r.s = 32; },
            +[](TextureRect& r) { r.t = 32; },
            +[](TextureRect& r) { r.dsdx = 2048; },
            +[](TextureRect& r) { r.dtdy = 512; },
            +[](TextureRect& r) { r.flip = true; },
            +[](TextureRect& r) { r.tileModes = 1u << 4; },
            +[](TextureRect& r) { r.tileSl = 4; },
            +[](TextureRect& r) { r.tileTl = 4; },
        };
        for (const auto change : intensityMutations) {
            auto item = intensity;
            change(item);
            require(!renderer.textureCopy(reinterpret_cast<uint8_t*>(memory.data()),
                                          memory.size() * 4, item, tmem, error),
                    "Unverified IA/I addressing was accepted");
            require(memory == afterI8,
                    "Rejected IA/I copy partially modified RDRAM");
        }
        memory = before;
        std::vector<TextureRect> rejected;
        const auto bad = [&](auto change) {
            auto item = rect; change(item); rejected.push_back(item);
        };
        bad([](auto& r) { r.cycleType = 0; });
        bad([](auto& r) { r.otherModesHi |= 1u << 19; }); // perspective
        bad([](auto& r) { r.otherModesHi |= 1u << 15; }); // TLUT
        bad([](auto& r) { r.otherModesLo |= 1u << 4; });  // depth compare
        bad([](auto& r) { r.scissorMode = 2; });
        bad([](auto& r) { r.fbFormat = 2; });
        bad([](auto& r) { r.fbSize = 3; });
        bad([](auto& r) { r.tileSize = 3; });
        bad([](auto& r) { r.tileFormat = 2; });
        bad([](auto& r) { r.tileFormat = 3; r.tileSize = 1; r.otherModesLo = 1; });
        bad([](auto& r) { r.tileFormat = 4; r.tileSize = 1; r.otherModesLo = 1; });
        bad([](auto& r) { r.tileFormat = 3; r.tileSize = 2; r.otherModesLo = 1; });
        bad([](auto& r) { r.xh = 1; });
        bad([](auto& r) { r.clipYl = 7; });
        bad([](auto& r) { r.fbAddress = 0x1001; });
        bad([](auto& r) { r.fbWidth = 0; });
        bad([](auto& r) { r.fbWidth = 2; });
        bad([](auto& r) { r.fbAddress = (4u << 20) - 2; });
        bad([](auto& r) { r.s = -1; });
        bad([](auto& r) { r.t = -1; });
        // Fail after an otherwise valid first row; rejection must be atomic.
        bad([](auto& r) { r.tileTmem = 511; r.dtdy = 1024; });
        for (const auto& item : rejected) {
            require(!renderer.textureCopy(reinterpret_cast<uint8_t*>(memory.data()),
                                          memory.size() * 4, item, tmem, error),
                    "Unsupported copy mode was accepted");
            require(memory == before, "Rejected copy partially modified RDRAM");
        }
        rect.xh = rect.xl + 4;
        require(renderer.textureCopy(reinterpret_cast<uint8_t*>(memory.data()),
                                     memory.size() * 4, rect, tmem, error) && memory == before,
                "Empty copy should leave RDRAM intact");
        Decoder decoder;
        TextureRect captured;
        decoder.textureRect = [&](const auto& r) { captured = r; return true; };
        const uint32_t commands[] = {
            0xed001008, 0x02020040, 0xef280000, 0x31,
            0xe400c004, 0, 0, 0x10000400,
            0xed000000, 0x00100010, 0xef300000, 0};
        decoder.push(commands);
        require(captured.clipXh == 1 && captured.clipYh == 8 && captured.clipXl == 32 &&
                captured.clipYl == 64 && captured.scissorMode == 2 &&
                captured.otherModesHi == 0x280000 && captured.otherModesLo == 0x31,
                "Rectangle did not retain its issue-time scissor and modes");
        std::puts("PASS: Metal copy word ownership, alpha, atomic rejection and immutable draw state");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
