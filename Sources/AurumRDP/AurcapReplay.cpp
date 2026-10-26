// SPDX-License-Identifier: MIT
#include "Capture.hpp"
#include "MemoryBus.hpp"
#include "RDP.hpp"
#include "TextureUpload.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint32_t readBE(const uint8_t* bytes) {
    return uint32_t(bytes[3]) | uint32_t(bytes[2]) << 8 |
           uint32_t(bytes[1]) << 16 | uint32_t(bytes[0]) << 24;
}

uint32_t readLE(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
           uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

void appendLE(std::vector<uint8_t>& bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes.push_back(uint8_t(value >> (i * 8)));
}

uint64_t fnv1a64(const void* bytes, size_t size) {
    uint64_t hash = 1469598103934665603ull;
    const auto* data = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Checkpoint {
    uint64_t type = 0, record = 0, submission = 0, command = 0, vi = 0, hash = 0;
};

struct Replay {
    std::vector<uint8_t> rdram;
    aurum::texture::Uploader textures;
    std::vector<std::vector<uint32_t>> color;
    std::vector<Checkpoint> checkpoints;
    std::vector<std::pair<uint32_t, uint32_t>> geometry;
    std::vector<uint8_t> states;
    uint64_t lastStateHash = 0;

    explicit Replay(uint32_t memoryBytes) : rdram(memoryBytes) {}

    void stateCheckpoint(const aurum::rdp::Decoder& decoder, uint64_t record) {
        require(decoder.hasState(), "Capture has no RDP state before submission.");
        const auto state = decoder.stateSnapshot();
        const auto stateWords = state.words();
        const auto* summary = reinterpret_cast<const uint8_t*>(stateWords.data());
        lastStateHash = fnv1a64(summary, sizeof(stateWords));
        states.insert(states.end(), summary, summary + sizeof(stateWords));
        checkpoints.push_back({1, record, decoder.submissionCount(), decoder.commands, 0,
                               lastStateHash});
    }

    void video(const aurum::capture::Record& record, uint64_t frames,
               uint64_t submissions) {
        require(record.payload.size() == 28, "Invalid VI capture payload.");
        constexpr uint32_t map[7] = {0, 1, 2, 3, 4, 5, 6};
        uint32_t vi[14]{};
        for (unsigned i = 0; i < 7; ++i) vi[map[i]] = readLE(record.payload.data() + i * 4);
        const uint32_t type = vi[0] & 3;
        int hStart = int((vi[3] >> 16) & 1023);
        const int hEnd = int(vi[3] & 1023);
        int vStart = int((vi[4] >> 16) & 1023);
        const int vEnd = int(vi[4] & 1023);
        int hres = hEnd - hStart;
        int vres = (vEnd - vStart) / 2;
        const uint32_t xScale = vi[5] & 4095;
        uint32_t xOffset = (vi[5] >> 16) & 4095;
        const uint32_t yScale = vi[6] & 4095;
        uint32_t yOffset = (vi[6] >> 16) & 4095;
        hStart -= 108;
        if (hStart < 0) {
            xOffset += xScale * uint32_t(-hStart);
            hres += hStart;
            hStart = 0;
        }
        vStart = (vStart - 34) / 2;
        if (vStart < 0) {
            yOffset += yScale * uint32_t(-vStart);
            vStart = 0;
        }
        const int scaledWidth = int(xScale) * hres / 1024;
        int scaledHeight = int(yScale) * vres / 1024;
        if (yScale * vres % 1024) ++scaledHeight;
        if (type < 2 || !vi[2] || hres <= 0 || vres <= 0 || scaledWidth <= 0 ||
            scaledHeight <= 0 || !xScale || !yScale) {
            color.emplace_back();
            geometry.emplace_back(0, 0);
            checkpoints.push_back({2, record.sequence, submissions, 0, frames + 1,
                                   lastStateHash});
            return;
        }
        const int border = (hres - (hres - 7)) * scaledWidth / hres;
        const uint32_t outWidth = uint32_t(std::max(scaledWidth - border, 0));
        const uint32_t outHeight = uint32_t(std::min(scaledHeight, 576));
        std::vector<uint32_t> pixels(outWidth * outHeight, 0);
        const uint32_t bytes = type == 2 ? 2 : 4;
        for (uint32_t y = 0; y < outHeight; ++y) {
                const uint32_t sourceY = y;
            for (uint32_t x = 0; x < outWidth; ++x) {
                const uint32_t sourceX = x + uint32_t(border / 2 + 1);
                const size_t address = ((vi[1] & 0xffffffu) & ~(bytes - 1)) +
                                       (size_t(sourceY) * vi[2] + sourceX) * bytes;
                if (address + bytes > rdram.size()) continue;
                if (bytes == 2) {
                    uint16_t value = 0;
                    if (!aurum::memory::readHalfword(rdram.data(), rdram.size(), address, value))
                        continue;
                    const uint32_t r = (value >> 8) & 0xf8;
                    const uint32_t g = (value & 0x7c0) >> 3;
                    const uint32_t b = (value & 0x3e) << 2;
                    pixels[size_t(y) * outWidth + x] = r | (g << 8) | (b << 16);
                } else {
                    uint32_t word = 0;
                    if (!aurum::memory::readWord(rdram.data(), rdram.size(), address, word))
                        continue;
                    uint32_t r = word >> 24, g = word >> 16 & 255, b = word >> 8 & 255;
                    pixels[size_t(y) * outWidth + x] = r | (g << 8) | (b << 16);
                }
            }
        }
        color.push_back(std::move(pixels));
        geometry.emplace_back(outWidth, outHeight);
        checkpoints.push_back({2, record.sequence, submissions, 0, frames + 1,
                               lastStateHash});
    }
};

void applyMemory(Replay& output, const aurum::capture::Record& record) {
    aurum::capture::MemoryPayload payload;
    require(aurum::capture::parseMemoryPayload(record, payload),
            "Memory record has no address.");
    require(aurum::capture::applyMemoryPayload(payload, output.rdram),
            "Memory record is outside RDRAM.");
}

void replay(aurum::rdp::Decoder& decoder, Replay& output,
            const std::vector<aurum::capture::Record>& records) {
    for (const auto& record : records) {
        if (record.type == aurum::capture::kRecordMemory) {
            applyMemory(output, record);
        } else if (record.type == aurum::capture::kRecordRDP) {
            if (record.payload.size() < 16 || (record.payload.size() - 16) % 4 != 0)
                throw std::runtime_error("Invalid RDP submission payload.");
            const uint32_t start = readLE(record.payload.data());
            const uint32_t end = readLE(record.payload.data() + 4);
            const uint32_t xbus = readLE(record.payload.data() + 12);
            require(end >= start && (end - start) % 4 == 0,
                    "Invalid RDP submission range.");
            const uint32_t range = end - start;
            require(record.payload.size() - 16 == range,
                    "RDP submission payload does not match its address range.");
            if (xbus)
                require(range <= 4096, "XBUS RDP range is too large.");
            else
                require(end <= output.rdram.size(), "RDRAM RDP range is outside memory.");
            decoder.beginSubmission();
            for (size_t offset = 16; offset < record.payload.size(); offset += 4) {
                const uint32_t words[1] = {readBE(record.payload.data() + offset)};
                decoder.push(words);
            }
            // Preserve the submitted command words at their DPC addresses so raw RDRAM
            // comparisons include the exact buffered stream, as the reference replay does.
            if (!xbus && end >= start && end <= output.rdram.size() &&
                record.payload.size() >= 16 + (end - start)) {
                for (uint32_t address = start; address < end; address += 4) {
                    const uint32_t word = readBE(record.payload.data() + 16 + (address - start));
                    require(aurum::memory::writeWord(output.rdram.data(), output.rdram.size(),
                                                     address, word),
                            "RDRAM command write is outside memory.");
                }
            }
            output.stateCheckpoint(decoder, record.sequence);
        } else if (record.type == aurum::capture::kRecordVI) {
            output.video(record, decoder.viCount(), decoder.submissionCount());
            decoder.beginVI();
        }
    }
    if (decoder.hasPendingWords()) throw std::runtime_error("Incomplete RDP command.");
}

void write(const std::string& prefix, const char* suffix, const void* bytes, size_t size) {
    std::ofstream output(prefix + suffix, std::ios::binary | std::ios::trunc);
    require(bool(output), "Cannot write replay layer.");
    output.write(static_cast<const char*>(bytes), std::streamsize(size));
}

}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "Usage: aurum-aurcap-replay file.aurcap [output-prefix]\n");
        return 2;
    }
    try {
        const bool emitLayers = argc == 3;
        const std::string prefix = emitLayers ? argv[2] : std::string{};
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open capture.");
        std::vector<aurum::capture::Record> records;
        std::string error;
        if (!aurum::capture::read(input, records, error)) throw std::runtime_error(error);

        bool haveInitial = false;
        uint32_t memoryBytes = 0;
        for (const auto& record : records) {
            if (record.type == aurum::capture::kRecordInitial) {
                require(!haveInitial && record.payload.size() == 8,
                        "Invalid initial capture payload.");
                memoryBytes = readLE(record.payload.data());
                haveInitial = true;
            }
        }
        require(haveInitial && (memoryBytes == 4 * 1024 * 1024 ||
                                memoryBytes == 8 * 1024 * 1024),
                "Capture has an unsupported RDRAM size.");

        Replay output{memoryBytes};
        aurum::rdp::Decoder decoder;
        decoder.fill = [&](const aurum::rdp::Fill& fill) {
            const uint32_t bytes = fill.size == 2 ? 2 : 4;
            for (uint32_t y = fill.y0; y < fill.y1; ++y) {
                for (uint32_t x = fill.x0; x < fill.x1; ++x) {
                    const size_t address = fill.address + (size_t(y) * fill.width + x) * bytes;
                    if (address + bytes > output.rdram.size()) continue;
                    if (bytes == 4) {
                        std::memcpy(output.rdram.data() + address, &fill.color, 4);
                    } else {
                        const uint32_t pixelIndex = (fill.address >> 1) +
                                                    (size_t(y) * fill.width + x);
                        const uint16_t half = uint16_t(fill.color >> ((pixelIndex & 1) ? 0 : 16));
                        require(aurum::memory::writeHalfword(output.rdram.data(),
                                                             output.rdram.size(), address, half),
                                "RDRAM fill write is outside memory.");
                    }
                }
            }
        };
        decoder.textureLoad = [&](const aurum::rdp::TextureLoad& load) {
            // Only the 8-bit, RGBA16 and RGBA32 paths are modeled. Anything
            // else stays reported as unsupported instead of producing a
            // plausible-looking TMEM.
            if (load.tiSize != 1 && load.tiSize != 2 && load.tiSize != 3)
                return false;
            // TMEM accumulates across uploads, so the same store is reused.
            // A rejected upload leaves it untouched.
            aurum::texture::Uploader& uploader = output.textures;
            uploader.setTextureImage(load.tiFormat, load.tiSize, load.tiWidth,
                                     load.tiAddress);
            uploader.setTile(load.tile, load.tileTmem, load.tileLine);
            if (load.opcode == 0x30)
                return uploader.loadTlut(output.rdram.data(), output.rdram.size(),
                                         load.tile, load.sl, load.tl, load.sh,
                                         load.th);
            if (load.opcode == 0x33)
                return uploader.loadBlock(output.rdram.data(),
                                          output.rdram.size(), load.tile,
                                          load.sl, load.tl, load.sh, load.dxt);
            return uploader.loadTile(output.rdram.data(), output.rdram.size(),
                                     load.tile, load.sl, load.tl, load.sh,
                                     load.th);
        };
        replay(decoder, output, records);
        if (emitLayers) {
            write(prefix, ".rdram", output.rdram.data(), output.rdram.size());
            // TMEM in hardware byte layout so it is directly comparable with
            // the reference layer of the same name.
            std::array<uint8_t, 0x1000> tmemBytes{};
            output.textures.store().copyBytes(tmemBytes);
            write(prefix, ".tmem", tmemBytes.data(), tmemBytes.size());
            std::vector<uint8_t> counts, geometryBytes, checkpointBytes;
            for (size_t i = 0; i < output.color.size(); ++i) {
                appendLE(counts, uint64_t(output.color[i].size()));
                appendLE(counts, uint64_t(0));
                appendLE(counts, uint64_t(0));
                if (!output.color[i].empty()) {
                    const std::string suffix = ".vi-color-" + std::to_string(i) + ".rgba8";
                    write(prefix, suffix.c_str(), output.color[i].data(),
                          output.color[i].size() * 4);
                }
                for (unsigned byte = 0; byte < 4; ++byte)
                    geometryBytes.push_back(uint8_t(output.geometry[i].first >> (byte * 8)));
                for (unsigned byte = 0; byte < 4; ++byte)
                    geometryBytes.push_back(uint8_t(output.geometry[i].second >> (byte * 8)));
            }
            for (const Checkpoint& item : output.checkpoints) {
                appendLE(checkpointBytes, item.type);
                appendLE(checkpointBytes, item.record);
                appendLE(checkpointBytes, item.submission);
                appendLE(checkpointBytes, item.command);
                appendLE(checkpointBytes, item.vi);
                appendLE(checkpointBytes, item.hash);
            }
            write(prefix, ".counts", counts.data(), counts.size());
            write(prefix, ".geometry", geometryBytes.data(), geometryBytes.size());
            write(prefix, ".checkpoints", checkpointBytes.data(), checkpointBytes.size());
            write(prefix, ".states", output.states.data(), output.states.size());
            std::ofstream manifest(prefix + ".manifest", std::ios::binary | std::ios::trunc);
            require(bool(manifest), "Cannot write replay manifest.");
            manifest << "format=aurum-reference-layers/1\n"
                        "engine=aurum-scalar-replay\n"
                        "rdram=host-word-swapped:N64ByteAddress:whole\n"
                        "tmem=4096:hardware-byte-layout\n"
                        "vi-color=RGBA8:R,G,B,A little-endian\n"
                        "counts=little-endian-uint64-triples:color-depth-coverage\n"
                        "geometry=little-endian-uint32-pairs:width-height-per-vi\n"
                        "checkpoints=little-endian-uint64-sextuples:type-record-submission-command-vi-hash\n";
            manifest << "states=256-byte-little-endian-uint32-grid:v1:49-named-rdp-fields\n";
        }

        std::printf("{\"records\":%zu,\"submissions\":%llu,\"commands\":%llu,"
                    "\"unsupported\":%llu,\"pending\":false,\"layers\":%s}\n",
                    records.size(), decoder.submissionCount(), decoder.commands,
                    decoder.unsupported, emitLayers ? "true" : "false");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "{\"error\":\"%s\"}\n", error.what());
        return 1;
    }
}
