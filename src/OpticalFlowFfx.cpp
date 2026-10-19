#include "OpticalFlowFfx.h"

#if !defined(DLSS_VIDEO_PLAYER_HAS_FFX_OPTICAL_FLOW)

// No SDK was pointed at the build, so there is nothing to wrap. The class still
// exists - a caller asks for optical flow guides, finds none available and
// carries on with whatever it was using - which is what keeps this out of every
// header that mentions it.
OpticalFlowFfx::OpticalFlowFfx() = default;
OpticalFlowFfx::~OpticalFlowFfx() = default;
bool OpticalFlowFfx::Initialize(ID3D12Device*, uint32_t, uint32_t, std::string* why)
{
    if (why)
        *why = "this build has no FidelityFX SDK, so optical flow is unavailable";
    return false;
}
void OpticalFlowFfx::Shutdown() {}
void OpticalFlowFfx::Reset() {}
bool OpticalFlowFfx::Submit(const uint8_t*, uint32_t, uint32_t, std::vector<float>&, std::string* why)
{
    if (why)
        *why = "this build has no FidelityFX SDK, so optical flow is unavailable";
    return false;
}

#else

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "ffx_opticalflow.h"
#include "ffx_dx12.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr uint32_t kFlowBlockSize = 8;

bool Fail(std::string* why, const char* text)
{
    if (why)
        *why = text;
    return false;
}

// The flow field is one texel per 8x8 block of source pixels, rounded up.
uint32_t FlowWidth(uint32_t w) { return (w + kFlowBlockSize - 1) / kFlowBlockSize; }

}  // namespace

struct OpticalFlowFfx::State {
    ID3D12Device* device{};

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    HANDLE eventHandle{};
    uint64_t fenceValue = 0;

    std::vector<uint8_t> scratch;
    FfxInterface iface{};
    std::unique_ptr<FfxOpticalflowContext> context;
    uint32_t effectContextId = 0;
    FfxApiResource flowRes{};
    FfxApiResource scdRes{};
    ID3D12Resource* flowTex{};
    ID3D12Resource* scdTex{};

    ComPtr<ID3D12Resource> color;
    ComPtr<ID3D12Resource> upload;
    ComPtr<ID3D12Resource> readback;
    FfxApiResource ffxColor{};
    std::vector<uint8_t> rgba;

    uint32_t flowW = 0;
    uint32_t flowH = 0;
    uint64_t rowBytes = 0;
    uint64_t totalBytes = 0;

    bool hasPrevious = false;
    bool resetPending = true;

    HRESULT CloseExecuteWait()
    {
        const HRESULT hr = cmd->Close();
        if (FAILED(hr))
            return hr;
        ID3D12CommandList* lists[] = { cmd.Get() };
        queue->ExecuteCommandLists(1, lists);
        ++fenceValue;
        if (FAILED(queue->Signal(fence.Get(), fenceValue)))
            return E_FAIL;
        if (fence->SetEventOnCompletion(fenceValue, eventHandle) != S_OK)
            return E_FAIL;
        WaitForSingleObject(eventHandle, INFINITE);
        if (FAILED(allocator->Reset()))
            return E_FAIL;
        return cmd->Reset(allocator.Get(), nullptr);
    }
};

OpticalFlowFfx::OpticalFlowFfx() = default;
OpticalFlowFfx::~OpticalFlowFfx() { Shutdown(); }

bool OpticalFlowFfx::Initialize(ID3D12Device* device, uint32_t width, uint32_t height, std::string* why)
{
    Shutdown();
    if (!device || width == 0 || height == 0)
        return Fail(why, "optical flow was given no device or an empty size");

    auto state = std::make_unique<State>();
    state->device = device;

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&state->queue))))
        return Fail(why, "could not create the optical flow command queue");
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&state->allocator))))
        return Fail(why, "could not create the optical flow command allocator");
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         state->allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&state->cmd))))
        return Fail(why, "could not create the optical flow command list");
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence))))
        return Fail(why, "could not create the optical flow fence");
    state->eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!state->eventHandle)
        return Fail(why, "could not create the optical flow wait event");

    // The backend owns a scratch buffer for as long as the contexts that use it.
    state->scratch.resize(ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT));
    if (FAILED(ffxGetInterfaceDX12(&state->iface, ffxGetDeviceDX12(device),
                                   state->scratch.data(), state->scratch.size(),
                                   FFX_OPTICALFLOW_CONTEXT_COUNT)))
        return Fail(why, "the FidelityFX DX12 backend would not initialise");

    state->context = std::make_unique<FfxOpticalflowContext>();
    FfxOpticalflowContextDescription contextDesc{};
    contextDesc.backendInterface = state->iface;
    contextDesc.flags = 0;  // LDR input: the frames are display-encoded bytes
    contextDesc.resolution.width = width;
    contextDesc.resolution.height = height;
    if (ffxOpticalflowContextCreate(state->context.get(), &contextDesc) != FFX_OK)
        return Fail(why, "the FidelityFX optical flow context would not be created");

    // The two shared resources are created through the backend, not by creating
    // a texture and wrapping the pointer: the backend keeps its own table of
    // what it created, and a resource it did not create is missing the entry
    // that lets it be bound and transitioned. AMD's own fsr3framegeneration
    // provider does exactly this.
    if (state->iface.fpCreateBackendContext(&state->iface, FFX_EFFECT_OPTICALFLOW, nullptr,
                                            &state->effectContextId) != FFX_OK)
        return Fail(why, "the FidelityFX backend context would not be created");
    FfxOpticalflowSharedResourceDescriptions shared{};
    if (ffxOpticalflowGetSharedResourceDescriptions(state->context.get(), &shared) != FFX_OK)
        return Fail(why, "the optical flow shared resource descriptions are unavailable");
    FfxCreateResourceDescription flowCreateDesc = shared.opticalFlowVector;
    FfxCreateResourceDescription scdCreateDesc = shared.opticalFlowSCD;
    FfxResourceInternal flowInternal{};
    FfxResourceInternal scdInternal{};
    if (state->iface.fpCreateResource(&state->iface, &flowCreateDesc, state->effectContextId,
                                      &flowInternal) != FFX_OK)
        return Fail(why, "the optical flow vector resource could not be allocated");
    if (state->iface.fpCreateResource(&state->iface, &scdCreateDesc, state->effectContextId,
                                      &scdInternal) != FFX_OK)
        return Fail(why, "the optical flow scene-change resource could not be allocated");
    state->flowRes = state->iface.fpGetResource(&state->iface, flowInternal);
    state->scdRes = state->iface.fpGetResource(&state->iface, scdInternal);
    state->flowTex = reinterpret_cast<ID3D12Resource*>(state->flowRes.resource);
    state->scdTex = reinterpret_cast<ID3D12Resource*>(state->scdRes.resource);
    if (!state->flowTex || !state->scdTex)
        return Fail(why, "the backend returned no resource pointer for the optical flow output");

    const auto& vecDesc = shared.opticalFlowVector.resourceDescription;
    state->flowW = vecDesc.width;
    state->flowH = vecDesc.height;

    // The input frame, in the state the effect reads it in.
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&state->color))))
            return Fail(why, "the optical flow input texture could not be allocated");
    }
    const uint32_t pixelBytes = width * height * 4;
    {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = pixelBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&state->upload))))
            return Fail(why, "the optical flow upload buffer could not be allocated");
    }
    {
        D3D12_RESOURCE_DESC flowDesc = state->flowTex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows = 0;
        device->GetCopyableFootprints(&flowDesc, 0, 1, 0, &footprint, &rows,
                                      &state->rowBytes, &state->totalBytes);
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = state->totalBytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&state->readback))))
            return Fail(why, "the optical flow readback buffer could not be allocated");
    }

    FfxApiResourceDescription colorDesc{};
    colorDesc.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    colorDesc.format = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    colorDesc.width = width;
    colorDesc.height = height;
    colorDesc.depth = 1;
    colorDesc.mipCount = 1;
    colorDesc.flags = FFX_API_RESOURCE_FLAGS_NONE;
    colorDesc.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    state->ffxColor = ffxGetResourceDX12(state->color.Get(), colorDesc, L"OF_Color",
                                         FFX_API_RESOURCE_STATE_COMPUTE_READ);

    state->rgba.resize(pixelBytes);
    m_state = std::move(state);
    m_width = width;
    m_height = height;
    m_ready = true;
    return true;
}

void OpticalFlowFfx::Shutdown()
{
    if (!m_state)
        return;
    State& s = *m_state;
    if (s.context) {
        ffxOpticalflowContextDestroy(s.context.get());
        s.context.reset();
    }
    if (s.eventHandle)
        CloseHandle(s.eventHandle);
    m_state.reset();
    m_ready = false;
    m_width = 0;
    m_height = 0;
}

void OpticalFlowFfx::Reset()
{
    if (!m_ready)
        return;
    // Handed to the next dispatch: a reset clears the luminance pyramid the
    // previous frame left, which is what makes the next field meaningless in
    // exactly the way a cut should.
    m_state->hasPrevious = false;
    m_state->resetPending = true;
}

bool OpticalFlowFfx::Submit(const uint8_t* bgra, uint32_t width, uint32_t height,
                            std::vector<float>& outMotion, std::string* why)
{
    if (!m_ready)
        return Fail(why, "optical flow was submitted to before it was initialised");
    if (width != m_width || height != m_height)
        return Fail(why, "optical flow was given a frame that is not the size it was built for");
    if (!bgra)
        return Fail(why, "optical flow was given no frame");

    State& s = *m_state;
    const uint32_t pixelBytes = width * height * 4;

    // The effect reads RGBA; the decoder hands over BGRA. The luma the pyramid
    // is built from weights the channels differently, so passing the bytes
    // through unswapped measures a slightly different picture than the one on
    // screen.
    for (uint32_t i = 0; i < width * height; ++i) {
        s.rgba[i * 4 + 0] = bgra[i * 4 + 2];
        s.rgba[i * 4 + 1] = bgra[i * 4 + 1];
        s.rgba[i * 4 + 2] = bgra[i * 4 + 0];
        s.rgba[i * 4 + 3] = bgra[i * 4 + 3];
    }
    void* mapped = nullptr;
    if (FAILED(s.upload->Map(0, nullptr, &mapped)))
        return Fail(why, "the optical flow upload buffer would not map");
    std::memcpy(mapped, s.rgba.data(), pixelBytes);
    s.upload->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = s.color.Get();
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s.cmd->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = s.upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = 0;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = width;
    src.PlacedFootprint.Footprint.Height = height;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = width * 4;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = s.color.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    s.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER toRead = toCopy;
    toRead.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    toRead.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    s.cmd->ResourceBarrier(1, &toRead);

    FfxOpticalflowDispatchDescription dispatch{};
    dispatch.commandList = ffxGetCommandListDX12(s.cmd.Get());
    dispatch.color = s.ffxColor;
    dispatch.opticalFlowVector = s.flowRes;
    dispatch.opticalFlowSCD = s.scdRes;
    dispatch.reset = s.resetPending;
    dispatch.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    dispatch.minMaxLuminance.x = 0.0f;
    dispatch.minMaxLuminance.y = 1.0f;
    if (ffxOpticalflowContextDispatch(s.context.get(), &dispatch) != FFX_OK)
        return Fail(why, "the optical flow dispatch failed");
    s.resetPending = false;

    if (FAILED(s.CloseExecuteWait()))
        return Fail(why, "the optical flow dispatch would not execute");

    const bool hadPrevious = s.hasPrevious;
    s.hasPrevious = true;

    outMotion.assign(static_cast<size_t>(width) * height * 2, 0.0f);
    // The first frame has nothing behind it. A zero field is what the renderer
    // would have used anyway, so it is returned rather than reported as a
    // failure the caller has to special-case.
    if (!hadPrevious)
        return true;

    D3D12_RESOURCE_BARRIER toSource{};
    toSource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSource.Transition.pResource = s.flowTex;
    toSource.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    toSource.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toSource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s.cmd->ResourceBarrier(1, &toSource);
    D3D12_TEXTURE_COPY_LOCATION flowSrc{};
    flowSrc.pResource = s.flowTex;
    flowSrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    flowSrc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION flowDst{};
    flowDst.pResource = s.readback.Get();
    flowDst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    D3D12_RESOURCE_DESC flowDesc = s.flowTex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    s.device->GetCopyableFootprints(&flowDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    flowDst.PlacedFootprint = footprint;
    s.cmd->CopyTextureRegion(&flowDst, 0, 0, 0, &flowSrc, nullptr);
    D3D12_RESOURCE_BARRIER toUav = toSource;
    toUav.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toUav.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    s.cmd->ResourceBarrier(1, &toUav);
    if (FAILED(s.CloseExecuteWait()))
        return Fail(why, "the optical flow field would not be read back");

    std::vector<uint8_t> bytes(static_cast<size_t>(s.totalBytes));
    {
        const D3D12_RANGE range{ 0, static_cast<SIZE_T>(s.totalBytes) };
        void* flowMapped = nullptr;
        if (FAILED(s.readback->Map(0, &range, &flowMapped)))
            return Fail(why, "the optical flow readback buffer would not map");
        std::memcpy(bytes.data(), flowMapped, bytes.size());
        s.readback->Unmap(0, &range);
    }

    // Expand the block field onto the frame and flip its direction. The effect
    // reports previous -> current in source pixels; the guides this renderer
    // hands to the runtime point the other way, current -> previous, so the sign
    // is inverted here rather than anywhere downstream where it would be easy to
    // miss.
    const uint32_t blocksX = FlowWidth(width);
    const uint32_t blocksY = FlowWidth(height);
    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t by = std::min(y / kFlowBlockSize, blocksY - 1);
        const uint8_t* row = bytes.data() + static_cast<size_t>(by) * s.rowBytes;
        float* out = outMotion.data() + static_cast<size_t>(y) * width * 2;
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t bx = std::min(x / kFlowBlockSize, blocksX - 1);
            const size_t offset = static_cast<size_t>(bx) * 4;
            const int16_t vx = static_cast<int16_t>(row[offset] | (row[offset + 1] << 8));
            const int16_t vy = static_cast<int16_t>(row[offset + 2] | (row[offset + 3] << 8));
            out[x * 2 + 0] = -static_cast<float>(vx);
            out[x * 2 + 1] = -static_cast<float>(vy);
        }
    }
    return true;
}

#endif  // DLSS_VIDEO_PLAYER_HAS_FFX_OPTICAL_FLOW
