// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>
#include <stdexcept>

namespace {

namespace fs = std::filesystem;

struct Options {
    fs::path expectedPrefix;
    fs::path actualPrefix;
    fs::path capture;
};

struct Checkpoint {
    uint64_t type = 0;
    uint64_t record = 0;
    uint64_t submission = 0;
    uint64_t command = 0;
    uint64_t vi = 0;
    uint64_t hash = 0;
};

const char* const stateFieldNames[64] = {
    "fb_format", "fb_size", "fb_width", "fb_address", "fill_color",
    "clip_xh", "clip_yh", "clip_xl", "clip_yl", "cycle_type",
    "image_read_en", "z_update_en", "z_compare_en", "ti_format", "ti_size",
    "ti_width", "ti_address", "zb_address",
    "tile0_format", "tile0_size", "tile0_line", "tile0_tmem",
    "tile1_format", "tile1_size", "tile1_line", "tile1_tmem",
    "tile2_format", "tile2_size", "tile2_line", "tile2_tmem",
    "tile3_format", "tile3_size", "tile3_line", "tile3_tmem",
    "tile4_format", "tile4_size", "tile4_line", "tile4_tmem",
    "tile5_format", "tile5_size", "tile5_line", "tile5_tmem",
    "tile6_format", "tile6_size", "tile6_line", "tile6_tmem",
    "tile7_format", "tile7_size", "tile7_line", "tile7_tmem"
};

struct Layer {
    const char* name;
    bool perFrame;
};

uint64_t readLE64(const uint8_t* bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(bytes[i]) << (i * 8);
    return value;
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<uint8_t> read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(bool(input), ("Cannot open " + path.string()).c_str());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>{});
}

std::vector<Checkpoint> checkpoints(const fs::path& prefix) {
    const std::vector<uint8_t> bytes = read(prefix.parent_path() /
                                            (prefix.filename().string() + ".checkpoints"));
    require(bytes.size() % 48 == 0, "Invalid checkpoint record size.");
    std::vector<Checkpoint> result;
    for (size_t offset = 0; offset < bytes.size(); offset += 48) {
        Checkpoint item;
        item.type = readLE64(bytes.data() + offset);
        item.record = readLE64(bytes.data() + offset + 8);
        item.submission = readLE64(bytes.data() + offset + 16);
        item.command = readLE64(bytes.data() + offset + 24);
        item.vi = readLE64(bytes.data() + offset + 32);
        item.hash = readLE64(bytes.data() + offset + 40);
        result.push_back(item);
    }
    return result;
}

std::vector<uint8_t> states(const fs::path& prefix) {
    return read(prefix.parent_path() / (prefix.filename().string() + ".states"));
}

std::string typeName(uint64_t type) {
    if (type == 1) return "rdp";
    if (type == 2) return "vi";
    return "unknown";
}

std::string number(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%llu", static_cast<unsigned long long>(value));
    return buffer;
}

void printHeader(const char* status, const Options& options) {
    std::printf("{\"status\":\"%s\",\"format\":\"aurum-reference-diff/1\"", status);
    std::printf(",\"expected\":\"%s\",\"actual\":\"%s\"",
                options.expectedPrefix.string().c_str(), options.actualPrefix.string().c_str());
}

void printCoordinates(const std::string& layer, size_t index, size_t offset,
                      uint64_t stride = 0) {
    if (stride) {
        std::printf(",\"layer\":\"%s\",\"checkpoint\":%zu,\"x\":%llu,\"y\":%llu",
                    layer.c_str(), index,
                    static_cast<unsigned long long>(offset % stride),
                    static_cast<unsigned long long>(offset / stride));
    } else {
        std::printf(",\"layer\":\"%s\",\"checkpoint\":%zu,\"offset\":%zu",
                    layer.c_str(), index, offset);
    }
}

void printExpectedActual(uint8_t expected, uint8_t actual) {
    std::printf(",\"expected\":%u,\"actual\":%u}\n", expected, actual);
}

void printExpectedActual(uint32_t expected, uint32_t actual) {
    std::printf(",\"expected\":%u,\"actual\":%u}\n", expected, actual);
}

Options parse(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "Usage: aurum-reference-diff expected-prefix actual-prefix capture.aurcap\n");
        throw std::runtime_error("argument count");
    }
    return {argv[1], argv[2], argv[3]};
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse(argc, argv);
        const auto expectedCheckpoints = checkpoints(options.expectedPrefix);
        const auto actualCheckpoints = checkpoints(options.actualPrefix);
        const std::vector<uint8_t> expectedStates = states(options.expectedPrefix);
        const std::vector<uint8_t> actualStates = states(options.actualPrefix);
        const size_t commonCheckpoints = std::min(expectedCheckpoints.size(),
                                                   actualCheckpoints.size());
        for (size_t i = 0; i < commonCheckpoints; ++i) {
            const auto& expected = expectedCheckpoints[i];
            const auto& actual = actualCheckpoints[i];
            if (expected.type == actual.type && expected.hash == actual.hash) continue;
            uint64_t expectedField = 0, actualField = 0;
            const char* fieldName = "event-shape";
            expectedField = expected.type;
            actualField = actual.type;
            if (expectedStates.size() >= (i + 1) * 256 &&
                actualStates.size() >= (i + 1) * 256) {
                fieldName = "state-hash";
                expectedField = expected.hash;
                actualField = actual.hash;
                for (unsigned word = 0; word < 64; ++word) {
                    const size_t offset = i * 256 + word * 4;
                    uint64_t expectedValue = 0, actualValue = 0;
                    for (unsigned b = 0; b < 4; ++b) {
                        expectedValue |= uint64_t(expectedStates[offset + b]) << (b * 8);
                        actualValue |= uint64_t(actualStates[offset + b]) << (b * 8);
                    }
                    if (expectedValue == actualValue) continue;
                    expectedField = expectedValue;
                    actualField = actualValue;
                    fieldName = stateFieldNames[word];
                    break;
                }
            }
            printHeader("differ", options);
            std::printf(",\"firstDifference\":\"checkpoint\"");
            std::printf(",\"checkpointIndex\":%zu,\"type\":\"%s\"", i,
                        typeName(expected.type).c_str());
            std::printf(",\"record\":%s,\"submission\":%s,\"command\":%s,\"vi\":%s",
                        number(expected.record).c_str(), number(expected.submission).c_str(),
                        number(expected.command).c_str(), number(expected.vi).c_str());
            std::printf(",\"expectedHash\":\"%016llx\",\"actualHash\":\"%016llx\"",
                        static_cast<unsigned long long>(expected.hash),
                        static_cast<unsigned long long>(actual.hash));
            std::printf(",\"rdpState\":{\"field\":\"%s\",\"expected\":%llu,\"actual\":%llu}}\n",
                        fieldName, static_cast<unsigned long long>(expectedField),
                        static_cast<unsigned long long>(actualField));
            return 1;
        }
        if (expectedCheckpoints.size() != actualCheckpoints.size()) {
            printHeader("differ", options);
            std::printf(",\"firstDifference\":\"checkpoint-count\",\"expected\":%zu,\"actual\":%zu}\n",
                        expectedCheckpoints.size(), actualCheckpoints.size());
            return 1;
        }

        const Layer layers[] = {
            {"rdram", false}, {"hidden", false}, {"tmem", false},
            {"vi-color", true}, {"vi-depth", true}, {"vi-coverage", true}
        };
        for (const auto& [name, perFrame] : layers) {
            const size_t count = perFrame ? expectedCheckpoints.size() : 1;
            for (size_t i = 0; i < count; ++i) {
                const std::string suffix = perFrame
                    ? std::string(".") + name + "-" + std::to_string(i) + ".rgba8"
                    : std::string(".") + name;
                const fs::path expectedPath = options.expectedPrefix.parent_path() /
                                              (options.expectedPrefix.filename().string() + suffix);
                const fs::path actualPath = options.actualPrefix.parent_path() /
                                            (options.actualPrefix.filename().string() + suffix);
                std::error_code error;
                if (!fs::exists(expectedPath, error) || !fs::exists(actualPath, error)) continue;
                const std::vector<uint8_t> expectedBytes = read(expectedPath);
                const std::vector<uint8_t> actualBytes = read(actualPath);
                if (expectedBytes.size() != actualBytes.size()) {
                    printHeader("differ", options);
                    std::printf(",\"firstDifference\":\"layer-size\",\"layer\":\"%s\",\"checkpoint\":%zu,\"expected\":%zu,\"actual\":%zu}\n",
                                name, i, expectedBytes.size(), actualBytes.size());
                    return 1;
                }
                if (!perFrame) {
                    for (size_t offset = 0; offset < expectedBytes.size(); ++offset) {
                        if (expectedBytes[offset] == actualBytes[offset]) continue;
                        printHeader("differ", options);
                        std::printf(",\"firstDifference\":\"byte\"");
                        printCoordinates(name, i, offset);
                        printExpectedActual(expectedBytes[offset], actualBytes[offset]);
                        return 1;
                    }
                } else {
                    const fs::path geometryPath = options.expectedPrefix.parent_path() /
                        (options.expectedPrefix.filename().string() + ".geometry");
                    uint32_t width = 0;
                    if (std::vector<uint8_t> geometry = read(geometryPath);
                        geometry.size() >= (i + 1) * 8) {
                        width = uint32_t(geometry[i * 8]) | uint32_t(geometry[i * 8 + 1]) << 8 |
                                uint32_t(geometry[i * 8 + 2]) << 16 |
                                uint32_t(geometry[i * 8 + 3]) << 24;
                    }
                    for (size_t pixel = 0; pixel < expectedBytes.size() / 4; ++pixel) {
                        const uint32_t expectedPixel = uint32_t(expectedBytes[pixel * 4]) |
                                                       uint32_t(expectedBytes[pixel * 4 + 1]) << 8 |
                                                       uint32_t(expectedBytes[pixel * 4 + 2]) << 16 |
                                                       uint32_t(expectedBytes[pixel * 4 + 3]) << 24;
                        const uint32_t actualPixel = uint32_t(actualBytes[pixel * 4]) |
                                                     uint32_t(actualBytes[pixel * 4 + 1]) << 8 |
                                                     uint32_t(actualBytes[pixel * 4 + 2]) << 16 |
                                                     uint32_t(actualBytes[pixel * 4 + 3]) << 24;
                        if (expectedPixel == actualPixel) continue;
                        printHeader("differ", options);
                        std::printf(",\"firstDifference\":\"pixel\"");
                        printCoordinates(name, i, pixel, width);
                        printExpectedActual(expectedPixel, actualPixel);
                        return 1;
                    }
                }
            }
        }

        printHeader("match", options);
        std::printf(",\"checked\":{\"checkpoints\":%zu", expectedCheckpoints.size());
        std::printf(",\"rdram\":true,\"hidden\":true,\"tmem\":true");
        size_t viEvents = 0;
        for (const auto& checkpoint : expectedCheckpoints) {
            if (checkpoint.type == 2) ++viEvents;
        }
        std::printf(",\"viColor\":%zu,\"viDepth\":%zu,\"viCoverage\":%zu}}\n",
                    viEvents, viEvents, viEvents);
        return 0;
    } catch (const std::exception& error) {
        std::printf("{\"status\":\"error\",\"message\":\"%s\"}\n", error.what());
        return 2;
    }
}
