// SPDX-License-Identifier: MIT
#import <Metal/Metal.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 2) return 2;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"presentVertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"presentFragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
        auto* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
        textureDescriptor.storageMode = MTLStorageModeShared;
        textureDescriptor.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> input = [device newTextureWithDescriptor:textureDescriptor];
        const uint32_t colors[] = {0xff0000ff, 0xff00ff00, 0xffff0000, 0xffffffff};
        [input replaceRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0 withBytes:colors bytesPerRow:8];
        textureDescriptor.pixelFormat = MTLPixelFormatBGRA8Unorm;
        textureDescriptor.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> output = [device newTextureWithDescriptor:textureDescriptor];
        for (bool linear : {false, true}) {
            auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = output;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:pipeline];
            [encoder setFragmentTexture:input atIndex:0];
            [encoder setFragmentBytes:&linear length:sizeof(linear) atIndex:0];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
            if (command.status != MTLCommandBufferStatusCompleted) return 1;
            uint32_t actual[4];
            [output getBytes:actual bytesPerRow:8 fromRegion:MTLRegionMake2D(0,0,2,2) mipmapLevel:0];
            const uint32_t expected[] = {0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
            if (memcmp(actual,expected,sizeof(actual))) { fprintf(stderr,"Presentation color/orientation mismatch\n"); return 1; }
        }
        puts("PASS: offscreen Metal presentation, orientation and nearest/linear filtering");
    }
}
