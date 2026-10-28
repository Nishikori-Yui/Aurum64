// SPDX-License-Identifier: MIT
#include "Renderer.hpp"
#include "Capture.hpp"
#include "TriangleReference.hpp"
#include "TriangleSpans.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <random>
#include <sstream>
#include <string_view>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static uint32_t readBE(const uint8_t* bytes) {
    return uint32_t(bytes[3]) | uint32_t(bytes[2]) << 8 |
           uint32_t(bytes[1]) << 16 | uint32_t(bytes[0]) << 24;
}
int main(int argc, char** argv) {
    try {
        require(argc >= 2, "Usage: aurum-rdp-tests metallib [network-order.rdp]");
        using namespace aurum::rdp;
        Renderer renderer; std::string error;
        require(renderer.initialize(argv[1], error), error.c_str());
        std::vector<uint32_t> ram(8 * 1024 * 1024 / 4, 0x11223344);
        Decoder decoder; unsigned fills = 0, syncs = 0;
        decoder.fill = [&](const Fill& fill) {
            ++fills;
            require(renderer.fill(reinterpret_cast<uint8_t*>(ram.data()), ram.size()*4, fill, error), error.c_str());
        };
        decoder.sync = [&] { ++syncs; };
        decoder.beginSubmission();
        const uint32_t commands[] = {
            0xff100007, 0x1000, // RGBA16, stride 8
            0xed000000, (32 << 12) | 32,
            0xef300000, 0,
            0xf7000000, 0xf80107c1,
            0xf6000000 | (16 << 12) | 8, (4 << 12) | 4,
            0xe9000000, 0};
        // Split every word to exercise partial 64-bit commands across DMA submissions.
        for (auto& word : commands) decoder.push(std::span(&word, 1));
        require(fills == 1 && syncs == 1 && decoder.commands == 6, "Decoder counters");
        require(decoder.opcodeCount(0x3f) == 1 && decoder.firstCommand(0x3f) == 0,
                "Opcode first-seen statistics");
        require(decoder.opcodeCount(0x36) == 1 && decoder.firstCommand(0x36) == 4,
                "Fill opcode statistics");
        require(decoder.submissionCount() == 1 && decoder.firstSubmission(0x3f) == 1 &&
                decoder.firstVI(0x3f) == 1, "Submission and VI statistics");
        decoder.beginVI();
        require(decoder.recentCommands() == 6 && decoder.recentUnsupported() == 0,
                "Recent VI sample statistics");
        const uint32_t noOpStream[] = {0, 0, 0xff100040, 0x40};
        for (auto& word : noOpStream) decoder.push(std::span(&word, 1));
        require(decoder.commands == 8 && decoder.opcodeCount(0x00) == 1 &&
                decoder.stateSnapshot().fbWidth == 65,
                "No-op command boundary");
        require(decoder.unsupported == 0, "Supported fill stream marked unsupported");
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            uint32_t word = ram[(0x1000 + (y*8+x)*2)/4];
            const unsigned halfIndex = (0x1000 / 2) + y * 8 + x;
            const unsigned address = 0x1000 + (y * 8 + x) * 2;
            unsigned value = (word >> ((((address >> 1) ^ 1) & 1) * 16)) & 65535;
            unsigned expected = y >= 1 && y < 3 && x >= 1 && x < 5
                                    ? ((halfIndex & 1) ? 0x07c1 : 0xf801)
                                    : ((halfIndex & 1) ? 0x3344 : 0x1122);
            require(value == expected, "Metal RGBA16 masked fill mismatch");
        }
        Fill rgba32{0x2000, 4, 3, 0x12345678, 1, 0, 3, 2};
        require(renderer.fill(reinterpret_cast<uint8_t*>(ram.data()), ram.size()*4, rgba32, error), error.c_str());
        require(ram[0x2000/4] == 0x11223344 && ram[0x2000/4+1] == 0x12345678, "RGBA32 fill mismatch");
        VideoInterface vi{3, 0x2000, 4, 4, 4, 1024, 1024};
        uint32_t w,h; std::vector<uint32_t> pixels;
        require(renderer.scanout(reinterpret_cast<uint8_t*>(ram.data()), ram.size()*4, vi, pixels,w,h,error),error.c_str());
        require(w == 4 && h == 2 && pixels[1] == 0xff563412, "RGBA32 VI conversion mismatch");
        vi = {2, 0x1000, 8, 8, 16, 1024, 1024};
        require(renderer.scanout(reinterpret_cast<uint8_t*>(ram.data()), ram.size()*4, vi,pixels,w,h,error),error.c_str());
        require(pixels[9] == 0xff00f800 && pixels[10] == 0xff0000f8,
                "RGBA5551 VI conversion mismatch");
        vi = {3, 0x2000, 4, 4, 4, 512, 512};
        require(renderer.scanout(reinterpret_cast<uint8_t*>(ram.data()), ram.size()*4, vi,
                                 pixels, w, h, error), error.c_str());
        require(w == 2 && h == 1 && pixels.size() == 2,
                "VI scale was not applied to output dimensions");
        const uint32_t triangle[44] = {0xcf000000};
        decoder.push(std::span(triangle, 43));
        require(decoder.unsupported == 0, "Partial triangle consumed too early");
        decoder.push(std::span(triangle + 43, 1));
        require(decoder.unsupported == 1 && Decoder::wordCount(0x24) == 4, "Unsupported opcode accounting");
        const auto& context = decoder.firstUnsupported();
        require(context.valid && context.opcode == 0x0f && context.wordCount == 44 &&
                context.capturedWords == 32 && context.command == 8 &&
                context.submission == 1 && context.vi == 2 &&
                context.words[0] == triangle[0], "Unsupported context");
        const size_t expectedWords[64] = {
            2, 2, 2, 2, 2, 2, 2, 2,
            8, 12, 24, 28, 24, 28, 40, 44,
            2, 2, 2, 2, 2, 2, 2, 2,
            2, 2, 2, 2, 2, 2, 2, 2,
            2, 2, 2, 2, 4, 4, 2, 2,
            2, 2, 2, 2, 2, 2, 2, 2,
            2, 2, 2, 2, 2, 2, 2, 2,
            2, 2, 2, 2, 2, 2, 2, 2
        };
        for (size_t opcode = 0; opcode < 64; ++opcode)
            require(Decoder::wordCount(static_cast<uint8_t>(opcode)) == expectedWords[opcode],
                    "RDP opcode word length table");
        decoder.reset();
        decoder.beginSubmission();
        for (size_t opcode = 0; opcode < 64; ++opcode) {
            std::vector<uint32_t> command(expectedWords[opcode], 0);
            command[0] = static_cast<uint32_t>(opcode << 24);
            decoder.push(command);
        }
        require(!decoder.hasPendingWords() && decoder.commands == 64,
                "All RDP opcode boundaries");
        decoder.reset();
        decoder.beginSubmission();
        const uint32_t stateCommands[] = {
            0xfd000000 | (1 << 21) | (2 << 19) | 16, 0x0123456,
            0xfe000000, 0x00345678,
            0xef300000, 0x00000070,
            0xf2000000 | (12 << 12) | 20, (3 << 24) | (28 << 12) | 36,
            0x35000000 | (3 << 21) | (2 << 19) | (17 << 9) | 0x88, 3 << 24
        };
        decoder.push(stateCommands);
        const auto state = decoder.stateSnapshot();
        require(state.tiFormat == 1 && state.tiSize == 2 && state.tiWidth == 17 &&
                state.tiAddress == 0x123456 && state.zbAddress == 0x345678 &&
                state.cycleType == 3 && state.imageReadEn == 1 &&
                state.zUpdateEn == 1 && state.zCompareEn == 1 &&
                state.tiles[12] == 3 && state.tiles[13] == 2 &&
                state.tiles[14] == 17 && state.tiles[15] == 0x88,
                "RDP state snapshot fields");
        require(decoder.tileSizes()[12] == 12 && decoder.tileSizes()[13] == 20 &&
                decoder.tileSizes()[14] == 28 && decoder.tileSizes()[15] == 36,
                "RDP tile size state");
        const auto encodedState = state.words();
        const auto decodedState = StateSnapshot::fromWords(encodedState);
        require(encodedState[63] == StateSnapshot::kVersion &&
                decodedState.tiAddress == state.tiAddress &&
                decodedState.zbAddress == state.zbAddress &&
                decodedState.tiles == state.tiles,
                "RDP state snapshot round-trip");
        decoder.reset();
        decoder.beginSubmission();
        std::vector<TextureLoad> loads;
        decoder.textureLoad = [&](const TextureLoad& load) {
            loads.push_back(load);
            return true;
        };
        const uint32_t textureCommands[] = {
            // SetTextureImage RGBA16, stride 8, address 0x1000
            0xfd000000 | (0 << 21) | (2 << 19) | 7, 0x00001000,
            // SetTile tile 1: format 0, size 2, line 1, tmem 4
            0xf5000000 | (0 << 21) | (2 << 19) | (1 << 9) | 4, 1 << 24,
            // LoadTile tile 1, sl 0, tl 0, sh 8, th 4
            0xf4000000 | (0 << 12) | 0, (1 << 24) | (8 << 12) | 4,
            // LoadBlock tile 1, sl 0, tl 0, sh 8, dxt 0x40
            0xf3000000 | (0 << 12) | 0, (1 << 24) | (8 << 12) | 0x40,
            // LoadTLUT tile 1, sl 0, tl 0, sh 4, th 0
            0xf0000000 | (0 << 12) | 0, (1 << 24) | (4 << 12) | 0};
        decoder.push(textureCommands);
        require(decoder.unsupported == 0 && loads.size() == 3,
                "Texture load opcodes reported");
        require(loads[0].opcode == 0x34 && loads[0].tile == 1 &&
                loads[0].tileSize == 2 && loads[0].tileLine == 1 &&
                loads[0].tileTmem == 4 && loads[0].tiSize == 2 &&
                loads[0].tiWidth == 8 && loads[0].tiAddress == 0x1000 &&
                loads[0].sl == 0 && loads[0].sh == 8 && loads[0].th == 4,
                "LoadTile decoded fields");
        require(loads[1].opcode == 0x33 && loads[1].dxt == 0x40,
                "LoadBlock decoded fields");
        require(loads[2].opcode == 0x30 && loads[2].sh == 4 && loads[2].th == 0,
                "LoadTLUT decoded fields");
        decoder.textureLoad = [](const TextureLoad&) { return false; };
        decoder.push(std::span(textureCommands + 4, 2));
        require(decoder.unsupported == 1 && decoder.firstUnsupported().opcode == 0x34,
                "Rejected texture load reported as unsupported");
        decoder.textureLoad = nullptr;
        decoder.push(std::span(textureCommands + 4, 2));
        require(decoder.unsupported == 2,
                "Missing texture load handler reported as unsupported");

        // TextureRectangle (0x24) and Flip (0x25) decode their four fields and
        // sign-extend the 16-bit s/t and derivatives.
        decoder.reset();
        decoder.beginSubmission();
        std::vector<TextureRect> rects;
        decoder.textureRect = [&](const TextureRect& rect) {
            rects.push_back(rect);
            return true;
        };
        // word0 = xl<<12 | yl, word1 = tile<<24 | xh<<12 | yh,
        // word2 = s<<16 | t, word3 = dsdx<<16 | dtdy.
        const uint32_t rectCommands[] = {
            0xe4000000 | (40 << 12) | 24, (1 << 24) | (12 << 12) | 8,
            uint32_t((0xffff << 16) | 0x0010), uint32_t((0x8000 << 16) | 0x0020),
            0xe5000000 | (40 << 12) | 24, (2 << 24) | (12 << 12) | 8,
            uint32_t((0x0002 << 16) | 0x0003), uint32_t((0x0004 << 16) | 0x0005)};
        decoder.push(rectCommands);
        require(decoder.unsupported == 0 && rects.size() == 2,
                "TextureRectangle opcodes reported");
        require(!rects[0].flip && rects[0].tile == 1 && rects[0].xl == 40 &&
                rects[0].yl == 24 && rects[0].xh == 12 && rects[0].yh == 8 &&
                rects[0].s == -1 && rects[0].t == 16 &&
                rects[0].dsdx == -32768 && rects[0].dtdy == 32,
                "TextureRectangle decoded fields");
        require(rects[1].flip && rects[1].tile == 2 && rects[1].s == 2 &&
                rects[1].t == 3 && rects[1].dsdx == 4 && rects[1].dtdy == 5,
                "TextureRectangleFlip decoded fields");
        decoder.textureRect = [](const TextureRect&) { return false; };
        decoder.push(std::span(rectCommands, 4));
        require(decoder.unsupported == 1 &&
                decoder.firstUnsupported().opcode == 0x24,
                "Rejected TextureRectangle reported as unsupported");
        decoder.textureRect = nullptr;
        decoder.push(std::span(rectCommands, 4));
        require(decoder.unsupported == 2,
                "Missing TextureRectangle handler reported as unsupported");
        decoder.textureLoad = nullptr;

        decoder.reset();
        decoder.beginSubmission();
        std::vector<Triangle> triangles;
        decoder.triangle = [&](const Triangle& triangle) {
            triangles.push_back(triangle);
            return true;
        };
        for (uint32_t opcode = 0x08; opcode <= 0x0f; ++opcode) {
            const size_t count = Decoder::wordCount(static_cast<uint8_t>(opcode));
            std::vector<uint32_t> command(count, 0);
            command[0] = opcode << 24 | 0x0123;
            command[1] = (0x0234u << 16) | 0x0345u;
            command[2] = 0x01234567;
            command[3] = 0x23456789;
            command[4] = 0x03456789;
            command[5] = 0x3456789a;
            command[6] = 0x0456789a;
            command[7] = 0x456789ab;
            decoder.push(command);
            require(!decoder.hasPendingWords(), "Triangle command remained pending");
        }
        require(decoder.unsupported == 0 && triangles.size() == 8,
                "Triangle commands were not dispatched");
        for (const auto& triangle : triangles) {
            require(triangle.opcode >= 0x08 && triangle.opcode <= 0x0f &&
                    triangle.wordCount == Decoder::wordCount(
                        static_cast<uint8_t>(triangle.opcode)) &&
                    triangle.yl == 0x123 && triangle.ym == 0x234 &&
                    triangle.yh == 0x345 && triangle.xl == 0x01234567 &&
                    triangle.dxldy == static_cast<int32_t>(0xe3456789u) &&
                    triangle.xh == 0x03456789 &&
                    triangle.dxhdy == static_cast<int32_t>(0xf456789au) &&
                    triangle.xm == 0x0456789a &&
                    triangle.dxmdy == 0x056789ab,
                    "Triangle edge fields were decoded incorrectly");
            require(triangle.shade == ((triangle.opcode & 4) != 0) &&
                    triangle.texture == ((triangle.opcode & 2) != 0) &&
                    triangle.z == ((triangle.opcode & 1) != 0),
                    "Triangle payload flags were decoded incorrectly");
        }
        decoder.triangle = [](const Triangle&) { return false; };
        const uint32_t partialTriangle[44] = {0x0f000123};
        decoder.push(std::span(partialTriangle, 43));
        require(decoder.unsupported == 0 && decoder.hasPendingWords(),
                "Partial triangle was consumed too early");
        decoder.push(std::span(partialTriangle + 43, 1));
        require(decoder.unsupported == 1 && decoder.firstUnsupported().opcode == 0x0f,
                "Rejected triangle was not reported as unsupported");
        decoder.triangle = nullptr;

        Triangle flat;
        flat.yh = 0; flat.ym = 8; flat.yl = 16;
        flat.xh = 2 << 16; flat.xm = 2 << 16; flat.xl = 6 << 16;
        flat.dxhdy = 0; flat.dxmdy = 0; flat.dxldy = 0;
        const auto flatSpans = generateTriangleSpans(flat);
        require(flatSpans.size() == 4 && flatSpans[0].y == 0 &&
                flatSpans[0].left == 2 && flatSpans[0].right == 2 &&
                flatSpans[0].coverage == 15 && !flatSpans[0].valid &&
                flatSpans[2].left == 6 &&
                !flatSpans[2].valid,
                "Flat triangle span boundaries");

        Triangle stepped;
        stepped.yh = 1; stepped.ym = 5; stepped.yl = 13;
        stepped.xh = 0; stepped.xm = 4 << 16; stepped.xl = 8 << 16;
        stepped.dxhdy = 4 << 16; stepped.dxmdy = 2 << 16;
        stepped.dxldy = 1 << 16;
        const auto steppedSpans = generateTriangleSpans(stepped);
        require(steppedSpans.size() == 3 && steppedSpans.front().y == 1 &&
                steppedSpans.front().coverage == 15 &&
                steppedSpans.back().y == 3 && steppedSpans.back().coverage == 1 &&
                steppedSpans[1].left <= steppedSpans[2].left,
                "Subpixel and segmented triangle spans");
        Triangle filled;
        filled.yh = 0; filled.ym = 8; filled.yl = 16;
        filled.xh = 8 << 16; filled.xm = 2 << 16; filled.xl = 2 << 16;
        auto clippedSpans = generateTriangleSpans(filled);
        clipTriangleSpans(clippedSpans, 4, 1, 7, 3);
        require(!clippedSpans[0].valid && clippedSpans[1].left == 4 &&
                clippedSpans[1].right == 7 && clippedSpans[1].valid &&
                !clippedSpans[3].valid,
                "Triangle scissor clipping");

        Triangle wide = stepped;
        wide.xh = INT32_MIN + (8 << 16);
        wide.xm = INT32_MAX - (8 << 16);
        wide.xl = INT32_MAX - (8 << 16);
        wide.dxhdy = INT32_MAX;
        wide.dxmdy = INT32_MIN;
        wide.dxldy = INT32_MIN;
        const auto wideSpans = generateTriangleSpans(wide);
        require(wideSpans.size() == 3, "Large fixed-point triangle range");

        Triangle degenerate;
        degenerate.yh = 12; degenerate.ym = 12; degenerate.yl = 12;
        require(generateTriangleSpans(degenerate).empty(),
                "Degenerate triangle produced spans");

        std::vector<uint8_t> triangleMemory(0x4000, 0x5a);
        std::string triangleError;
        require(drawFlatTriangleReference(triangleMemory.data(), triangleMemory.size(),
                                           0x1000, 16, 2, 0x7c01, filled,
                                           0, 0, 8, 4, triangleError),
                triangleError.c_str());
        for (uint32_t y = 0; y < 4; ++y) {
            for (uint32_t x = 0; x < 8; ++x) {
                uint16_t pixel = 0;
                require(aurum::memory::readHalfword(triangleMemory.data(),
                                                    triangleMemory.size(),
                                                    0x1000 + (y * 16 + x) * 2,
                                                    pixel),
                        "Read flat triangle reference pixel");
                const uint16_t expected = (x >= 2 && x < 8) ? 0x7c01 : 0x5a5a;
                require(pixel == expected, "Flat triangle pixel ownership");
            }
        }
        const auto beforeTriangleReject = triangleMemory;
        require(!drawFlatTriangleReference(triangleMemory.data(), triangleMemory.size(),
                                            0x1000, 16, 3, 0x7c01, filled,
                                            0, 0, 8, 4, triangleError) &&
                triangleMemory == beforeTriangleReject,
                "Invalid flat triangle target changed memory");
        std::vector<uint8_t> metalTriangleMemory(0x4000, 0x5a);
        require(renderer.flatTriangle(metalTriangleMemory.data(),
                                       metalTriangleMemory.size(), 0x1000, 16, 2,
                                       0x7c01, filled, 0, 0, 8, 4, error),
                error.c_str());
        require(metalTriangleMemory == triangleMemory,
                "Metal flat triangle differs from CPU reference");

        std::mt19937 random(0x64);
        for (unsigned trial = 0; trial < 48; ++trial) {
            std::vector<uint32_t> actual(16384), expected;
            for (auto& word : actual) word = random();
            expected = actual;
            const uint32_t pixelSize = trial & 1 ? 2 : 3, bytes = pixelSize == 2 ? 2 : 4;
            const uint32_t stride = 1 + random() % 37;
            Fill test{uint32_t(random() % 8192) * bytes, stride, pixelSize, uint32_t(random()),
                      uint32_t(random() % stride), uint32_t(random() % 15), stride, 32};
            for (uint32_t y = test.y0; y < test.y1; ++y) for (uint32_t x = test.x0; x < test.x1; ++x) {
                const size_t address = test.address + (y * stride + x) * bytes;
                if (address + bytes > expected.size() * 4) continue;
                if (bytes == 4) expected[address / 4] = test.color;
                else {
                    const uint32_t halfIndex = address >> 1;
                    const auto half = (test.color >> ((halfIndex & 1) ? 0 : 16)) & 65535;
                    const auto shift = ((halfIndex ^ 1) & 1) * 16;
                    expected[address / 4] = (expected[address / 4] & ~(65535u << shift)) | (half << shift);
                }
            }
            require(renderer.fill(reinterpret_cast<uint8_t*>(actual.data()), actual.size() * 4, test, error), error.c_str());
            require(actual == expected, "Randomized Metal fill differs from scalar byte-address reference");
        }
        Fill invalid{0, 0, 2, 0, 0, 0, 1, 1};
        require(!renderer.fill(reinterpret_cast<uint8_t*>(ram.data()), ram.size()*4, invalid, error), "Zero stride accepted");
        invalid.width = 1; invalid.address = 1;
        require(!renderer.fill(reinterpret_cast<uint8_t*>(ram.data()), ram.size()*4, invalid, error), "Misaligned framebuffer accepted");
        vi.status = 0;
        require(renderer.scanout(reinterpret_cast<uint8_t*>(ram.data()), ram.size()*4, vi,pixels,w,h,error) && pixels.empty(), "VI blanking failed");
        if (argc >= 3) {
            std::ifstream input(argv[2], std::ios::binary);
            require(bool(input), "Cannot open replay trace");
            decoder.reset(); uint32_t word;
            while (input.read(reinterpret_cast<char*>(&word), 4)) { word = __builtin_bswap32(word); decoder.push(std::span(&word,1)); }
            require(input.eof() && input.gcount() == 0, "Truncated replay word");
            require(!decoder.hasPendingWords(), "Incomplete replay command");
            std::printf("Replay: %llu commands, %llu unsupported\n", decoder.commands, decoder.unsupported);
        }
        if (argc >= 4) {
            std::ifstream input(argv[3], std::ios::binary);
            require(bool(input), "Cannot open aurcap capture");
            std::vector<aurum::capture::Record> records;
            std::string error;
            require(aurum::capture::read(input, records, error), error.c_str());
            require(records.size() >= 4 && records.front().type == 1 &&
                    records.back().type == 0xffff, "Capture boundaries");
            unsigned submissions = 0, vis = 0;
            for (const auto& record : records) {
                if (record.type == 3) {
                    ++submissions;
                    require(record.payload.size() >= 16 &&
                            (record.payload.size() - 16) % 4 == 0, "Capture submission payload");
                }
                if (record.type == 4) {
                    ++vis;
                    require(record.payload.size() == 28, "Capture VI payload");
                }
            }
            require(submissions >= 1 && vis >= 1, "Capture events");
            require(records.front().payload.size() == 8 && records.back().payload.size() > 8,
                    "Capture initial/end payload");
            for (unsigned repeat = 0; repeat < 20; ++repeat) {
                decoder.reset();
                for (const auto& record : records) {
                    if (record.type != 3) continue;
                    decoder.beginSubmission();
                    for (size_t offset = 16; offset < record.payload.size(); offset += 4) {
                        uint32_t words[1] = {readBE(record.payload.data() + offset)};
                        decoder.push(words);
                    }
                }
                require(!decoder.hasPendingWords() && decoder.commands == 6 &&
                        decoder.unsupported == 0, "Capture replay result");
            }
            std::printf("Capture: %zu records, %u submissions, %u VI events\n",
                        records.size(), submissions, vis);
        }
        if (argc >= 5) {
            std::ifstream input(argv[4], std::ios::binary);
            require(bool(input), "Cannot open limited aurcap capture");
            std::vector<aurum::capture::Record> records;
            std::string error;
            require(aurum::capture::read(input, records, error), error.c_str());
            unsigned vis = 0;
            for (const auto& record : records) if (record.type == aurum::capture::kRecordVI) ++vis;
            require(vis == 1 && records.back().type == aurum::capture::kRecordEnd &&
                    records.back().payload.size() > 10 &&
                    std::string_view(reinterpret_cast<const char*>(records.back().payload.data()) + 8, 2) == "VI",
                    "Capture VI limit and reason");
            std::puts("PASS: capture stops at the configured VI limit");
        }
        std::puts("PASS: fragmented RDP decoding, RGBA16/32 fills (48 randomized cases), VI conversion/blanking, invalid inputs");
    } catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
