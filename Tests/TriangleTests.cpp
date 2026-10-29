// SPDX-License-Identifier: MIT
#include "Capture.hpp"
#include "MemoryBus.hpp"
#include "TriangleMode.hpp"
#include "TrianglePlan.hpp"
#include "TriangleReference.hpp"
#include "TriangleColor.hpp"
#include "TriangleDepth.hpp"
#include "TriangleTexture.hpp"
#include "TriangleTextureDepth.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

using namespace aurum::rdp;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static uint32_t little(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

static void attributeTests() {
    const std::array<TriangleAttribute, 4> rgba = {{
        {0x12345678, -1, INT32_MIN, INT32_MAX},
        {-32769, 0x01020304, -0x12345678, 9},
        {0x76543210, -0x10203040, 0x13572468, -17},
        {0x00ff1111, 0x00112233, -0x00334455, -65536}}};
    const std::array<TriangleAttribute, 3> stw = {{rgba[2], rgba[0], rgba[1]}};
    const TriangleAttribute depth{INT32_MAX, INT32_MIN, 0x55667788, -1};
    // Test encoder packs every field separately, using different values for
    // each channel and derivative so displaced halves cannot pass by accident.
    auto pack = [](auto attributes) {
        std::array<uint32_t, 16> block{};
        for (size_t channel = 0; channel < attributes.size(); ++channel) {
            const auto a = attributes[channel];
            const int32_t values[] = {a.initial, a.dx, a.de, a.dy};
            const unsigned groups[] = {0, 2, 8, 10};
            for (unsigned field = 0; field < 4; ++field) {
                const unsigned index = groups[field] + channel / 2;
                const unsigned shift = channel & 1 ? 0 : 16;
                const uint32_t bits = uint32_t(values[field]);
                block[index] |= (bits >> 16) << shift;
                block[index + 4] |= (bits & 65535u) << shift;
            }
        }
        return block;
    };
    for (unsigned opcode = 8; opcode < 16; ++opcode) {
        std::vector<uint32_t> words{(opcode << 24) | 0x00eb0010u, 0x00080000,
            8u << 16, 0, 2u << 16, 0xffffffffu, 8u << 16, 0};
        if (opcode & 4) { auto block = pack(rgba); words.insert(words.end(), block.begin(), block.end()); }
        if (opcode & 2) { auto block = pack(stw); words.insert(words.end(), block.begin(), block.end()); }
        if (opcode & 1) for (int32_t value : {depth.initial, depth.dx, depth.de, depth.dy})
            words.push_back(uint32_t(value));
        require(words.size() == Decoder::wordCount(opcode), "Attribute payload size");
        for (size_t split = 1; split < words.size(); ++split) {
            Decoder decoder;
            unsigned calls = 0;
            Triangle captured;
            decoder.triangle = [&](const Triangle& value) { ++calls; captured = value; return true; };
            decoder.push(std::span(words).first(split));
            require(calls == 0 && decoder.hasPendingWords(), "Incomplete attribute payload dispatched");
            decoder.push(std::span(words).subspan(split));
            require(calls == 1 && !decoder.hasPendingWords() && captured.leftMajor &&
                    captured.majorSlopeNegative && captured.tile == 3 && captured.level == 5,
                    "Triangle attribute header/fragment decode");
            require(captured.rgba == ((opcode & 4) ? rgba : std::array<TriangleAttribute, 4>{}),
                    "RGBA integer/fraction half decode");
            require(captured.stw == ((opcode & 2) ? stw : std::array<TriangleAttribute, 3>{}),
                    "STW offset after optional shade block");
            require(captured.depth == ((opcode & 1) ? depth : TriangleAttribute{}), "Z block offset");
        }
    }
    ShadeParameters setup{};
    setup.leftMajor = 1;
    const ShadeChannel alpha{128 << 16, 1 << 16, 2 << 16, 0};
    require(shadeChannel(alpha, setup, 3, 2, 0) == 135, "Alpha X/edge interpolation");
    setup.xh = 8 << 16; setup.leftMajor = 0; setup.offset = 1;
    setup.rgba[0] = {128 << 16, 16 << 16, 0, 0};
    setup.rgba[1] = {64 << 16, 0, 8 << 16, 8 << 16};
    setup.rgba[2] = {32 << 16, 0, 0, 0};
    for (int y = 0; y < 4; ++y) for (int x = 2; x < 8; ++x)
        require(shadeRGB16(setup, x, y, 8) == uint32_t((x * 2 << 11) | ((8 + y) << 6) | 8),
                "X/Y gradient closed-form control");
    setup = {}; setup.leftMajor = 1;
    for (const auto& [input, expected] : std::array<std::pair<int32_t, uint32_t>, 4>{{
             {-1, 0}, {255 << 16, 255}, {300 << 16, 255}, {400 << 16, 0}}}) {
        require(shadeChannel({input, 0, 0, 0}, setup, 0, 0, 0) == expected, "Nine-bit clamp");
    }
}

static void unitTests() {
    require(depthCompress(0x5400) == 0x540 &&
            depthDecompress(depthCompress(0x5400)) == 0x5400 &&
            depthDeltaCompress(0x100) != 0 &&
            depthOpaquePass(0x1234, 0xffff, 0, 1, 0),
            "Depth compression and opaque compare controls");
    Decoder decoder;
    Triangle triangle;
    decoder.triangle = [&](const Triangle& value) { triangle = value; return true; };
    const uint32_t state[] = {0xff10000f, 0x1000, 0xed000000, 0x400020,
        0xef0000f0, 0, 0xfcffffff, 0xfffdf6fb, 0xfa000000, 0x85432700};
    const uint32_t command[] = {0xc8800010, 0x80000, 8u << 16, 0, 2u << 16, 0, 8u << 16, 0};
    const uint32_t blend[] = {0x39000000, 0x00000080};
    decoder.push(state);
    decoder.push(blend);
    for (const auto& word : command) decoder.push(std::span(&word, 1));
    require(triangle.leftMajor && isFlatPrimitiveTriangle(triangle), "Left-major mode decode");
    require(primitiveColor16(triangle.primitiveColor) == 0x8208, "Primitive RGB quantization");
    require(triangle.blendColorValid && triangle.blendColor == 0x80, "Blend color snapshot");
    const Triangle valid = triangle;
    Triangle shadeMode = triangle;
    shadeMode.shade = true; shadeMode.combineLo = kShadeCombineLo;
    shadeMode.primitiveColorValid = false;
    require(isShadeTriangle(shadeMode) && isColorTriangle(shadeMode), "Shade mode needs no primitive state");
    shadeMode.texture = true; require(!isColorTriangle(shadeMode), "Textured shade accepted");
    shadeMode.texture = false; shadeMode.z = true;
    require(!isColorTriangle(shadeMode), "Unimplemented Z mode accepted");
    shadeMode.z = false; shadeMode.otherModesLo = 8;
    require(!isColorTriangle(shadeMode), "AA shade used non-AA centroid assumption");
    shadeMode.otherModesLo = 1;
    require(!isColorTriangle(shadeMode), "Shade alpha compare accepted");
    shadeMode.otherModesLo = 0; shadeMode.otherModesHi = 0;
    require(!isColorTriangle(shadeMode), "Shade dithering accepted");
    shadeMode.otherModesHi = kFlatPrimitiveModesHi;
    shadeMode.combineHi = kFlatPrimitiveCombineHi;
    shadeMode.combineLo = kShadeCombineLo;
    require(isSimpleCombinerTriangle(shadeMode), "Shade combiner C1 gate");
    Triangle shadeTextureMode = shadeMode;
    shadeTextureMode.texture = true;
    shadeTextureMode.otherModesHi = 0x8f0;
    shadeTextureMode.combineHi = (1u << 5) | 4u;
    shadeTextureMode.combineLo = (15u << 24) | (1u << 21) | (4u << 18) |
        (7u << 6) | (7u << 3) | 7u;
    shadeTextureMode.primitiveColorValid = false;
    shadeTextureMode.tileFormat = 0; shadeTextureMode.tileSize = 2;
    require(isSimpleCombinerTriangle(shadeTextureMode), "Shade-texture combiner C1 gate");
    Triangle alphaMode = valid;
    alphaMode.otherModesLo = 1;
    alphaMode.blendColorValid = true;
    require(isSimpleCombinerTriangle(alphaMode), "Alpha compare threshold gate");
    alphaMode.blendColorValid = false;
    require(!isSimpleCombinerTriangle(alphaMode), "Alpha compare without threshold accepted");
    auto reject = [&](Triangle value) { require(!isFlatPrimitiveTriangle(value), "Invalid mode accepted"); };
    triangle.cycleType = 1; reject(triangle); triangle = valid;
    triangle.otherModesHi = 0x100000; reject(triangle); triangle = valid;
    triangle.combineHi = 0x333c00; triangle.combineLo = 0x881bffff; reject(triangle); triangle = valid;
    triangle.combineValid = false; reject(triangle); triangle = valid;
    triangle.primitiveColorValid = false; reject(triangle); triangle = valid;
    triangle.shade = true; reject(triangle); triangle = valid;
    triangle.texture = true; reject(triangle); triangle = valid;
    triangle.z = true; reject(triangle); triangle = valid;
    triangle.otherModesLo = 8; reject(triangle); triangle = valid;
    triangle.scissorMode = 1; reject(triangle); triangle = valid;
    triangle.fbSize = 3; reject(triangle);
    decoder.reset(); decoder.push(command);
    require(!triangle.combineValid && !triangle.primitiveColorValid, "Reset retained draw state");
    require(valid.combineValid && valid.primitiveColor == 0x85432700, "Snapshot mutated");

    auto spans = generateTriangleSpans(valid);
    require(spans.size() == 4 && trianglePixelCoverage(spans[0], 2) == 255 &&
            trianglePixelCoverage(spans[0], 8) == 0, "Eight-sample edge ownership");
    clipTriangleSpans(spans, 0, 0, 9, 32);
    require(trianglePixelCoverage(spans[0], 2) == 0x88 &&
            triangleCoverageColor(0x8208, 0x88) == 0x8208, "Fractional clip/coverage bit");
    Triangle segmented;
    segmented.yh = 1; segmented.ym = 5; segmented.yl = 13;
    segmented.xh = 0; segmented.xm = 4 << 16; segmented.xl = 8 << 16;
    segmented.dxhdy = 4 << 16; segmented.dxmdy = 2 << 16; segmented.dxldy = 1 << 16;
    segmented.leftMajor = true;
    const auto rows = generateTriangleSpans(segmented);
    require(rows.size() == 4 && rows[0].y == 0 && rows[0].coverage == 14 &&
            trianglePixelCoverage(rows[0], 4) == 0x2f && !rows[3].valid,
            "YH integer-row anchor, YM switch and crossed lower edges");
    require((trianglePixelCoverage(rows[0], 4) & 0x80) == 0,
            "Partial top row incorrectly passed non-AA sample test");
    segmented.yh = segmented.yl;
    require(generateTriangleSpans(segmented).empty(), "Degenerate Y range");
    std::vector<TriangleWord> plan;
    std::string error;
    require(!makeFlatTrianglePlan(valid, (8 << 20) + 4, 0, 16, 2, 0, 0, 0, 64, 32, plan, error),
            "Oversize memory accepted");
    require(!makeFlatTrianglePlan(valid, 16383, 0, 16, 2, 0, 0, 0, 64, 32, plan, error),
            "Partial host word accepted");
    require(makeFlatTrianglePlan(valid, 16384, 0, 16, 2, 0, 0, 0, 0, 32, plan, error) && plan.empty(),
            "Empty scissor should be a no-op");
    std::vector<uint8_t> memory(0x100, 0x5a);
    const auto before = memory;
    require(!drawFlatTriangleReference(memory.data(), memory.size(), 0xf0, 16, 2, 0,
                valid, 0, 0, 64, 32, error) && memory == before, "Partial out-of-bounds write");

    memory.assign(0x4000, 0x5a);
    const auto seed = memory;
    decoder.reset();
    decoder.triangle = [&](const Triangle& draw) {
        if (!isFlatPrimitiveTriangle(draw)) return false;
        return drawFlatTriangleReference(memory.data(), memory.size(), draw.fbAddress,
            draw.fbWidth, draw.fbSize, primitiveColor16(draw.primitiveColor), draw,
            draw.clipXh, draw.clipYh, draw.clipXl, draw.clipYl, error);
    };
    decoder.push(state);
    const uint32_t legacy[] = {0xef100000, 0, 0xfc333c00, 0x881bffff};
    decoder.push(legacy); decoder.push(command);
    require(decoder.unsupported == 1 && memory == seed, "Legacy configuration did not fail closed");
    decoder.push(state); decoder.push(command);
    require(decoder.unsupported == 1 && memory != seed, "Corrected configuration did not draw");
}

static void depthTests() {
    Decoder decoder;
    Triangle triangle;
    decoder.triangle = [&](const Triangle& value) { triangle = value; return true; };
    const uint32_t state[] = {0xff10000f, 0x1000, 0xfe000000, 0x2000,
        0xed000000, 0x400020, 0xef0000f0, 0x24,
        0xfcffffff, 0xfffdf6fb, 0xfa000000, 0x854327ff,
        0xee000000, 0x12348000};
    const uint32_t command[] = {0xc8800010, 0x80000, 8u << 16, 0,
        2u << 16, 0, 8u << 16, 0};
    decoder.push(state);
    decoder.push(command);
    const Triangle valid = triangle;
    require(isDepthUpdateTriangle(valid) && !valid.z && valid.primitiveDepthValid &&
            valid.primitiveDepth == 0x12348000 && valid.zbAddress == 0x2000,
            "SetPrimDepth/Z-image issue-time state");
    const auto parameters = makeTriangleDepthParameters(valid);
    require(parameters.dzPixel == 0x8000 && parameters.dzEncoded == 15 &&
            triangleDepthValue(parameters, 2, 0, 2) == 0x91a0,
            "Primitive Z/delta encoding");
    const uint32_t mutation[] = {0xee000000, 0x76540004, 0xfe000000, 0x3000};
    decoder.push(mutation); decoder.push(command);
    require(triangle.primitiveDepth == 0x76540004 && triangle.zbAddress == 0x3000 &&
            valid.primitiveDepth == 0x12348000 && valid.zbAddress == 0x2000,
            "Primitive depth snapshot mutation");
    decoder.reset(); decoder.push(command);
    require(!triangle.primitiveDepthValid && triangle.primitiveDepth == 0,
            "Reset retained primitive depth");
    require(depthPixelDelta(0, 0) == 1 && depthPixelDelta(1 << 16, 0) == 3 &&
            depthPixelDelta(-65536, 0) == 1 && depthPixelDelta(-131072, 0) == 3 &&
            depthPixelDelta(4 << 16, 2 << 16) == 8 &&
            depthPixelDelta(INT32_MIN, INT32_MIN) == 0x8000,
            "Depth derivative normalization boundaries");
    std::vector<TriangleWord> plan;
    std::string error;
    require(makeOpaqueDepthPlan(valid, 0x4000, plan, error) && !plan.empty(),
            "Primitive depth plan rejected");
    using Mutation = void (*)(Triangle&);
    const Mutation rejected[] = {
        +[](Triangle& t) { t.primitiveDepthValid = false; },
        +[](Triangle& t) { t.otherModesLo |= 0x10; },
        +[](Triangle& t) { t.otherModesLo |= 8; },
        +[](Triangle& t) { t.otherModesLo |= 1; },
        +[](Triangle& t) { t.otherModesLo |= 0x400; },
        +[](Triangle& t) { t.zbAddress = 0x3ff0; },
        +[](Triangle& t) { t.zbAddress = t.fbAddress; },
        +[](Triangle& t) { t.zbAddress = t.fbAddress + 4; },
        +[](Triangle& t) { t.zbAddress += 2; },
        +[](Triangle& t) { t.zbAddress += 1; },
    };
    for (const auto change : rejected) {
        auto item = valid;
        change(item);
        require(!makeOpaqueDepthPlan(item, 0x4000, plan, error) && plan.empty(),
                "Invalid depth plan accepted or left partial writes");
    }
}

static void textureTests() {
    Decoder decoder;
    Triangle triangle;
    decoder.triangle = [&](const Triangle& value) { triangle = value; return true; };
    const uint32_t state[] = {0xff10000f, 0x1000, 0xed000000, (64u << 12) | 32u,
        0xef0008f0, 0, 0xfcffffff, 0xfffcf279,
        0xf5100880, 0x03080200, 0xf2000000, 0x0301c01c};
    std::array<uint32_t, 24> command{};
    command[0] = 0xca830010; command[1] = 0x00080000;
    command[2] = command[6] = 8 << 16; command[4] = 2 << 16;
    decoder.push(state); decoder.push(command);
    require(isTexturedTriangle(triangle) && triangle.tile == 3 && triangle.tileSize == 2 &&
            triangle.tileLine == 4 && triangle.tileTmem == 128 &&
            triangle.tileSh == 28 && triangle.tileTh == 28 && triangle.tileModes == 0x80200,
            "Triangle issue-time tile snapshot");
    const auto saved = triangle;
    const uint32_t changed[] = {0xf5100200, 0x03000000, 0xf2001002, 0x03003004};
    decoder.push(changed); decoder.push(command);
    require(saved.tileLine == 4 && saved.tileTmem == 128 && triangle.tileLine == 1 &&
            triangle.tileTmem == 0 && triangle.tileSl == 1 && triangle.tileTl == 2,
            "Tile mutation altered captured triangle or failed to update next draw");
    triangle = saved;
    std::vector<TriangleWord> plan;
    std::string error;
    require(makeTexturedTrianglePlan(triangle, 0x4000, plan, error) && !plan.empty(),
            "Valid textured plan rejected");
    triangle.stw[1].initial = 7 * 32 << 16;
    triangle.tileTmem = 511;
    require(!makeTexturedTrianglePlan(triangle, 0x4000, plan, error) && plan.empty(),
            "Out-of-range TMEM left a partial textured plan");
    for (unsigned mutation = 0; mutation < 8; ++mutation) {
        triangle = saved;
        switch (mutation) {
            case 0: triangle.shade = true; break;
            case 1: triangle.z = true; break;
            case 2: triangle.tileFormat = 2; break;
            case 3: triangle.otherModesHi |= 1u << 13; break;
            case 4: triangle.otherModesHi |= 1u << 16; break;
            case 5: triangle.otherModesLo = 1; break;
            case 6: triangle.combineLo ^= 1; break;
            case 7: triangle.cycleType = 1; break;
        }
        require(!makeTexturedTrianglePlan(triangle, 0x4000, plan, error) && plan.empty(),
                "Unimplemented textured mode accepted");
    }
    require(clampTextureBus(0x08000) == 32767 && clampTextureBus(0x10000) == -32768 &&
            clampTextureBus(0x20000) == -32768 && clampTextureBus(0x60000) == 32767,
            "Perspective overflow clamp priority");
}

static void compare(const char* capture, const char* reference) {
    std::ifstream input(capture, std::ios::binary);
    std::vector<aurum::capture::Record> records;
    std::string error;
    require(aurum::capture::read(input, records, error), error.c_str());
    std::vector<uint8_t> memory;
    std::map<uint32_t, uint8_t> writtenCoverage;
    Decoder decoder;
    aurum::texture::Uploader textures;
    decoder.textureLoad = [&](const TextureLoad& load) { return textures.load(memory.data(), memory.size(), load); };
    size_t draws = 0, pixels = 0, shadedDraws = 0, texturedDraws = 0;
    decoder.triangle = [&](const Triangle& triangle) {
        const bool depth = isDepthUpdateTriangle(triangle);
        const bool simple = isSimpleCombinerTriangle(triangle);
        require(depth || simple, "Differential fixture mode rejected");
        if (triangle.shade) ++shadedDraws;
        if (triangle.texture) ++texturedDraws;
        std::vector<TriangleWord> plan;
        require(depth ? makeOpaqueDepthPlan(triangle, memory.size(), plan, error) :
                        makeTriangleColorPlan(triangle, memory.size(), plan, error), error.c_str());
        auto oracle = memory;
        uint32_t previous = 0;
        bool first = true;
        std::set<std::pair<int32_t, int32_t>> acceptedPixels;
        const auto parameters = makeTriangleColorParameters(triangle);
        const auto depthParameters = makeTriangleDepthParameters(triangle);
        for (auto word : plan) {
            uint32_t writeMask = 0;
            if (depth) {
                uint32_t oracleMask = 0;
                require(opaqueDepthWord(memory.data(), memory.size(), depthParameters, word, writeMask),
                        "Scalar depth evaluation");
                require(opaqueDepthWord(oracle.data(), oracle.size(), depthParameters, word, oracleMask) &&
                            oracleMask == writeMask, "Scalar depth oracle evaluation");
            } else {
                uint16_t high = word.high, low = word.low;
                require(triangleColorWord(parameters, triangle.texture ? &textures.store() : nullptr,
                                           word, high, low, writeMask), "Scalar combiner evaluation");
                const bool highWrite = word.high != UINT32_MAX && (writeMask & 1u);
                const bool lowWrite = word.low != UINT32_MAX && (writeMask & 2u);
                if (highWrite) {
                    high = uint16_t((high & 0xfffeu) | (word.high & 1u));
                    acceptedPixels.emplace(word.highX, word.highY);
                    ++pixels;
                }
                if (lowWrite) {
                    low = uint16_t((low & 0xfffeu) | (word.low & 1u));
                    acceptedPixels.emplace(word.lowX, word.lowY);
                    ++pixels;
                }
                require(first || word.word > previous, "Duplicate/out-of-order GPU word ownership");
                first = false; previous = word.word;
                const auto apply = [&](std::vector<uint8_t>& target) {
                    uint32_t value;
                    require(aurum::memory::readWord(target.data(), target.size(), word.word * 4, value),
                            "Plan read");
                    if (highWrite) value = (value & 0xffff) | (uint32_t(high) << 16);
                    if (lowWrite) value = (value & 0xffff0000) | low;
                    require(aurum::memory::writeWord(target.data(), target.size(), word.word * 4, value),
                            "Plan write");
                };
                apply(memory);
                apply(oracle);
                continue;
            }
            if (writeMask & 1u) {
                acceptedPixels.emplace(word.highX, word.highY);
                ++pixels;
            }
            if (writeMask & 2u) {
                acceptedPixels.emplace(word.lowX, word.lowY);
                ++pixels;
            }
            require(first || word.word > previous, "Duplicate/out-of-order GPU word ownership");
            first = false; previous = word.word;
        }
        auto spans = generateTriangleSpans(triangle);
        clipTriangleSpans(spans, triangle.clipXh, triangle.clipYh, triangle.clipXl, triangle.clipYl);
        for (const auto& span : spans) {
            if (!span.valid) continue;
            for (int32_t x = span.left; x < span.right; ++x) {
                const uint8_t mask = trianglePixelCoverage(span, x);
                if ((mask & 0x80) && acceptedPixels.contains({x, span.y})) {
                    const uint32_t halfword = triangle.fbAddress / 2 + span.y * triangle.fbWidth + x;
                    writtenCoverage[halfword] = uint8_t((std::popcount(mask) - 1) & 3);
                }
            }
        }
        require(memory == oracle, "Plan and scalar coverage writes differ");
        ++draws;
        return true;
    };
    for (const auto& record : records) {
        if (record.type == aurum::capture::kRecordInitial) {
            require(record.payload.size() == 8 && memory.empty(), "Initial record");
            const auto size = little(record.payload.data());
            require(size == (4 << 20) || size == (8 << 20), "Memory size");
            memory.resize(size);
        } else if (record.type == aurum::capture::kRecordMemory) {
            aurum::capture::MemoryPayload payload;
            require(aurum::capture::parseMemoryPayload(record, payload) &&
                    aurum::capture::applyMemoryPayload(payload, memory), "Memory record");
        } else if (record.type == aurum::capture::kRecordRDP) {
            require(record.payload.size() >= 16 && record.payload.size() % 4 == 0, "RDP record size");
            const uint32_t start = little(record.payload.data()), end = little(record.payload.data() + 4);
            require(start <= end && end <= memory.size() && end - start == record.payload.size() - 16,
                    "RDP record range");
            std::vector<uint32_t> words;
            for (size_t offset = 16; offset < record.payload.size(); offset += 4) {
                const uint8_t* p = record.payload.data() + offset;
                const uint32_t word = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
                words.push_back(word);
                require(aurum::memory::writeWord(memory.data(), memory.size(), start + offset - 16, word), "Command write");
            }
            // Fragment every command to test the production decoder, not a parallel parser.
            decoder.beginSubmission();
            for (const auto& word : words) decoder.push(std::span(&word, 1));
        }
    }
    require(draws >= 256 && pixels > 1000 && decoder.unsupported == 0 && !decoder.hasPendingWords(),
            "Vacuous or incomplete triangle differential");
    std::ifstream expectedFile(reference, std::ios::binary);
    std::vector<uint8_t> expected((std::istreambuf_iterator<char>(expectedFile)), {});
    require(expected.size() == memory.size(), "Reference length");
    for (size_t i = 0; i < memory.size(); ++i) {
        if (memory[i] != expected[i]) {
            std::fprintf(stderr, "byte=%zu expected=%02x actual=%02x\n", i, expected[i], memory[i]);
            throw std::runtime_error("Independent triangle RDRAM differential");
        }
    }
    std::string hiddenPath(reference);
    require(hiddenPath.ends_with(".rdram"), "Reference suffix");
    hiddenPath.replace(hiddenPath.size() - 6, 6, ".hidden");
    std::ifstream hiddenFile(hiddenPath, std::ios::binary);
    std::vector<uint8_t> hidden((std::istreambuf_iterator<char>(hiddenFile)), {});
    require(hidden.size() >= memory.size() / 2, "Reference hidden length");
    if (texturedDraws) {
        auto tmemPath = std::string(reference);
        tmemPath.replace(tmemPath.size() - 6, 6, ".tmem");
        std::ifstream tmemFile(tmemPath, std::ios::binary);
        std::vector<uint8_t> expectedTmem((std::istreambuf_iterator<char>(tmemFile)), {});
        std::array<uint8_t, 4096> actualTmem;
        textures.store().copyBytes(actualTmem);
        require(expectedTmem.size() == actualTmem.size() &&
                std::equal(expectedTmem.begin(), expectedTmem.end(), actualTmem.begin()), "Independent triangle TMEM mismatch");
    }
    for (const auto& [address, coverage] : writtenCoverage) {
        if (hidden[address] != coverage) {
            std::fprintf(stderr, "hidden=%u expected=%u actual=%u\n", address, hidden[address], coverage);
            throw std::runtime_error("Independent triangle coverage differential");
        }
    }
    std::printf("PASS: %zu original triangles (%zu shaded, %zu textured), %zu pixel writes match independent RDRAM and written coverage\n",
                draws, shadedDraws, texturedDraws, pixels);
}

int main(int argc, char** argv) {
    try {
        unitTests();
        attributeTests();
        depthTests();
        textureTests();
        require(argc == 1 || argc == 3, "Usage: aurum-triangle-tests [capture reference.rdram]");
        if (argc == 3) compare(argv[1], argv[2]);
        std::puts("PASS: triangle state, coverage, mode gate, memory bounds");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
