// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <array>
#include <functional>
#include <span>
#include <vector>

namespace aurum::rdp {
struct Fill {
    uint32_t address, width, size, color;
    uint32_t x0, y0, x1, y1;
};
struct UnsupportedContext {
    uint32_t valid = 0, opcode = 0, wordCount = 0, capturedWords = 0;
    uint64_t command = 0, submission = 0, vi = 0;
    uint32_t words[32]{};
};

// Commands use host-order 32-bit words, as in Mupen's word-swapped RDRAM.
class Decoder {
public:
    Decoder() { reset(); }
    std::function<void(const Fill&)> fill;
    std::function<void()> sync;
    uint64_t commands = 0, unsupported = 0;
    uint64_t opcodeCount(uint8_t opcode) const;
    uint64_t firstCommand(uint8_t opcode) const;
    uint64_t firstSubmission(uint8_t opcode) const;
    uint64_t firstVI(uint8_t opcode) const;
    uint64_t submissionCount() const { return submissions; }
    uint64_t viCount() const { return viEvents; }
    uint64_t recentCommands() const { return recentCommandCount; }
    uint64_t recentUnsupported() const { return recentUnsupportedCount; }
    const UnsupportedContext& firstUnsupported() const { return unsupportedContext; }
    void beginSubmission();
    void beginVI();
    void push(std::span<const uint32_t> words);
    void reset();
    static size_t wordCount(uint8_t opcode);
    bool hasPendingWords() const { return !pending.empty(); }
private:
    void execute(std::span<const uint32_t> words);
    std::vector<uint32_t> pending;
    std::array<uint64_t, 64> opcodeCounts{};
    std::array<uint64_t, 64> firstCommands{};
    std::array<uint64_t, 64> firstSubmissions{};
    std::array<uint64_t, 64> firstVIs{};
    uint64_t submissions = 0, viEvents = 0, currentVI = 0;
    uint64_t commandBaseline = 0, unsupportedBaseline = 0;
    uint64_t recentCommandCount = 0, recentUnsupportedCount = 0;
    UnsupportedContext unsupportedContext{};
    uint32_t address = 0, width = 0, size = 0, format = 0, color = 0, cycle = 0;
    uint32_t sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    void markUnsupported(std::span<const uint32_t> words);
};
}
