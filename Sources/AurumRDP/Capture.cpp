// SPDX-License-Identifier: MIT
#include "Capture.hpp"
#include "MemoryBus.hpp"
#include <array>
#include <algorithm>
#include <cstring>
#include <istream>
#include <ostream>
#include <limits>
#include <utility>

namespace aurum::capture {
bool parseMemoryPayload(const Record& record, MemoryPayload& payload) {
    if (record.payload.size() < sizeof(uint32_t)) return false;
    payload.address = uint32_t(record.payload[0]) |
                      uint32_t(record.payload[1]) << 8 |
                      uint32_t(record.payload[2]) << 16 |
                      uint32_t(record.payload[3]) << 24;
    payload.bytes = record.payload.data() + sizeof(uint32_t);
    payload.count = record.payload.size() - sizeof(uint32_t);
    return true;
}

bool applyMemoryPayload(const MemoryPayload& payload, std::span<uint8_t> host) {
    // Validate the whole span before writing so a rejected record cannot leave
    // RDRAM partially modified.
    for (size_t i = 0; i < payload.count; ++i) {
        const uint64_t guest = uint64_t(payload.address) + i;
        if (guest > UINT32_MAX) return false;
        if (aurum::memory::hostByteAddress(uint32_t(guest)) >= host.size())
            return false;
    }
    for (size_t i = 0; i < payload.count; ++i)
        host[aurum::memory::hostByteAddress(payload.address + uint32_t(i))] =
            payload.bytes[i];
    return true;
}

namespace {
constexpr std::array<uint8_t, 8> kMagic{'A', 'U', 'R', 'C', 'A', 'P', 1, 0};
constexpr uint32_t kEndianMarker = 0x01020304;
constexpr uint32_t kHeaderBytes = 32;
constexpr uint32_t kChunkHeaderBytes = 24;

uint32_t checksum(std::span<const uint8_t> bytes) {
    uint32_t hash = 2166136261u;
    for (uint8_t byte : bytes) { hash ^= byte; hash *= 16777619u; }
    return hash;
}
void put16(uint8_t* p, uint16_t value) { p[0] = value; p[1] = value >> 8; }
void put32(uint8_t* p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = value >> (i * 8);
}
void put64(uint8_t* p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = value >> (i * 8);
}
uint16_t get16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1]) << 8; }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t get64(const uint8_t* p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}
bool readExact(std::istream& input, uint8_t* data, size_t size) {
    return size == 0 || static_cast<bool>(input.read(reinterpret_cast<char*>(data), size));
}
bool knownRequiredType(uint32_t type) {
    return type == kRecordInitial || type == kRecordMemory || type == kRecordRDP ||
           type == kRecordVI || type == kRecordEnd;
}
}

bool write(std::ostream& output, std::span<const Record> records, std::string& error) {
    std::array<uint8_t, kHeaderBytes> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    put16(header.data() + 8, 1);
    put16(header.data() + 10, 0);
    put32(header.data() + 12, kEndianMarker);
    put32(header.data() + 16, kHeaderBytes);
    put64(header.data() + 20, kMaxCaptureBytes);
    if (!output.write(reinterpret_cast<const char*>(header.data()), header.size())) {
        error = "Cannot write capture header."; return false;
    }
    uint64_t total = header.size();
    bool havePrevious = false;
    uint64_t previousSequence = 0;
    for (const Record& record : records) {
        const uint32_t requiredType = record.type & ~kRecordOptional;
        if (!(record.type & kRecordOptional) && !knownRequiredType(requiredType)) {
            error = "Unknown required capture chunk."; return false;
        }
        if (havePrevious && record.sequence <= previousSequence) {
            error = "Capture sequence is not monotonic."; return false;
        }
        havePrevious = true;
        previousSequence = record.sequence;
        if (record.payload.size() > kMaxChunkBytes) { error = "Capture chunk exceeds limit."; return false; }
        if (record.payload.size() > std::numeric_limits<uint32_t>::max() ||
            total > kMaxCaptureBytes - kChunkHeaderBytes - record.payload.size()) {
            error = "Capture exceeds size limit."; return false;
        }
        std::array<uint8_t, kChunkHeaderBytes> chunk{};
        put32(chunk.data(), record.type);
        put32(chunk.data() + 4, static_cast<uint32_t>(record.payload.size()));
        put64(chunk.data() + 8, record.sequence);
        put32(chunk.data() + 16, checksum(record.payload));
        if (!output.write(reinterpret_cast<const char*>(chunk.data()), chunk.size()) ||
            !record.payload.empty() &&
                !output.write(reinterpret_cast<const char*>(record.payload.data()), record.payload.size())) {
            error = "Cannot write capture chunk."; return false;
        }
        total += kChunkHeaderBytes + record.payload.size();
    }
    return true;
}

bool read(std::istream& input, std::vector<Record>& records, std::string& error, uint64_t maxBytes) {
    records.clear();
    if (maxBytes < kHeaderBytes || maxBytes > kMaxCaptureBytes) { error = "Invalid capture size limit."; return false; }
    std::array<uint8_t, kHeaderBytes> header{};
    if (!readExact(input, header.data(), header.size())) { error = "Truncated capture header."; return false; }
    if (!std::equal(kMagic.begin(), kMagic.end(), header.begin())) { error = "Invalid capture magic."; return false; }
    if (get16(header.data() + 8) != 1 || get32(header.data() + 12) != kEndianMarker ||
        get32(header.data() + 16) != kHeaderBytes) {
        error = "Unsupported capture version or byte order."; return false;
    }
    const uint64_t declaredLimit = get64(header.data() + 20);
    if (declaredLimit == 0 || declaredLimit > maxBytes) { error = "Capture limit exceeds reader policy."; return false; }
    uint64_t total = header.size();
    bool havePrevious = false;
    uint64_t previousSequence = 0;
    for (;;) {
        uint8_t first = 0;
        if (!input.read(reinterpret_cast<char*>(&first), 1)) {
            if (input.eof()) return true;
            error = "Cannot read capture chunk."; return false;
        }
        std::array<uint8_t, kChunkHeaderBytes> chunk{};
        chunk[0] = first;
        if (!readExact(input, chunk.data() + 1, chunk.size() - 1)) { error = "Truncated capture chunk header."; return false; }
        const uint32_t length = get32(chunk.data() + 4);
        // Check the subtraction operands before evaluating the size bound;
        // callers may intentionally supply a reader limit smaller than one
        // chunk header plus payload.
        if (length > kMaxChunkBytes || length > maxBytes ||
            kChunkHeaderBytes > maxBytes - length ||
            total > maxBytes - kChunkHeaderBytes - length) {
            error = "Capture chunk exceeds reader limit."; return false;
        }
        Record record;
        record.type = get32(chunk.data());
        record.sequence = get64(chunk.data() + 8);
        record.payload.resize(length);
        if (!readExact(input, record.payload.data(), length)) { error = "Truncated capture chunk."; return false; }
        if (checksum(record.payload) != get32(chunk.data() + 16)) { error = "Capture checksum mismatch."; return false; }
        const uint32_t requiredType = record.type & ~kRecordOptional;
        if (!(record.type & kRecordOptional) && !knownRequiredType(requiredType)) {
            error = "Unknown required capture chunk."; return false;
        }
        if (havePrevious && record.sequence <= previousSequence) {
            error = "Capture sequence is not monotonic."; return false;
        }
        havePrevious = true;
        previousSequence = record.sequence;
        if (record.type & kRecordOptional) continue;
        records.push_back(std::move(record));
        total += kChunkHeaderBytes + length;
    }
}
}
