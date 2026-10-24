// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace aurum::tmem {
class Store {
public:
    static constexpr size_t kBytes = 4096;
    static constexpr size_t kWords16 = kBytes / 2;
    static constexpr size_t kBankWords16 = kWords16 / 2;

    Store() { clear(); }
    void clear() { bytes.fill(0); }

    static constexpr size_t word16Address(uint32_t logicalWord, uint32_t bank = 0) {
        return ((logicalWord & (kBankWords16 - 1)) |
                ((bank & 1u) * kBankWords16)) * 2;
    }
    static constexpr size_t byteAddress(uint32_t logicalWord, uint32_t bank,
                                       uint32_t byteInWord) {
        return word16Address(logicalWord, bank) + (byteInWord & 1u);
    }
    static constexpr size_t tlutAddress(uint32_t entry) {
        return word16Address(entry, 1);
    }

    uint8_t readByte(uint32_t logicalByte) const {
        return bytes[logicalByte & (kBytes - 1)];
    }
    void writeByte(uint32_t logicalByte, uint8_t value) {
        bytes[logicalByte & (kBytes - 1)] = value;
    }
    uint16_t read16(uint32_t logicalWord, uint32_t bank = 0) const {
        const size_t address = word16Address(logicalWord, bank);
        return uint16_t(bytes[address]) | uint16_t(bytes[address + 1]) << 8;
    }
    void write16(uint32_t logicalWord, uint16_t value, uint32_t bank = 0) {
        const size_t address = word16Address(logicalWord, bank);
        bytes[address] = uint8_t(value);
        bytes[address + 1] = uint8_t(value >> 8);
    }

private:
    std::array<uint8_t, kBytes> bytes{};
};
}
