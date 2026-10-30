// SPDX-License-Identifier: MIT
#pragma once
#include "TextureCoordinateProbe.hpp"

namespace aurum::attribute_probe {
using aurum::coordinate_probe::readMagic;
using aurum::coordinate_probe::writeMagic;
using aurum::coordinate_probe::readWords;
using aurum::coordinate_probe::writeWords;
constexpr size_t kMaxCases = 100'000;
// Fixed 44-word triangle (unused tail zero), scissor[2], target Y.
using Request = std::array<uint32_t, 47>;
// valid, anchorX, clipped scan start, RGBA/STW/Z row values[8], X steps[8],
// STW Y steps[3], affine S/T, corrected non-AA Z.
using Response = std::array<uint32_t, 25>;
inline constexpr std::array<char, 8> kInputMagic{'A','U','R','A','T','I','N','1'};
inline constexpr std::array<char, 8> kOutputMagic{'A','U','R','A','T','O','U','1'};
}
