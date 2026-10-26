// SPDX-License-Identifier: MIT
#include <metal_stdlib>
using namespace metal;
struct PresentationVertex { float4 position [[position]]; float2 uv; };
vertex PresentationVertex presentVertex(uint i [[vertex_id]]) {
    float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)};
    return {float4(p[i],0,1), float2((p[i].x+1)/2, (1-p[i].y)/2)};
}
fragment float4 presentFragment(PresentationVertex v [[stage_in]], texture2d<float> t [[texture(0)]],
                                constant bool& linear [[buffer(0)]]) {
    constexpr sampler nearest(coord::normalized, address::clamp_to_edge, filter::nearest);
    constexpr sampler smooth(coord::normalized, address::clamp_to_edge, filter::linear);
    return linear ? t.sample(smooth,v.uv) : t.sample(nearest,v.uv);
}
