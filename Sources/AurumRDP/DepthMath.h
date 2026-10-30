// SPDX-License-Identifier: MIT
#pragma once

#ifndef __METAL_VERSION__
#include <cstdint>
#endif

#include "ShadeMath.h"

inline ShadeUInt depthCompress(ShadeUInt value) {
    const ShadeUInt bucket = (value >> 11) & 0x7fu;
    if (bucket <= 0x3fu) return (value >> 4) & 0x1ffcu;
    if (bucket <= 0x5fu) return ((value >> 3) & 0x1ffcu) | 0x2000u;
    if (bucket <= 0x6fu) return ((value >> 2) & 0x1ffcu) | 0x4000u;
    if (bucket <= 0x77u) return ((value >> 1) & 0x1ffcu) | 0x6000u;
    if (bucket <= 0x7bu) return (value & 0x1ffcu) | 0x8000u;
    if (bucket <= 0x7du) return ((value << 1) & 0x1ffcu) | 0xa000u;
    if (bucket == 0x7eu) return ((value << 2) & 0x1ffcu) | 0xc000u;
    return ((value << 2) & 0x1ffcu) | 0xe000u;
}

inline ShadeUInt depthDecompress(ShadeUInt stored) {
    const ShadeUInt index = (stored >> 2) & 0x3fffu;
    const ShadeUInt exponent = (index >> 11) & 7u;
    const ShadeUInt mantissa = index & 0x7ffu;
    switch (exponent) {
    case 0: return (mantissa << 6) & 0x3ffffu;
    case 1: return ((mantissa << 5) + 0x20000u) & 0x3ffffu;
    case 2: return ((mantissa << 4) + 0x30000u) & 0x3ffffu;
    case 3: return ((mantissa << 3) + 0x38000u) & 0x3ffffu;
    case 4: return ((mantissa << 2) + 0x3c000u) & 0x3ffffu;
    case 5: return ((mantissa << 1) + 0x3e000u) & 0x3ffffu;
    case 6: return (mantissa + 0x3f000u) & 0x3ffffu;
    default: return (mantissa + 0x3f800u) & 0x3ffffu;
    }
}

inline ShadeUInt depthDeltaCompress(ShadeUInt value) {
    ShadeUInt result = 0;
    if (value & 0xff00u) result |= 8u;
    if (value & 0xf0f0u) result |= 4u;
    if (value & 0xccccu) result |= 2u;
    if (value & 0xaaaau) result |= 1u;
    return result;
}

inline ShadeUInt depthDeltaDecompress(ShadeUInt value) {
    return 1u << (value & 15u);
}

inline ShadeUInt depthDeltaComparator(ShadeUInt value) {
    if (!value) return 0;
    ShadeUInt result = 1;
    for (unsigned bit = 1; bit < 16; ++bit)
        if (value & (1u << bit)) result = 1u << bit;
    return result;
}

inline ShadeUInt depthPixelDelta(ShadeInt dx, ShadeInt dy) {
    const auto magnitude = [](ShadeInt derivative) {
        const ShadeUInt high = (ShadeUInt(derivative) >> 16) & 0xffffu;
        return (high & 0x8000u) ? (~high & 0x7fffu) : high;
    };
    const ShadeUInt sum = (magnitude(dx) + magnitude(dy)) & 0xffffu;
    if (sum & 0xc000u) return 0x8000u;
    if (!sum) return 1;
    if (sum == 1) return 3;
    return depthDeltaComparator(sum) << 1;
}

inline bool depthOpaquePass(ShadeUInt incoming, ShadeUInt stored,
                            ShadeUInt rawDzMem, ShadeUInt dzPixel,
                            bool coverageOverflow) {
    const ShadeUInt oz = depthDecompress(stored);
    ShadeUInt dzmem = depthDeltaDecompress(rawDzMem);
    bool forceCoplanar = false;
    const ShadeUInt precision = (stored >> 13) & 15u;
    if (precision < 3u) {
        if (dzmem != 0x8000u) {
            const ShadeUInt modifier = 16u >> precision;
            dzmem <<= 1;
            if (dzmem < modifier) dzmem = modifier;
        } else {
            forceCoplanar = true;
            dzmem = 0xffffu;
        }
    }
    const ShadeUInt dznew = depthDeltaComparator(dzPixel | dzmem) << 3;
    const bool max = oz == 0x3ffffu;
    const ShadeInt diff = ShadeInt(incoming) - ShadeInt(dznew);
    const bool nearer = forceCoplanar || diff <= ShadeInt(oz);
    // Full accumulated coverage requires strict front ordering; only edges
    // without overflow use the delta-Z tolerance (including coplanar forcing).
    return max || (coverageOverflow ? incoming < oz : nearer);
}
