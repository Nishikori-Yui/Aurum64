// SPDX-License-Identifier: MIT
#include "AttributeProbe.hpp"
#include "TriangleShade.hpp"
#include "TextureDepthMath.h"
#include "TriangleTextureDepth.hpp"
#include <bit>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using namespace aurum::rdp;
using namespace aurum::attribute_probe;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static void generate(const char* path) {
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    std::ofstream output(path, std::ios::binary);
    writeMagic(output, kInputMagic);
    std::mt19937 random(0xa7741b);
    for (unsigned trial = 0; trial < 8192; ++trial) {
        Request request{};
        const unsigned opcode = 8 + trial % 8;
        const int32_t yh = int32_t(random() % 24) - 8;
        const int32_t ym = yh + 16 + random() % 8;
        const int32_t yl = ym + 24;
        request[0] = (opcode << 24) | (((trial / 8) & 1u) << 23) | (yl & 0x3fff);
        request[1] = ((ym & 0x3fff) << 16) | (yh & 0x3fff);
        for (unsigned word : {2u, 4u, 6u}) request[word] = random() & 0xfffffu;
        for (unsigned word : {3u, 5u, 7u}) request[word] = random();
        if (trial % 3) {
            // Keep most slopes small and many rows nonempty, but retain raw
            // signed derivative limits and wrap in the remaining cases.
            for (unsigned word : {3u, 5u, 7u}) request[word] = uint32_t(int32_t(random() % 131072) - 65536);
        }
        for (size_t word = 8; word < Decoder::wordCount(opcode); ++word) request[word] = random();
        request[44] = 0xed000000u | ((random() % 8) << 12) | (random() % 4);
        request[45] = (80u << 12) | 64u;
        request[46] = 1 + (trial / 16) % 7;
        writeWords(output, request);
    }
    output.flush(); require(bool(output), "Write attribute cases");
}

static void compare(const char* inputPath, const char* referencePath) {
    std::ifstream input(inputPath, std::ios::binary), reference(referencePath, std::ios::binary);
    readMagic(input, kInputMagic); readMagic(reference, kOutputMagic);
    Request request;
    size_t cases = 0, valid = 0;
    std::array<unsigned, 8> perOpcode{};
    while (readWords(input, request)) {
        require(++cases <= kMaxCases, "Too many attribute cases");
        Response expected;
        require(readWords(reference, expected), "Missing attribute reference record");
        const uint32_t opcode = (request[0] >> 24) & 63;
        require(opcode >= 8 && opcode <= 15 && request[46] < 1024, "Invalid attribute request");
        require((request[44] >> 24) == 0xed && !(request[45] & 0xff000000u) &&
                ((request[44] >> 12) & 4095) <= ((request[45] >> 12) & 4095) &&
                (request[44] & 4095) <= (request[45] & 4095), "Invalid attribute scissor");
        Decoder decoder;
        Triangle triangle;
        decoder.triangle = [&](const Triangle& value) { triangle = value; return true; };
        decoder.push(std::span(request).first(Decoder::wordCount(opcode)));
        auto spans = generateTriangleSpans(triangle);
        clipTriangleSpans(spans, (request[44] >> 12) & 4095, request[44] & 4095,
                          (request[45] >> 12) & 4095, request[45] & 4095);
        const auto row = std::find_if(spans.begin(), spans.end(), [&](const auto& span) {
            return span.y == int32_t(request[46]);
        });
        if (!expected[0]) continue; // Unused intermediate registers are undefined.
        if (row == spans.end() || !row->coverage) {
            std::fprintf(stderr, "case=%zu y=%u yh=%d ym=%d yl=%d coverage=%u left=%d right=%d\n",
                cases - 1, request[46], triangle.yh, triangle.ym, triangle.yl,
                row == spans.end() ? 0 : row->coverage, row == spans.end() ? 0 : row->left,
                row == spans.end() ? 0 : row->right);
            throw std::runtime_error("Reference row missing in local spans");
        }
        ++valid; ++perOpcode[opcode - 8];
        const auto setup = makeShadeParameters(triangle);
        Response actual{};
        actual[0] = 1;
        actual[1] = attributeAnchorX(setup, row->y);
        actual[2] = shadeScanStart(triangle, *row);
        std::array<TriangleAttribute, 8> attributes{};
        std::copy(triangle.rgba.begin(), triangle.rgba.end(), attributes.begin());
        std::copy(triangle.stw.begin(), triangle.stw.end(), attributes.begin() + 4);
        attributes[7] = triangle.depth;
        for (unsigned channel = 0; channel < 8; ++channel) {
            const auto a = attributes[channel];
            actual[3 + channel] = attributeRow({a.initial, a.dx, a.de, a.dy}, setup, row->y);
            actual[11 + channel] = uint32_t(a.dx) & (channel == 7 ? ~0u : ~31u);
        }
        for (unsigned channel = 0; channel < 3; ++channel)
            actual[19 + channel] = uint32_t(triangle.stw[channel].dy) & ~32767u;
        actual[22] = affineTextureCoordinate(actual[7]);
        actual[23] = affineTextureCoordinate(actual[8]);
        actual[24] = nonAADepth(actual[10]);
        for (size_t field = 0; field < actual.size(); ++field) if (actual[field] != expected[field]) {
            std::fprintf(stderr, "case=%zu opcode=%u field=%zu expected=%08x actual=%08x\n",
                         cases - 1, opcode, field, expected[field], actual[field]);
            throw std::runtime_error("Independent attribute mismatch");
        }
        // Advance the externally observed row registers one step at a time.
        // This tests clipped per-pixel stepping independently of the helper's
        // direct evaluation, including depth's unmasked low derivative bits.
        const int32_t direction = triangle.leftMajor ? 1 : -1;
        const int32_t start = std::bit_cast<int32_t>(expected[2]);
        const int32_t anchor = std::bit_cast<int32_t>(expected[1]);
        const uint32_t skip = uint32_t(direction * (start - anchor)) & 4095u;
        std::array<uint32_t, 4> accumulators{expected[7], expected[8], expected[9], expected[10]};
        for (unsigned step = 0; step < skip; ++step)
            for (unsigned channel = 0; channel < 4; ++channel)
                accumulators[channel] += uint32_t(direction) * expected[15 + channel];
        for (int step = 0; step < 5; ++step) {
            const auto sample = interpolateTextureDepth(triangle, *row, start + direction * step, false);
            for (unsigned channel = 0; channel < 3; ++channel)
                require(sample.stw[channel] == accumulators[channel], "Clipped STW stepping mismatch");
            require(sample.z == accumulators[3], "Clipped Z stepping mismatch");
            for (unsigned channel = 0; channel < 4; ++channel)
                accumulators[channel] += uint32_t(direction) * expected[15 + channel];
        }
    }
    Response extra;
    require(!readWords(reference, extra), "Trailing attribute reference record");
    require(cases >= 8192 && valid > 1000, "Vacuous attribute differential");
    for (const auto count : perOpcode) require(count > 100, "Missing opcode attribute coverage");
    std::printf("PASS: %zu attribute requests; %zu valid rows, all eight opcodes match independent setup\n", cases, valid);
}

static void unitTests() {
    ShadeParameters setup{}; setup.leftMajor = 1;
    const ShadeChannel a{0x12345, 31, 0, 0};
    require(interpolateTexture(a, setup, 1, 0, 0) == 0x12000 &&
            interpolateDepth(a, setup, 1, 0, 0) == 0x1201f, "Depth must preserve low X derivative bits");
    require(affineTextureCoordinate(0xffff8000) == 0x1ffff &&
            affineTextureCoordinate(0x80000000) == 0x18000, "Affine signed TC bus");
    require(nonAADepth(0x7fffe000) == 0x3ffff && nonAADepth(0x80000000) == 0x3ffff &&
            nonAADepth(0xc0000000) == 0 && nonAADepth(0xffffffff) == 0, "Non-AA depth clamp");
    Triangle triangle;
    triangle.leftMajor = true; triangle.yh = 0; triangle.ym = triangle.yl = 32;
    triangle.xh = 0; triangle.xm = 8 << 16; triangle.xl = 8 << 16;
    triangle.stw[0] = {32 << 16, 32 << 16, 0, 0};
    triangle.stw[1] = {64 << 16, 0, 32 << 16, 32 << 16};
    triangle.stw[2] = {0x40000000, 0, 0, 0};
    triangle.depth = {0x01000000, 0x2000, 0, 0};
    const auto rows = generateTriangleSpans(triangle);
    const auto affine = interpolateTextureDepth(triangle, rows[2], 3, false);
    const auto perspective = interpolateTextureDepth(triangle, rows[2], 3, true);
    require(affine.coordinate.s == 128 && affine.coordinate.t == 128 &&
            perspective.coordinate.s == 256 && perspective.coordinate.t == 256 &&
            affine.correctedZ == 2051, "STW/divider/depth integrated control");
}

int main(int argc, char** argv) {
    try {
        unitTests();
        if (argc == 3 && std::string(argv[1]) == "--generate") generate(argv[2]);
        else if (argc == 4 && std::string(argv[1]) == "--compare") compare(argv[2], argv[3]);
        else throw std::runtime_error("Usage: aurum-attribute-tests --generate path | --compare input reference");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
