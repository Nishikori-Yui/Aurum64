// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace aurum::dpc {
constexpr uint32_t kXbusDmemDma = 0x001;
constexpr uint32_t kFreeze = 0x002;
constexpr uint32_t kFlush = 0x004;

bool shouldProcess(uint32_t status);
bool flushRequested(uint32_t status);
bool validRange(uint32_t start, uint32_t end, bool xbus, uint32_t rdramBytes);
uint32_t busAddress(uint32_t address, bool xbus);
uint32_t wordsAvailable(uint32_t current, uint32_t end);
}
