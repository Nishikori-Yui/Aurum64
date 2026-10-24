// SPDX-License-Identifier: MIT
#include "Renderer.hpp"
#include "Capture.hpp"
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
        require(decoder.opcodeCount(0x00) == 0 && decoder.firstCommand(0x00) == UINT64_MAX,
                "Absent opcode statistics");
        require(decoder.unsupported == 0, "Supported fill stream marked unsupported");
        for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
            uint32_t word = ram[(0x1000 + (y*8+x)*2)/4];
            unsigned value = (word >> ((x & 1) ? 0 : 16)) & 65535;
            unsigned expected = y >= 1 && y < 3 && x >= 1 && x < 5 ? ((x & 1) ? 0x07c1 : 0xf801)
                                                                               : ((x & 1) ? 0x3344 : 0x1122);
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
        require(pixels[9] == 0xff00ff00 && pixels[10] == 0xff0000ff, "RGBA5551 VI conversion mismatch");
        const uint32_t triangle[44] = {0xcf000000};
        decoder.push(std::span(triangle, 43));
        require(decoder.unsupported == 0, "Partial triangle consumed too early");
        decoder.push(std::span(triangle + 43, 1));
        require(decoder.unsupported == 1 && Decoder::wordCount(0x24) == 4, "Unsupported opcode accounting");
        const auto& context = decoder.firstUnsupported();
        require(context.valid && context.opcode == 0x0f && context.wordCount == 44 &&
                context.capturedWords == 32 && context.command == 6 &&
                context.submission == 1 && context.vi == 2 &&
                context.words[0] == triangle[0], "Unsupported context");
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
                    const auto shift = address % 4 ? 0 : 16;
                    expected[address / 4] = (expected[address / 4] & ~(65535u << shift)) | (test.color & (65535u << shift));
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
