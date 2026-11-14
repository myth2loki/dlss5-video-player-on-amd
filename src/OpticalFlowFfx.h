#pragma once
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <wrl/client.h>

// FidelityFX Optical Flow as the temporal motion guide.
//
// Why this exists: on an AMD card there is no flow engine to ask, so the guide
// came from TemporalGuides.cpp - a 160x90 CPU block matcher whose finest step is
// three pixels and which accepts 0% of its cells at half a pixel per frame.
// FidelityFX Optical Flow estimates on 8x8 blocks through a seven-level luma
// pyramid, entirely in compute: no driver API, no vendor dll, and it runs on
// NVIDIA hardware too (the SDK's own memory table lists an RTX 4080).
//
// What it is not: a frame generator. It produces a motion field and a scene-cut
// flag, nothing else. The playback interpolator stays with FidelityFX Frame
// Generation, which runs this same technique internally and additionally owns
// the warp, the blend and the disocclusion handling this class does not.
//
// The output is R16G16_SINT at one vector per 8x8 block - the same format NVOFA
// writes, on a coarser grid (NVOFA offers 1, 2 or 4). Whole pixels, not S10.5:
// the search is integer-SAD, so a caller scaling these vectors must not reuse
// NVOFA's 1/32.
class OpticalFlowFfx final {
public:
    OpticalFlowFfx();
    ~OpticalFlowFfx();
    OpticalFlowFfx(const OpticalFlowFfx&) = delete;
    OpticalFlowFfx& operator=(const OpticalFlowFfx&) = delete;

    // `width`/`height` are the resolution of the frames handed to Dispatch(),
    // which is also the unit the emitted vectors are measured in.
    bool Initialize(ID3D12Device* device, uint32_t width, uint32_t height);
    void Shutdown();
    bool Available() const { return m_ready; }

    // One vector per kGrid x kGrid block, the block side fixed by the technique.
    static constexpr uint32_t kGrid = 8;
    uint32_t Grid() const { return kGrid; }
    uint32_t FlowWidth() const { return (m_width + kGrid - 1) / kGrid; }
    uint32_t FlowHeight() const { return (m_height + kGrid - 1) / kGrid; }

    ID3D12Resource* Flow() const { return m_flow.Get(); }

    // Records the whole technique on `cmd`: luminance, pyramid, scene-change
    // histogram and divergence, then seven search/filter/scale iterations.
    // `frame` must be BGRA8 at the initialized size and already readable. The
    // field is left readable (`PIXEL_SHADER_RESOURCE`) for the resolve pass that
    // follows, and the class remembers that so the next frame can move it back.
    //
    // Unlike NVOFA this is synchronous inside the list - there is no engine to
    // fence against, and the caller has nothing to wait for.
    bool Dispatch(ID3D12CommandList* cmd, ID3D12Resource* frame, bool reset);

    // Diagnostic, and deliberately blocking: copies the field just dispatched back
    // to the CPU and counts what it holds. A pass that reports FFX_OK and writes
    // zeros passes every check that does not look at the pixels, so this exists to
    // tell "estimated" from "ran". Never call it per frame - it waits on the queue.
    bool CountNonZeroVectors(ID3D12CommandQueue* queue, uint32_t* nonZero,
                             uint32_t* total, float* meanMagnitude);

    // Failure detail from Initialize, for the log line that says which motion
    // backend came up.
    const std::wstring& LastError() const { return m_lastError; }

private:
    bool CreateResources(ID3D12Device* device);
    void Fail(const wchar_t* stage, int code);

    struct Impl;
    Impl* m_impl = nullptr;

    ID3D12Device* m_device = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_ready = false;
    D3D12_RESOURCE_STATES m_flowState = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES m_scdState = D3D12_RESOURCE_STATE_COMMON;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_flow;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_scd;

    // Readback ring for CountNonZeroVectors, created on first use. Its own list and
    // fence: the caller's frame list is mid-recording when this is asked for, and
    // closing that one to wait on a diagnostic would be a worse bug than the
    // diagnostic is worth.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_readAllocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_readList;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_readFence;
    HANDLE m_readEvent = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readback;

    std::wstring m_lastError;
};
