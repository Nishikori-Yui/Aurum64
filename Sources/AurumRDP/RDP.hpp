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

struct StateSnapshot {
    static constexpr uint32_t kVersion = 1;
    uint32_t fbFormat = 0, fbSize = 0, fbWidth = 0, fbAddress = 0, fillColor = 0;
    uint32_t clipXh = 0, clipYh = 0, clipXl = 0, clipYl = 0, cycleType = 0;
    uint32_t imageReadEn = 0, zUpdateEn = 0, zCompareEn = 0;
    uint32_t tiFormat = 0, tiSize = 0, tiWidth = 0, tiAddress = 0, zbAddress = 0;
    std::array<uint32_t, 32> tiles{};
    std::array<uint32_t, 64> words() const;
    static StateSnapshot fromWords(const std::array<uint32_t, 64>& words);
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
    const std::array<uint32_t, 32>& tileSizes() const { return tileSizes_; }
    StateSnapshot stateSnapshot() const {
        StateSnapshot state;
        state.fbFormat = format; state.fbSize = size; state.fbWidth = width;
        state.fbAddress = address; state.fillColor = color;
        state.clipXh = sx0; state.clipYh = sy0; state.clipXl = sx1; state.clipYl = sy1;
        state.cycleType = cycle; state.imageReadEn = imageReadEn;
        state.zUpdateEn = zUpdateEn; state.zCompareEn = zCompareEn;
        state.tiFormat = tiFormat; state.tiSize = tiSize; state.tiWidth = tiWidth;
        state.tiAddress = tiAddress; state.zbAddress = zbAddress; state.tiles = tiles;
        return state;
    }
    bool hasState() const { return haveState; }
    void beginSubmission();
    void beginVI();
    void push(std::span<const uint32_t> words);
    void reset();
    static size_t wordCount(uint8_t opcode);
    bool hasPendingWords() const { return !pending.empty(); }
private:
    void execute(std::span<const uint32_t> words);
    uint32_t pendingAddress_ = 0;
    bool haveState = false;
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
    uint32_t imageReadEn = 0, zUpdateEn = 0, zCompareEn = 0;
    uint32_t tiFormat = 0, tiSize = 0, tiWidth = 0, tiAddress = 0, zbAddress = 0;
    std::array<uint32_t, 32> tiles{};
    std::array<uint32_t, 32> tileSizes_{};
    uint32_t sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    void markUnsupported(std::span<const uint32_t> words);
};
}
