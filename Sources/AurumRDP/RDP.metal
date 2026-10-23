// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;
struct Fill { uint address, width, size, color, x0, y0, x1, y1; };
struct Scanout { uint origin, stride, size, width, height, xScale, yScale, xOffset, yOffset, length; };

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
            uint shift = (1 - lane) * 16;
            result = (result & ~(65535u << shift)) | (f.color & (65535u << shift));
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
            value = (value >> ((address & 2) ? 0 : 16)) & 65535;
            r = (value >> 11) & 31; r = (r << 3) | (r >> 2);
            g = (value >> 6) & 31; g = (g << 3) | (g >> 2);
            b = (value >> 1) & 31; b = (b << 3) | (b >> 2);
        } else { r = value >> 24; g = (value >> 16) & 255; b = (value >> 8) & 255; }
    }
    output[p.y * s.width + p.x] = r | (g << 8) | (b << 16) | 0xff000000;
}
