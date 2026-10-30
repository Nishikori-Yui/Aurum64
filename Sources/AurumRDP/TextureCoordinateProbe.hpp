// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <istream>
#include <ostream>
#include <stdexcept>

// Development-only neutral protocol. Request words are SetTile[2],
// SetTileSize[2], S, T; response words are point S/T, copy S[4]/T and RGBA[4].
// All words use little endian. Neither executable shares mapping algorithms.
namespace aurum::coordinate_probe {
constexpr size_t kMaxCases = 2'000'000;
using Request = std::array<uint32_t, 6>;
using Response = std::array<uint32_t, 11>;
inline constexpr std::array<char, 8> kInputMagic{'A','U','R','T','C','I','N','1'};
inline constexpr std::array<char, 8> kOutputMagic{'A','U','R','T','C','O','U','1'};

inline void readMagic(std::istream& in, const std::array<char, 8>& expected) {
    std::array<char, 8> magic{};
    if (!in.read(magic.data(), magic.size()) || magic != expected)
        throw std::runtime_error("Invalid coordinate probe header.");
}
inline void writeMagic(std::ostream& out, const std::array<char, 8>& magic) {
    if (!out.write(magic.data(), magic.size()))
        throw std::runtime_error("Cannot write coordinate probe header.");
}
template<size_t N>
bool readWords(std::istream& in, std::array<uint32_t, N>& words) {
    std::array<uint8_t, N * 4> bytes{};
    in.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (in.gcount() == 0 && in.eof()) return false;
    if (!in) throw std::runtime_error("Truncated coordinate probe record.");
    for (size_t i = 0; i < N; ++i)
        words[i] = uint32_t(bytes[4*i]) | uint32_t(bytes[4*i+1]) << 8 |
                   uint32_t(bytes[4*i+2]) << 16 | uint32_t(bytes[4*i+3]) << 24;
    return true;
}
template<size_t N>
void writeWords(std::ostream& out, const std::array<uint32_t, N>& words) {
    std::array<uint8_t, N * 4> bytes{};
    for (size_t i = 0; i < N; ++i)
        for (unsigned b = 0; b < 4; ++b) bytes[4*i+b] = uint8_t(words[i] >> (b*8));
    if (!out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()))
        throw std::runtime_error("Cannot write coordinate probe record.");
}
} // namespace aurum::coordinate_probe
