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

enum RecordType : uint32_t {
    kRecordInitial = 1,
    kRecordMemory = 2,
    kRecordRDP = 3,
    kRecordVI = 4,
    kRecordEnd = 0xffff,
    kRecordOptional = 0x80000000u,
};

constexpr uint32_t kMaxChunkBytes = 1u << 20;
constexpr uint64_t kMaxCaptureBytes = 64ull << 20;

// Payload layout of a kRecordMemory record: a little-endian N64 byte address
// followed by that many guest-order bytes.
struct MemoryPayload {
    uint32_t address = 0;
    const uint8_t* bytes = nullptr;
    size_t count = 0;
};
bool parseMemoryPayload(const Record& record, MemoryPayload& payload);

// Applies a memory record to a host buffer whose words are byte swapped, which
// is how both the capture's RDRAM image and the .rdram layer are laid out.
// Guest byte address A maps to host address A ^ 3.
bool applyMemoryPayload(const MemoryPayload& payload, std::span<uint8_t> host);

bool write(std::ostream& output, std::span<const Record> records, std::string& error);
bool read(std::istream& input, std::vector<Record>& records, std::string& error,
          uint64_t maxBytes = kMaxCaptureBytes);
}
