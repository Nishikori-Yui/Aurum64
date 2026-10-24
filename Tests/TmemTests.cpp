// SPDX-License-Identifier: MIT
#include "Tmem.hpp"
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
        require(Store::tlutAddress(0) == 2048 &&
                Store::tlutAddress(511) == 3070,
                "TLUT window address");
        tmem.write16(3, 0x1234);
        tmem.write16(3, 0xabcd, 1);
        require(tmem.read16(3) == 0x1234 && tmem.read16(3, 1) == 0xabcd,
                "TMEM bank read/write");
        tmem.writeByte(4096, 0x5a);
        require(tmem.readByte(0) == 0x5a, "TMEM byte wrap");
        std::puts("PASS: 4 KiB TMEM banks and TLUT window");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
