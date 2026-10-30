// SPDX-License-Identifier: MIT
#pragma once
#include "ShadeMath.h"

// Bit 2 marks an untouched halfword. Resolve it from the current RDRAM low
// bit on reads; explicit RDP writes instead store the two physical bits.
enum : ShadeUInt { kHiddenUnwritten = 4 };

inline ShadeUInt hiddenRead(ShadeUInt entry, ShadeUInt halfword) {
    return (entry & kHiddenUnwritten) ? ((halfword & 1u) ? 3u : 0u) : entry & 3u;
}

// A GPU invocation owns both hidden bytes corresponding to one RDRAM word.
// Hidden byte order is guest halfword order, not host word-swapped order.
inline ShadeUInt hiddenWriteLane(ShadeUInt pair, ShadeUInt lane, ShadeUInt bits) {
    const ShadeUInt shift = lane * 8u;
    return (pair & ~(0xffu << shift)) | ((bits & 3u) << shift);
}
