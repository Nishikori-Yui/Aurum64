// SPDX-License-Identifier: MIT
#include "TextureCoordinateProbe.hpp"
#include "TextureFetch.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>

namespace {
using namespace aurum::texture;
using namespace aurum::coordinate_probe;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
uint32_t mode(uint32_t shift, uint32_t mask, uint32_t flags) {
    return shift | mask << 4 | (flags & 1) << 8 | ((flags >> 1) & 1) << 9;
}
Request makeCase(uint32_t sl, uint32_t sh, uint32_t tl, uint32_t th,
                 uint32_t smode, uint32_t tmode, int32_t s, int32_t t,
                 uint32_t tile = 0, uint32_t size = 2,
                 uint32_t line = 2, uint32_t tmem = 0,
                 uint32_t format = 0) {
    return {0xf5000000u | format << 21 | size << 19 | line << 9 | tmem,
            tile << 24 | smode | tmode << 10,
            0xf2000000u | sl << 12 | tl, tile << 24 | sh << 12 | th,
            uint32_t(s), uint32_t(t)};
}
void generate(const char* path) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    writeMagic(out, kInputMagic);
    size_t count = 0;
    const auto emit = [&](const Request& request) {
        writeWords(out, request);
        ++count;
    };
    // Exhaust every signed input bit pattern at every shift. S and T use
    // different inputs and modes to catch accidental sharing between axes.
    for (uint32_t shift = 0; shift < 16; ++shift)
        for (uint32_t bits = 0; bits < 65536; ++bits)
            emit(makeCase(shift & 3, 28 + (shift & 3), 7, 42,
                          mode(shift, shift, shift & 3),
                          mode(15 - shift, 15 - shift, (shift >> 2) & 3),
                          signedCoordinate(bits), signedCoordinate(bits * 109 + 17),
                          shift & 7, 2 + (shift & 1)));
    constexpr uint32_t bounds[][2] = {{0,28}, {1,31}, {7,42},
                                      {4092,4095}, {4095,0}, {36,12}};
    constexpr int32_t inputs[] = {-32768, -32767, -16384, -8193, -8192,
        -4097, -4096, -1025, -1024, -129, -128, -127, -33, -32, -31,
        -9, -8, -1, 0, 1, 7, 8, 31, 32, 33, 95, 96, 127, 128,
        129, 255, 256, 1023, 1024, 16383, 32767};
    for (const auto& bound : bounds)
        for (uint32_t shift = 0; shift < 16; ++shift)
            for (uint32_t mask = 0; mask < 16; ++mask)
                for (uint32_t flags = 0; flags < 4; ++flags)
                    for (int32_t s : inputs)
                        emit(makeCase(bound[0], bound[1], bound[1], bound[0],
                                      mode(shift, mask, flags),
                                      mode(15 - shift, mask, flags ^ 3), s, -s - 1,
                                      mask & 7, 2 + (flags & 1)));
    std::mt19937 rng(0x4c005);
    for (unsigned i = 0; i < 10000; ++i) {
        const uint32_t smode = rng() & 1023, tmode = rng() & 1023;
        emit(makeCase(rng() & 4095, rng() & 4095, rng() & 4095, rng() & 4095,
                      smode, tmode, signedCoordinate(rng()), signedCoordinate(rng()),
                      rng() & 7, 2 + (rng() & 1), rng() & 31, rng() & 255));
    }
    // Query the reference's actual color-channel fetch for IA8, IA16 and I8
    // against a deterministic TMEM pattern, covering both row parities.
    for (const auto [format, size, width, height, line] : {
             std::array<uint32_t, 5>{3, 1, 16, 4, 2},
             std::array<uint32_t, 5>{3, 2, 8, 4, 2},
             std::array<uint32_t, 5>{4, 1, 16, 4, 2}}) {
        for (uint32_t t = 0; t < height; ++t)
            for (uint32_t s = 0; s < width; ++s)
                emit(makeCase(0, (width - 1) * 4, 0, (height - 1) * 4,
                              mode(0, 0, 1), mode(0, 0, 1),
                              int32_t(s * 32), int32_t(t * 32), 0, size, line, 0,
                              format));
    }
    out.flush();
    require(bool(out) && count <= kMaxCases, "Cannot finish coordinate cases.");
    std::printf("Generated %zu original coordinate cases\n", count);
}

void compare(const char* casesPath, const char* referencePath) {
    std::ifstream cases(casesPath, std::ios::binary), reference(referencePath, std::ios::binary);
    readMagic(cases, kInputMagic);
    readMagic(reference, kOutputMagic);
    aurum::tmem::Store store;
    for (uint32_t i = 0; i < 4096; ++i)
        store.writeByte(i, uint8_t(i * 73u + (i >> 3) * 19u + 11u));
    aurum::rdp::Decoder decoder;
    TileDescriptor tile;
    decoder.textureRect = [&](const aurum::rdp::TextureRect& rect) {
        tile = tileDescriptor(rect);
        return false; // coordinate testing must not advertise a rendered draw
    };
    Sampler sampler;
    size_t count = 0, fetched = 0;
    Request request;
    Response expected{};
    while (readWords(cases, request)) {
        require(++count <= kMaxCases && readWords(reference, expected),
                "Missing coordinate reference result.");
        decoder.push(std::span(request.data(), 4));
        const uint32_t rect[] = {0xe4004004, request[1] & 0x07000000u,
            (request[4] & 0xffffu) << 16 | (request[5] & 0xffffu), 0};
        decoder.push(rect);
        const int32_t s = signedCoordinate(request[4]), t = signedCoordinate(request[5]);
        const int32_t mappedS = mapPointCoordinate(s, tile.s);
        const int32_t mappedT = mapPointCoordinate(t, tile.t);
        const auto copy = mapCopyCoordinates(s, t, tile.s, tile.t);
        Response actual{uint32_t(mappedS), uint32_t(mappedT), uint32_t(copy.s[0]),
            uint32_t(copy.s[1]), uint32_t(copy.s[2]), uint32_t(copy.s[3]), uint32_t(copy.t)};
        Texel texel;
        const bool accepted = sampler.fetchFixedPoint(store, tile, s, t, texel);
        if (accepted) {
            ++fetched;
            actual[7] = texel.r; actual[8] = texel.g;
            actual[9] = texel.b; actual[10] = texel.a;
        }
        for (unsigned field = 0; field < (accepted ? 11u : 7u); ++field) {
            if (actual[field] == expected[field]) continue;
            std::fprintf(stderr, "case=%zu field=%u s=%d t=%d modes=%06x "
                         "expected=%08x actual=%08x\n", count, field, s, t,
                         request[1] & 0xffffff, expected[field], actual[field]);
            throw std::runtime_error("Coordinate/texel differs from pinned reference.");
        }
    }
    require(count != 0 && fetched > 10000 && !readWords(reference, expected),
            "Empty or extra coordinate reference data.");
    require(decoder.unsupported == count, "Coordinate tests concealed unsupported rectangles.");
    std::printf("PASS: %zu point/copy coordinate cases, %zu reference texel fetches "
                "against independent Angrylion process\n", count, fetched);
}

void checkUnits() {
    AxisCoordinates axis{0, 28, 0, 0, false, false};
    require(mapPointCoordinate(-2, axis) == 0, "mask=0 implies point clamp");
    auto copy = mapCopyCoordinates(-2, -2, axis, axis);
    require(copy.s[0] == -1 && copy.s[1] == 0 && copy.t == -1,
            "Copy bypasses clamp and floors negative fractions");
    axis.mask = 2;
    require(mapPointCoordinate(-2, axis) == 3, "Negative fraction wraps after floor");
    axis.mirror = true;
    require(mapPointCoordinate(-2, axis) == 0, "Negative mirror interval");
    axis = {0, 28, 1, 2, false, false};
    copy = mapCopyCoordinates(6 * 32, 0, axis, axis);
    require(copy.s == std::array<int32_t,4>{3,0,1,2},
            "Copy lane increment occurs after shift and before mask");
    axis = {7, 31, 0, 0, false, true};
    require(mapPointCoordinate(55, axis) == 0 && mapPointCoordinate(88, axis) == 1,
            "Fractional tile-relative lower bound");
    require(mapPointCoordinate(248, axis) == 6, "Upper clamp uses integer bound difference");

    aurum::rdp::Decoder decoder;
    std::vector<aurum::rdp::TextureRect> rects;
    decoder.textureRect = [&](const aurum::rdp::TextureRect& rect) {
        rects.push_back(rect);
        return false;
    };
    const auto setup = makeCase(4, 28, 8, 36, mode(1, 3, 1), mode(15, 2, 2),
                                0, 0, 3);
    decoder.push(std::span(setup.data(), 4));
    const uint32_t rect[] = {0xe4004004, 3u << 24, 0, 0};
    decoder.push(rect);
    for (uint32_t opcode : {0x34u, 0x33u, 0x30u}) {
        // Exercise absent, rejected and accepted upload handlers. All three
        // commands update the selected tile's bounds before the next draw.
        if (opcode == 0x33) decoder.textureLoad = [](const auto&) { return false; };
        if (opcode == 0x30) decoder.textureLoad = [](const auto&) { return true; };
        const uint32_t load[] = {opcode << 24 | 12u << 12 | 16u,
                                 3u << 24 | 40u << 12 | opcode};
        decoder.push(load);
        decoder.push(rect);
        const auto& current = rects.back();
        require(current.tileSl == 12 && current.tileTl == 16 &&
                current.tileSh == 40 && current.tileTh == opcode,
                "Upload command did not replace tile bounds");
        require(current.tileModes == (setup[1] & 0xffffffu),
                "Upload overwrote tile coordinate modes");
    }
    require(rects.front().tileSl == 4 && rects.front().tileSh == 28 &&
            rects.front().tileTl == 8 && rects.front().tileTh == 36,
            "Later uploads changed a captured rectangle");
    decoder.reset();
    decoder.push(rect);
    require(rects.back().tileModes == 0 && rects.back().tileSl == 0 &&
            rects.back().tileSh == 0, "Reset retained coordinate state");
}
} // namespace

int main(int argc, char** argv) {
    try {
        checkUnits();
        if (argc == 3 && std::string(argv[1]) == "--generate") generate(argv[2]);
        else if (argc == 4 && std::string(argv[1]) == "--compare") compare(argv[2], argv[3]);
        else throw std::runtime_error("Usage: --generate cases | --compare cases reference");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
