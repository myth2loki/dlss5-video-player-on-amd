#pragma once
#include <cstdint>

// Playback frame-generation runtime policy.
//
// The FidelityFX island can come up, fail, be asked for, and actually double
// the swapchain - four facts that are easy to conflate. This header is the one
// place that turns them into the two questions the UI and the renderer ask:
//   * is the control usable right now? (RuntimeAvailable)
//   * is the screen actually showing interpolated frames? (DoublingActive)
// and the one question the island asks each frame: discard accumulated history?
// (ResetHistory). Keeping it here stops the menu, the toolbar tip, the status
// pill and the renderer from each re-deriving (and diverging on) the same
// truth.
//
// NOTE: this file was reconstructed to unblock the build. Commit 9f0face added
// the #include and the playback_frame_gen::* usages but did not add the header
// itself; the struct field order and the three predicates below are taken
// verbatim from src/D3D12Renderer.h / src/D3D12Renderer.cpp and the
// playback_frame_generation_policy_separates_request_from_runtime_state_test
// in tests/PolicyTests.cpp. Reconcile with the author's original before
// committing.
namespace playback_frame_gen {

// Runtime frame-generation status, in the exact braced-init order the renderer
// builds it: {requested, islandUp, failed, doublingPresented}. Trailing fields
// default to false, so the four-element initializers the tests already use stay
// valid.
struct RuntimeStatus {
    bool requested = false;           // the user has asked for playback frame generation
    bool islandUp = false;            // the FidelityFX island came up (own optical flow ready)
    bool failed = false;             // the island failed and was abandoned this session
    bool doublingPresented = false;  // a generated/source present pair was actually shown
    bool doublingEstablished = false;// a pair has been shown and nothing has revoked it since
};

// The playback-frame-generation control is usable: the island is up and has not
// (yet) been abandoned by a failure. Independent of whether the user asked.
inline bool RuntimeAvailable(const RuntimeStatus& s)
{
    return s.islandUp && !s.failed;
}

// The swapchain is actually being doubled right now: the user asked, the island
// is up and healthy, and a generated/source pair was just presented. The status
// chip scales its rate by this, so it never claims frames the screen does not
// show.
inline bool DoublingActive(const RuntimeStatus& s)
{
    return s.requested && s.islandUp && !s.failed && s.doublingPresented;
}

// What the UI reports as "Interpolate . On": the user asked, the island is up
// and healthy, and the cadence has been shown at least once since the session
// was (re)started - and not revoked by a failure or by the switch going off.
//
// Deliberately NOT DoublingActive. That one answers "did the frame that was
// just presented come in a pair?", which the renderer clears at the top of
// every frame and only sets again at the bottom of a frame that completed both
// presents. A reset frame (seek, dropped frame, scene cut), a raw diagnostic
// view, or a single frame the island could not complete all leave it false for
// one frame, and the pill painted from it strobed between On and Off at the
// drop rate. The pill is a control's state, not a per-frame counter: it is
// asked by WM_PAINT on the window thread while the playback thread is midway
// through a frame, so it must not read a flag that frame is still writing.
inline bool DoublingEstablished(const RuntimeStatus& s)
{
    return s.requested && s.islandUp && !s.failed && s.doublingEstablished;
}

// Whether the runtime should discard its accumulated history this frame:
// on the very first frame (no history exists yet), on a genuine temporal
// discontinuity the pipeline declared, or when the session was re-enabled (so
// it must not reuse the pre-off history). Mirrors the renderer's ResetHistory
// call: ResetHistory(m_fgFrameId, temporalReset, m_fgResetPending).
inline bool ResetHistory(uint64_t frameId, bool temporalReset, bool resetPending)
{
    return frameId == 0 || temporalReset || resetPending;
}

}  // namespace playback_frame_gen

// ---- Which texture the interpolation island reads -------------------------
//
// C (playback frame generation) used to sit BEFORE the SR chain at render
// resolution and read m_dlssColor - the neural frame, which is also the
// upscaler's INPUT. The "reuse the upscaled rung" topology moved C after B #1,
// so it reads m_upscaleSharp (the chain's OUTPUT) at the output rung instead.
// That made the island depend on the SR chain: with interpolation on, turning
// the SR switch off still had to dispatch FSR 4 just to produce a frame C
// could read - the pill said Off while the upscaler kept running, and the only
// way to really stop it was to turn interpolation off too.
//
// The island's bridge textures and the FidelityFX context are sized once at
// Initialize and cannot be resized under a live context, so the third case
// keeps that geometry and changes WHAT the convert pass reads: with the SR
// switch off it goes back to the upscaler's INPUT (m_dlssColor) and resamples
// it to the island's latched rung inside the convert pass itself. No FSR 4
// dispatch, no renderer rebuild, and the source half of the cadence already
// presents that same texture through the compositor stretch.
//
// `chainAtOutputRung` is the renderer's latch (m_fgUsesUpscaledInput), not a
// live measurement: it says the SR chain was built and the island was latched
// to its output size. `upscaleChainServing` is whether B #1 actually runs this
// frame - the user's SR switch AND every resource it needs. It has no
// frame-generation term, so an Off pill can never keep the chain alive. Asking
// whether the chain SERVES rather than only whether it was requested also
// keeps a half-built chain from being read as if it held a frame.
namespace frame_gen_input {

enum class Mode : uint8_t {
    UpscaleInput,        // no SR chain: legacy 1:1 read at render resolution
    UpscaleOutput,       // SR on: 1:1 read of B #1's output at the output rung
    UpscaleInputScaled,  // SR off but the chain exists: read the INPUT, resample
};

struct Decision {
    Mode mode{};
    bool readsUpscaleOutput{};  // m_upscaleSharp rather than m_dlssColor
    bool resamplesInput{};      // the source is smaller than the island's rung
};

inline constexpr Decision Decide(bool chainAtOutputRung, bool upscaleChainServing) noexcept
{
    if (!chainAtOutputRung) return {Mode::UpscaleInput, false, false};
    if (upscaleChainServing) return {Mode::UpscaleOutput, true, false};
    return {Mode::UpscaleInputScaled, false, true};
}

}  // namespace frame_gen_input
