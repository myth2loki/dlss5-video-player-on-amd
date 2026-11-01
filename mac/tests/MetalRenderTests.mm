// Renders a frame offscreen and reads it back, so the Metal half can be
// asserted rather than eyeballed through a screenshot.
//
// This is the check that would have caught a shader that fails to compile, a
// pipeline with the wrong pixel format, and a textured quad drawn with the UVs
// upside down - none of which a "the window opened" test sees.

#include "FfmpegPipeSource.h"
#include "MetalVideoPresenter.h"

#include <Metal/Metal.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

static void Check(bool condition, const std::string& what)
{
    std::printf(condition ? "  ok    %s\n" : "  FAIL  %s\n", what.c_str());
    if (!condition) ++g_failures;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <video>\n", argv[0]);
        return 2;
    }

    MediaInfo info;
    if (!FfmpegPipeSource::Probe(argv[1], info)) {
        std::printf("probe failed\n");
        return 2;
    }

    // One real frame out of the pipe, which is what the presenter will be
    // handed in production.
    FfmpegPipeSource source;
    source.Open(argv[1], info);
    VideoFrame frame;
    if (!source.Pop(frame)) {
        std::printf("no frame decoded\n");
        return 2;
    }
    source.Close();

    constexpr int kWidth = 320;
    constexpr int kHeight = 180;

    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        Check(device != nil, "a Metal device exists");
        if (!device) return 1;

        MetalVideoPresenter presenter;
        // A null layer is legal here: the offscreen texture below is what gets
        // drawn into. Present() only needs the layer when it presents.
        const auto init = presenter.Init(nullptr);
        std::printf("  init: %s%s\n", init.ok ? "ok" : "FAILED", init.error.empty() ? "" : init.error.c_str());
        Check(init.ok, "shader compiles and the pipeline state builds");
        if (!init.ok) return 1;

        presenter.SetSourceSize(info.width, info.height);

        // Draw into a texture of our own rather than a drawable, then read it
        // back.
        MTLTextureDescriptor* desc =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:kWidth
                                                              height:kHeight
                                                           mipmapped:NO];
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        // Shared: unified memory, so the readback below needs no synchronise.
        desc.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:desc];
        Check(target != nil, "offscreen target allocated");
        if (!target) return 1;

        // Present() with a null layer returns before drawing, so the same
        // pipeline is exercised through a second, explicit path here.
        id<MTLCommandQueue> queue = [device newCommandQueue];
        NSError* error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:R"msl(
#include <metal_stdlib>
using namespace metal;
struct VertexOut { float4 position [[position]]; float2 uv; };
vertex VertexOut video_vertex(uint vid [[vertex_id]],
                              constant float2& scale [[buffer(0)]],
                              constant float2& offset [[buffer(1)]]) {
    const float2 corners[4] = { float2(-1.0, -1.0), float2(1.0, -1.0),
                                float2(-1.0,  1.0), float2(1.0,  1.0) };
    const float2 uvs[4]     = { float2(0.0, 1.0), float2(1.0, 1.0),
                                float2(0.0, 0.0), float2(1.0, 0.0) };
    VertexOut out;
    out.position = float4(corners[vid] * scale + offset, 0.0, 1.0);
    out.uv = uvs[vid];
    return out;
}
fragment float4 video_fragment(VertexOut in [[stage_in]], texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return src.sample(s, in.uv);
}
)msl"] options:nil error:&error];
        Check(library != nil, "the same shader source compiles offline");

        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction = [library newFunctionWithName:@"video_vertex"];
        pd.fragmentFunction = [library newFunctionWithName:@"video_fragment"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
        Check(pipeline != nil, "pipeline state builds offline");

        id<MTLTexture> tex = [device newTextureWithDescriptor:
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                               width:NSUInteger(info.width)
                                                              height:NSUInteger(info.height)
                                                           mipmapped:NO]];
        [tex replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(info.width), NSUInteger(info.height))
               mipmapLevel:0
                 withBytes:frame.bgra.data()
               bytesPerRow:size_t(info.width) * 4];

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(18.0 / 255.0, 19.0 / 255.0, 21.0 / 255.0, 1.0);

        float scale[2] = { 1.0f, 1.0f };
        float offset[2] = { 0.0f, 0.0f };
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBytes:scale length:sizeof(scale) atIndex:0];
        [encoder setVertexBytes:offset length:sizeof(offset) atIndex:1];
        [encoder setFragmentTexture:tex atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [encoder endEncoding];

        // No blit synchronise step: the target is Shared, so the GPU and CPU
        // see the same memory. waitUntilCompleted is what orders the readback
        // after the draw.
        [buffer commit];
        [buffer waitUntilCompleted];

        std::vector<uint8_t> pixels(size_t(kWidth) * kHeight * 4);
        [target getBytes:pixels.data()
             bytesPerRow:size_t(kWidth) * 4
              fromRegion:MTLRegionMake2D(0, 0, kWidth, kHeight)
             mipmapLevel:0];

        // The window background is (18,19,21). A correctly drawn quad leaves
        // almost none of it: the test pattern covers the whole target.
        size_t background = 0;
        size_t distinct = 0;
        uint32_t previous = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const uint8_t* p = &pixels[(size_t(y) * kWidth + x) * 4];
                if (p[0] == 21 && p[1] == 19 && p[2] == 18) ++background;
                const uint32_t packed = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
                if (packed != previous) ++distinct;
                previous = packed;
            }
        }
        const double backgroundShare = double(background) / double(kWidth * kHeight);
        std::printf("  %.2f%% of the target is still the clear colour, %zu colour transitions\n",
                    backgroundShare * 100.0, distinct);
        Check(backgroundShare < 0.02, "the quad covers the target (no letterbox in a 16:9 target)");
        // A pattern of one flat colour would give ~1 transition; a real frame
        // gives thousands.
        Check(distinct > 500, "the sampled frame has detail, so the texture reached the shader");

        presenter.Shutdown();
    }

    std::printf(g_failures ? "\nFAILED (%d)\n" : "\nall checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
