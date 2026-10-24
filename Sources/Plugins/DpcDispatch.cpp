// SPDX-License-Identifier: GPL-2.0-or-later
#include "DpcDispatch.hpp"

namespace aurum::dpc {
bool shouldProcess(uint32_t status) {
    return !(status & kFreeze);
}

bool flushRequested(uint32_t status) {
    return status & kFlush;
}

bool validRange(uint32_t start, uint32_t end, bool xbus, uint32_t rdramBytes) {
    if (end < start) return false;
    const uint32_t limit = xbus ? 4096u : rdramBytes;
    return end - start <= limit;
}

uint32_t busAddress(uint32_t address, bool xbus) {
    return xbus ? address & 4095u : address;
}

uint32_t wordsAvailable(uint32_t current, uint32_t end) {
    if (end <= current) return 0;
    const uint32_t words = (end - current) / 4;
    return words > 2 ? 2 : words;
}
}
