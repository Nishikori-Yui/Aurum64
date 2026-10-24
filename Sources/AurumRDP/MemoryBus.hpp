// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace aurum::memory {
constexpr size_t hostWordAddress(uint32_t address) {
    return address & ~size_t(3);
}

constexpr size_t hostHalfwordAddress(uint32_t address) {
    return address ^ 2u;
}

inline bool readWord(const uint8_t* memory, size_t length, uint32_t address,
                     uint32_t& value) {
    const size_t host = hostWordAddress(address);
    if (host + sizeof(value) > length) return false;
    std::memcpy(&value, memory + host, sizeof(value));
    return true;
}

inline bool writeWord(uint8_t* memory, size_t length, uint32_t address,
                      uint32_t value) {
    const size_t host = hostWordAddress(address);
    if (host + sizeof(value) > length) return false;
    std::memcpy(memory + host, &value, sizeof(value));
    return true;
}

inline bool readHalfword(const uint8_t* memory, size_t length, uint32_t address,
                         uint16_t& value) {
    const size_t host = hostHalfwordAddress(address);
    if (host + sizeof(value) > length) return false;
    std::memcpy(&value, memory + host, sizeof(value));
    return true;
}

inline bool writeHalfword(uint8_t* memory, size_t length, uint32_t address,
                          uint16_t value) {
    const size_t host = hostHalfwordAddress(address);
    if (host + sizeof(value) > length) return false;
    std::memcpy(memory + host, &value, sizeof(value));
    return true;
}
}
