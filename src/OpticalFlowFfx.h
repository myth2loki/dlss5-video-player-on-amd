#pragma once

// FidelityFX Optical Flow, as a source of motion guides for the AMD neural
// render.
//
// The CPU block matcher in TemporalGuides was measured against a zero field and
// lost (see docs/measurements/amd-guide-ab-20261020): it wins 0.166 dB on real
// game motion and loses 0.892 dB on a depth pan. This is the other candidate
// for the same job - the estimator AMD ships for it, run on the GPU.
//
// It is built only when an FFX SDK is pointed at the build and its shader blobs
// have been generated (see CMakeLists); without one, Initialize() fails and
// Available() is false, so a caller that treats it as an optional upgrade needs
// no conditional of its own.
//
// Two things about the field it returns, both measured on an RX 9070 XT with a
// synthetic shift rather than taken from the header:
//
//   * the effect writes R16G16_SINT at 1/8 of the source resolution, and the
//     number is the shift in SOURCE PIXELS - a 2 px shift reads 2, an 8 px
//     shift reads 8. Nothing scales it.
//   * it is a FORWARD field, previous -> current. The guides this renderer
//     hands to the runtime are the opposite direction, current -> previous, so
//     the sign is flipped on the way out. Getting this wrong does not fail,
//     it makes the runtime reproject every frame the wrong way.
//
// The first frame after Initialize() or Reset() has no previous frame to
// compare with, so it returns a zero field rather than failing: a zero field is
// exactly what the renderer would have used anyway.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12Resource;

class OpticalFlowFfx {
public:
    OpticalFlowFfx();
    ~OpticalFlowFfx();

    OpticalFlowFfx(const OpticalFlowFfx&) = delete;
    OpticalFlowFfx& operator=(const OpticalFlowFfx&) = delete;

    // Fixed input size: the effect's context is sized to one resolution.
    bool Initialize(ID3D12Device* device, uint32_t width, uint32_t height, std::string* why = nullptr);
    void Shutdown();
    bool Available() const { return m_ready; }

    // Drops the previous frame, so the next Submit() starts a new pair.
    void Reset();

    // Feeds one frame and returns the field between it and the one before it.
    //
    // `bgra` is width * height * 4, the frame as the decoder hands it over.
    // `outMotion` is filled with width * height * 2 floats, (x, y) per pixel in
    // source pixels, pointing from this frame to where that content was in the
    // previous one - the unit and direction AmdLmxxfEvaluator::FrameGuides asks
    // for.
    //
    // The call runs the GPU work and waits for it, because the field has to be
    // read back to be handed over: this is a per-frame stall by construction,
    // and that is the reason it belongs on the offline export path and not on
    // playback.
    bool Submit(const uint8_t* bgra, uint32_t width, uint32_t height,
                std::vector<float>& outMotion, std::string* why = nullptr);

private:
    struct State;
    std::unique_ptr<State> m_state;
    bool m_ready = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
