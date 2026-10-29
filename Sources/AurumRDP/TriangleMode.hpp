// SPDX-License-Identifier: MIT
#pragma once

#include "RDP.hpp"

namespace aurum::rdp {
// (0 - 0) * 0 + PRIMITIVE, for both RGB and alpha in both cycles.
inline constexpr uint32_t kFlatPrimitiveCombineHi = 0x00ffffffu;
inline constexpr uint32_t kFlatPrimitiveCombineLo = 0xfffdf6fbu;
// One-cycle encoding is zero; both RGB and alpha dithering are disabled.
inline constexpr uint32_t kFlatPrimitiveModesHi = 0x000000f0u;
inline constexpr uint32_t kShadeCombineLo = 0xfffe793cu;
inline constexpr uint32_t kTextureCombineLo = 0xfffcf279u;

inline uint16_t primitiveColor16(uint32_t color) {
    return uint16_t(((color >> 27) & 0x1fu) << 11) |
           uint16_t(((color >> 19) & 0x1fu) << 6) |
           uint16_t(((color >> 11) & 0x1fu) << 1);
}

inline bool isFlatPrimitiveTriangle(const Triangle& triangle) {
    return !triangle.shade && !triangle.texture && !triangle.z &&
           triangle.cycleType == 0 && triangle.fbFormat == 0 &&
           triangle.fbSize == 2 && triangle.scissorMode == 0 &&
           triangle.otherModesHi == kFlatPrimitiveModesHi && triangle.otherModesLo == 0 &&
           triangle.combineValid &&
           triangle.combineHi == kFlatPrimitiveCombineHi &&
           triangle.combineLo == kFlatPrimitiveCombineLo &&
           triangle.primitiveColorValid;
}

inline bool isShadeTriangle(const Triangle& triangle) {
    return triangle.shade && !triangle.texture && !triangle.z &&
           triangle.cycleType == 0 && triangle.fbFormat == 0 && triangle.fbSize == 2 &&
           triangle.scissorMode == 0 && triangle.otherModesHi == kFlatPrimitiveModesHi &&
           triangle.otherModesLo == 0 && triangle.combineValid &&
           triangle.combineHi == kFlatPrimitiveCombineHi && triangle.combineLo == kShadeCombineLo;
}

inline bool isColorTriangle(const Triangle& triangle) {
    return isFlatPrimitiveTriangle(triangle) || isShadeTriangle(triangle);
}

inline bool isTexturedTriangle(const Triangle& triangle) {
    // sample_type=0 with bi_lerp0=1 is direct point fetch, not three-point filtering.
    const uint32_t modes = triangle.otherModesHi & ~(1u << 19);
    return triangle.texture && !triangle.shade && !triangle.z &&
           triangle.cycleType == 0 && triangle.fbFormat == 0 && triangle.fbSize == 2 &&
           triangle.scissorMode == 0 && modes == 0x8f0u && triangle.otherModesLo == 0 &&
           triangle.combineValid && triangle.combineHi == kFlatPrimitiveCombineHi &&
           triangle.combineLo == kTextureCombineLo && triangle.tileFormat == 0 && triangle.tileSize == 2;
}
} // namespace aurum::rdp
