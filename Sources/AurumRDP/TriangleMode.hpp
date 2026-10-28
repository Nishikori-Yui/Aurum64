// SPDX-License-Identifier: MIT
#pragma once

#include "RDP.hpp"

namespace aurum::rdp {
inline constexpr uint32_t kFlatPrimitiveCombineHi =
    (3u << 20) | (6u << 15) | (3u << 12) | (6u << 9);
inline constexpr uint32_t kFlatPrimitiveCombineLo =
    (8u << 28) | (8u << 24) | (6u << 18) | (7u << 15) |
    (7u << 12) | (7u << 9) | (7u << 6) | (7u << 3) | 7u;

inline uint16_t primitiveColor16(uint32_t color) {
    return uint16_t(((color >> 27) & 0x1fu) << 11) |
           uint16_t(((color >> 19) & 0x1fu) << 6) |
           uint16_t(((color >> 11) & 0x1fu) << 1) |
           uint16_t((color >> 7) & 1u);
}

inline bool isFlatPrimitiveTriangle(const Triangle& triangle) {
    return !triangle.shade && !triangle.texture && !triangle.z &&
           triangle.cycleType == 1 && triangle.fbFormat == 0 &&
           triangle.fbSize == 2 && triangle.scissorMode == 0 &&
           triangle.otherModesHi == 0x100000 && triangle.otherModesLo == 0 &&
           triangle.combineValid &&
           triangle.combineHi == kFlatPrimitiveCombineHi &&
           triangle.combineLo == kFlatPrimitiveCombineLo &&
           triangle.primitiveColorValid;
}
} // namespace aurum::rdp
