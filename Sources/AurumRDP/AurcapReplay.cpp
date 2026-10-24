// SPDX-License-Identifier: MIT
#include "Capture.hpp"
#include "RDP.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
uint32_t readBE(const uint8_t* bytes) {
    return uint32_t(bytes[3]) | uint32_t(bytes[2]) << 8 |
           uint32_t(bytes[1]) << 16 | uint32_t(bytes[0]) << 24;
}

void replay(aurum::rdp::Decoder& decoder,
            const std::vector<aurum::capture::Record>& records) {
    for (const auto& record : records) {
        if (record.type != aurum::capture::kRecordRDP) continue;
        if (record.payload.size() < 16 || (record.payload.size() - 16) % 4 != 0)
            throw std::runtime_error("Invalid RDP submission payload.");
        decoder.beginSubmission();
        for (size_t offset = 16; offset < record.payload.size(); offset += 4) {
            const uint32_t words[1] = {readBE(record.payload.data() + offset)};
            decoder.push(words);
        }
    }
    if (decoder.hasPendingWords()) throw std::runtime_error("Incomplete RDP command.");
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: aurum-aurcap-replay file.aurcap\n");
        return 2;
    }
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open capture.");
        std::vector<aurum::capture::Record> records;
        std::string error;
        if (!aurum::capture::read(input, records, error)) throw std::runtime_error(error);

        aurum::rdp::Decoder decoder;
        replay(decoder, records);
        std::printf("{\"records\":%zu,\"submissions\":%llu,\"commands\":%llu,"
                    "\"unsupported\":%llu,\"pending\":false}\n",
                    records.size(), decoder.submissionCount(), decoder.commands,
                    decoder.unsupported);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "{\"error\":\"%s\"}\n", error.what());
        return 1;
    }
}
