// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

namespace aurum::rdp {
struct Fill {
    uint32_t address, width, size, color;
    uint32_t x0, y0, x1, y1;
};

// Commands use host-order 32-bit words, as in Mupen's word-swapped RDRAM.
class Decoder {
public:
    std::function<void(const Fill&)> fill;
    std::function<void()> sync;
    uint64_t commands = 0, unsupported = 0;
    void push(std::span<const uint32_t> words);
    void reset();
    static size_t wordCount(uint8_t opcode);
    bool hasPendingWords() const { return !pending.empty(); }
private:
    void execute(std::span<const uint32_t> words);
    std::vector<uint32_t> pending;
    uint32_t address = 0, width = 0, size = 0, format = 0, color = 0, cycle = 0;
    uint32_t sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
};
}
