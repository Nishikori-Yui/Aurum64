// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;
#include "ShadeMath.h"
#include "CombinerMath.h"
#include "TriangleTextureMath.h"
#include "TriangleColorMath.h"
struct Fill { uint address, width, size, color, x0, y0, x1, y1; };
struct Scanout { uint origin, stride, size, width, height, xScale, yScale, xOffset, yOffset, length; };
struct CopyWord { uint word, high, low; };
struct TriangleWord {
    uint word, high, low;
    int highX, highY, highStart, lowX, lowY, lowStart;
};
static_assert(sizeof(TriangleWord) == 36);
static_assert(sizeof(ShadeParameters) == 84);
static_assert(sizeof(TriangleTextureParameters) == 192);
static_assert(sizeof(TriangleColorParameters) == 212);

kernel void triangleColor(device uint* ram [[buffer(0)]],
                          device const TriangleWord* plan [[buffer(1)]],
                          device const ushort* tmem [[buffer(2)]],
                          constant TriangleColorParameters& parameters [[buffer(3)]],
                          uint index [[thread_position_in_grid]]) {
    TriangleWord item = plan[index];
    const CombinerColor primitive = primitiveCombinerColor(parameters.primitiveColor);
    const auto write = [&](int x, int y, int start, uint& destination) {
        const CombinerColor shade = (parameters.flags & 1u)
            ? shadeCombinerColor(parameters.texture.setup, x, y, start) : CombinerColor{};
        CombinerColor texel{};
        if (parameters.flags & 2u) {
            const uint source = triangleTextureIndex(parameters.texture, x, y, start);
            const uint value = uint(tmem[source]);
            const auto expand = [](uint channel) { return (channel << 3) | (channel >> 2); };
            texel = {expand((value >> 11) & 31u), expand((value >> 6) & 31u),
                     expand((value >> 1) & 31u), (value & 1u) ? 255u : 0u};
        }
        const CombinerColor color = simpleCombinerColor(parameters.combineHi, parameters.combineLo,
                                                        texel, shade, primitive);
        if ((parameters.flags & 4u) && color.a < (parameters.blendColor & 0xffu)) return false;
        destination = ((color.r >> 3) << 11) | ((color.g >> 3) << 6) | ((color.b >> 3) << 1);
        return true;
    };
    if (item.high != 0xffffffffu) {
        uint value = 0;
        if (write(item.highX, item.highY, item.highStart, value))
            item.high = value | (item.high & 1u);
        else
            item.high = 0xffffffffu;
    }
    if (item.low != 0xffffffffu) {
        uint value = 0;
        if (write(item.lowX, item.lowY, item.lowStart, value))
            item.low = value | (item.low & 1u);
        else
            item.low = 0xffffffffu;
    }
    uint value = ram[item.word];
    if (item.high != 0xffffffffu) value = (value & 0x0000ffffu) | (item.high << 16);
    if (item.low != 0xffffffffu) value = (value & 0xffff0000u) | item.low;
    ram[item.word] = value;
}

kernel void triangleTexture(device uint* ram [[buffer(0)]],
                            device const TriangleWord* plan [[buffer(1)]],
                            device const ushort* tmem [[buffer(2)]],
                            constant TriangleTextureParameters& parameters [[buffer(3)]],
                            uint index [[thread_position_in_grid]]) {
    TriangleWord item = plan[index];
    if (item.high != 0xffffffffu) {
        const uint source = triangleTextureIndex(parameters, item.highX, item.highY, item.highStart);
        item.high = (uint(tmem[source]) & 0xfffeu) | (item.high & 1u);
    }
    if (item.low != 0xffffffffu) {
        const uint source = triangleTextureIndex(parameters, item.lowX, item.lowY, item.lowStart);
        item.low = (uint(tmem[source]) & 0xfffeu) | (item.low & 1u);
    }
    uint value = ram[item.word];
    if (item.high != 0xffffffffu) value = (value & 0xffffu) | (item.high << 16);
    if (item.low != 0xffffffffu) value = (value & 0xffff0000u) | item.low;
    ram[item.word] = value;
}

kernel void triangleFill(device uint* ram [[buffer(0)]],
                         device const TriangleWord* plan [[buffer(1)]],
                         constant ShadeParameters& shade [[buffer(2)]],
                         constant uint& shaded [[buffer(3)]],
                         uint index [[thread_position_in_grid]]) {
    TriangleWord item = plan[index];
    if (shaded) {
        if (item.high != 0xffffffffu)
            item.high = shadeRGB16(shade, item.highX, item.highY, item.highStart) | (item.high & 1u);
        if (item.low != 0xffffffffu)
            item.low = shadeRGB16(shade, item.lowX, item.lowY, item.lowStart) | (item.low & 1u);
    }
    uint value = ram[item.word];
    if (item.high != 0xffffffffu)
        value = (value & 0x0000ffffu) | (item.high << 16);
    if (item.low != 0xffffffffu)
        value = (value & 0xffff0000u) | item.low;
    ram[item.word] = value;
}

kernel void textureCopy(device uint* ram [[buffer(0)]], device const ushort* tmem [[buffer(1)]],
                        device const CopyWord* plan [[buffer(2)]],
                        constant uint& options [[buffer(3)]],
                        uint index [[thread_position_in_grid]]) {
    CopyWord item = plan[index];
    uint value = ram[item.word];
    for (uint lane = 0; lane < 2; ++lane) {
        uint source = lane == 0 ? item.high : item.low;
        if (source == 0xffffffffu) continue;
        uint texel;
        if (options & 8u) {
            device const uchar* bytes = reinterpret_cast<device const uchar*>(tmem);
            uint first = bytes[source & 4095u];
            uint second = bytes[(source >> 16) & 4095u];
            if (options & 16u) {
                first = ((first >> 4) & 15u) * 17u;
                second = ((second >> 4) & 15u) * 17u;
            }
            uint pair = (first << 8) | second;
            if (source & (1u << 31)) {
                first = bytes[source & 4095u];
                second = bytes[(source >> 16) & 4095u];
                pair = (first << 8) | second;
            }
            texel = pair;
        } else if (options & 2u) {
            device const uchar* bytes = reinterpret_cast<device const uchar*>(tmem);
            uint entry = bytes[source & 4095u];
            if (options & 4u)
                entry = ((entry >> ((source & (1u << 14)) ? 0 : 4)) & 15u) |
                        (((options >> 8) & 15u) << 4);
            texel = tmem[1024u + entry * 4u + ((source >> 12) & 3u)];
        } else texel = tmem[source];
        if ((options & 1u) && !(texel & 1u)) continue;
        uint shift = lane == 0 ? 16 : 0;
        value = (value & ~(65535u << shift)) | (texel << shift);
    }
    ram[item.word] = value;
}

kernel void fillRectangle(device uint* ram [[buffer(0)]], constant Fill& f [[buffer(1)]],
                          constant uint& length [[buffer(2)]], uint word [[thread_position_in_grid]]) {
    // One invocation owns a complete host word, avoiding overlapping 16-bit stores.
    uint address = word * 4;
    if (address + 4 > length) return;
    uint result = ram[word];
    uint count = f.size == 2 ? 2 : 1;
    uint bytes = f.size == 2 ? 2 : 4;
    for (uint lane = 0; lane < count; ++lane) {
        uint pixelAddress = address + lane * bytes;
        if (pixelAddress < f.address) continue;
        uint pixel = (pixelAddress - f.address) / bytes;
        uint x = pixel % f.width, y = pixel / f.width;
        if (x < f.x0 || x >= f.x1 || y < f.y0 || y >= f.y1) continue;
        if (count == 1) result = f.color;
        else {
            uint halfIndex = pixelAddress >> 1;
            uint hostHalf = halfIndex ^ 1;
            uint shift = (hostHalf & 1) * 16;
            uint fillHalf = (f.color >> ((halfIndex & 1) ? 0 : 16)) & 65535u;
            result = (result & ~(65535u << shift)) | (fillHalf << shift);
        }
    }
    ram[word] = result;
}
kernel void scanout(device const uint* ram [[buffer(0)]], device uint* output [[buffer(1)]],
                    constant Scanout& s [[buffer(2)]], uint2 p [[thread_position_in_grid]]) {
    if (p.x >= s.width || p.y >= s.height) return;
    uint x = (p.x * s.xScale + s.xOffset) >> 10;
    uint y = (p.y * s.yScale + s.yOffset) >> 10;
    uint bytes = s.size == 2 ? 2 : 4;
    uint address = s.origin + (y * s.stride + x) * bytes;
    uint r = 0, g = 0, b = 0;
    if (x < s.stride && address + bytes <= s.length) {
        uint value = ram[address >> 2];
        if (s.size == 2) {
            value = (value >> ((((address >> 1) ^ 1) & 1) * 16)) & 65535;
            r = (value >> 8) & 0xf8;
            g = (value & 0x7c0) >> 3;
            b = (value & 0x3e) << 2;
        } else { r = value >> 24; g = (value >> 16) & 255; b = (value >> 8) & 255; }
    }
    output[p.y * s.width + p.x] = r | (g << 8) | (b << 16) | 0xff000000;
}
