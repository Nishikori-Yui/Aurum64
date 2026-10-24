// SPDX-License-Identifier: GPL-2.0-or-later
#include "Capture.hpp"

extern "C" {
#include <core/n64video.h>
#include <core/msg.h>
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
    std::vector<uint32_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t frames = 0;
    uint64_t invalidFrames = 0;

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
        n64video_frame_buffer frame{};
        n64video_update_screen(&frame);
        if (frame.valid) {
            require(frame.width <= frame.pitch, "Reference VI width exceeds pitch.");
            require(static_cast<uint64_t>(frame.pitch) * frame.height <=
                        std::numeric_limits<size_t>::max(),
                    "Reference VI geometry overflows output storage.");
            pixels.resize(static_cast<size_t>(frame.pitch) * frame.height);
            std::memcpy(pixels.data(), frame.pixels, pixels.size() * sizeof(uint32_t));
            width = frame.width;
            height = frame.height;
            ++frames;
        } else {
            ++invalidFrames;
        }
    }

    ~Session() {
        if (initialized) n64video_close();
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "Usage: aurum-angrylion-reference file.aurcap\n");
        return 2;
    }
    try {
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
            else if (record.type == aurum::capture::kRecordVI) session.video(record);
        }

        uint64_t writtenWords = 0;
        for (size_t i = 0; i + 2 <= session.rdram.size(); i += 2) {
            const uint16_t value = uint16_t(session.rdram[i + 1]) | uint16_t(session.rdram[i]) << 8;
            if (value) ++writtenWords;
        }
        std::printf("{\"engine\":\"angrylion-rdp-plus\",\"commit\":\"9c8b9ed3e7d7f00dff8bc872ccdd3fba1a3673fc\",\"records\":%zu,\"frames\":%llu,\"invalidFrames\":%llu,\"width\":%u,\"height\":%u,\"nonzero16\":%llu}\n",
                    records.size(), session.frames, session.invalidFrames,
                    session.width, session.height, writtenWords);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "aborted: %s\n", error.what());
        return 1;
    }
}
