#pragma once

#include <string_view>

// The depth guide's expansion.
//
// The host computes depth as the analysis grid - 160x68 at 1920x822 - and the
// runtime samples depth over the whole frame: its kernel indexes
// Depth.Load(int3(uv * float2(renderWidth, renderHeight))) with no extent of its
// own, unlike motion, so the texture it reads has to be frame-sized and every
// texel in it is read. Resampling that on the CPU every frame cost 6.3 MB of
// area averages through a write-combined heap, about 11 ms at 1920x822, which is
// a quarter of the frame budget for a field the model mostly treats as smooth.
//
// This pass does the same resample on the GPU from a 43 KB upload. It is the
// same arithmetic, not an approximation of it: GuideFiles.h's ResampleArea, when
// the source is smaller than the target, takes each output pixel's source cell as
//
//     x0 = (gx * sourceWidth) / targetWidth        (integer division)
//     x1 = max(x0 + 1, ...)                        (always x0 + 1 here)
//
// which for an upscale is nearest-neighbour replication by floor division, and
// that is exactly what this shader computes from the output pixel's integer
// coordinate. Pixel centres come from SV_Position rather than from a normalised
// uv, because the CPU indexes by integer pixel and a half-pixel of drift would
// move every cell boundary by one texel and make the two disagree in a way that
// looks like noise.
inline constexpr char kAmdDepthExpandHlsl[] = R"(
Texture2D<float> DepthGrid:register(t0);
// The frame the pass is expanding into. The grid's own size is read off the
// texture, so this is the only thing the shader needs that it cannot ask for.
cbuffer Params:register(b0){uint FrameW;uint FrameH;};
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float PSDepthExpand(V i):SV_Target{
    uint gw,gh;DepthGrid.GetDimensions(gw,gh);
    uint2 pixel=uint2(max(i.p.xy,0));
    uint2 cell=min(uint2(pixel.x*gw/FrameW,pixel.y*gh/FrameH),uint2(gw-1,gh-1));
    return DepthGrid.Load(int3(cell,0));
}
)";

// The entry point's own target. The pass compiles one vertex shader and one pixel
// shader, and a fixed target here is a mistake only a device sees - which is how
// this one was found: ps_5_0 refuses SV_VertexID and SV_Position outright.
inline constexpr const char* DepthExpandShaderProfileFor(std::string_view entry) noexcept
{
    return entry == "VS" ? "vs_5_1" : "ps_5_1";
}
