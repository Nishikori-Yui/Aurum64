// SPDX-License-Identifier: MIT
#include "RDP.hpp"
#include <algorithm>
#include <limits>

namespace aurum::rdp {
size_t Decoder::wordCount(uint8_t op) {
    if (op >= 8 && op <= 15) {
        return 8 + ((op & 4) ? 16 : 0) + ((op & 2) ? 16 : 0) + ((op & 1) ? 4 : 0);
    }
    return (op == 0x24 || op == 0x25) ? 4 : 2;
}
void Decoder::reset() {
    pending.clear(); commands = unsupported = 0;
    opcodeCounts.fill(0);
    firstCommands.fill(std::numeric_limits<uint64_t>::max());
    address = width = size = format = color = cycle = 0;
    sx0 = sy0 = sx1 = sy1 = 0;
}
uint64_t Decoder::opcodeCount(uint8_t opcode) const {
    return opcode < opcodeCounts.size() ? opcodeCounts[opcode] : 0;
}
uint64_t Decoder::firstCommand(uint8_t opcode) const {
    return opcode < firstCommands.size() ? firstCommands[opcode] : std::numeric_limits<uint64_t>::max();
}
void Decoder::push(std::span<const uint32_t> words) {
    pending.insert(pending.end(), words.begin(), words.end());
    size_t used = 0;
    while (used < pending.size()) {
        const size_t length = wordCount((pending[used] >> 24) & 63);
        if (pending.size() - used < length) break;
        execute(std::span(pending).subspan(used, length));
        used += length;
    }
    pending.erase(pending.begin(), pending.begin() + used);
}
void Decoder::execute(std::span<const uint32_t> w) {
    const uint8_t opcode = (w[0] >> 24) & 63;
    if (firstCommands[opcode] == std::numeric_limits<uint64_t>::max()) firstCommands[opcode] = commands;
    ++opcodeCounts[opcode];
    ++commands;
    const uint32_t a = w[0], b = w[1];
    switch (opcode) {
    case 0x00: case 0x26: case 0x27: case 0x28: break;
    case 0x29: if (sync) sync(); break;
    case 0x2d:
        sx0 = (a >> 12) & 4095; sy0 = a & 4095;
        sx1 = (b >> 12) & 4095; sy1 = b & 4095;
        // Field/interlace scissoring is not implemented.
        if (b & 0x03000000) ++unsupported;
        break;
    case 0x2f: cycle = (a >> 20) & 3; break;
    case 0x37: color = b; break;
    case 0x3f:
        format = (a >> 21) & 7; size = (a >> 19) & 3;
        width = (a & 1023) + 1; address = b & 0x03ffffff;
        break;
    case 0x36: {
        if (cycle != 3 || format != 0 || (size != 2 && size != 3)) {
            ++unsupported; break;
        }
        // Fill-cycle lower-right bounds are inclusive; scissor bounds are exclusive.
        const uint32_t x0 = std::max(((b >> 12) & 4095) >> 2, (sx0 + 3) >> 2);
        const uint32_t y0 = std::max((b & 4095) >> 2, (sy0 + 3) >> 2);
        const uint32_t x1 = std::min({(((a >> 12) & 4095) >> 2) + 1, sx1 >> 2, width});
        const uint32_t y1 = std::min(((a & 4095) >> 2) + 1, sy1 >> 2);
        if (x0 < x1 && y0 < y1 && fill) fill({address, width, size, color, x0, y0, x1, y1});
        break;
    }
    default: ++unsupported; break;
    }
}
}
