// SPDX-License-Identifier: MIT
#include "Capture.hpp"
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

template<class Message>
static void require(bool value, Message&& message) {
    if (!value) throw std::runtime_error(std::forward<Message>(message));
}
static void appendLE(std::vector<uint8_t>& payload, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) payload.push_back(uint8_t(value >> (i * 8)));
}
static void appendBE(std::vector<uint8_t>& payload, uint32_t value) {
    for (int i = 3; i >= 0; --i) payload.push_back(uint8_t(value >> (i * 8)));
}
static aurum::capture::Record submission(uint64_t sequence,
                                         std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> payload;
    appendLE(payload, 0);
    appendLE(payload, uint32_t(words.size() * 4));
    appendLE(payload, 0);
    appendLE(payload, 0);
    for (uint32_t word : words) appendBE(payload, word);
    return {aurum::capture::kRecordRDP, sequence, std::move(payload)};
}

int main() {
    try {
        using aurum::capture::Record;
        using aurum::capture::kRecordEnd;
        using aurum::capture::kRecordInitial;
        using aurum::capture::kRecordOptional;
        using aurum::capture::kRecordVI;

        const std::vector<Record> records{{1, 7, {1, 2, 3, 4}}, {2, 8, {0, 255, 17}}};
        std::ostringstream serialized;
        std::string error;
        require(aurum::capture::write(serialized, records, error), error);
        const std::string bytes = serialized.str();
        std::ostringstream serializedAgain;
        require(aurum::capture::write(serializedAgain, records, error), error);
        require(serializedAgain.str() == bytes, "Capture serialization is not deterministic");
        for (unsigned repeat = 0; repeat < 20; ++repeat) {
            std::istringstream input(bytes);
            std::vector<Record> decoded;
            require(aurum::capture::read(input, decoded, error), error);
            require(decoded == records, "Capture replay changed record data");
        }

        std::vector<Record> decoded;
        std::string truncated = bytes.substr(0, bytes.size() - 1);
        std::istringstream truncatedInput(truncated);
        require(!aurum::capture::read(truncatedInput, decoded, error), "Truncated capture accepted");
        std::string badVersion = bytes;
        badVersion[8] = 2;
        std::istringstream badVersionInput(badVersion);
        require(!aurum::capture::read(badVersionInput, decoded, error), "Unknown capture version accepted");
        std::string badChecksum = bytes;
        badChecksum[48] ^= 0x80;
        std::istringstream badChecksumInput(badChecksum);
        require(!aurum::capture::read(badChecksumInput, decoded, error), "Bad capture checksum accepted");
        std::string oversized = bytes;
        oversized[36] = static_cast<char>((aurum::capture::kMaxChunkBytes + 1) & 255);
        oversized[37] = static_cast<char>((aurum::capture::kMaxChunkBytes + 1) >> 8);
        oversized[38] = static_cast<char>((aurum::capture::kMaxChunkBytes + 1) >> 16);
        oversized[39] = static_cast<char>((aurum::capture::kMaxChunkBytes + 1) >> 24);
        std::istringstream oversizedInput(oversized);
        require(!aurum::capture::read(oversizedInput, decoded, error), "Oversized capture chunk accepted");
        std::istringstream limitedInput(bytes);
        require(!aurum::capture::read(limitedInput, decoded, error, 32), "Reader size limit ignored");

        const std::vector<Record> nonMonotonic{{1, 2, {1}}, {2, 2, {2}}};
        std::ostringstream invalidOutput;
        require(!aurum::capture::write(invalidOutput, nonMonotonic, error),
                "Non-monotonic capture sequence accepted by writer");
        std::string badSequence = bytes;
        badSequence[40] = static_cast<char>(records[1].sequence);
        std::istringstream badSequenceInput(badSequence);
        require(!aurum::capture::read(badSequenceInput, decoded, error),
                "Non-monotonic capture sequence accepted by reader");

        const std::vector<Record> multiSubmission{
            {kRecordInitial, 1, {8, 0, 0, 0}},
            submission(2, {0xef300000, 0}),
            {kRecordOptional | 0x12345678u, 3, {1, 2, 3}},
            submission(4, {0xf7000000, 0xf80107c1}),
            submission(5, {0xe9000000, 0}),
            {kRecordVI, 6, std::vector<uint8_t>(28, 0)},
            {kRecordEnd, 7, {0, 0, 0, 0}}};
        std::ostringstream multiOutput;
        require(aurum::capture::write(multiOutput, multiSubmission, error), error);
        std::istringstream multiInput(multiOutput.str());
        require(aurum::capture::read(multiInput, decoded, error), error);
        require(decoded.size() == multiSubmission.size() - 1, "Optional capture chunk was not skipped");

        const std::vector<Record> unknownRequired{{0x7777, 1, {1}}};
        std::ostringstream unknownOutput;
        require(!aurum::capture::write(unknownOutput, unknownRequired, error),
                "Unknown required capture chunk accepted by writer");
        std::puts("PASS: CPU-only capture format, malformed inputs, sequence and chunk policy");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
