// SPDX-License-Identifier: MIT
#include "RDP.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace aurum::rdp {
std::array<uint32_t, 64> StateSnapshot::words() const {
    std::array<uint32_t, 64> result{};
    result[0] = fbFormat; result[1] = fbSize; result[2] = fbWidth; result[3] = fbAddress;
    result[4] = fillColor; result[5] = clipXh; result[6] = clipYh; result[7] = clipXl;
    result[8] = clipYl; result[9] = cycleType; result[10] = imageReadEn;
    result[11] = zUpdateEn; result[12] = zCompareEn; result[13] = tiFormat;
    result[14] = tiSize; result[15] = tiWidth; result[16] = tiAddress; result[17] = zbAddress;
    for (size_t i = 0; i < tiles.size(); ++i) result[18 + i] = tiles[i];
    result[63] = kVersion;
    return result;
}

StateSnapshot StateSnapshot::fromWords(const std::array<uint32_t, 64>& words) {
    StateSnapshot result;
    result.fbFormat = words[0]; result.fbSize = words[1]; result.fbWidth = words[2];
    result.fbAddress = words[3]; result.fillColor = words[4]; result.clipXh = words[5];
    result.clipYh = words[6]; result.clipXl = words[7]; result.clipYl = words[8];
    result.cycleType = words[9]; result.imageReadEn = words[10]; result.zUpdateEn = words[11];
    result.zCompareEn = words[12]; result.tiFormat = words[13]; result.tiSize = words[14];
    result.tiWidth = words[15]; result.tiAddress = words[16]; result.zbAddress = words[17];
    for (size_t i = 0; i < result.tiles.size(); ++i) result.tiles[i] = words[18 + i];
    return result;
}

size_t Decoder::wordCount(uint8_t op) {
    // Keep this table aligned with Angrylion's 64-entry command dispatch table.
    static constexpr std::array<uint8_t, 64> words = {
        2, 2, 2, 2, 2, 2, 2, 2,
        8, 12, 24, 28, 24, 28, 40, 44,
        2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 4, 4, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2
    };
    return words[op & 63];
}
void Decoder::reset() {
    pending.clear(); commands = unsupported = 0;
    opcodeCounts.fill(0);
    firstCommands.fill(std::numeric_limits<uint64_t>::max());
    firstSubmissions.fill(std::numeric_limits<uint64_t>::max());
    firstVIs.fill(std::numeric_limits<uint64_t>::max());
    submissions = 0; viEvents = 0; currentVI = 1;
    commandBaseline = unsupportedBaseline = 0;
    recentCommandCount = recentUnsupportedCount = 0;
    unsupportedContext = {};
    address = width = size = format = color = cycle = 0;
    imageReadEn = zUpdateEn = zCompareEn = 0;
    tiFormat = tiSize = tiWidth = tiAddress = zbAddress = 0;
    tiles.fill(0);
    tileSizes_.fill(0);
    tileModes_.fill(0);
    sx0 = sy0 = sx1 = sy1 = 0;
    scissorMode = otherModesHi = otherModesLo = 0;
    haveState = false;
}
uint64_t Decoder::opcodeCount(uint8_t opcode) const {
    return opcode < opcodeCounts.size() ? opcodeCounts[opcode] : 0;
}
uint64_t Decoder::firstCommand(uint8_t opcode) const {
    return opcode < firstCommands.size() ? firstCommands[opcode] : std::numeric_limits<uint64_t>::max();
}
uint64_t Decoder::firstSubmission(uint8_t opcode) const {
    return opcode < firstSubmissions.size() ? firstSubmissions[opcode] : std::numeric_limits<uint64_t>::max();
}
uint64_t Decoder::firstVI(uint8_t opcode) const {
    return opcode < firstVIs.size() ? firstVIs[opcode] : std::numeric_limits<uint64_t>::max();
}
void Decoder::beginSubmission() { ++submissions; }
void Decoder::beginVI() {
    recentCommandCount = commands - commandBaseline;
    recentUnsupportedCount = unsupported - unsupportedBaseline;
    commandBaseline = commands;
    unsupportedBaseline = unsupported;
    ++viEvents;
    currentVI = viEvents + 1;
}
void Decoder::markUnsupported(std::span<const uint32_t> words) {
    ++unsupported;
    if (unsupportedContext.valid) return;
    unsupportedContext.valid = 1;
    unsupportedContext.opcode = (words[0] >> 24) & 63;
    unsupportedContext.wordCount = static_cast<uint32_t>(words.size());
    unsupportedContext.capturedWords = static_cast<uint32_t>(std::min<size_t>(words.size(), 32));
    unsupportedContext.command = commands - 1;
    unsupportedContext.submission = submissions;
    unsupportedContext.vi = currentVI;
    std::copy_n(words.begin(), unsupportedContext.capturedWords, unsupportedContext.words);
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
    if (firstSubmissions[opcode] == std::numeric_limits<uint64_t>::max()) firstSubmissions[opcode] = submissions;
    if (firstVIs[opcode] == std::numeric_limits<uint64_t>::max()) firstVIs[opcode] = currentVI;
    ++opcodeCounts[opcode];
    ++commands;
    const uint32_t a = w[0], b = w[1];
    switch (opcode) {
    case 0x00: case 0x26: case 0x27: case 0x28: break;
    case 0x29: if (sync) sync(); break;
    case 0x2d:
        sx0 = (a >> 12) & 4095; sy0 = a & 4095;
        sx1 = (b >> 12) & 4095; sy1 = b & 4095;
        scissorMode = (b >> 24) & 3;
        // Field/interlace scissoring is not implemented.
        if (b & 0x03000000) markUnsupported(w);
        haveState = true;
        break;
    case 0x2f:
        otherModesHi = a & 0xffffff;
        otherModesLo = b;
        cycle = (a >> 20) & 3;
        imageReadEn = (b >> 6) & 1;
        zUpdateEn = (b >> 5) & 1;
        zCompareEn = (b >> 4) & 1;
        break;
    case 0x32: {
        const uint32_t tile = (b >> 24) & 7;
        tileSizes_[tile * 4 + 0] = (a >> 12) & 0xfff;
        tileSizes_[tile * 4 + 1] = a & 0xfff;
        tileSizes_[tile * 4 + 2] = (b >> 12) & 0xfff;
        tileSizes_[tile * 4 + 3] = b & 0xfff;
        break;
    }
    case 0x35: {
        const uint32_t tile = (b >> 24) & 7;
        tiles[tile * 4 + 0] = (a >> 21) & 7;
        tiles[tile * 4 + 1] = (a >> 19) & 3;
        tiles[tile * 4 + 2] = (a >> 9) & 0x1ff;
        tiles[tile * 4 + 3] = a & 0x1ff;
        tileModes_[tile] = b & 0xffffff;
        break;
    }
    case 0x30:
    case 0x33:
    case 0x34: {
        // LoadTLUT, LoadBlock and LoadTile. The tile descriptor from SetTile
        // and the SetTextureImage state are carried along so the handler can
        // resolve the source layout and TMEM destination.
        const uint32_t tile = (b >> 24) & 7;
        TextureLoad load;
        load.opcode = opcode;
        load.tile = tile;
        load.tileFormat = tiles[tile * 4 + 0];
        load.tileSize = tiles[tile * 4 + 1];
        load.tileLine = tiles[tile * 4 + 2];
        load.tileTmem = tiles[tile * 4 + 3];
        load.tiFormat = tiFormat; load.tiSize = tiSize;
        load.tiWidth = tiWidth; load.tiAddress = tiAddress;
        load.sl = (a >> 12) & 0xfff;
        load.tl = a & 0xfff;
        load.sh = (b >> 12) & 0xfff;
        load.th = b & 0xfff;
        // LoadTile/LoadBlock/LoadTLUT also replace this tile's bounds, even
        // when the upload handler is unavailable. Later draws see that state.
        tileSizes_[tile * 4 + 0] = load.sl;
        tileSizes_[tile * 4 + 1] = load.tl;
        tileSizes_[tile * 4 + 2] = load.sh;
        tileSizes_[tile * 4 + 3] = load.th;
        if (opcode == 0x33) load.dxt = load.th;
        if (!textureLoad || !textureLoad(load)) markUnsupported(w);
        break;
    }
    case 0x24:
    case 0x25: {
        TextureRect rect;
        rect.flip = (opcode == 0x25);
        rect.tile = (b >> 24) & 7;
        rect.xl = (a >> 12) & 0xfff;
        rect.yl = a & 0xfff;
        rect.xh = (b >> 12) & 0xfff;
        rect.yh = b & 0xfff;
        rect.s = int16_t((w[2] >> 16) & 0xffff);
        rect.t = int16_t(w[2] & 0xffff);
        rect.dsdx = int16_t((w[3] >> 16) & 0xffff);
        rect.dtdy = int16_t(w[3] & 0xffff);
        rect.cycleType = cycle;
        rect.fbFormat = format; rect.fbSize = size;
        rect.fbWidth = width; rect.fbAddress = address;
        rect.tileFormat = tiles[rect.tile * 4 + 0];
        rect.tileSize = tiles[rect.tile * 4 + 1];
        rect.tileLine = tiles[rect.tile * 4 + 2];
        rect.tileTmem = tiles[rect.tile * 4 + 3];
        rect.tileSl = tileSizes_[rect.tile * 4 + 0];
        rect.tileTl = tileSizes_[rect.tile * 4 + 1];
        rect.tileSh = tileSizes_[rect.tile * 4 + 2];
        rect.tileTh = tileSizes_[rect.tile * 4 + 3];
        rect.tileModes = tileModes_[rect.tile];
        rect.clipXh = sx0; rect.clipYh = sy0; rect.clipXl = sx1; rect.clipYl = sy1;
        rect.scissorMode = scissorMode;
        rect.otherModesHi = otherModesHi; rect.otherModesLo = otherModesLo;
        if (!textureRect || !textureRect(rect)) markUnsupported(w);
        break;
    }
    case 0x37: color = b; break;
    case 0x3d:
        tiFormat = (a >> 21) & 7; tiSize = (a >> 19) & 3;
        tiWidth = (a & 0x3ff) + 1; tiAddress = b & 0x0ffffff;
        break;
    case 0x3e: zbAddress = b & 0x0ffffff; break;
    case 0x3f:
        format = (a >> 21) & 7; size = (a >> 19) & 3;
        width = (a & 1023) + 1; address = b & 0x03ffffff;
        haveState = true;
        break;
    case 0x36: {
        if (cycle != 3 || format != 0 || (size != 2 && size != 3)) {
            markUnsupported(w); break;
        }
        // Fill-cycle lower-right bounds are inclusive; scissor bounds are exclusive.
        const uint32_t x0 = std::max(((b >> 12) & 4095) >> 2, sx0 >> 2);
        const uint32_t y0 = std::max((b & 4095) >> 2, sy0 >> 2);
        const uint32_t x1 = std::min({(((a >> 12) & 4095) >> 2) + 1, sx1 >> 2, width});
        const uint32_t y1 = std::min(((a & 4095) >> 2) + 1, sy1 >> 2);
        if (x0 < x1 && y0 < y1 && fill) fill({address, width, size, color, x0, y0, x1, y1});
        break;
    }
    default: markUnsupported(w); break;
    }
}
}
