// SPDX-License-Identifier: MIT
#include "Renderer.hpp"
#include "TrianglePlan.hpp"
#include "TriangleMode.hpp"
#include "TriangleTexture.hpp"
#include "TriangleColor.hpp"
#include "TextureCopy.hpp"
#import <Metal/Metal.h>
#include <algorithm>
#include <cstring>

namespace aurum::rdp {
struct Renderer::Impl {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> fillPipeline, viPipeline, copyPipeline, trianglePipeline,
        textureTrianglePipeline, colorTrianglePipeline;
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
        p.copyPipeline = [p.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"textureCopy"] error:&e];
        p.trianglePipeline = [p.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"triangleFill"] error:&e];
        p.textureTrianglePipeline = [p.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"triangleTexture"] error:&e];
        p.colorTrianglePipeline = [p.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"triangleColor"] error:&e];
        p.queue = [p.device newCommandQueue];
        p.ram = [p.device newBufferWithLength:8 * 1024 * 1024 options:MTLResourceStorageModeShared];
        p.output = [p.device newBufferWithLength:640 * 576 * 4 options:MTLResourceStorageModeShared];
        if (!p.fillPipeline || !p.viPipeline || !p.copyPipeline || !p.trianglePipeline ||
            !p.textureTrianglePipeline || !p.colorTrianglePipeline ||
            !p.queue || !p.ram || !p.output) {
            error = e.localizedDescription.UTF8String ?: "Cannot allocate Metal resources."; return false;
        }
        return true;
    }
}
bool Renderer::textureCopy(uint8_t* memory, size_t length, const TextureRect& rect,
                           const aurum::tmem::Store& tmem, std::string& error) {
    if (!memory || !impl->ram) { error = "Texture copy renderer is not ready."; return false; }
    std::vector<aurum::texture::CopyWord> words;
    if (!aurum::texture::makeCopyPlan(rect, length, words, error)) return false;
    if (words.empty()) return true;
    @autoreleasepool {
        auto& p = *impl;
        std::array<uint8_t, aurum::tmem::Store::kBytes> bytes;
        tmem.copyBytes(bytes);
        id<MTLBuffer> texture = [p.device newBufferWithBytes:bytes.data() length:bytes.size()
                                                   options:MTLResourceStorageModeShared];
        id<MTLBuffer> plan = [p.device newBufferWithBytes:words.data()
                            length:words.size() * sizeof(words[0]) options:MTLResourceStorageModeShared];
        if (!texture || !plan) { error = "Cannot allocate texture copy buffers."; return false; }
        memcpy(p.ram.contents, memory, length);
        const uint32_t copyOptions = (rect.otherModesLo & 1u) |
            (rect.tileFormat == 2 ? 2u : 0u) | (rect.tileSize == 0 ? 4u : 0u) |
            (((rect.tileModes >> 20) & 15u) << 8) |
            ((rect.tileSize == 1 && (rect.tileFormat == 3 || rect.tileFormat == 4)) ? 8u : 0u) |
            ((rect.tileFormat == 3 && rect.tileSize == 1) ? 16u : 0u);
        id<MTLCommandBuffer> command = [p.queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:p.copyPipeline];
        [encoder setBuffer:p.ram offset:0 atIndex:0];
        [encoder setBuffer:texture offset:0 atIndex:1];
        [encoder setBuffer:plan offset:0 atIndex:2];
        [encoder setBytes:&copyOptions length:sizeof(copyOptions) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(words.size(), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [encoder endEncoding];
        if (!p.finish(command, error)) return false;
        memcpy(memory, p.ram.contents, length);
        return true;
    }
}
bool Renderer::combinerTriangle(uint8_t* memory, size_t length, const Triangle& triangle,
                                const aurum::tmem::Store& tmem, std::string& error) {
    if (!memory || !impl->ram || !length || length % 4 || length > impl->ram.length) {
        error = "Invalid one-cycle combiner triangle memory or renderer.";
        return false;
    }
    std::vector<TriangleWord> words;
    if (!makeTriangleColorPlan(triangle, length, words, error)) return false;
    if (words.empty()) return true;
    @autoreleasepool {
        auto& p = *impl;
        std::array<uint8_t, aurum::tmem::Store::kBytes> bytes{};
        tmem.copyBytes(bytes);
        id<MTLBuffer> texture = [p.device newBufferWithBytes:bytes.data() length:bytes.size()
                                                    options:MTLResourceStorageModeShared];
        id<MTLBuffer> plan = [p.device newBufferWithBytes:words.data()
                                                    length:words.size() * sizeof(words[0])
                                                   options:MTLResourceStorageModeShared];
        if (!texture || !plan) {
            error = "Cannot allocate one-cycle combiner triangle buffers.";
            return false;
        }
        const auto parameters = makeTriangleColorParameters(triangle);
        memcpy(p.ram.contents, memory, length);
        id<MTLCommandBuffer> command = [p.queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!command || !encoder) {
            error = "Cannot create one-cycle combiner triangle command.";
            return false;
        }
        [encoder setComputePipelineState:p.colorTrianglePipeline];
        [encoder setBuffer:p.ram offset:0 atIndex:0];
        [encoder setBuffer:plan offset:0 atIndex:1];
        [encoder setBuffer:texture offset:0 atIndex:2];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(words.size(), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(p.colorTrianglePipeline.threadExecutionWidth, 1, 1)];
        [encoder endEncoding];
        if (!p.finish(command, error)) return false;
        memcpy(memory, p.ram.contents, length);
        return true;
    }
}
bool Renderer::flatTriangle(uint8_t* memory, size_t length, uint32_t framebufferAddress,
                            uint32_t framebufferWidth, uint32_t framebufferSize,
                            uint16_t color, const Triangle& triangle,
                            int32_t clipXh, int32_t clipYh,
                            int32_t clipXl, int32_t clipYl, std::string& error) {
    if (!memory || !impl->ram) {
        error = "Flat triangle renderer is not ready.";
        return false;
    }
    if (!length || length % 4 || length > impl->ram.length) {
        error = "Invalid flat triangle memory size.";
        return false;
    }
    std::vector<aurum::rdp::TriangleWord> words;
    if (!aurum::rdp::makeFlatTrianglePlan(
            triangle, length, framebufferAddress, framebufferWidth, framebufferSize,
            color, clipXh, clipYh, clipXl, clipYl, words, error))
        return false;
    if (words.empty()) return true;
    @autoreleasepool {
        auto& p = *impl;
        id<MTLBuffer> plan = [p.device newBufferWithBytes:words.data()
                            length:words.size() * sizeof(words[0])
                            options:MTLResourceStorageModeShared];
        if (!plan) { error = "Cannot allocate flat triangle plan."; return false; }
        memcpy(p.ram.contents, memory, length);
        id<MTLCommandBuffer> command = [p.queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:p.trianglePipeline];
        [encoder setBuffer:p.ram offset:0 atIndex:0];
        [encoder setBuffer:plan offset:0 atIndex:1];
        const ShadeParameters shade = makeShadeParameters(triangle);
        const uint32_t shaded = isShadeTriangle(triangle);
        [encoder setBytes:&shade length:sizeof(shade) atIndex:2];
        [encoder setBytes:&shaded length:sizeof(shaded) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(words.size(), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [encoder endEncoding];
        if (!p.finish(command, error)) return false;
        memcpy(memory, p.ram.contents, length);
        return true;
    }
}
bool Renderer::texturedTriangle(uint8_t* memory, size_t length, const Triangle& triangle,
                                 const aurum::tmem::Store& tmem, std::string& error) {
    if (!memory || !impl->ram || !length || length % 4 || length > impl->ram.length) {
        error = "Invalid textured triangle memory or renderer."; return false;
    }
    std::vector<TriangleWord> words;
    if (!makeTexturedTrianglePlan(triangle, length, words, error)) return false;
    if (words.empty()) return true;
    @autoreleasepool {
        auto& p = *impl;
        std::array<uint8_t, aurum::tmem::Store::kBytes> bytes;
        tmem.copyBytes(bytes);
        id<MTLBuffer> texture = [p.device newBufferWithBytes:bytes.data() length:bytes.size()
            options:MTLResourceStorageModeShared];
        id<MTLBuffer> plan = [p.device newBufferWithBytes:words.data() length:words.size() * sizeof(words[0])
            options:MTLResourceStorageModeShared];
        if (!texture || !plan) { error = "Cannot allocate textured triangle buffers."; return false; }
        const auto parameters = makeTriangleTextureParameters(triangle);
        memcpy(p.ram.contents, memory, length);
        id<MTLCommandBuffer> command = [p.queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        if (!command || !encoder) { error = "Cannot create textured triangle command."; return false; }
        [encoder setComputePipelineState:p.textureTrianglePipeline];
        [encoder setBuffer:p.ram offset:0 atIndex:0];
        [encoder setBuffer:plan offset:0 atIndex:1];
        [encoder setBuffer:texture offset:0 atIndex:2];
        [encoder setBytes:&parameters length:sizeof(parameters) atIndex:3];
        [encoder dispatchThreads:MTLSizeMake(words.size(), 1, 1)
          threadsPerThreadgroup:MTLSizeMake(p.textureTrianglePipeline.threadExecutionWidth, 1, 1)];
        [encoder endEncoding];
        if (!p.finish(command, error)) return false;
        memcpy(memory, p.ram.contents, length);
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
    const uint32_t xScale = vi.xScale & 4095;
    const uint32_t yScale = vi.yScale & 4095;
    if (type < 2 || !vi.width || h <= 0 || v < 2 || !xScale || !yScale) {
        pixels.clear(); return true;
    }
    if (!impl->ram || !length || length % 4 || length > impl->ram.length || vi.width > 4096) {
        error = "Invalid VI memory layout."; return false;
    }
    struct Parameters { uint32_t origin, stride, size, width, height, xScale, yScale, xOffset, yOffset, length; };
    const uint64_t scaledWidth = (uint64_t(h) * xScale + 1023) / 1024;
    const uint64_t scaledHeight = (uint64_t(v / 2) * yScale + 1023) / 1024;
    if (!scaledWidth || !scaledHeight) { pixels.clear(); return true; }
    // The presentation path has a fixed maximum texture size. Clipping the
    // output dimensions still leaves the shader's source-coordinate mapping
    // driven by the VI scale registers.
    width = uint32_t(std::min<uint64_t>(scaledWidth, 640));
    height = uint32_t(std::min<uint64_t>(scaledHeight, 576));
    Parameters s{vi.origin & 0xffffff, vi.width, type, width, height, xScale,
                 yScale, (vi.xScale >> 16) & 4095, (vi.yScale >> 16) & 4095, uint32_t(length)};
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
