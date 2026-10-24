// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

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
    uint8_t readByte(uint32_t logicalByte) const {
        return bytes[logicalByte & (kBytes - 1)];
    }
    void writeByte(uint32_t logicalByte, uint8_t value) {
        bytes[logicalByte & (kBytes - 1)] = value;
    }
    // Hardware TMEM uses an 11-bit word address spanning both banks, where
    // bit 10 selects the bank.
    static constexpr uint32_t hwBank(uint32_t hwWord) { return (hwWord >> 10) & 1u; }
    static constexpr uint32_t hwLogicalWord(uint32_t hwWord) { return hwWord & 1023u; }

    uint16_t readHwWord(uint32_t hwWord) const {
        return read16(hwLogicalWord(hwWord), hwBank(hwWord));
    }
    void writeHwWord(uint32_t hwWord, uint16_t value) {
        write16(hwLogicalWord(hwWord), value, hwBank(hwWord));
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

    // Raw 4 KiB image in hardware byte layout: bank 0 occupies the first
    // half, bank 1 the second. Suitable for byte-exact reference comparison.
    void copyBytes(std::span<uint8_t> destination) const {
        const size_t count = std::min(destination.size(), bytes.size());
        for (size_t i = 0; i < count; ++i) destination[i] = bytes[i];
    }

private:
    std::array<uint8_t, kBytes> bytes{};
};
}
