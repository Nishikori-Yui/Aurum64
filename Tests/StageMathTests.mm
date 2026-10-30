// SPDX-License-Identifier: MIT
#include "PerspectiveProbe.hpp"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <fstream>
#include <vector>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    @autoreleasepool {
        try {
            require(argc == 4, "Usage: aurum-stage-metal-tests library input reference");
            using namespace aurum::perspective_probe;
            static_assert(sizeof(Request) == 16 && sizeof(Response) == 8);
            std::ifstream input(argv[2], std::ios::binary), expectedFile(argv[3], std::ios::binary);
            readMagic(input, kInputMagic); readMagic(expectedFile, kOutputMagic);
            std::vector<Request> requests;
            std::vector<Response> expected;
            Request request;
            Response response;
            while (readWords(input, request)) {
                require(requests.size() < kMaxCases && request[0] <= 1, "Invalid GPU probe input");
                require(readWords(expectedFile, response), "Missing GPU reference record");
                requests.push_back(request); expected.push_back(response);
            }
            require(!readWords(expectedFile, response) && requests.size() == 9 * 65536,
                    "Incomplete or extra GPU reference records");
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            require(device != nil, "No Metal device is available.");
            NSError* error = nil;
            id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[1])] error:&error];
            require(library != nil, "Cannot load stage shader library");
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:
                [library newFunctionWithName:@"perspectiveProbe"] error:&error];
            require(pipeline != nil, "Cannot create perspective pipeline");
            id<MTLBuffer> in = [device newBufferWithBytes:requests.data()
                length:requests.size() * sizeof(Request) options:MTLResourceStorageModeShared];
            id<MTLBuffer> out = [device newBufferWithLength:expected.size() * sizeof(Response)
                options:MTLResourceStorageModeShared];
            id<MTLCommandQueue> queue = [device newCommandQueue];
            require(in != nil && out != nil && queue != nil, "Cannot allocate GPU probe resources");
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
            require(command != nil && encoder != nil, "Cannot create GPU probe command");
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:in offset:0 atIndex:0];
            [encoder setBuffer:out offset:0 atIndex:1];
            [encoder dispatchThreads:MTLSizeMake(requests.size(), 1, 1)
              threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
            [encoder endEncoding];
            [command commit]; [command waitUntilCompleted];
            require(command.status == MTLCommandBufferStatusCompleted, "GPU probe execution failed");
            const auto* actual = static_cast<const Response*>(out.contents);
            for (size_t i = 0; i < expected.size(); ++i) if (actual[i] != expected[i]) {
                std::fprintf(stderr, "case=%zu expected=%05x,%05x actual=%05x,%05x\n",
                    i, expected[i][0], expected[i][1], actual[i][0], actual[i][1]);
                throw std::runtime_error("GPU perspective differs from independent reference");
            }
            std::printf("PASS: %zu Metal perspective cases match independent reference\n", requests.size());
        } catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
        }
    }
}
