// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>

namespace aurum::texture {

struct AxisCoordinates {
    uint32_t lower = 0;   // SetTileSize 10.2 coordinate
    uint32_t upper = 0;   // SetTileSize 10.2 coordinate
    uint32_t shift = 0;
    uint32_t mask = 0;
    bool mirror = false;
    bool clamp = false;
};

inline int32_t signedCoordinate(uint32_t bits) {
    return int32_t((bits & 0xffffu) ^ 0x8000u) - 0x8000;
}

inline int32_t shiftCoordinate(int32_t input, uint32_t rawShift) {
    const uint32_t shift = rawShift & 15u;
    if (shift <= 10) return signedCoordinate(uint32_t(input)) >> shift;
    // Shift unsigned bits, then truncate/sign-extend the hardware's 16-bit bus.
    return signedCoordinate(uint32_t(input) << (16 - shift));
}

inline int32_t maskCoordinate(int32_t coordinate, const AxisCoordinates& axis) {
    const uint32_t mask = axis.mask & 15u;
    if (mask != 0) {
        const uint32_t wrapShift = std::min(mask, 10u);
        if (axis.mirror && ((coordinate >> wrapShift) & 1)) coordinate = ~coordinate;
        const uint32_t maskBits = (1u << std::min(mask, 10u)) - 1;
        coordinate &= static_cast<int32_t>(maskBits);
    }
    return coordinate;
}

// Point-cycle mapping consumes signed 16-bit coordinates with five fraction
// bits. Tile bounds retain the raw unsigned 10.2 SetTileSize fields.
inline int32_t mapPointCoordinate(int32_t input, const AxisCoordinates& axis) {
    const int32_t shifted = shiftCoordinate(input, axis.shift);
    const uint32_t lower = axis.lower & 0xfffu, upper = axis.upper & 0xfffu;
    const int32_t relative = shifted - int32_t(lower * 8);
    int32_t texel = relative >> 5;
    if (axis.clamp || (axis.mask & 15u) == 0) {
        if ((shifted >> 3) >= int32_t(upper))
            texel = int32_t(((upper >> 2) - (lower >> 2)) & 0x3ffu);
        else if (relative & 0x10000)
            texel = 0;
    }
    return maskCoordinate(texel, axis);
}

struct CopyCoordinates {
    std::array<int32_t, 4> s{};
    int32_t t = 0;
};

// Copy-cycle coordinates bypass clamp. The four S lanes are incremented after
// shift/origin conversion and individually masked; re-shifting each lane would
// give the wrong samples for nonzero tile shifts.
inline CopyCoordinates mapCopyCoordinates(int32_t s, int32_t t,
                                          const AxisCoordinates& sAxis,
                                          const AxisCoordinates& tAxis) {
    CopyCoordinates out;
    const int32_t first = (shiftCoordinate(s, sAxis.shift) -
                          int32_t((sAxis.lower & 0xfffu) * 8)) >> 5;
    for (int32_t lane = 0; lane < 4; ++lane)
        out.s[lane] = maskCoordinate(first + lane, sAxis);
    out.t = maskCoordinate((shiftCoordinate(t, tAxis.shift) -
                           int32_t((tAxis.lower & 0xfffu) * 8)) >> 5, tAxis);
    return out;
}

} // namespace aurum::texture
