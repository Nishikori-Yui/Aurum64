// SPDX-License-Identifier: MIT
#include "Renderer.hpp"
#import <Metal/Metal.h>
#include <algorithm>
#include <cstring>

namespace aurum::rdp {
struct Renderer::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> fillPipeline, viPipeline;
    id<MTLBuffer> ram, output;
    bool finish(id<MTLCommandBuffer> command, std::string& error) {
        [command commit]; [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted) {
            error = command.error.localizedDescription.UTF8String ?: "Metal command failed";
            return false;
        }
        return true;
    }
};
Renderer::Renderer() : impl(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;
bool Renderer::initialize(const char* path, std::string& error) {
    if (!path || !*path) { error = "No Metal library path was supplied."; return false; }
    @autoreleasepool {
        auto& p = *impl;
        p.device = MTLCreateSystemDefaultDevice();
        if (!p.device) { error = "No Metal device is available."; return false; }
        NSError* e = nil;
        id<MTLLibrary> library = [p.device newLibraryWithURL:[NSURL fileURLWithPath:@(path)] error:&e];
        if (!library) { error = e.localizedDescription.UTF8String; return false; }
        p.fillPipeline = [p.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"fillRectangle"] error:&e];
        p.viPipeline = [p.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"scanout"] error:&e];
        p.queue = [p.device newCommandQueue];
        p.ram = [p.device newBufferWithLength:8 * 1024 * 1024 options:MTLResourceStorageModeShared];
        p.output = [p.device newBufferWithLength:640 * 576 * 4 options:MTLResourceStorageModeShared];
        if (!p.fillPipeline || !p.viPipeline || !p.queue || !p.ram || !p.output) {
            error = e.localizedDescription.UTF8String ?: "Cannot allocate Metal resources."; return false;
        }
        return true;
    }
}
bool Renderer::fill(uint8_t* memory, size_t length, const Fill& f, std::string& error) {
    if (!impl->ram || !length || length % 4 || length > impl->ram.length || !f.width ||
        (f.size != 2 && f.size != 3) || f.address >= length || f.address % (f.size == 2 ? 2 : 4)) {
        error = "Invalid RDP framebuffer address or memory size."; return false;
    }
    @autoreleasepool {
        auto& p = *impl;
        // Conservative ownership baseline: complete GPU writes before returning to the CPU.
        memcpy(p.ram.contents, memory, length);
        uint32_t bytes = uint32_t(length);
        id<MTLCommandBuffer> command = [p.queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:p.fillPipeline];
        [encoder setBuffer:p.ram offset:0 atIndex:0];
        [encoder setBytes:&f length:sizeof(f) atIndex:1];
        [encoder setBytes:&bytes length:sizeof(bytes) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(length / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [encoder endEncoding];
        if (!p.finish(command, error)) return false;
        memcpy(memory, p.ram.contents, length);
        return true;
    }
}
bool Renderer::scanout(const uint8_t* memory, size_t length, const VideoInterface& vi,
                       std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height, std::string& error) {
    width = height = 0;
    uint32_t type = vi.status & 3;
    int h = int(vi.hStart & 1023) - int((vi.hStart >> 16) & 1023);
    int v = int(vi.vStart & 1023) - int((vi.vStart >> 16) & 1023);
    if (type < 2 || !vi.width || h <= 0 || v < 2 || !(vi.xScale & 4095) || !(vi.yScale & 4095)) {
        pixels.clear(); return true;
    }
    if (!impl->ram || !length || length % 4 || length > impl->ram.length || vi.width > 4096) {
        error = "Invalid VI memory layout."; return false;
    }
    struct Parameters { uint32_t origin, stride, size, width, height, xScale, yScale, xOffset, yOffset, length; };
    width = std::min(h, 640); height = std::min(v / 2, 576);
    Parameters s{vi.origin & 0xffffff, vi.width, type, width, height, vi.xScale & 4095,
                 vi.yScale & 4095, (vi.xScale >> 16) & 4095, (vi.yScale >> 16) & 4095, uint32_t(length)};
    @autoreleasepool {
        auto& p = *impl;
        memcpy(p.ram.contents, memory, length);
        id<MTLCommandBuffer> command = [p.queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:p.viPipeline];
        [encoder setBuffer:p.ram offset:0 atIndex:0];
        [encoder setBuffer:p.output offset:0 atIndex:1];
        [encoder setBytes:&s length:sizeof(s) atIndex:2];
        [encoder dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [encoder endEncoding];
        if (!p.finish(command, error)) return false;
        pixels.resize(width * height); memcpy(pixels.data(), p.output.contents, pixels.size() * 4);
    }
    return true;
}
}
