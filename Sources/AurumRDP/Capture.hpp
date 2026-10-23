// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <vector>

namespace aurum::capture {
struct Record {
    uint32_t type = 0;
    uint64_t sequence = 0;
    std::vector<uint8_t> payload;
    bool operator==(const Record&) const = default;
};

constexpr uint32_t kMaxChunkBytes = 1u << 20;
constexpr uint64_t kMaxCaptureBytes = 64ull << 20;

bool write(std::ostream& output, std::span<const Record> records, std::string& error);
bool read(std::istream& input, std::vector<Record>& records, std::string& error,
          uint64_t maxBytes = kMaxCaptureBytes);
}
