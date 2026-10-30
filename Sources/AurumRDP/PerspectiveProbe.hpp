// SPDX-License-Identifier: MIT
#pragma once
#include "TextureCoordinateProbe.hpp"

namespace aurum::perspective_probe {
using aurum::coordinate_probe::readMagic;
using aurum::coordinate_probe::writeMagic;
using aurum::coordinate_probe::readWords;
using aurum::coordinate_probe::writeWords;
constexpr size_t kMaxCases = 1'000'000;
// Perspective enable, raw S/T/W high halfwords -> TC S/T with overflow flags.
using Request = std::array<uint32_t, 4>;
using Response = std::array<uint32_t, 2>;
inline constexpr std::array<char, 8> kInputMagic{'A','U','R','P','C','I','N','1'};
inline constexpr std::array<char, 8> kOutputMagic{'A','U','R','P','C','O','U','1'};
}
