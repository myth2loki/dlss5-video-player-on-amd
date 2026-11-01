#include "MetalVideoPresenter.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstring>

// The whole drawing program: one textured quad. Letterboxing shrinks the
// quad's clip-space rectangle, which is cheaper and less error-prone than a
// viewport per axis.
static const char* kShaderSource = R"msl(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

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

fragment float4 video_fragment(VertexOut in [[stage_in]],
                               texture2d<float> src [[texture(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    return src.sample(s, in.uv);
}
)msl";

struct MetalVideoPresenter::Impl {
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLRenderPipelineState> pipeline = nil;
    CAMetalLayer* layer = nil;
    id<MTLTexture> source = nil;
    int sourceWidth = 0;
    int sourceHeight = 0;
};

MetalVideoPresenter::MetalVideoPresenter() : m_impl(std::make_unique<Impl>()) {}

MetalVideoPresenter::~MetalVideoPresenter() { Shutdown(); }

MetalVideoPresenter::InitResult MetalVideoPresenter::Init(void* layer)
{
    InitResult result;
    Impl& state = *m_impl;

    state.device = MTLCreateSystemDefaultDevice();
    if (!state.device) {
        result.error = "no Metal device";
        return result;
    }
    state.queue = [state.device newCommandQueue];
    if (!state.queue) {
        result.error = "no command queue";
        return result;
    }

    state.layer = (__bridge CAMetalLayer*)layer;
    if (state.layer) {
        state.layer.device = state.device;
        // BGRA to match the pipe's pixel format, so the upload needs no swizzle.
        state.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        state.layer.framebufferOnly = YES;
    }

    NSError* error = nil;
    id<MTLLibrary> library = [state.device newLibraryWithSource:[NSString stringWithUTF8String:kShaderSource]
                                                        options:nil
                                                          error:&error];
    if (!library) {
        result.error = error ? error.localizedDescription.UTF8String : "shader compilation failed";
        return result;
    }

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"video_vertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"video_fragment"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    state.pipeline = [state.device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (!state.pipeline) {
        result.error = error ? error.localizedDescription.UTF8String : "pipeline state failed";
        return result;
    }
    result.ok = true;
    return result;
}

void MetalVideoPresenter::SetSourceSize(int width, int height)
{
    Impl& state = *m_impl;
    if (!state.device || width <= 0 || height <= 0) return;
    if (state.source && state.sourceWidth == width && state.sourceHeight == height) return;

    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:NSUInteger(width)
                                                          height:NSUInteger(height)
                                                       mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    // Shared, and the earlier choice of Managed was simply wrong. Managed
    // storage is a discrete-GPU concept: the CPU writes to a copy in system
    // memory and the driver blits it across the bus. On Apple Silicon there is
    // no bus to cross, so Managed is documented to have no effect - and since
    // macOS 27 it is deprecated outright with "use Shared storage instead".
    //
    // Shared means the texture lives in unified memory and replaceRegion writes
    // straight into what the GPU reads, with no synchronise step. That is both
    // faster and correct here; the frame is produced by ffmpeg on the CPU and
    // consumed by one draw, so there is nothing to double-buffer.
    descriptor.storageMode = MTLStorageModeShared;
    state.source = [state.device newTextureWithDescriptor:descriptor];
    state.sourceWidth = width;
    state.sourceHeight = height;
}

void MetalVideoPresenter::Present(const uint8_t* bgra, int width, int height,
                                  double drawableWidth, double drawableHeight, bool fill)
{
    Impl& state = *m_impl;
    if (!state.pipeline || !bgra || width <= 0 || height <= 0) return;
    if (!state.layer || drawableWidth <= 0.0 || drawableHeight <= 0.0) return;

    SetSourceSize(width, height);
    if (!state.source) return;

    [state.source replaceRegion:MTLRegionMake2D(0, 0, NSUInteger(width), NSUInteger(height))
                    mipmapLevel:0
                      withBytes:bgra
                    bytesPerRow:size_t(width) * 4];

    id<CAMetalDrawable> drawable = [state.layer nextDrawable];
    if (!drawable) return;

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    // The player's own window background: ui_palette::Window in UiResources.h.
    pass.colorAttachments[0].clearColor =
        MTLClearColorMake(18.0 / 255.0, 19.0 / 255.0, 21.0 / 255.0, 1.0);

    // Letterbox by aspect ratio. `fill` crops instead of fitting.
    const double sourceAspect = double(width) / double(height);
    const double targetAspect = drawableWidth / drawableHeight;
    float sx = 1.0f;
    float sy = 1.0f;
    if (fill) {
        if (sourceAspect > targetAspect) sx = float(targetAspect / sourceAspect);
        else sy = float(sourceAspect / targetAspect);
    } else {
        if (sourceAspect > targetAspect) sy = float(targetAspect / sourceAspect);
        else sx = float(sourceAspect / targetAspect);
    }
    float scale[2] = { sx, sy };
    float offset[2] = { 0.0f, 0.0f };

    id<MTLCommandBuffer> buffer = [state.queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [buffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:state.pipeline];
    [encoder setVertexBytes:scale length:sizeof(scale) atIndex:0];
    [encoder setVertexBytes:offset length:sizeof(offset) atIndex:1];
    [encoder setFragmentTexture:state.source atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];

    [buffer presentDrawable:drawable];
    [buffer commit];
}

void MetalVideoPresenter::Shutdown()
{
    Impl& state = *m_impl;
    state.pipeline = nil;
    state.source = nil;
    state.queue = nil;
    state.device = nil;
    state.layer = nil;
    state.sourceWidth = 0;
    state.sourceHeight = 0;
}
