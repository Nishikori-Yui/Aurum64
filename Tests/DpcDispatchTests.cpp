// SPDX-License-Identifier: MIT
#include "DpcDispatch.hpp"
#include <cstdio>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        using namespace aurum::dpc;
        require(shouldProcess(0) && !shouldProcess(kFreeze), "DPC freeze policy");
        require(flushRequested(kFlush) && !flushRequested(0), "DPC flush policy");
        require(validRange(0, 8, false, 8 * 1024 * 1024), "RDRAM range rejected");
        require(!validRange(0, 8 * 1024 * 1024 + 4, false, 8 * 1024 * 1024),
                "Oversized RDRAM range accepted");
        require(validRange(0xffc, 0x1004, true, 4 * 1024 * 1024),
                "Wrapped XBUS window rejected");
        require(!validRange(0x100, 0x2104, true, 4 * 1024 * 1024),
                "Oversized XBUS range accepted");
        require(busAddress(0x1000, true) == 0 && busAddress(0x1000, false) == 0x1000,
                "XBUS address wrapping");
        require(wordsAvailable(0, 0) == 0 && wordsAvailable(0, 4) == 1 &&
                wordsAvailable(0, 8) == 2 && wordsAvailable(4, 12) == 2,
                "DPC word chunking");
        std::puts("PASS: DPC freeze/flush policy, XBUS wrap and submission chunking");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
