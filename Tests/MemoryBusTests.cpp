// SPDX-License-Identifier: MIT
#include "MemoryBus.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        std::vector<uint8_t> memory(8, 0);
        require(aurum::memory::writeHalfword(memory.data(), memory.size(), 0, 0x1122),
                "even halfword write");
        require(aurum::memory::writeHalfword(memory.data(), memory.size(), 2, 0x3344),
                "odd halfword write");
        uint16_t half = 0;
        require(aurum::memory::readHalfword(memory.data(), memory.size(), 0, half) &&
                half == 0x1122, "even halfword read");
        require(aurum::memory::readHalfword(memory.data(), memory.size(), 2, half) &&
                half == 0x3344, "odd halfword read");
        require(aurum::memory::hostHalfwordAddress(0) == 2 &&
                aurum::memory::hostHalfwordAddress(2) == 0,
                "host halfword mapping");
        require(!aurum::memory::readHalfword(memory.data(), memory.size(), 8, half) &&
                !aurum::memory::writeWord(memory.data(), memory.size(), 8, 1),
                "memory boundary rejection");
        std::puts("PASS: N64 address to host word-swapped memory mapping");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
