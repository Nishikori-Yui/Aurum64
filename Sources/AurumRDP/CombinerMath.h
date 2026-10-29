// SPDX-License-Identifier: MIT
#pragma once

#ifndef __METAL_VERSION__
#include <cstdint>
#endif

struct CombinerColor {
    ShadeUInt r = 0, g = 0, b = 0, a = 0;
};

inline ShadeInt combinerSignExtend9(ShadeUInt value) {
    const ShadeUInt bits = value & 0x1ffu;
    return (bits & 0x180u) == 0x180u ? ShadeInt(bits) - 512 : ShadeInt(bits);
}

inline ShadeUInt combinerClamp9(ShadeUInt value) {
    const ShadeUInt bits = value & 0x1ffu;
    const ShadeUInt range = bits >> 7;
    return range == 2u ? 255u : range == 3u ? 0u : bits & 0xffu;
}

inline ShadeUInt combinerRGBSource(ShadeUInt selector, ShadeUInt channel,
                                   CombinerColor combined, CombinerColor texel,
                                   CombinerColor shade, CombinerColor primitive) {
    switch (selector & 0xfu) {
    case 0: return channel == 0 ? combined.r : channel == 1 ? combined.g : channel == 2 ? combined.b : combined.a;
    case 1: return channel == 0 ? texel.r : channel == 1 ? texel.g : channel == 2 ? texel.b : texel.a;
    case 3: return channel == 0 ? primitive.r : channel == 1 ? primitive.g : channel == 2 ? primitive.b : primitive.a;
    case 4: return channel == 0 ? shade.r : channel == 1 ? shade.g : channel == 2 ? shade.b : shade.a;
    case 6: return 255u;
    default: return 0u;
    }
}

inline ShadeUInt combinerRGBMultiplier(ShadeUInt selector, ShadeUInt channel,
                                       CombinerColor combined, CombinerColor texel,
                                       CombinerColor shade, CombinerColor primitive) {
    switch (selector & 0x1fu) {
    case 0: return channel == 0 ? combined.r : channel == 1 ? combined.g : channel == 2 ? combined.b : combined.a;
    case 1: return channel == 0 ? texel.r : channel == 1 ? texel.g : channel == 2 ? texel.b : texel.a;
    case 3: return channel == 0 ? primitive.r : channel == 1 ? primitive.g : channel == 2 ? primitive.b : primitive.a;
    case 4: return channel == 0 ? shade.r : channel == 1 ? shade.g : channel == 2 ? shade.b : shade.a;
    case 7: return combined.a;
    case 8: return texel.a;
    case 10: return primitive.a;
    case 11: return shade.a;
    default: return 0u;
    }
}

inline ShadeUInt combinerRGBAddend(ShadeUInt selector, ShadeUInt channel,
                                   CombinerColor combined, CombinerColor texel,
                                   CombinerColor shade, CombinerColor primitive) {
    switch (selector & 7u) {
    case 0: return channel == 0 ? combined.r : channel == 1 ? combined.g : channel == 2 ? combined.b : combined.a;
    case 1: return channel == 0 ? texel.r : channel == 1 ? texel.g : channel == 2 ? texel.b : texel.a;
    case 3: return channel == 0 ? primitive.r : channel == 1 ? primitive.g : channel == 2 ? primitive.b : primitive.a;
    case 4: return channel == 0 ? shade.r : channel == 1 ? shade.g : channel == 2 ? shade.b : shade.a;
    case 6: return 255u;
    default: return 0u;
    }
}

inline ShadeUInt combinerAlphaSource(ShadeUInt selector, CombinerColor combined,
                                     CombinerColor texel, CombinerColor shade,
                                     CombinerColor primitive) {
    switch (selector & 7u) {
    case 0: return combined.a;
    case 1: return texel.a;
    case 3: return primitive.a;
    case 4: return shade.a;
    case 6: return 255u;
    default: return 0u;
    }
}

inline ShadeUInt combinerAlphaMultiplier(ShadeUInt selector, CombinerColor combined,
                                         CombinerColor texel, CombinerColor shade,
                                         CombinerColor primitive) {
    switch (selector & 7u) {
    case 0: return 0u; // LOD fraction is outside the C1 contract.
    case 1: return texel.a;
    case 3: return primitive.a;
    case 4: return shade.a;
    default: return 0u;
    }
}

inline ShadeUInt combinerRGBEquation(ShadeUInt a, ShadeUInt b, ShadeUInt c, ShadeUInt d) {
    const ShadeInt value = (combinerSignExtend9(a) - combinerSignExtend9(b)) *
        combinerSignExtend9(c) + (combinerSignExtend9(d) << 8) + 0x80;
    return combinerClamp9(ShadeUInt(value) >> 8);
}

inline ShadeUInt combinerAlphaEquation(ShadeUInt a, ShadeUInt b, ShadeUInt c, ShadeUInt d) {
    const ShadeInt value = ((combinerSignExtend9(a) - combinerSignExtend9(b)) *
        combinerSignExtend9(c) + (combinerSignExtend9(d) << 8) + 0x80) >> 8;
    return combinerClamp9(ShadeUInt(value));
}

inline bool combinerRGBSourceSupported(ShadeUInt selector, bool hasTexture, bool hasShade,
                                       bool hasPrimitive) {
    switch (selector & 0xfu) {
    case 1: return hasTexture;
    case 3: return hasPrimitive;
    case 4: return hasShade;
    case 6:
    case 15: return true;
    default: return false;
    }
}

inline bool combinerRGBMultiplierSupported(ShadeUInt selector, bool hasTexture, bool hasShade,
                                           bool hasPrimitive) {
    switch (selector & 0x1fu) {
    case 1: return hasTexture;
    case 3: return hasPrimitive;
    case 4: return hasShade;
    case 7: return true;
    case 8: return hasTexture;
    case 10: return hasPrimitive;
    case 11: return hasShade;
    case 31: return true;
    default: return false;
    }
}

inline bool combinerRGBAddendSupported(ShadeUInt selector, bool hasTexture, bool hasShade,
                                       bool hasPrimitive) {
    switch (selector & 7u) {
    case 1: return hasTexture;
    case 3: return hasPrimitive;
    case 4: return hasShade;
    case 6:
    case 7: return true;
    default: return false;
    }
}

inline bool combinerAlphaSourceSupported(ShadeUInt selector, bool hasTexture, bool hasShade,
                                         bool hasPrimitive) {
    switch (selector & 7u) {
    case 1: return hasTexture;
    case 3: return hasPrimitive;
    case 4: return hasShade;
    case 6:
    case 7: return true;
    default: return false;
    }
}

inline bool combinerAlphaMultiplierSupported(ShadeUInt selector, bool hasTexture, bool hasShade,
                                             bool hasPrimitive) {
    switch (selector & 7u) {
    case 1: return hasTexture;
    case 3: return hasPrimitive;
    case 4: return hasShade;
    case 7: return true;
    default: return false;
    }
}

inline bool simpleCombinerSupported(ShadeUInt hi, ShadeUInt lo, bool hasTexture,
                                   bool hasShade, bool hasPrimitive) {
    const ShadeUInt aRGB = hi >> 5;
    const ShadeUInt cRGB = hi & 0x1fu;
    const ShadeUInt bRGB = lo >> 24;
    const ShadeUInt dRGB = lo >> 6;
    const ShadeUInt aAlpha = lo >> 21;
    const ShadeUInt cAlpha = lo >> 18;
    const ShadeUInt bAlpha = lo >> 3;
    const ShadeUInt dAlpha = lo;
    return combinerRGBSourceSupported(aRGB, hasTexture, hasShade, hasPrimitive) &&
           combinerRGBSourceSupported(bRGB, hasTexture, hasShade, hasPrimitive) &&
           combinerRGBMultiplierSupported(cRGB, hasTexture, hasShade, hasPrimitive) &&
           combinerRGBAddendSupported(dRGB, hasTexture, hasShade, hasPrimitive) &&
           combinerAlphaSourceSupported(aAlpha, hasTexture, hasShade, hasPrimitive) &&
           combinerAlphaSourceSupported(bAlpha, hasTexture, hasShade, hasPrimitive) &&
           combinerAlphaMultiplierSupported(cAlpha, hasTexture, hasShade, hasPrimitive) &&
           combinerAlphaSourceSupported(dAlpha, hasTexture, hasShade, hasPrimitive);
}

inline CombinerColor simpleCombinerColor(ShadeUInt hi, ShadeUInt lo,
                                         CombinerColor texel, CombinerColor shade,
                                         CombinerColor primitive) {
    const CombinerColor combined{};
    const ShadeUInt aRGB = hi >> 5;
    const ShadeUInt cRGB = hi & 0x1fu;
    const ShadeUInt bRGB = lo >> 24;
    const ShadeUInt dRGB = lo >> 6;
    const ShadeUInt aAlpha = lo >> 21;
    const ShadeUInt cAlpha = lo >> 18;
    const ShadeUInt bAlpha = lo >> 3;
    const ShadeUInt dAlpha = lo;
    CombinerColor result{};
    result.r = combinerRGBEquation(combinerRGBSource(aRGB, 0, combined, texel, shade, primitive),
                                    combinerRGBSource(bRGB, 0, combined, texel, shade, primitive),
                                    combinerRGBMultiplier(cRGB, 0, combined, texel, shade, primitive),
                                    combinerRGBAddend(dRGB, 0, combined, texel, shade, primitive));
    result.g = combinerRGBEquation(combinerRGBSource(aRGB, 1, combined, texel, shade, primitive),
                                    combinerRGBSource(bRGB, 1, combined, texel, shade, primitive),
                                    combinerRGBMultiplier(cRGB, 1, combined, texel, shade, primitive),
                                    combinerRGBAddend(dRGB, 1, combined, texel, shade, primitive));
    result.b = combinerRGBEquation(combinerRGBSource(aRGB, 2, combined, texel, shade, primitive),
                                    combinerRGBSource(bRGB, 2, combined, texel, shade, primitive),
                                    combinerRGBMultiplier(cRGB, 2, combined, texel, shade, primitive),
                                    combinerRGBAddend(dRGB, 2, combined, texel, shade, primitive));
    result.a = combinerAlphaEquation(combinerAlphaSource(aAlpha, combined, texel, shade, primitive),
                                      combinerAlphaSource(bAlpha, combined, texel, shade, primitive),
                                      combinerAlphaMultiplier(cAlpha, combined, texel, shade, primitive),
                                      combinerAlphaSource(dAlpha, combined, texel, shade, primitive));
    return result;
}

inline ShadeUInt simpleCombinerRGBA16(ShadeUInt hi, ShadeUInt lo,
                                      CombinerColor texel, CombinerColor shade,
                                      CombinerColor primitive) {
    const CombinerColor result = simpleCombinerColor(hi, lo, texel, shade, primitive);
    return ((result.r >> 3) << 11) | ((result.g >> 3) << 6) | ((result.b >> 3) << 1);
}
