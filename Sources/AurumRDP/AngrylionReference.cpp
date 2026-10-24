// SPDX-License-Identifier: GPL-2.0-or-later
#include "AngrylionExports.h"
#include "Capture.hpp"

extern "C" {
#include <core/n64video.h>
}

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

uint32_t readLE32(const uint8_t* bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 |
           uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

uint32_t readBE32(const uint8_t* bytes) {
    return uint32_t(bytes[3]) | uint32_t(bytes[2]) << 8 |
           uint32_t(bytes[1]) << 16 | uint32_t(bytes[0]) << 24;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Checkpoint {
    std::string type;
    uint64_t record = 0;
    uint64_t submission = 0;
    uint64_t command = 0;
    uint64_t vi = 0;
    std::vector<uint8_t> state;
};

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

extern "C" void msg_error(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
    throw std::runtime_error("Angrylion reference reported a fatal error.");
}

extern "C" void msg_warning(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
}

struct Session {
    std::vector<uint8_t> rdram;
    std::vector<uint8_t> dmem;
    std::vector<uint32_t> dpRegisters;
    std::vector<uint32_t> viRegisters;
    std::vector<uint32_t*> dpPointers;
    std::vector<uint32_t*> viPointers;
    uint32_t interrupts = 0;
    bool initialized = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frames = 0;
    uint64_t invalidFrames = 0;
    std::vector<std::vector<uint32_t>> colorLayers;
    std::vector<std::vector<uint32_t>> depthLayers;
    std::vector<std::vector<uint32_t>> coverageLayers;
    std::vector<Checkpoint> checkpoints;
    uint64_t submissions = 0;

    explicit Session(uint32_t memoryBytes)
        : rdram(memoryBytes), dmem(4096), dpRegisters(9), viRegisters(14),
          dpPointers(9), viPointers(14) {
        for (unsigned i = 0; i < dpRegisters.size(); ++i) dpPointers[i] = &dpRegisters[i];
        for (unsigned i = 0; i < viRegisters.size(); ++i) viPointers[i] = &viRegisters[i];
    }

    void configure() {
        n64video_config config;
        n64video_config_init(&config);
        config.gfx.rdram = rdram.data();
        config.gfx.rdram_size = uint32_t(rdram.size());
        config.gfx.dmem = dmem.data();
        config.gfx.vi_reg = viPointers.data();
        config.gfx.dp_reg = dpPointers.data();
        config.gfx.mi_intr_reg = &interrupts;
        config.gfx.mi_intr_cb = [] {};
        config.vi.mode = VI_MODE_COLOR;
        config.vi.interp = VI_INTERP_NEAREST;
        config.vi.widescreen = false;
        config.vi.hide_overscan = false;
        config.vi.vsync = false;
        config.vi.exclusive = false;
        config.vi.integer_scaling = false;
        config.dp.compat = DP_COMPAT_HIGH;
        config.parallel = false;
        config.busyloop = false;
        config.num_workers = 1;
        n64video_init(&config);
        initialized = true;
    }

    void submission(const aurum::capture::Record& record) {
        require(record.payload.size() >= 16 && (record.payload.size() - 16) % 4 == 0,
                "Invalid RDP submission payload.");
        const uint32_t start = readLE32(record.payload.data());
        const uint32_t end = readLE32(record.payload.data() + 4);
        const uint32_t status = readLE32(record.payload.data() + 8);
        const uint32_t xbus = readLE32(record.payload.data() + 12);
        require(end >= start && end <= rdram.size(),
                "RDP submission range is outside captured RDRAM.");
        require((end - start) % 4 == 0, "RDP submission range is not word aligned.");
        dpRegisters[DP_START] = start;
        dpRegisters[DP_CURRENT] = start;
        dpRegisters[DP_END] = end;
        dpRegisters[DP_STATUS] = status | (xbus ? 1u : 0u);
        ++submissions;
        for (uint32_t address = start; address < end; address += 4) {
            const size_t payloadOffset = 16 + (address - start);
            const uint32_t word = readBE32(record.payload.data() + payloadOffset);
            if (xbus) {
                require(address <= dmem.size() - 4, "XBUS RDP word is outside DMEM.");
            } else {
                require(address <= rdram.size() - 4, "RDP word is outside RDRAM.");
            }
            auto& destination = xbus ? dmem : rdram;
            std::copy_n(reinterpret_cast<const uint8_t*>(&word), 4,
                        destination.begin() + address);
        }
        n64video_process_list();
        checkpoint("rdp", record.sequence, submissions, 0, 0);
    }

    void video(const aurum::capture::Record& record) {
        require(record.payload.size() == 28, "Invalid VI record payload.");
        constexpr unsigned map[7] = {VI_STATUS, VI_ORIGIN, VI_WIDTH, VI_H_START,
                                     VI_V_START, VI_X_SCALE, VI_Y_SCALE};
        for (unsigned i = 0; i < 7; ++i)
            viRegisters[map[i]] = readLE32(record.payload.data() + i * 4);
        // .aurcap v1 stores only the seven registers the production renderer consumes.
        // Supply NTSC defaults for the timing registers Angrylion additionally reads.
        viRegisters[VI_V_SYNC] = 525;
        viRegisters[VI_V_CURRENT_LINE] = 0;
        width = height = 0;
    }

    void checkpoint(const char* type, uint64_t recordSequence, uint64_t submission,
                    uint64_t command, uint64_t vi) {
        Checkpoint value;
        value.type = type;
        value.record = recordSequence;
        value.submission = submission;
        value.command = command;
        value.vi = vi;
        value.state.resize(256);
        n64video_rdp_state_summary(value.state.data());
        checkpoints.push_back(std::move(value));
    }

    void captureFrame() {
        n64video_set_vi_mode(reference_vi_color);
        n64video_frame_buffer frame{};
        n64video_update_screen(&frame);
        if (frame.valid) {
            std::vector<uint32_t> pixels(static_cast<size_t>(frame.width) * frame.height);
            copyFrame(pixels, frame);
            colorLayers.push_back(std::move(pixels));
            width = frame.width;
            height = frame.height;
            ++frames;
        } else {
            colorLayers.emplace_back();
            ++invalidFrames;
        }
        depthLayers.push_back(runMode(reference_vi_depth));
        coverageLayers.push_back(runMode(reference_vi_coverage));
    }

    std::vector<uint32_t> runMode(int mode) {
        n64video_set_vi_mode(mode);
        n64video_frame_buffer frame{};
        n64video_update_screen(&frame);
        if (!frame.valid) return {};
        std::vector<uint32_t> pixels(static_cast<size_t>(frame.width) * frame.height);
        copyFrame(pixels, frame);
        return pixels;
    }

    static void copyFrame(std::vector<uint32_t>& pixels,
                          const n64video_frame_buffer& frame) {
        if (frame.pitch <= 0 || frame.pitch < frame.width) throw std::runtime_error("Invalid frame pitch.");
        for (uint32_t y = 0; y < frame.height; ++y)
            std::memcpy(pixels.data() + size_t(y) * frame.width,
                        frame.pixels + size_t(y) * frame.pitch, frame.width * sizeof(uint32_t));
    }

    ~Session() {
        if (initialized) n64video_close();
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "Usage: aurum-angrylion-reference file.aurcap [output-prefix]\n");
        return 2;
    }
    try {
        const bool emitLayers = argc == 3;
        const std::string prefix = emitLayers ? argv[2] : std::string{};
        std::ofstream manifest;
        if (emitLayers) {
            manifest.open(prefix + ".manifest", std::ios::binary | std::ios::trunc);
            require(bool(manifest), "Cannot write reference manifest.");
            manifest <<
                "format=aurum-reference-layers/1\n"
                "engine=angrylion-rdp-plus\n"
                "commit=9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc\n"
                "rdram=host-word-swapped:N64ByteAddress:whole\n"
                "hidden=oneBytePer16BitPixel:coverage-compressed\n"
                "tmem=4096:hardware-byte-layout\n"
                "vi-color=RGBA8:R,G,B,A little-endian\n"
                "vi-depth=RGBA8:grayscale-high-byte-of-Z16\n"
                "vi-coverage=RGBA8:grayscale-Angrylion-coverage-encoding\n"
                "counts=little-endian-uint64-triples:color-depth-coverage\n"
                "geometry=little-endian-uint32-pairs:width-height-per-vi\n"
                "checkpoints=little-endian-uint64-sextuples:type-record-submission-command-vi-hash\n"
                "states=256-byte-little-endian-uint64-grid:49-named-rdp-fields\n";
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot open capture.");
        std::vector<aurum::capture::Record> records;
        std::string error;
        if (!aurum::capture::read(input, records, error)) throw std::runtime_error(error);

        bool haveInitial = false;
        uint32_t memoryBytes = 0;
        for (const auto& record : records) {
            if (record.type == aurum::capture::kRecordInitial) {
                require(!haveInitial, "Capture contains more than one initial record.");
                require(record.payload.size() == 8, "Invalid initial capture payload.");
                memoryBytes = readLE32(record.payload.data());
                haveInitial = true;
            }
        }
        require(haveInitial, "Capture has no initial memory-size record.");
        require(memoryBytes == 4 * 1024 * 1024 || memoryBytes == 8 * 1024 * 1024,
                "Initial record has an unsupported RDRAM size.");

        Session session{memoryBytes};
        session.configure();
        for (const auto& record : records) {
            if (record.type == aurum::capture::kRecordRDP) session.submission(record);
            else if (record.type == aurum::capture::kRecordVI) {
                session.video(record);
                session.captureFrame();
                session.checkpoint("vi", record.sequence, session.submissions, 0,
                                   session.frames);
            }
        }

        uint64_t writtenWords = 0;
        for (size_t i = 0; i + 2 <= session.rdram.size(); i += 2) {
            const uint16_t value = uint16_t(session.rdram[i + 1]) | uint16_t(session.rdram[i]) << 8;
            if (value) ++writtenWords;
        }
        if (emitLayers) {
            const auto writeLayer = [&](const char* suffix, const void* bytes, size_t size) {
                std::ofstream output(prefix + suffix, std::ios::binary | std::ios::trunc);
                require(bool(output), "Cannot write reference layer.");
                output.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
            };
            writeLayer(".rdram", session.rdram.data(), session.rdram.size());
            writeLayer(".tmem", n64video_tmem(), 0x1000);
            writeLayer(".hidden", n64video_hidden_rdram(), session.rdram.size() / 2);
            std::vector<uint8_t> littleEndianCounts;
            for (size_t i = 0; i < session.colorLayers.size(); ++i) {
                const std::string index = std::to_string(i);
                const size_t colorSize = session.colorLayers[i].size();
                const size_t depthSize = session.depthLayers[i].size();
                const size_t coverageSize = session.coverageLayers[i].size();
                appendLE(littleEndianCounts, colorSize);
                appendLE(littleEndianCounts, depthSize);
                appendLE(littleEndianCounts, coverageSize);
                if (!session.colorLayers[i].empty())
                    writeLayer((".vi-color-" + index + ".rgba8").c_str(),
                               session.colorLayers[i].data(), session.colorLayers[i].size() * 4);
                if (!session.depthLayers[i].empty())
                    writeLayer((".vi-depth-" + index + ".rgba8").c_str(),
                               session.depthLayers[i].data(), session.depthLayers[i].size() * 4);
                if (!session.coverageLayers[i].empty())
                    writeLayer((".vi-coverage-" + index + ".rgba8").c_str(),
                               session.coverageLayers[i].data(), session.coverageLayers[i].size() * 4);
            }
            writeLayer(".counts", littleEndianCounts.data(), littleEndianCounts.size());
            std::vector<uint8_t> geometry;
            for (const Checkpoint& item : session.checkpoints) {
                if (item.type != "vi") continue;
                for (unsigned i = 0; i < 4; ++i) geometry.push_back(uint8_t(session.width >> (i * 8)));
                for (unsigned i = 0; i < 4; ++i) geometry.push_back(uint8_t(session.height >> (i * 8)));
            }
            writeLayer(".geometry", geometry.data(), geometry.size());
            std::vector<uint8_t> checkpointBytes;
            std::vector<uint8_t> checkpointStates;
            for (const Checkpoint& item : session.checkpoints) {
                const uint64_t typeCode = item.type == "rdp" ? 1 : 2;
                appendLE(checkpointBytes, typeCode);
                appendLE(checkpointBytes, item.record);
                appendLE(checkpointBytes, item.submission);
                appendLE(checkpointBytes, item.command);
                appendLE(checkpointBytes, item.vi);
                appendLE(checkpointBytes, fnv1a64(item.state.data(), item.state.size()));
                checkpointStates.insert(checkpointStates.end(), item.state.begin(),
                                        item.state.end());
            }
            writeLayer(".checkpoints", checkpointBytes.data(), checkpointBytes.size());
            writeLayer(".states", checkpointStates.data(), checkpointStates.size());
        }

        std::printf("{\"engine\":\"angrylion-rdp-plus\",\"commit\":\"9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc\",\"records\":%zu,\"frames\":%llu,\"invalidFrames\":%llu,\"width\":%u,\"height\":%u,\"nonzero16\":%llu,\"layers\":%s}\n",
                    records.size(), session.frames, session.invalidFrames,
                    session.width, session.height, writtenWords, emitLayers ? "true" : "false");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "aborted: %s\n", error.what());
        return 1;
    }
}
