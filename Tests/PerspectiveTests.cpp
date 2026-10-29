// SPDX-License-Identifier: MIT
#include "PerspectiveProbe.hpp"
#include "PerspectiveMath.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace aurum::perspective_probe;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static void generate(const char* path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream output(path, std::ios::binary);
    writeMagic(output, kInputMagic);
    std::mt19937 random(0xc001d1);
    const uint32_t controls[] = {0, 1, 0xffff, 0x7fff, 0x8000, 0x4000, 0xc000};
    for (uint32_t w = 0; w < 65536; ++w) {
        for (uint32_t s : controls) writeWords(output, Request{1, s, uint32_t(random()), w});
        writeWords(output, Request{1, uint32_t(random()), uint32_t(random()), w});
        writeWords(output, Request{0, w, uint32_t(random()), uint32_t(random())});
    }
    output.flush(); require(bool(output), "Cannot write perspective probes");
}

static void compare(const char* inputPath, const char* referencePath) {
    std::ifstream input(inputPath, std::ios::binary), reference(referencePath, std::ios::binary);
    readMagic(input, kInputMagic); readMagic(reference, kOutputMagic);
    Request request;
    Response expected;
    size_t count = 0;
    while (readWords(input, request)) {
        require(++count <= kMaxCases && request[0] <= 1, "Invalid perspective request");
        require(readWords(reference, expected), "Missing perspective reference");
        const auto actual = divideTexture(request[1], request[2], request[3], request[0]);
        if (actual.s != expected[0] || actual.t != expected[1]) {
            std::fprintf(stderr, "case=%zu mode=%u S=%08x T=%08x W=%08x expected=%05x,%05x actual=%05x,%05x\n",
                count - 1, request[0], request[1], request[2], request[3], expected[0], expected[1], actual.s, actual.t);
            throw std::runtime_error("Independent perspective mismatch");
        }
    }
    require(!readWords(reference, expected), "Trailing perspective reference");
    require(count == 9 * 65536, "Incomplete perspective matrix");
    std::printf("PASS: %zu independent perspective/affine cases, all signed W values\n", count);
}

static void controls() {
    const auto unitW = divideTexture(32, 64, 0x4000, true);
    require(unitW.s == 64 && unitW.t == 128, "Normalized W scale");
    const auto zeroW = divideTexture(0, 0, 0, true);
    require(zeroW.s == 0x40000 && zeroW.t == 0x40000, "Zero W carry flags");
    const auto negativeW = divideTexture(0, 0, 0xffff, true);
    require(negativeW.s == 0x40000 && negativeW.t == 0x40000, "Negative W carry flags");
    const auto segmentSix = divideTexture(0, 0xe2e5d4f9u, 35, true);
    require(segmentSix.s == 0 && segmentSix.t == 0x2a656, "Reciprocal segment-six quantization");
    const auto affine = divideTexture(0xffff, 0x8000, 0, false);
    require(affine.s == 0x1ffff && affine.t == 0x18000, "Affine mode bypasses W");
}

int main(int argc, char** argv) {
    try {
        controls();
        if (argc == 3 && std::string(argv[1]) == "--generate") generate(argv[2]);
        else if (argc == 4 && std::string(argv[1]) == "--compare") compare(argv[2], argv[3]);
        else throw std::runtime_error("Usage: aurum-perspective-tests --generate path | --compare input reference");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
