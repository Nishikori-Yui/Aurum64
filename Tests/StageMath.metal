// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;
#include "PerspectiveMath.h"

kernel void perspectiveProbe(device const uint4* input [[buffer(0)]],
                             device uint2* output [[buffer(1)]],
                             uint index [[thread_position_in_grid]]) {
    const uint4 request = input[index];
    const auto result = divideTexture(request.y, request.z, request.w, request.x != 0);
    output[index] = uint2(result.s, result.t);
}
