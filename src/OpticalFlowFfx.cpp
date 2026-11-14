#include "OpticalFlowFfx.h"

#if defined(DLSS_VIDEO_HAVE_FFX_OF)

// The SDK's public types only. ffx_dx12.h brings the DX12 backend entry points
// that turn an ID3D12Device, a list and a resource into the SDK's abstractions.
#include "ffx_dx12.h"
#include "ffx_opticalflow.h"

#include <malloc.h> // _aligned_malloc / _aligned_free
#include <cmath>    // std::sqrt, for the diagnostic's mean magnitude

namespace {

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    cmd->ResourceBarrier(1, &barrier);
}

FfxApiResourceDescription Texture(uint32_t width, uint32_t height, uint32_t format)
{
    FfxApiResourceDescription description{};
    description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    description.format = format;
    description.width = width;
    description.height = height;
    description.depth = 1;
    description.mipCount = 1;
    description.flags = 0;
    description.usage = 0;
    return description;
}

} // namespace

struct OpticalFlowFfx::Impl {
    FfxInterface iface{};
    FfxOpticalflowContext context{};
    bool contextCreated = false;
    void* scratch = nullptr;
    size_t scratchSize = 0;
    uint64_t readFenceValue = 0;
};

OpticalFlowFfx::OpticalFlowFfx() = default;

OpticalFlowFfx::~OpticalFlowFfx() { Shutdown(); }

void OpticalFlowFfx::Fail(const wchar_t* stage, int code)
{
    m_ready = false;
    m_lastError = std::wstring(L"Optical flow ") + stage + L" failed (" +
                  std::to_wstring(code) + L")";
}

bool OpticalFlowFfx::Initialize(ID3D12Device* device, uint32_t width, uint32_t height)
{
    Shutdown();
    if (!device || width == 0 || height == 0) {
        Fail(L"geometry", -1);
        return false;
    }

    m_impl = new Impl();
    m_impl->scratchSize = ffxGetScratchMemorySizeDX12(1);
    if (m_impl->scratchSize == 0) {
        Fail(L"scratch size", -2);
        return false;
    }
    // The SDK hands out sizes but no storage: the caller owns the scratch and
    // must keep it alive for the context's whole life. 64-byte alignment matches
    // the cache line the backend's allocator assumes.
    m_impl->scratch = _aligned_malloc(m_impl->scratchSize, 64);
    if (!m_impl->scratch) {
        Fail(L"scratch alloc", -3);
        return false;
    }

    FfxErrorCode error = ffxGetInterfaceDX12(&m_impl->iface, ffxGetDeviceDX12(device),
                                             m_impl->scratch, m_impl->scratchSize, 1);
    if (error != FFX_OK) {
        Fail(L"backend interface", error);
        return false;
    }

    FfxOpticalflowContextDescription description{};
    description.backendInterface = m_impl->iface;
    description.flags = 0;
    description.resolution.width = width;
    description.resolution.height = height;

    error = ffxOpticalflowContextCreate(&m_impl->context, &description);
    if (error != FFX_OK) {
        Fail(L"context", error);
        return false;
    }
    m_impl->contextCreated = true;

    m_device = device;
    m_width = width;
    m_height = height;
    if (!CreateResources(device)) return false;

    m_ready = true;
    m_lastError.clear();
    return true;
}

bool OpticalFlowFfx::CreateResources(ID3D12Device* device)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    const auto make = [&](uint32_t width, uint32_t height, DXGI_FORMAT format,
                          const wchar_t* name) -> Microsoft::WRL::ComPtr<ID3D12Resource> {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        const HRESULT hr = device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&resource));
        if (FAILED(hr)) return nullptr;
        resource->SetName(name);
        return resource;
    };

    // One vector per 8x8 block, signed 16-bit per axis: the format the resolve
    // pass already reads, which is why this backend can share that pass with
    // NVOFA instead of owning a second copy of it.
    m_flow = make(FlowWidth(), FlowHeight(), DXGI_FORMAT_R16G16_SINT, L"OF_FlowVector");
    if (!m_flow) {
        Fail(L"flow resource", -4);
        return false;
    }
    m_flowState = D3D12_RESOURCE_STATE_COMMON;

    // 3x1 SCD result: slot 0 is the bit pattern of a float sceneChangeValue
    // (the shader fires above 0.45), slot 1 holds the last four frames' threshold
    // bits, slot 2 is a workgroup counter. Deliberately NOT consumed. The player's
    // CPU cut classifier (TemporalGuides.cpp / SceneCut.h) already decides cuts
    // and feeds FSR 4's history reset on that same frame; an offline replay of
    // this histogram divergence over the labelled corpus found 5/5 cuts at
    // 0.854-0.953 with zero false positives (0.216 elsewhere) - accurate, but no
    // information the CPU classifier lacks. Reading it back would add a GPU sync
    // per frame, and description.reset clears this buffer on every accepted cut,
    // so the two detectors cannot be independent anyway. See
    // docs/ARCHITECTURE.md, "Scene-change output". The dispatch still requires
    // the binding, which is why the resource exists.
    m_scd = make(3, 1, DXGI_FORMAT_R32_UINT, L"OF_SceneChange");
    if (!m_scd) {
        Fail(L"scene-change resource", -5);
        return false;
    }
    m_scdState = D3D12_RESOURCE_STATE_COMMON;
    return true;
}

bool OpticalFlowFfx::Dispatch(ID3D12CommandList* cmdList, ID3D12Resource* frame, bool reset)
{
    if (!m_ready || !cmdList || !frame) return false;
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(cmdList);

    // The SDK records its passes but does not transition resources, so both
    // sides of the technique are this class's responsibility: writes need UAV,
    // and the caller reads the field right after.
    Barrier(cmd, m_flow.Get(), m_flowState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_flowState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    Barrier(cmd, m_scd.Get(), m_scdState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_scdState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

    FfxOpticalflowDispatchDescription description{};
    description.commandList = ffxGetCommandListDX12(cmdList);
    description.color = ffxGetResourceDX12(
        frame, Texture(m_width, m_height, FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM),
        L"OF_Color", FFX_API_RESOURCE_STATE_COMPUTE_READ);
    description.opticalFlowVector = ffxGetResourceDX12(
        m_flow.Get(), Texture(FlowWidth(), FlowHeight(), FFX_API_SURFACE_FORMAT_R16G16_SINT),
        L"OF_Vector", FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    description.opticalFlowSCD = ffxGetResourceDX12(
        m_scd.Get(), Texture(3, 1, FFX_API_SURFACE_FORMAT_R32_UINT), L"OF_SCD",
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    description.reset = reset;
    // Decoded video is gamma-encoded Rec.709 in an UNORM texture, which is what
    // the sRGB arm of the luminance conversion expects. minMaxLuminance stays
    // zero: it only matters for PQ/scRGB inputs, and a zero pair is the SDK's
    // own "no HDR range supplied" value.
    description.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    description.minMaxLuminance = {0.0f, 0.0f};

    const FfxErrorCode error = ffxOpticalflowContextDispatch(&m_impl->context, &description);
    if (error != FFX_OK) {
        Fail(L"dispatch", error);
        return false;
    }

    Barrier(cmd, m_flow.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_flowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

bool OpticalFlowFfx::CountNonZeroVectors(ID3D12CommandQueue* queue, uint32_t* nonZero,
                                         uint32_t* total, float* meanMagnitude)
{
    if (!m_ready || !queue || !nonZero || !total || !meanMagnitude) return false;

    const uint32_t width = FlowWidth();
    const uint32_t height = FlowHeight();
    const uint32_t rowPitch = (width * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) /
                              D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    const uint64_t byteSize = uint64_t(rowPitch) * height;

    if (!m_readback) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = byteSize;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&m_readback))))
            return false;
        m_readback->SetName(L"OF_FlowReadback");
    }
    if (!m_readAllocator &&
        FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                IID_PPV_ARGS(&m_readAllocator))))
        return false;
    if (!m_readList && FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                          m_readAllocator.Get(), nullptr,
                                                          IID_PPV_ARGS(&m_readList))))
        return false;
    if (!m_readFence &&
        FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_readFence))))
        return false;
    if (!m_readEvent) {
        m_readEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_readEvent) return false;
    }

    m_readAllocator->Reset();
    m_readList->Reset(m_readAllocator.Get(), nullptr);

    // The field is a shader read right now, because that is how Dispatch leaves it
    // for the resolve pass. Bounce it through COPY_SOURCE and back so the state the
    // renderer recorded against is untouched.
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = m_flow.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = m_flowState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    m_readList->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = m_readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16_SINT;
    destination.PlacedFootprint.Footprint.Width = width;
    destination.PlacedFootprint.Footprint.Height = height;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = m_flow.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    m_readList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = m_flowState;
    m_readList->ResourceBarrier(1, &barrier);

    if (FAILED(m_readList->Close())) return false;
    ID3D12CommandList* lists[] = {m_readList.Get()};
    queue->ExecuteCommandLists(1, lists);
    const uint64_t value = ++m_impl->readFenceValue;
    if (FAILED(queue->Signal(m_readFence.Get(), value))) return false;
    if (m_readFence->GetCompletedValue() < value) {
        if (FAILED(m_readFence->SetEventOnCompletion(value, m_readEvent))) return false;
        if (WaitForSingleObject(m_readEvent, 5000) != WAIT_OBJECT_0) return false;
    }

    void* mapped = nullptr;
    if (FAILED(m_readback->Map(0, nullptr, &mapped))) return false;
    uint32_t nonZeroCount = 0;
    uint32_t cellCount = 0;
    double magnitudeSum = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const int16_t*>(
            static_cast<const uint8_t*>(mapped) + uint64_t(y) * rowPitch);
        for (uint32_t x = 0; x < width; ++x) {
            const int32_t vx = row[x * 2];
            const int32_t vy = row[x * 2 + 1];
            ++cellCount;
            if (vx != 0 || vy != 0) {
                ++nonZeroCount;
                magnitudeSum += std::sqrt(double(vx) * vx + double(vy) * vy);
            }
        }
    }
    m_readback->Unmap(0, nullptr);

    *nonZero = nonZeroCount;
    *total = cellCount;
    *meanMagnitude = nonZeroCount ? float(magnitudeSum / nonZeroCount) : 0.0f;
    return true;
}

void OpticalFlowFfx::Shutdown()
{
    if (m_impl) {
        if (m_impl->contextCreated) {
            ffxOpticalflowContextDestroy(&m_impl->context);
            m_impl->contextCreated = false;
        }
        if (m_impl->scratch) {
            _aligned_free(m_impl->scratch);
            m_impl->scratch = nullptr;
        }
        delete m_impl;
        m_impl = nullptr;
    }
    m_flow.Reset();
    m_scd.Reset();
    m_readList.Reset();
    m_readAllocator.Reset();
    m_readback.Reset();
    m_readFence.Reset();
    if (m_readEvent) {
        CloseHandle(m_readEvent);
        m_readEvent = nullptr;
    }
    m_flowState = D3D12_RESOURCE_STATE_COMMON;
    m_scdState = D3D12_RESOURCE_STATE_COMMON;
    m_device = nullptr;
    m_width = 0;
    m_height = 0;
    m_ready = false;
}

#else // !DLSS_VIDEO_HAVE_FFX_OF

// A build without the SDK still compiles every caller: the class exists, reports
// itself unavailable, and the renderer falls back to the CPU block matcher by the
// same line it would take if the technique had failed to come up on this device.
OpticalFlowFfx::OpticalFlowFfx() = default;
OpticalFlowFfx::~OpticalFlowFfx() = default;

bool OpticalFlowFfx::Initialize(ID3D12Device*, uint32_t, uint32_t)
{
    m_lastError = L"Optical flow unavailable: this build was configured without the FidelityFX SDK";
    m_ready = false;
    return false;
}

void OpticalFlowFfx::Shutdown()
{
    m_flow.Reset();
    m_scd.Reset();
    m_flowState = D3D12_RESOURCE_STATE_COMMON;
    m_scdState = D3D12_RESOURCE_STATE_COMMON;
    m_device = nullptr;
    m_width = 0;
    m_height = 0;
    m_ready = false;
}

bool OpticalFlowFfx::Dispatch(ID3D12CommandList*, ID3D12Resource*, bool) { return false; }

bool OpticalFlowFfx::CountNonZeroVectors(ID3D12CommandQueue*, uint32_t*, uint32_t*, float*)
{
    return false;
}

#endif // DLSS_VIDEO_HAVE_FFX_OF
