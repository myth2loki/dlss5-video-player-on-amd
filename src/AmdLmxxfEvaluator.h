#pragma once

// Stage B of docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md.
//
// The AMD backend behind the renderer's existing INeuralFrameEvaluator seam, so
// the offline job's frame loop, retry accounting and capture plumbing are the
// ones production already runs - nothing here re-implements a render loop.
//
// What this owns that the NVIDIA evaluator does not:
//
//   * A D3D12 device and one DIRECT queue of its own (or a borrowed pair), which
//     is what LmxxfNrCreateInfo wants. Nothing crosses processes.
//   * One PERSISTENT colour texture, uploaded into every frame. The runtime's
//     own documentation is explicit that changing the colour pointer, size,
//     format or exposure binding rebuilds or rebinds the codec chain and may
//     wait on the GPU, so the texture is created once in Initialize and its
//     pointer never moves.
//   * The documented per-frame call order, one job at a time (the session's
//     state machine allows exactly one job in flight):
//
//       upload into the persistent colour texture (+ barrier to the declared
//       read state) -> [rewrite the guide textures from this frame's files] ->
//       PrepareFrame -> RecordInputs -> Close + Execute (producer) ->
//       EnqueueHip -> fence wait -> RecordOutputs + transition private_output ->
//       COPY_SOURCE + copy into the readback buffer -> Close + Execute
//       (consumer) -> fence wait -> Retire
//
//     The output reader is submitted BEFORE Retire, which is the contract the
//     ABI states for output reuse ("submit the output reader before Retire,
//     Drain, or Destroy"). The stage A probe copied after Retire; this does not.
//   * The produced frame comes from job.private_output, NOT from the colour
//     texture: the colour texture is the codec's INPUT. See
//     third_party/lmxxf/LmxxfNrApi.h.
//
// Not in this stage (they are stage C's job): the backend-neutral evidence
// chain, the cache identity, the runtime lock and the preflight probe. The
// renderer's NVIDIA evidence gates are therefore not meaningful here and a
// stage B job runs with requireNeural = false; see tests/AmdLmxxfEvaluatorSmoke.cpp
// for exactly what is and is not proven by a run through this evaluator.

#include "AmdLmxxfRuntime.h"
#include "AmdLmxxfSession.h"
#include "MediaSource.h"
#include "OfflineNeuralRenderer.h"
#include "PixelLayout.h"
#include "TemporalStabilityPolicy.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <utility>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Where the runtime pack and the device come from. Every default resolves to the
// layout docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md section 3.4 describes:
//
//   runtimeDirectory/            LmxxfNrRuntime.dll, shaders/ (the runtime's)
//     lmxxf-modules/             gfx1200/ + gfx1201/, 40 hsaco each
//     native-game-tiled-assets/  weights + shaders   (LMXXF_WEIGHTS_DIR)
struct AmdLmxxfEvaluatorConfig {
    // Holds LmxxfNrRuntime.dll. Required.
    std::filesystem::path runtimeDirectory;
    // The directory Create is handed as assets_directory. Empty means
    // runtimeDirectory/lmxxf-modules, which is the only layout the runtime
    // accepts: native-game-tiled-assets/HIP carries a stale 24-per-arch subset
    // and Create refuses it with "SHA256SUMS incomplete".
    std::filesystem::path modulesDirectory;
    // The weights and shaders the runtime reads through LMXXF_WEIGHTS_DIR.
    // Empty means runtimeDirectory/native-game-tiled-assets.
    std::filesystem::path weightsDirectory;
    // Borrowed device and queue, or null to create a pair on the AMD adapter.
    // A shared device is what stage C wants (the worker's renderer owns them);
    // the smoke test leaves them null.
    ID3D12Device* device{};
    ID3D12CommandQueue* queue{};
    // LmxxfNrCreateInfo::flags. The zero-output fallback is a recovery mode for
    // a live queue contract and the recording leases are a separate job model;
    // this carries whichever the caller wants, and temporalHistory ORs the
    // lease flag in on top of it.
    uint32_t createFlags{0};

    // The offline temporal experiment (docs/AMD-LMXXF-INTEGRATION-PLAN-REV2-zh.md
    // section 5.3). The runtime only engages its temporal model under the
    // recording-lease lifecycle, so this changes the create flags AND the
    // per-frame call order - it is not a switch on the frame info alone.
    //
    // The guides it supplies were CONSTANT when this was written, because the
    // source it was written for is synthetic and its motion is analytically
    // known: a still scene has zero motion, and a pan has a fixed per-frame
    // displacement. That is the point of measuring here first - it asks what
    // the temporal model does when it is told the truth, with no estimator in
    // the loop at all. A real video host has to compute both guides, and
    // section 5.3 says why that is the expensive half.
    //
    // The player now computes them: the offline renderer's AMD adapter runs the
    // same TemporalGuideGenerator the NVIDIA path runs (the CPU block matcher,
    // because the hardware flow engine is NVIDIA's and never comes up on an AMD
    // card) and hands each frame's field to Submit, where it takes precedence
    // over these constants. The constants remain what a caller that supplies no
    // field gets, which is the smoke test's synthetic sources and a job whose
    // guide controls are both off.
    bool temporalHistory{false};
    // Motion in frame pixels per frame, (x, y), current frame <- previous. The
    // motion texture carries this one value everywhere. Zero is a still scene.
    float motionPixelsPerFrameX{};
    float motionPixelsPerFrameY{};
    // What the depth texture carries, 0 = near and 1 = far. The runtime reads
    // depth to judge what the reprojection may trust, and a synthetic source has
    // no geometry, so a single plane is the honest answer for it.
    float depthValue{1.0f};

    // Per-frame guides read from files instead of held constant, in the layout
    // src/GuideFiles.h defines: one directory per guide, one Portable Float Map
    // per source frame named by the frame's own number, motion as three channels
    // of frame-pixel displacement, depth as one channel in [0,1]. Empty means
    // the two constants above and nothing else, so the two sources compose - a
    // caller may drive motion from files and leave depth constant.
    //
    // A field the caller computed itself (Submit's `guides`) takes precedence
    // over these directories, and the offline renderer's AMD adapter now always
    // supplies one - so on the player's own path these are the smoke test's
    // input, not the product's. They stay because the smoke test constructs
    // this evaluator directly, with no adapter to run an estimator for it.
    //
    // This is the half a synthetic source cannot answer. With files the host is
    // fed the motion and depth a real clip actually has, so the measurement run 5
    // takes on a still scene can be taken on moving content; and it is the only
    // way to ask whether the stored vector is READ at all, which a constant
    // cannot distinguish from a texture the runtime never samples.
    //
    // The number in the filename is the source's own 0-based decoded index
    // (FrameIdentity::frameNumber), which is what tools/benchmark's `--guide-dump`
    // writes and what the renderer's estimator is indexed by - so a dump can be
    // fed straight back, frame for frame, without renumbering.
    //
    // The texture POINTER never moves and the texture SIZE never changes, which
    // is the persistent-input rule the colour texture follows too. Only the
    // contents change, which is unavoidable: a real host computes a new field
    // every frame. A missing or malformed file FAILS the frame rather than
    // falling back to the constant, because a run that quietly mixed the two
    // would be a measurement of neither.
    std::filesystem::path motionGuideDirectory;
    std::filesystem::path depthGuideDirectory;

    // How much of the motion-compensated history the frame keeps
    // (TemporalStabilityPolicy.h). This is the player's own pass, not the
    // runtime's: it runs after the runtime has published the frame and before the
    // readback takes it, so nothing about it is a runtime feature and
    // TemporalStability::Off is the exact code path this evaluator had before the
    // pass existed. It is also NOT the same switch as `temporalHistory` above -
    // that one asks the runtime to fuse its own model under the lease lifecycle
    // and needs guides, while this one needs nothing from the runtime at all.
    //
    // No motion estimator is behind it: the pass decides trust from the two
    // source frames themselves, and this backend hands it a zero motion field, so
    // it accumulates where the source held still and returns the frame untouched
    // everywhere else. That is the whole of "P1" in
    // docs/AMD-TEMPORAL-ACCUMULATION-zh.md, and it is why the default here can be
    // a rung at all on a backend with no flow at all.
    //
    // Off rather than a rung is the struct's own default so that an evaluator
    // built without saying anything renders the picture it always did; the
    // caller that wants the default accumulation the backend ships with asks for
    // it by name (kAmdDefaultStability in AmdLmxxfPolicy.h).
    TemporalStability stability{TemporalStability::Off};

    // The capture size, when the export asked to upscale (UpscalingTarget's
    // rungs; equal to the frame size when it did not). The runtime is a
    // fixed-box denoiser and renders 1:1, so the capture is this evaluator's
    // own pass over the runtime's output - a Catmull-Rom stretch and a CAS
    // sharpen (AmdUpscaleShader.h) - rather than something the runtime is
    // asked for. A capture SMALLER than the frame is still refused: that is
    // not an upscale and the pass does not downsize.
    uint32_t outputWidth{};
    uint32_t outputHeight{};
    // The CAS strength on the stretched frame, 0..1. 0 is the stretch alone;
    // 1 is the official filter at full weight. It rides the draw as a constant
    // rather than a bring-up fact, so it is not in the reuse question below.
    float upscaleSharpness{0.6f};

    // Hand the capture to the encoder as P010 - 10-bit 4:2:0, BT.709 limited
    // range - instead of 8-bit BGRA. This is what the EncoderQuality rungs above
    // Standard ask the NVIDIA path for, and until now this backend answered by
    // storing 8 bits and letting ffmpeg expand them, which carried no precision
    // the picture did not already have.
    //
    // It needs the accumulation, and that is not an accident of the code. The
    // runtime publishes 8-bit sRGB, so the only place this backend holds more
    // than 8 bits is the FP16 history slot a rung keeps; without a rung there is
    // nothing for a 10-bit store to carry, and the capture stays 8-bit. 4:2:0
    // also needs an even capture size, the same rule the NVIDIA capture applies.
    // Either way the fallback is logged rather than silent.
    bool tenBitCapture{false};
    // The ordered dither at the 8-bit capture store, the same Bayer map the
    // NVIDIA capture uses (DitherPolicy.h states why Bayer there and blue noise
    // in the window). It applies only to an 8-bit store: the rung that can carry
    // the precision does not need the map, which is the rule the NVIDIA path
    // applies too. Off by default so an evaluator built without saying anything
    // writes the bytes it always did.
    bool captureDither{false};

    // Per-stage trace, for the smoke test and for a receipt. Empty disables it.
    std::function<void(std::string_view)> log;

    // ---- the source's own layout ------------------------------------------
    //
    // The bytes the caller submits, in PixelLayout's terms, and the colour
    // description they are encoded under when they are NV12. BGRA is the
    // default because it is what every caller before this one handed over, and
    // an evaluator built without saying anything reads exactly that.
    //
    // NV12 is not a different picture, it is a different number of bytes on the
    // wire: the decoder can hand Y+UV straight from its own pipe instead of
    // paying an ffmpeg conversion per frame, and this evaluator converts it on
    // the GPU into the same persistent colour texture - one full-screen draw
    // (kAmdSourceHlsl) in place of the row-by-row upload. The colour texture the
    // runtime is told about does not change: same size, same format, same
    // pointer, so nothing downstream of the upload knows which path ran.
    //
    // The conversion is resolved at Initialize from SourceNv12ConversionFor,
    // which is the same predicate the decoder uses to decide it may deliver
    // NV12 at all - so a pair that reaches here is one the pass implements. A
    // description it does not implement is REFUSED rather than converted under
    // a guessed matrix: this is the one place where a wrong guess would be
    // silently wrong colour rather than a failed render.
    PixelLayout sourceLayout{PixelLayout::Bgra};
    SourceColorDescription sourceColor{};
};

class AmdLmxxfEvaluator final : public INeuralFrameEvaluator
{
public:
    explicit AmdLmxxfEvaluator(AmdLmxxfEvaluatorConfig config);
    ~AmdLmxxfEvaluator() override;
    AmdLmxxfEvaluator(const AmdLmxxfEvaluator&) = delete;
    AmdLmxxfEvaluator& operator=(const AmdLmxxfEvaluator&) = delete;

    bool Initialize(HWND renderWindow, uint32_t width, uint32_t height, uint32_t outputWidth,
                    uint32_t outputHeight, double fps, const GuideControls& guides) override;
    bool Submit(const OfflineDecodedFrame& frame, const FrameIdentity& id, bool capture,
                OfflineEvaluation& out) override;
    bool FeatureCreated() const override;
    uint64_t EvaluationCount() const override;
    uint64_t NeuralEvaluations() const override;
    void ResetTemporal() override;
    NeuralRenderFailure LastFailure() const override;
    double LastNeuralGpuMs() const override;
    uint64_t PeakLocalVideoMemoryMiB() const override;
    // The same measurement asked as a point question rather than a running
    // maximum, which is what an idle helper reports between jobs. Zero until a
    // frame has been submitted, and zero is a measurement there rather than a
    // gap: see LocalVideoMemoryNote.
    uint64_t CurrentLocalVideoMemoryMiB() const noexcept { return currentLocalVramMib_; }

    // Diagnostics. Empty until the corresponding step ran.
    // Also the interface's: the renderer reports it beside the failure kind,
    // which is the only place a run that rendered nothing says why.
    std::string LastError() const override { return lastError_; }
    const std::string& AdapterName() const noexcept { return adapterName_; }
    // The runtime's own GetStatus line after PrepareSession: arch, module count,
    // HIP readiness. The ABI 3 capabilities struct no longer carries hip_ready.
    const std::string& RuntimeStatus() const noexcept { return runtimeStatus_; }
    // The same GetStatus line read again, live. RuntimeStatus() is the bring-up
    // snapshot, taken before the first frame, so the counters that only move once
    // frames flow - arch, HIP readiness, bridge creates, codec recreates, passes -
    // are all still at their initial values there. This answers them.
    std::string LiveRuntimeStatus() const;
    // What the local video memory sampling actually saw, so a zero in
    // PeakLocalVideoMemoryMiB() can be read as a measurement rather than a gap.
    std::string LocalVideoMemoryNote() const;
    // What the last produced frame was read back from, so a caller can prove the
    // host colour texture was not the source. Null when nothing was produced.
    const void* LastOutputResource() const noexcept { return lastOutput_; }
    bool LastOutputWasPrivate() const noexcept { return lastOutputWasPrivate_; }
    // Whether this session's capture is P010 rather than 8-bit BGRA, which is
    // what the caller has to know to describe the bytes it is handed. Decided at
    // Initialize from config_.tenBitCapture AND what the capture can honestly
    // carry there (an accumulation to keep the precision in, an even size for
    // 4:2:0), so asking is the only correct source for it.
    bool TenBitCapture() const noexcept { return tenBitCapture_; }
    // Whether the frames this session is fed are NV12 and converted on the GPU,
    // which is what the caller has to know to describe the bytes it submits -
    // and what the job logs so a render says which of the two upload paths it
    // paid for. Resolved at Initialize from config_.sourceLayout and the colour
    // description's conversion, so asking is the only correct source for it.
    bool SourceIsNv12() const noexcept { return sourceNv12_; }
    // Wall time of the last Submit, which is what a receipt's per-frame pacing
    // would carry. The runtime's own GetTimings network_ms is LastNeuralGpuMs.
    double LastSubmitMs() const noexcept { return lastSubmitMs_; }
    // The history= token from the runtime's own GetStatus, refreshed after every
    // lease frame. "off" means the frame rendered spatially and says so nowhere
    // else. "priming" is the first frame of a run, which has no history to fuse
    // with yet. "active" is the temporal model actually fusing. Empty when no
    // lease frame has run, which is every frame of a non-temporal evaluator.
    const std::string& HistoryStatus() const noexcept { return lastHistoryStatus_; }
    // How many frames went through the lease path, so a run can tell "the
    // temporal model was engaged and reported off" apart from "the lease path
    // never ran" - two things whose logs look identical otherwise.
    uint64_t TemporalFrames() const noexcept { return temporalFrames_; }
    // Frames whose guides came from a per-frame field - the estimator the
    // player runs for this backend, a benchmark's files applied over it, or a
    // configured directory read directly - rather than from the two constants.
    // Non-zero only when one of those was configured, and the number a run has
    // to check to know it measured the field path: a config that silently lost
    // its source would otherwise produce a healthy-looking run of the constant
    // path.
    uint64_t GuideFrames() const noexcept { return guideFrames_; }
    // How many frames the host-side temporal accumulation ran on. Zero when
    // config_.stability is Off, and the number a run has to check to know the
    // pass it asked for actually ran - the rung is in no result field, so a
    // frame that skipped it otherwise looks like one that did not.
    uint64_t AccumulationFrames() const noexcept { return accumulationFrames_; }

    // The size the readback actually carries: the capture size when the job
    // upscales, the frame size when it does not. The OfflineNeuralRenderer's
    // settle check asks this of every evaluator that has it, so the AMD answer
    // is always the size it was asked for - the pass imposes no ratio bound the
    // way DLSS's input-to-output window does.
    std::pair<uint32_t, uint32_t> OutputSize() const noexcept
    {
        return Upscaling() ? std::pair{upscaleWidth_, upscaleHeight_}
                           : std::pair{width_, height_};
    }

private:
    // True when the session exists and every object the frame path needs does.
    bool Ready() const noexcept;
    // The device, queue, command objects and the three textures. Idempotent.
    bool BringUpDevice(std::string_view why);
    bool CreateFrameResources();
    // Loads the module, negotiates the ABI, creates and prepares the session.
    bool BringUpRuntime();
    void TearDown();

    void Log(std::string_view text) const;
    void Report(std::string_view call, int32_t status);
    bool Fail(NeuralRenderFailure failure, std::string detail);
    bool Refuse(std::string detail);

    // One frame in the default call order. Answers false with lastFailure_ set.
    // `guides` is this frame's field, or null when none was computed.
    bool SubmitFrame(const std::vector<uint8_t>& bgra, bool resetRequested,
                     uint64_t sourceFrameNumber, const GuideFrame* guides,
                     std::vector<uint8_t>& out);
    // One frame in the lease order, which is the only one the temporal model
    // runs under. The two differ in where RecordOutputs and the first
    // submission sit; see the class comment in AmdLmxxfSession.h.
    bool SubmitFrameLease(const std::vector<uint8_t>& bgra, bool resetRequested,
                          uint64_t sourceFrameNumber, const GuideFrame* guides,
                          std::vector<uint8_t>& out);
    // `sourceSlot` is which of the two source textures the temporal
    // accumulation's trust test should find this frame in, or -1 when it is not
    // running and the copy is skipped. It is the same slot the frame's output
    // goes into, which is what makes the pair the test compares the pair of
    // consecutive frames it means to compare. `pixels` is BGRA or NV12,
    // whichever this session was brought up for.
    bool UploadColor(const std::vector<uint8_t>& pixels, int sourceSlot);
    // The same upload for an NV12 frame: the two planes go into their own
    // persistent pair and one full-screen draw decodes them into `color_`, which
    // is what the row-by-row memcpy above would otherwise have filled. Leaves
    // `color_` where UploadColor's copy branch leaves it, so the source-slot
    // copy that follows is the same code either way.
    bool UploadColorFromNv12(const std::vector<uint8_t>& nv12);
    // The NV12 planes, their upload buffer, the conversion's signature and
    // pipeline, and the render target view `color_` needs to be a draw's output.
    // A no-op that answers true when the source is BGRA. Called from
    // Initialize before the temporal resources, because the frame path picks
    // its upload branch from what this resolved.
    bool CreateSourceResources();
    // The motion and depth textures, plus the upload buffer that fills them. The
    // pointers and the sizes are fixed for the life of the session - the runtime
    // rebuilds its chain around a pointer that moves - and only the contents
    // ever change. This fills the two constants; UploadGuides replaces them per
    // frame when a guide directory was configured.
    bool CreateGuideTextures();
    // The same, with the motion field carried at an extent of its own. The
    // runtime normalises the stored vector against motion_width, so a field in
    // grid pixels declared at the grid's own width is the same displacement a
    // frame-pixel field declared at the frame's width is - and the per-frame
    // upload is kilobytes instead of megabytes. Depth has no such extent (the
    // runtime requires the colour to fit inside it), so it stays at frame size.
    // Reached by the default grid-extent arm (DLSS5_AMD_GRID_GUIDES=0 opts out),
    // and only before this session's first frame: the runtime rebuilds its chain
    // around a pointer that moves, so this recreates rather than resizes.
    bool CreateGuideResources(uint32_t motionWidth, uint32_t motionHeight);
    // Swaps the motion texture to the extent this frame's field arrived at, once
    // per session. Called before the frame's command list is reset, because
    // creating the resources waits the queue out and reuses that same list.
    bool EnsureGuideGeometry(const GuideFrame* guides);
    // Depth has no extent of its own: the runtime's kernel indexes it by
    // renderWidth/renderHeight over the whole texture, so the frame-sized
    // resample cannot be skipped the way motion's can. It can be moved off the
    // CPU, which is where the 6.3 MB per frame was going - this pass stretches
    // the analysis grid into the frame-sized depth texture with one draw over
    // 43 KB of upload. See AmdDepthExpandShader.h for why that is the same
    // resample ResampleArea performs and not a different one.
    bool CreateDepthExpansion();
    // Sizes the grid texture and its upload buffer to this frame's field, once
    // per session, and rebuilds the depth texture's render-target view - which a
    // guide recreation invalidates, because it recreates the texture.
    bool EnsureDepthGrid(uint32_t gridWidth, uint32_t gridHeight);
    // Records this frame's grid copy and the draw that expands it into `depth_`.
    bool RecordDepthExpansion(ID3D12GraphicsCommandList* list);
    // Rewrites the guide textures for this frame and records the copy into
    // `list`. Three sources, in precedence order: `guides` when the caller
    // computed a field (the player's estimator, or a benchmark's files already
    // applied over it), the configured directories, and the constants
    // CreateGuideTextures filled. A no-op that answers true when there is
    // neither a field nor a directory. A missing or malformed file fails the
    // frame; which of the two channels is written answers to `controls_`, so a
    // guide the job switched off keeps its constant rather than a zero.
    bool UploadGuides(ID3D12GraphicsCommandList* list, uint64_t sourceFrameNumber,
                      const GuideFrame* guides);
    bool CopyProducedToReadback(ID3D12GraphicsCommandList* list);
    // ---- temporal accumulation (TemporalStabilityPolicy.h) ----------------
    //
    // Everything below exists only when config_.stability is not Off, and every
    // one of it is required for a frame to run at a rung: the pass has no
    // fall-back to the plain copy, because a rung that silently did not run
    // would publish a picture the job did not ask for and a receipt that says it
    // did.
    //
    // Three steps and a copy, in this order and for this reason:
    //
    //   decode     the runtime's 8-bit sRGB frame -> linear FP16, which is the
    //              input contract kTemporalStabilityHlsl's t0 states
    //   accumulate that pass itself, whose output is the history slot for this
    //              frame in linear FP16
    //   encode     the history slot -> 8-bit sRGB, which is what the readback
    //              buffer and every consumer of it carries
    bool CreateTemporalResources();
    bool CreateTemporalPipeline();
    // Records the three passes and the copy on `list`. `plan` is the frame's
    // step from temporal_stability::PlanFrame and `frame` the identity it was
    // taken for; both come from the caller because the upload has to put this
    // frame's source where the plan says the pass will read it, which happens
    // before either pass is recorded.
    bool RecordTemporalAccumulation(ID3D12GraphicsCommandList* list,
                                    const temporal_stability::Plan& plan,
                                    const FrameIdentity& frame);
    // The copy both the plain and the accumulated path end with: `from` into the
    // readback buffer at the frame's own footprint, in `format`.
    void CopyTextureToReadback(ID3D12GraphicsCommandList* list, ID3D12Resource* from,
                               DXGI_FORMAT format);
    // The stretch-and-sharpen the capture runs instead of that copy, when the
    // job upscales. `from` must arrive in PIXEL_SHADER_RESOURCE and is put back
    // in it; the readback is recorded from the sharpened target at the capture
    // footprint.
    bool RecordUpscale(ID3D12GraphicsCommandList* list, ID3D12Resource* from);
    bool Upscaling() const noexcept { return upscaleWidth_ != 0 && upscaleHeight_ != 0; }
    bool CreateUpscaleResources();
    // The width and height the readback is at: the capture's when the job
    // upscales, the frame's otherwise. Both because the four call sites that
    // size a buffer need the same answer.
    uint32_t CaptureWidth() const noexcept { return Upscaling() ? upscaleWidth_ : width_; }
    uint32_t CaptureHeight() const noexcept { return Upscaling() ? upscaleHeight_ : height_; }
    // The P010 planes and their readbacks (config_.tenBitCapture). Called from
    // CreateTemporalResources, because the precision it stores is the
    // accumulation's - see the config field for why there is nowhere else.
    bool CreateCaptureResources();
    // Records the two plane draws and their copies. `srvIndex` names the texture
    // in captureSrvHeap_ the frame is read from, which arrives in
    // PIXEL_SHADER_RESOURCE and is left there.
    void RecordP010Capture(ID3D12GraphicsCommandList* list, uint32_t srvIndex);
    // Maps the readback buffer and copies the frame out of it. Shared by both
    // call orders, which differ only in which list the copy was recorded into.
    bool ReadBackProduced(std::vector<uint8_t>& out);
    bool WaitForGpu();
    // Signals the frame fence and reports the value and the HRESULT separately.
    // EndRecording wants both as facts about the submission rather than as a
    // wait, and refuses a zero value or a failed status.
    bool SignalFence(uint64_t& value, int32_t& status);
    void RefreshHistoryStatus();
    void SampleLocalVram();

    AmdLmxxfEvaluatorConfig config_;
    amd_lmxxf::Runtime runtime_;
    std::optional<amd_lmxxf::Session> session_;

    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
    // The lease order puts the producer and the consumer on the queue as two
    // separate submissions with the runtime's HIP work between them, so the
    // consumer needs a list and an allocator of its own - the first pair is
    // still executing when the second is recorded.
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> consumerAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> consumerList_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_{};
    uint64_t fenceValue_{};

    // Created once. `color_` is the pointer the runtime is told about every
    // frame, which is the whole point of the persistent-input rule.
    Microsoft::WRL::ComPtr<ID3D12Resource> color_;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    // The temporal guides, filled once in CreateGuideTextures from the two
    // constants and rewritten every frame by UploadGuides when a guide directory
    // was configured. Their absence is what makes history=off in the first
    // place, so they are only created when the caller asked for temporalHistory.
    Microsoft::WRL::ComPtr<ID3D12Resource> motion_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depth_;
    Microsoft::WRL::ComPtr<ID3D12Resource> guideUpload_;
    // Both guide footprints and the mapped base of the one upload buffer that
    // carries them, kept rather than recomputed because the per-frame rewrite
    // needs the same three values the initial fill used. The buffer is reused
    // rather than doubled per frame for the same reason as the colour one:
    // SubmitFrameLease waits the queue out before it returns, so the frame that
    // is about to overwrite it is the one that just finished reading it.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT motionFootprint_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthFootprint_{};
    // The motion FIELD's extent, which is what the frame info declares and what
    // the kernel normalises the stored vectors against. Equal to the frame's own
    // size unless the grid arm moved it, and never larger than motion_'s texture.
    uint32_t motionW_{}, motionH_{};
    // DLSS5_AMD_GRID_GUIDES, on unless set to 0: carry the motion field at the
    // analysis grid's extent rather than resampling it onto the frame every
    // frame.
    bool gridGuides_{};
    uint8_t* guideUploadMapped_{};
    uint8_t* uploadMapped_{};

    // ---- the depth guide's expansion (DLSS5_AMD_DEPTH_EXPAND) ---------------
    bool depthExpand_{};
    // The FIELD's extent, which is the grid texture's size - not the frame's.
    uint32_t depthGridW_{}, depthGridH_{};
    // The field as uploaded (43 KB at 1920x822) and as the expand pass reads it.
    // The frame-sized depth texture the runtime samples is `depth_`, unchanged.
    Microsoft::WRL::ComPtr<ID3D12Resource> depthGrid_;
    Microsoft::WRL::ComPtr<ID3D12Resource> depthGridUpload_;
    uint8_t* depthGridMapped_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthGridFootprint_{};
    // The state depthGrid_ was last left in. The second frame's copy transitions
    // from what the first one actually left behind; without this the first copy
    // of a session would ask for COPY_DEST -> COPY_DEST.
    D3D12_RESOURCE_STATES depthGridState_{D3D12_RESOURCE_STATE_COPY_DEST};
    // Which resource the render-target view in depthRtvHeap_ was made for. A
    // guide rebuild replaces depth_, and a view into a released resource is not
    // something the debug layer has to catch - comparing the pointer is cheaper
    // than rebuilding a view per frame.
    ID3D12Resource* depthRtvOwner_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depthSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> depthRtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> depthExpandRootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoDepthExpand_;

    // ---- the temporal accumulation's own objects (config_.stability) -------
    //
    // Two root signatures rather than one: the accumulation's table holds five
    // textures and eight constants, the conversions' holds one texture and none,
    // and a signature that has to describe both would be a signature neither of
    // them is.
    Microsoft::WRL::ComPtr<ID3D12RootSignature> convertRootSig_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> accumulationRootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoDecode_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoAccumulate_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoEncode_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> accumulationSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> accumulationRtvHeap_;
    UINT accumulationSrvDescriptorSize_{};
    // The source frames the trust test compares, one per history slot. The text
    // is 8-bit sRGB, the same as the runtime's own frame and the same as what the
    // pass's t3/t4 expect to linearise themselves.
    Microsoft::WRL::ComPtr<ID3D12Resource> sourceSlot_[2];
    // The stabilized output per slot, in FP16 because it is what a recursive
    // filter is accumulated in: an 8-bit slot would quantise once per blend and
    // the rungs would stop separating from each other.
    Microsoft::WRL::ComPtr<ID3D12Resource> historySlot_[2];
    // The runtime's frame in the linear light the pass's t0 contract asks for,
    // and the result encoded back to the codes the readback carries.
    Microsoft::WRL::ComPtr<ID3D12Resource> neuralLinear_;
    Microsoft::WRL::ComPtr<ID3D12Resource> stabilised_;
    // One texel of zero motion. The pass needs a motion texture bound and reads
    // it as a vector; a zero field is what makes it a filter over the source's
    // own agreement rather than a motion-compensated one, which is the whole of
    // what this backend can honestly supply without an estimator.
    Microsoft::WRL::ComPtr<ID3D12Resource> zeroMotion_;
    temporal_stability::History accumulationHistory_{};
    uint64_t accumulationFrames_{};

    // ---- the upscale capture's own objects (config_.outputWidth/Height) ----
    //
    // Two targets because the two passes cannot share one: the CAS pass reads
    // the stretch's own 3x3 neighbourhood, and reading and writing the same
    // texture in one draw is a hazard no state can license. The per-frame SRV
    // is here rather than in the accumulation's heap, because the accumulation
    // builds its heap only when a rung is set and the stretch has to work with
    // the rung off.
    uint32_t upscaleWidth_{};
    uint32_t upscaleHeight_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> upscaleTarget_;
    Microsoft::WRL::ComPtr<ID3D12Resource> sharpenedTarget_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> upscaleRootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoUpscale_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoCas_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> upscaleSrvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> upscaleRtvHeap_;
    UINT upscaleSrvDescriptorSize_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT outputFootprint_{};
    Microsoft::WRL::ComPtr<ID3D12Resource> outputReadback_;
    // The same two targets at 16-bit, when the capture is 10-bit: the shaders
    // are unchanged and only the store differs, so what this buys is the
    // stretch and the CAS running without rounding the frame to eight bits
    // between them - which is exactly where a gradient at 1:1 would band.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoUpscale16_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoCas16_;

    // ---- the 10-bit capture's own objects (config_.tenBitCapture) ---------
    //
    // Resolved at Initialize rather than taken from the config: it also needs
    // the accumulation and an even capture size, and the answer decides the
    // format of three textures above, so it has to be a bring-up fact.
    bool tenBitCapture_{};
    // The two P010 planes, at the capture's size and half of it. Separate
    // resources rather than one interleaved texture because 4:2:0 is two
    // different geometries, and the readback assembles them.
    Microsoft::WRL::ComPtr<ID3D12Resource> lumaPlane_;
    Microsoft::WRL::ComPtr<ID3D12Resource> chromaPlane_;
    Microsoft::WRL::ComPtr<ID3D12Resource> lumaReadback_;
    Microsoft::WRL::ComPtr<ID3D12Resource> chromaReadback_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT lumaFootprint_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT chromaFootprint_{};
    // The two entry points of AmdTemporalAccumulationShader.h's capture
    // program, and the encode's 16-bit store beside them.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoCaptureLuma10_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoCaptureChroma10_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoEncode16_;
    // The dithered 8-bit store (config_.captureDither), null when it was not
    // asked for and always null on a 10-bit capture.
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoEncodeDithered_;
    // Slot 0 is the encoded frame, slot 1 the upscaled one - the two textures a
    // capture can read, both made once because neither is the runtime's.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> captureSrvHeap_;
    UINT captureSrvDescriptorSize_{};

    // ---- the NV12 source's own objects (config_.sourceLayout) --------------
    //
    // Resolved at Initialize, because the conversion has to be one of the four
    // SourceNv12ConversionFor names and only the colour description says which.
    // Everything below exists only when it resolved to NV12.
    bool sourceNv12_{};
    SourceNv12Conversion sourceConversion_{SourceNv12Conversion::Unsupported};
    // The two planes as the decoder delivers them: R8 at full size, interleaved
    // R8G8 at half. Separate resources because 4:2:0 is two geometries, and
    // because a copy region needs both sides to be the same format.
    Microsoft::WRL::ComPtr<ID3D12Resource> sourceLuma_;
    Microsoft::WRL::ComPtr<ID3D12Resource> sourceChroma_;
    Microsoft::WRL::ComPtr<ID3D12Resource> sourceUpload_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sourceSrvHeap_;
    // One render target view, for the colour texture: the conversion writes it
    // and nothing else ever does, so it is the only view of it that exists.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sourceRtvHeap_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> sourceRootSig_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> psoSourceNv12_;
    UINT sourceSrvDescriptorSize_{};
    // Both footprints and the mapped base of the one upload buffer that carries
    // them, kept for the same reason the guide pair's are: every frame rewrites
    // the same bytes in place.
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT lumaUploadFootprint_{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT chromaUploadFootprint_{};
    uint8_t* sourceUploadMapped_{};

    // The state the caller declared for color_ at the last RecordInputs and the
    // state the next transition must start from. The runtime reads the colour
    // resource in the declared state and does not transition it (it maps that
    // state onto its own codec textures), so the caller is the only writer of
    // this value.
    D3D12_RESOURCE_STATES colorState_{D3D12_RESOURCE_STATE_COPY_DEST};
    bool colorUploaded_{};

    uint32_t width_{};
    uint32_t height_{};
    uint64_t frameIndex_{};

    std::string adapterName_;
    std::string runtimeStatus_;
    std::string lastError_;
    NeuralRenderFailure lastFailure_{NeuralRenderFailure::None};
    double lastNeuralGpuMs_{};
    double lastSubmitMs_{};
    uint64_t evaluations_{};
    uint64_t neuralEvaluations_{};
    uint64_t peakLocalVramMib_{};
    // The local segment budget beside the usage, and why the usage could not be
    // read when it could not. A stage C preflight has to tell "this process is
    // using nothing" apart from "this driver will not say", and both look like a
    // bare zero in PeakLocalVideoMemoryMiB().
    uint64_t localVramBudgetMib_{};
    uint64_t currentLocalVramMib_{};
    std::string localVramNote_;
    const void* lastOutput_{};
    bool lastOutputWasPrivate_{};
    bool prepared_{};
    // Mirrors LMXXF_NR_CREATE_FLAG_RECORDING_LEASES being set on the session,
    // which is what selects the frame path.
    bool leases_{};
    // Frames that went through the lease path, and the history= token the
    // runtime answered with the last time it was asked. Kept as state because
    // the token is only ever printed into a status string.
    uint64_t temporalFrames_{};
    std::string lastHistoryStatus_;
    // Frames whose guides came from a per-frame field. See GuideFrames().
    uint64_t guideFrames_{};
    // The per-guide switches this session was brought up under, from the same
    // GuideControls the NVIDIA path takes. They decide which of the two guide
    // channels a per-frame field is allowed to write: a guide the job switched
    // off keeps the constant CreateGuideTextures filled, which is the neutral
    // value GuideControls promises (zero motion, flat depth), not a zero the
    // field happens to carry.
    GuideControls controls_{};
};
