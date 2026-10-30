// SPDX-License-Identifier: MIT
#include "Renderer.hpp"
#include "HiddenMath.h"
#include "MemoryBus.hpp"
#include "TriangleDepth.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

using namespace aurum::rdp;
static void require(bool ok, const char* reason) {
    if (!ok) throw std::runtime_error(reason);
}

int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: aurum-hidden-tests metallib");
        Renderer renderer;
        std::string error;
        require(renderer.initialize(argv[1], error), error.c_str());
        require(hiddenRead(4, 0) == 0 && hiddenRead(4, 1) == 3 &&
                hiddenRead(2, 1) == 2 && hiddenWriteLane(0x0404, 0, 1) == 0x0401 &&
                hiddenWriteLane(0x0404, 1, 2) == 0x0204, "Hidden encoding controls");
        for (size_t bytes : {4u << 20, 8u << 20}) {
            std::vector<uint8_t> ram(bytes), actual, expected(bytes / 2, 4);
            renderer.resetHidden();
            const auto check = [&] {
                require(renderer.copyHidden(bytes, actual), "Hidden state unavailable");
                require(actual == expected, "Whole hidden memory mismatch");
            };
            check();
            // Odd stride/base crosses host-word lanes and leaves adjacent data intact.
            Fill fill{0x1002, 3, 2, 0x12345679, 0, 0, 3, 2};
            require(renderer.fill(ram.data(), bytes, fill, error), error.c_str());
            for (unsigned i = 0; i < 6; ++i) {
                const unsigned h = fill.address / 2 + i;
                expected[h] = h & 1u ? 3 : 0;
            }
            check();
            fill = {0x2000, 2, 3, 0x80010000, 0, 0, 2, 1};
            require(renderer.fill(ram.data(), bytes, fill, error), error.c_str());
            for (unsigned i = 0; i < 4; ++i) expected[0x1000 + i] = i & 1 ? 0 : 3;
            check();
            // External RDRAM stores do not erase explicit RDP hidden writes.
            require(aurum::memory::writeHalfword(ram.data(), bytes, 0x1002, 0), "CPU store");
            check();
            Triangle t;
            t.leftMajor = true; t.yl = t.ym = 4;
            t.xh = 0; t.xm = t.xl = 1 << 16;
            t.fbSize = 2; t.fbWidth = 8; t.fbAddress = 0x4000; t.zbAddress = 0x5000;
            t.clipXl = 4; t.clipYl = 1; // Two samples; high bit zero, hidden bits one.
            t.otherModesHi = kFlatPrimitiveModesHi; t.otherModesLo = 0x24;
            t.combineHi = kFlatPrimitiveCombineHi; t.combineLo = kFlatPrimitiveCombineLo;
            t.combineValid = t.primitiveColorValid = t.primitiveDepthValid = true;
            t.primitiveColor = 0xff0000ff; t.primitiveDepth = 0x10000002;
            require(renderer.depthTriangle(ram.data(), bytes, t, error), error.c_str());
            expected[t.fbAddress / 2] = 1;
            expected[t.zbAddress / 2] = 1; // log2(2) low bits.
            check();
            // A subsequent primitive must update the other byte without losing the first.
            t.fbAddress += 2; t.zbAddress += 2; t.primitiveDepth = 0x10000004;
            require(renderer.depthTriangle(ram.data(), bytes, t, error), error.c_str());
            expected[t.fbAddress / 2] = 1; expected[t.zbAddress / 2] = 2;
            check();
            const auto before = ram;
            t.zbAddress = t.fbAddress;
            require(!renderer.depthTriangle(ram.data(), bytes, t, error) && ram == before,
                    "Invalid depth draw modified RDRAM");
            check();
            t.otherModesLo = 1; t.blendColorValid = true; t.blendColor = 255;
            t.primitiveColor = 0; t.fbAddress = 0x4000;
            aurum::tmem::Store texture;
            require(renderer.combinerTriangle(ram.data(), bytes, t, texture, error), error.c_str());
            check(); // Alpha rejection preserves explicit hidden coverage.
            TextureRect copy;
            copy.cycleType = 2; copy.otherModesHi = 2u << 20;
            copy.fbSize = copy.tileSize = 2; copy.fbWidth = 8; copy.fbAddress = 0x6000;
            copy.tileLine = 1; copy.xl = 12; copy.yl = 0; copy.clipXl = 32; copy.clipYl = 4;
            copy.dsdx = 4096; copy.dtdy = 1024;
            require(renderer.textureCopy(ram.data(), bytes, copy, texture, error), error.c_str());
            require(!renderer.copyHidden(bytes, actual) && actual.empty(),
                    "Unmodeled copy hidden writes reported as valid");
            renderer.resetHidden(); expected.assign(bytes / 2, 4); check();
        }
        std::puts("PASS: persistent hidden lanes, fill/depth, alpha rejection, external stores and reset");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
