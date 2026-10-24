// SPDX-License-Identifier: MIT
#include "Tmem.hpp"
#include <array>
#include <cstdio>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        using aurum::tmem::Store;
        Store tmem;
        require(Store::word16Address(0, 0) == 0 &&
                Store::word16Address(0, 1) == 2048,
                "TMEM bank address");

        // Hardware word index: bit 10 selects the bank, bits 0..9 the word.
        require(Store::hwBank(0) == 0 && Store::hwBank(1023) == 0 &&
                Store::hwBank(1024) == 1 && Store::hwBank(2047) == 1,
                "TMEM hardware bank selection");
        require(Store::hwLogicalWord(1024) == 0 &&
                Store::hwLogicalWord(2047) == 1023,
                "TMEM hardware word masking");

        tmem.write16(3, 0x1234);
        tmem.write16(3, 0xabcd, 1);
        require(tmem.read16(3) == 0x1234 && tmem.read16(3, 1) == 0xabcd,
                "TMEM bank read/write");
        // readHwWord/writeHwWord route through the hardware index.
        tmem.writeHwWord(1024 + 3, 0xbeef);
        require(tmem.readHwWord(1024 + 3) == 0xbeef && tmem.read16(3, 1) == 0xbeef,
                "TMEM hardware word maps to bank 1");

        tmem.writeByte(4096, 0x5a);
        require(tmem.readByte(0) == 0x5a, "TMEM byte wrap");

        // copyBytes exposes the raw 4 KiB hardware image for reference diffs.
        std::array<uint8_t, 4096> snapshot{};
        tmem.copyBytes(snapshot);
        require(snapshot[0] == 0x5a, "TMEM copyBytes byte 0");
        // Bank 1 starts at byte 2048, so its word 3 lands at byte 2054.
        require(snapshot[Store::word16Address(3, 1)] == uint8_t(0xbeef) &&
                snapshot[Store::word16Address(3, 1) + 1] == uint8_t(0xbeef >> 8),
                "TMEM copyBytes bank-1 word");

        std::puts("PASS: 4 KiB TMEM banks and hardware word addressing");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
