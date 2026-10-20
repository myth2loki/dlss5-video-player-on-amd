#pragma once

#include <string_view>

// The two one-line passes that carry the AMD runtime's frame across the
// boundary between its format and the temporal accumulation's.
//
// The accumulation itself is deliberately NOT here. It is kTemporalStabilityHlsl
// (TemporalStabilityShader.h), the same pass the NVIDIA capture path runs, and
// this file exists so that it can be reused unchanged. Its contract is linear
// FP16 in and linear FP16 out - t0 is the neural frame already in linear light,
// and t3/t4 are the decoded source as 8-bit sRGB, which the pass linearises
// itself - while the AMD runtime publishes and is read back as 8-bit sRGB BGRA.
// Something has to cross that boundary, and it is here rather than inside the
// pass so that the pass stays one program shared by two backends instead of two
// programs that have to be kept in step.
//
// Decode is a pass rather than an _SRGB view of the runtime's texture because
// the pass reads its own input with Load. Leaning on the SRV format to linearise
// would make the reuse rest on a format-conversion rule that is never written
// down anywhere in this tree, and a wrong guess about it would show up as a
// gamma error in the picture and nothing else. A line of shader text says the
// same thing and can be read.
//
// Bindings, one texture:
//   t0 the texture to convert (decode: the runtime's own output; encode: the
//      history slot the accumulation just wrote)
//
// Both entry points write what their target format wants: PSDecode writes
// linear light into an FP16 target, PSEncode writes sRGB-encoded codes into an
// 8-bit UNORM target, which is exactly what the runtime's own output carries.
//
// PSEncode writes the same codes into an FP16 target under a second pipeline
// when the capture is 10-bit. That is not a different picture: it is the same
// encode with the store left at 16-bit precision, so the accumulation's own
// precision survives as far as the YUV conversion instead of being rounded to
// 8 bits one pass early.
//
// PSEncodeDithered is the 8-bit store with the ordered map in it. The map and
// its arithmetic are the cache capture's own (D3D12Renderer.cpp's
// DitherOffsetBayer8, measured in DitherPolicy.h): the value is quantised as
// floor(v*255+t) with t in (0,1) over an 8x8 Bayer cell, so a smooth gradient
// becomes a mix of its two codes in the proportion the unquantised value sits
// between them rather than a staircase. Bayer rather than the window's blue
// noise for the same reason the NVIDIA capture uses it - blue noise does not
// survive the encode this buffer feeds. Static, not per frame: a moving pattern
// is temporal noise, and it would make two renders of one source differ.
inline constexpr char kAmdTemporalConvertHlsl[] = R"(
Texture2D Input:register(t0);
float3 SRGBToLinear(float3 c){float3 lo=c/12.92;float3 hi=pow(max((c+0.055)/1.055,0),2.4);return lerp(hi,lo,step(c,0.04045));}
float3 LinearToSRGB(float3 c){c=max(c,0);float3 lo=c*12.92;float3 hi=1.055*pow(c,1.0/2.4)-0.055;return saturate(lerp(hi,lo,step(c,0.0031308)));}
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
// The same full-screen triangle kTemporalStabilityHlsl draws. Repeated rather
// than lifted into a shared string because the two programs are compiled
// separately and a shared fragment would have to be kept byte-identical for no
// gain; this one is two lines and cannot drift in a way that matters.
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float4 PSDecode(V i):SV_Target{return float4(SRGBToLinear(Input.Load(int3(int2(i.p.xy),0)).rgb),1);}
float4 PSEncode(V i):SV_Target{return float4(LinearToSRGB(Input.Load(int3(int2(i.p.xy),0)).rgb),1);}
float DitherOffsetBayer8(float2 pixel){
    uint2 p=uint2(pixel)&7u;uint d=p.x^p.y;
    uint r=((d&1u)<<5)|((p.y&1u)<<4)|((d&2u)<<2)|((p.y&2u)<<1)|((d&4u)>>1)|((p.y&4u)>>2);
    return ((float(r)+0.5)/64.0-0.5)/255.0;
}
float4 PSEncodeDithered(V i):SV_Target{
    float3 c=LinearToSRGB(Input.Load(int3(int2(i.p.xy),0)).rgb);
    return float4(c+DitherOffsetBayer8(i.p.xy),1);
}
)";

// The 10-bit capture: the frame the encode just produced, in sRGB codes, taken
// to BT.709 limited-range P010 - Y 64..940, chroma 512 +- 448 - so the encoder
// is handed samples it can read rather than a BGRA frame it must convert on the
// CPU. Two planes, one draw each, exactly as the NVIDIA capture does it.
//
// The code goes in the TOP TEN BITS of a 16-bit sample, which is what P010 is.
// The R16 store rounds f*65535 to nearest, so the code is rounded here and the
// low six bits are zero: an encoder reads the top ten bits and would truncate,
// not round, whatever the low six held.
//
// Chroma is converted per sample and averaged after, which is what a CPU 4:2:0
// conversion does; averaging the colours first would pull saturated edges
// towards grey. The four taps are the 2x2 block this chroma sample owns, read
// by index rather than by a UV offset because the block's own texels are what
// is being averaged.
//
// Both entry points read a texture that already carries sRGB-encoded codes -
// PSEncode's output, or the upscale capture's when the job upscales - so
// neither converts from linear light. That is the whole reason the encode
// leaves them in code space: one conversion, in one place, at the precision the
// capture stores.
inline constexpr char kAmdCaptureHlsl[] = R"(
Texture2D Input:register(t0);
float Luma709(float3 c){return dot(c,float3(0.2126,0.7152,0.0722));}
float P010Store(float code){return round(code)*64.0/65535.0;}
float2 ChromaCodes10(float3 c){
    return float2(512.0+896.0*dot(c,float3(-0.114572,-0.385428,0.5)),
                  512.0+896.0*dot(c,float3(0.5,-0.454153,-0.045847)));
}
float3 CodeAt(int2 p){return Input.Load(int3(p,0)).rgb;}
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float PSCaptureLuma10(V i):SV_Target{
    return P010Store(64.0+876.0*Luma709(CodeAt(int2(i.p.xy))));
}
float2 PSCaptureChroma10(V i):SV_Target{
    int2 b=int2(i.p.xy)*2;
    float2 c=ChromaCodes10(CodeAt(b));
    c+=ChromaCodes10(CodeAt(b+int2(1,0)));
    c+=ChromaCodes10(CodeAt(b+int2(0,1)));
    c+=ChromaCodes10(CodeAt(b+int2(1,1)));
    c*=0.25;
    return float2(P010Store(c.x),P010Store(c.y));
}
)";

// The NV12 source conversion: the frame the decoder delivers - Y at full size,
// interleaved UV at half - taken to the 8-bit sRGB BGRA the runtime's input
// texture carries.
//
// This is the same picture the CPU conversion would have produced, and the same
// algebra D3D12Renderer's PSSourceNv12 runs: the decoder only asks for NV12 for
// a description SourceNv12ConversionFor accepted, and the pair of programs
// implements the same four (matrix, range) pairs, so a source that plays on one
// backend decodes identically on the other.
//
// SOURCE_* are supplied at compile time from the colour description the source
// declared - never from a default - and they are preprocessor tokens rather
// than constant-buffer values for the reason CompileSourceNv12 records on the
// NVIDIA side: fxc folds each one into the multiply-add it appears in, so a
// parameterised version would be a different program with a different rounding,
// and a source that declared a matrix the pass cannot honour is refused at
// compile time rather than rounded to BT.709.
//
// The guard is what makes that refusal mechanical: HLSL compiles the whole text
// for whichever entry point is asked for, so a build of this string with no
// SOURCE_* defines has no PSSourceNv12 at all, and the caller's Compile fails
// instead of silently decoding YUV under a guessed matrix.
inline constexpr char kAmdSourceHlsl[] = R"(
Texture2D Luma:register(t0);
Texture2D Chroma:register(t1);
SamplerState S:register(s0);
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
#ifdef SOURCE_LUMA_OFFSET
float4 PSSourceNv12(V i):SV_Target{
    // Sampled rather than loaded: the chroma plane is half size, so its value at
    // a luma pixel is the bilinear blend of the 2x2 chroma neighbours that pixel
    // sits among - which is what every 4:2:0 upsampler does and what the NVIDIA
    // path's linear sampler does at the same uv.
    float y=(Luma.SampleLevel(S,i.uv,0).r*255.0-SOURCE_LUMA_OFFSET)/SOURCE_LUMA_SCALE;
    float2 c=(Chroma.SampleLevel(S,i.uv,0).rg*255.0-128.0)/SOURCE_CHROMA_SCALE;
    float3 rgb=float3(y+SOURCE_RED_V*c.y,y-SOURCE_GREEN_U*c.x-SOURCE_GREEN_V*c.y,y+SOURCE_BLUE_U*c.x);
    return float4(saturate(rgb),1);
}
#endif
)";

// The profile the entry point `entry` compiles under, for either of the two
// programs this backend compiles.
//
// Derived from the entry point's name instead of taken as an argument at the
// call site, because the call sites compile several entry points in a row out of
// two strings and the mistake is invisible until a device compiles one:
// D3DCompile answers with X4502 about an invalid semantic on the shader's own
// struct, naming neither the entry point nor the profile. Five lines that
// cannot pair "VS" with ps_5_1 are cheaper than reading that.
//
// Both programs declare exactly one vertex entry point and spell it `VS`;
// everything else - PSDecode, PSEncode, PSEncodeDithered, PSTemporalStability,
// PSCaptureLuma10, PSCaptureChroma10, PSSourceNv12 - is a pixel shader. A new
// entry point under any other name is a pixel shader until someone says
// otherwise here.
inline constexpr const char* ShaderProfileFor(std::string_view entry) noexcept
{
    return entry == "VS" ? "vs_5_1" : "ps_5_1";
}
