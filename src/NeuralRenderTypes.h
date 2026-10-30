#pragma once

// Leaf value types of a neural render job. Kept apart from
// OfflineNeuralRenderer.h so consumers that only describe a job (range
// selection, runtime policy, the cache manifest) do not inherit <windows.h>,
// the encoder pipeline, and the orchestrator class along with it.

#include "ResidentHelperPolicy.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Which neural backend a job runs on. Selected per job rather than per build:
// this binary carries both, so a machine with no NGX is not a machine that has
// to be given a different player.
//
// Nvidia is the default, which is what keeps every existing caller - and so
// every path that has been verified on hardware - byte-identical to before this
// choice existed. Nothing selects Amd unless it says so.
//
// It lives here, in the leaf value types, because it is now part of a job's
// description: the command line, the export plan, the worker's wire and the
// cache key all carry it, and none of them should have to include the
// orchestrator to name it.
enum class NeuralBackend { Nvidia, Amd };

// The canonical name: what a log line, the worker's wire and (from stage C) a
// receipt carry. One spelling per backend, because two that can be confused is
// how a cache entry ends up describing a render that was not made.
const char* NeuralBackendName(NeuralBackend backend) noexcept;

// The inverse, for a wire field. Absent means the default rather than a guess,
// so nullopt here is a malformed request and not a silent fall back.
constexpr std::optional<NeuralBackend> ParseNeuralBackend(std::string_view name) noexcept
{
    using namespace std::literals;
    if (name == "nvidia-ngx"sv) return NeuralBackend::Nvidia;
    if (name == "amd-lmxxf"sv) return NeuralBackend::Amd;
    return std::nullopt;
}

// Which of the two runtimes inside an AMD pack an AMD job drives. Both speak the
// same API table (src/AmdLmxxfApi.h, ABI 6) and differ in the module, the frame
// struct and one create flag - see docs/AMD-MOCHIZUKI-COEXIST-PLAN-zh.md.
//
// A name rather than a second `NeuralBackend`: an AMD job is still an AMD job -
// same evaluator, same lease lifecycle, same capture - so the cache key, the
// plan and the worker wire keep one backend value and carry this beside it.
//
// Mochizuki is the default: it is the runtime that builds its network once and
// then answers from that network, so a job that says nothing about the runtime
// gets the one that does not rebuild its model per job. Lmxxf stays available
// and unchanged for anyone who picks it - the choice is a menu radio, and the
// name travels with the job either way.
enum class AmdNrRuntime { Lmxxf, Mochizuki };

inline const char* AmdNrRuntimeName(AmdNrRuntime runtime) noexcept
{
    switch (runtime) {
        case AmdNrRuntime::Lmxxf: return "lmxxf";
        case AmdNrRuntime::Mochizuki: return "mochizuki";
    }
    return "mochizuki";
}

// Absent means Mochizuki, which is what a job that names no runtime runs.
constexpr std::optional<AmdNrRuntime> ParseAmdNrRuntime(std::string_view name) noexcept
{
    using namespace std::literals;
    if (name == "lmxxf"sv) return AmdNrRuntime::Lmxxf;
    if (name == "mochizuki"sv) return AmdNrRuntime::Mochizuki;
    return std::nullopt;
}

// The Mochizuki runtime's own controls, as its menu would show them. A separate
// struct from `NeuralSettings` for two reasons: they are not the same knobs
// (DLSS-NR's are read by NVIDIA's add-on, these by the AMD runtime), and they
// travel a different road - a Mochizuki job hands them to MochizukiNrSetControls
// before its network is built, while lmxxf's options are lines in a file the
// runtime reads once per process.
//
// Every default is the runtime's own documented default, so a default-constructed
// value is a no-op: a job that changes nothing renders exactly what the runtime
// renders on its own, and the wire carries nothing (see FormatMochizukiOptions).
//
// The five values that are also `NeuralSettings` names - Intensity, Local tone,
// Local structure, Skin structure, Automatic mask - are filled from it when the
// job is built, so a preset moves them and the panel keeps one place per knob.
struct MochizukiOptions {
    // 0..2, default 1: above 1 raises the luminance ratio rather than "more NR".
    float intensity{1.0f};
    // 0..2, default 1: tone re-applied per frame.
    float localTone{1.0f};
    // 0..2, default 1.
    float localStructure{1.0f};
    // -1..2, default -1: negative follows local structure. Needs automatic mask.
    float skinStructure{-1.0f};
    // Default on: skin structure is inert without it.
    bool autoMask{true};
    // 1..8, default 2: the highlight guard. Higher lets the model brighten more
    // before it is held back - the one control that decides how a blown window or
    // a lamp behaves, and the reason a film with practicals looks either
    // protected or clipped.
    float maxRatio{2.0f};
    // 0..1, default 1: how much of the previous frame's answer is kept. With
    // motion vectors only, so on this package it is the one temporal knob that
    // reaches the network at all.
    float historyStrength{1.0f};
    // Default on. Off runs the whole network and shows the original frame - the
    // only honest A/B a viewer can make without changing any other setting.
    bool applyModel{true};
    // Default off: the picture the network is shown is normalised and every change
    // is taken back out of its answer. A game hands over HDR that already suits
    // the model; a film's grade may not, and this is what re-fits it. Costs one
    // network rebuild the first time it is on.
    bool preprocess{false};
    // Default off: the INT4 mixed network, which is smaller in memory and not the
    // same picture. Rebuilds; kept off unless a card is short of VRAM.
    bool int4Mixed{false};
    // 0 exact (default here), 1 auto bucket, 2 always bucket. Exact builds the
    // network for the frame's own render rect, so a changed rect rebuilds; the
    // buckets trade a little wasted work for not rebuilding. A video's rect is
    // fixed per job, which is why 0 is the default this host sends.
    uint32_t drsMode{0};

    friend bool operator==(const MochizukiOptions&, const MochizukiOptions&) = default;
};

// Whether every value is the runtime's own default.
inline bool MochizukiOptionsAreDefault(const MochizukiOptions& options) noexcept
{
    return options == MochizukiOptions{};
}

// One line, so the helper's command line stays one argument: `key=value` pairs
// separated by commas, floats at two decimals (the runtime's own precision).
//
// An empty string is the runtime's defaults, which is what a job that changed
// nothing sends: the helper then takes no argument at all and an older helper -
// one that has never heard of this key - renders the same picture.
inline std::string FormatMochizukiOptions(const MochizukiOptions& options)
{
    if (MochizukiOptionsAreDefault(options)) return {};
    char line[256]{};
    std::snprintf(line, sizeof(line),
                  "intensity=%.2f,tone=%.2f,structure=%.2f,skin=%.2f,mask=%d,maxratio=%.2f,"
                  "history=%.2f,apply=%d,pre=%d,int4=%d,drs=%u",
                  double(options.intensity), double(options.localTone),
                  double(options.localStructure), double(options.skinStructure),
                  options.autoMask ? 1 : 0, double(options.maxRatio),
                  double(options.historyStrength), options.applyModel ? 1 : 0,
                  options.preprocess ? 1 : 0, options.int4Mixed ? 1 : 0, options.drsMode);
    return line;
}

// Absent or malformed returns nullopt rather than a default: a job that named
// values and had them silently replaced by defaults would render a picture
// nobody asked for and call it success. Every key must be present, because the
// writer always writes all of them.
//
// Out of range is refused here as well, and for the same reason: this writer
// never emits one, so a number outside the runtime's documented range means the
// line did not come from it. The panel already clamps what it lets a reader
// set - the saved document clamps it again on the way in - and the wire is the
// one path left where a hand-assembled command line could hand the runtime a
// negative pass count for an unsigned field.
inline std::optional<MochizukiOptions> ParseMochizukiOptions(std::string_view text)
{
    if (text.empty()) return std::nullopt;
    MochizukiOptions options;
    int seen = 0;
    // The runtime's documented range, per field. `number()` has already rejected
    // a string that is not a number; the range is the second half of the same
    // question, and a NaN passes every comparison so it is named explicitly.
    const auto inRange = [](const std::optional<double>& n, double low, double high) {
        return n && std::isfinite(*n) && *n >= low && *n <= high;
    };
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string_view field = text.substr(0, comma);
        text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
        const size_t equals = field.find('=');
        if (equals == std::string_view::npos) return std::nullopt;
        const std::string_view key = field.substr(0, equals);
        const std::string_view value = field.substr(equals + 1);
        if (key.empty() || value.empty()) return std::nullopt;
        // std::from_chars for integers, strtof for the floats: the writer emits a
        // plain decimal point and no exponent, and strtof is what reads that
        // without a locale in the way.
        char buffer[64]{};
        if (value.size() >= sizeof(buffer)) return std::nullopt;
        std::memcpy(buffer, value.data(), value.size());
        const auto number = [&]() -> std::optional<double> {
            char* end = nullptr;
            const double parsed = std::strtod(buffer, &end);
            if (end == buffer || *end != '\0') return std::nullopt;
            return parsed;
        };
        const auto flag = [&]() -> std::optional<bool> {
            if (value == "1") return true;
            if (value == "0") return false;
            return std::nullopt;
        };
        if (key == "intensity") { const auto n = number(); if (!inRange(n, 0.0, 2.0)) return std::nullopt; options.intensity = float(*n); }
        else if (key == "tone") { const auto n = number(); if (!inRange(n, 0.0, 2.0)) return std::nullopt; options.localTone = float(*n); }
        else if (key == "structure") { const auto n = number(); if (!inRange(n, 0.0, 2.0)) return std::nullopt; options.localStructure = float(*n); }
        else if (key == "skin") { const auto n = number(); if (!inRange(n, -1.0, 2.0)) return std::nullopt; options.skinStructure = float(*n); }
        else if (key == "mask") { const auto b = flag(); if (!b) return std::nullopt; options.autoMask = *b; }
        else if (key == "maxratio") { const auto n = number(); if (!inRange(n, 1.0, 8.0)) return std::nullopt; options.maxRatio = float(*n); }
        else if (key == "history") { const auto n = number(); if (!inRange(n, 0.0, 1.0)) return std::nullopt; options.historyStrength = float(*n); }
        else if (key == "apply") { const auto b = flag(); if (!b) return std::nullopt; options.applyModel = *b; }
        else if (key == "pre") { const auto b = flag(); if (!b) return std::nullopt; options.preprocess = *b; }
        else if (key == "int4") { const auto b = flag(); if (!b) return std::nullopt; options.int4Mixed = *b; }
        else if (key == "drs") {
            // An unsigned field, so a fractional or negative value is refused
            // before the cast rather than converted to something arbitrary.
            const auto n = number();
            if (!inRange(n, 0.0, 2.0) || *n != std::floor(*n)) return std::nullopt;
            options.drsMode = uint32_t(*n);
        }
        else return std::nullopt;
        ++seen;
    }
    if (seen != 11) return std::nullopt;
    return options;
}

// Whether this backend's render reads `NeuralSettings` - the add-on's own panel
// - at all.
//
// The question is not whether the panel is enabled but whether a value on it
// can reach a pixel, and that is a property of the backend rather than of the
// dialog:
//
//   * Nvidia - yes, all of it. The values are written into the ReShade.ini the
//     helper loads (NeuralAddonOverridesFor) and they are a term of the render
//     key, so any of them can change what comes back.
//   * Amd - almost none of it. This backend loads no proxy and reads no INI;
//     of the panel it reads exactly two values: the pass count, which the
//     runtime stacks the model with, and which model that count selects - the
//     0.44 Small network (31 blocks at 0.5x) against the Original 71-block one,
//     the other spelling of the one DLSS5_MULTI_PASS key. The rung, the skin
//     switch and the multi-pass switches travel outside this struct, so no
//     other value of THIS struct reaches a pixel.
//
//     The runtime and the Mochizuki controls are the same kind of term as the
//     count: the job's key carries both in AmdNeuralSettingsDocument(), and that
//     document's digest is a term of NeuralCacheIdentity - which is the render
//     key - so a job cannot be served frames built by the other runtime, or
//     under a control that has since moved. ApplyNeuralSettings restarts an AMD
//     session when the document moved. A caller that wires a further panel value
//     through to this backend has to add it to the document in the same change,
//     or the picture moves and the key does not follow it.
//
// The interesting case is the one this leaves out rather than the one it
// covers. `NeuralSettings` is the add-on's struct: the same dialog also holds
// the temporal rung, and that one DOES reach an AMD render, because it selects
// this player's own accumulation over the runtime's output. So this function is
// not "nothing in the dialog matters on AMD", and a caller that reads it that
// way leaves a moved rung unapplied - ApplyNeuralSettings states the real
// condition, which on this backend is the rung, the pass count or the model
// having moved.
//
// A caller may ask this only about a picture a render would produce. It is not a
// statement that the panel is inert for the user - the values are still saved
// and still reach an NVIDIA render on a machine that has one.
constexpr bool BackendReadsNeuralSettings(NeuralBackend backend) noexcept
{
    return backend == NeuralBackend::Nvidia;
}

enum class NeuralRenderPhase {
    CheckingCache,
    Acquiring,
    Decoding,
    NeuralRendering,
    Encoding,
    Validating,
    Ready,
    Preflight,
    Paused,
    Recovering,
    // The AMD Mochizuki runtime compiles its network on first use and answers
    // UNAVAILABLE until it is done: 66-73 s the first time this driver ever sees
    // the model, 1.5-1.8 s afterwards. Nothing is being decoded, rendered or
    // encoded while that runs, so it is its own phase rather than a shade of
    // "preparing" - the wait is the whole story of it.
    BuildingNetwork,
};

// Explicit failure classification. Every failed job names exactly one kind so
// the player can show a distinct state instead of a generic failure and so
// receipts record what actually stopped the render.
enum class NeuralRenderFailure : uint8_t {
    None,
    Source,
    Encoder,
    Neural,
    GpuStall,
    DeviceRemoved,
    WorkerCrashed,
    RetryExhausted,
    Cancelled,
    Preflight,
    Identity,
    Protocol,
};

constexpr std::string_view NeuralRenderFailureName(NeuralRenderFailure failure) noexcept
{
    switch (failure) {
        case NeuralRenderFailure::None: return "none";
        case NeuralRenderFailure::Source: return "source";
        case NeuralRenderFailure::Encoder: return "encoder";
        case NeuralRenderFailure::Neural: return "neural";
        case NeuralRenderFailure::GpuStall: return "gpu-stall";
        case NeuralRenderFailure::DeviceRemoved: return "device-removed";
        case NeuralRenderFailure::WorkerCrashed: return "worker-crashed";
        case NeuralRenderFailure::RetryExhausted: return "retry-exhausted";
        case NeuralRenderFailure::Cancelled: return "cancelled";
        case NeuralRenderFailure::Preflight: return "preflight";
        case NeuralRenderFailure::Identity: return "identity";
        case NeuralRenderFailure::Protocol: return "protocol";
    }
    return "unknown";
}

// Half-open source interval [start, end) on the decoder's CFR timeline. Both
// zero means the whole source. The encoded output always starts at pts 0; the
// absolute start is recorded beside it so playback can realign.
struct NeuralRenderRange {
    int64_t start100ns{};
    int64_t end100ns{};

    friend bool operator==(const NeuralRenderRange&, const NeuralRenderRange&) = default;
    constexpr bool Whole() const noexcept { return start100ns == 0 && end100ns == 0; }
};

// Frames evaluated (never captured) before the first captured frame so the
// temporal history at the range start matches a continuous render.
inline constexpr uint32_t kDefaultPrerollFrames = 60;
// Exact-frame retries of the same frame before the job is classified as
// retry-exhausted. Retries never skip a frame.
inline constexpr uint32_t kDefaultFrameRetryLimit = 3;

struct NeuralRenderTiming {
    uint64_t samples{};
    double neuralGpuMsP50{};
    double neuralGpuMsP95{};
    double neuralGpuMsMax{};
    double guideMsMean{};
    double captureMsMean{};
    uint64_t peakLocalVramMiB{};
    // Two point samples of the helper process's local-segment usage, beside
    // the running per-frame maximum above. They are what makes the idle-VRAM
    // policy decidable from a receipt instead of a debugger: the parked cost
    // of residency is the first, and what the policy gave back is the second.
    //
    // postJobLocalVramMiB: what this process still held the moment this job's
    //   render went quiet. A single-shot helper exits here, so this is only a
    //   parked figure for a resident one.
    // idleLocalVramMiB: what it still held after the idle grace that preceded
    //   THIS job, i.e. after the idle policy had acted on the previous job's
    //   memory. Zero when no idle period preceded this job - the first job a
    //   process serves, and every single-shot render.
    uint64_t postJobLocalVramMiB{};
    uint64_t idleLocalVramMiB{};
    // Which arm produced the two samples above. Fixed for a helper process, so
    // it is a property of the run rather than of the job; carried here so two
    // receipts with different idle numbers can be told apart.
    resident_helper::IdleVramPolicy idleVramPolicy{resident_helper::kDefaultIdleVramPolicy};

    friend bool operator==(const NeuralRenderTiming&, const NeuralRenderTiming&) = default;
};

// A DLAA-only run is indistinguishable from a healthy one in every counter the
// evidence chain checks: feature 18 is created, it is evaluated, frames are
// produced and verified. What it cannot fake is GPU cost. Measured at 1920x1080
// on this project's RTX 5090 / driver 616.64: the worker that stopped
// presenting after a feature recreate reported a median of 0.46 ms per frame
// (docs/DLSS5_VIDEO_ROADMAP.md item 1 open gap), while healthy renders on the
// same machine and geometry report 3.683776, 3.696608 and 3.715776 ms
// (the three cache/v1 receipt.json files, 2534 to 2658 samples each) and the
// reference benchmark run reads 3.26 to 3.27 ms (docs/BENCHMARK.md reference
// table), the lowest healthy median in evidence. Those two ends are a factor of
// 7.1 apart, so the floor is their geometric midpoint, 1.223 ms at 1920x1080:
// 2.66x above the DLAA-only observation, 2.66x below the benchmark median and
// 3.01x below the lowest receipt median. Per output megapixel because the pass
// scales with pixels, and the remaining risk is one-sided in the safe
// direction: per-pixel neural cost only rises on slower hardware, so a fixed
// floor can misjudge a healthy run only on a GPU substantially faster than a
// 5090, while the failure it catches sits 7x below it on the fastest card that
// exists today.
inline constexpr double kNeuralGpuMsFloorPerMegapixel = 0.59;

inline double NeuralGpuMsFloor(uint32_t width, uint32_t height)
{
    return kNeuralGpuMsFloorPerMegapixel * (double(width) * double(height) / 1000000.0);
}

// No samples at all means the build carries no timing instrumentation (such
// builds report the ms fields as zero), so a missing measurement is never a
// verdict against the run.
inline bool NeuralTimingClearsFloor(const NeuralRenderTiming& timing, uint32_t width, uint32_t height)
{
    return timing.samples == 0 || timing.neuralGpuMsP50 >= NeuralGpuMsFloor(width, height);
}

// Cold-start stack of one neural render, in the order the phases happen. Every
// boundary is named once here and measured where the transition is actually
// observable: the player owns the four it can see from outside the helper, the
// helper owns the five that only exist inside it. The handoff's cold-start
// table (CreateProcess+AV, ReShade proxy, NGX init, CreateFeature, first
// segment) maps onto HelperStart..FirstOutput; the proxy's own load is inside
// HelperStart because the proxy is the helper's dxgi import and is therefore
// resolved by the loader before the entry point runs.
enum class NeuralColdStartPhase : uint32_t {
    Request,      // player: request instant -> the decision to render or to replay a cache entry
    Preflight,    // player: the feature-18 probe helper, when one ran
    Launch,       // player: that decision -> the render helper's process created
    HelperStart,  // helper: process creation -> its own entry point (AV scan, loader, proxy)
    RuntimeReady, // helper: entry point -> add-on contract verified, media runtime and render window up
    NeuralInit,   // helper: render start -> source opened and evaluator initialized (device, NGX)
    FeatureArm,   // helper: -> feature 18 created, evaluated and inline interception armed
    FirstOutput,  // helper: -> first finalized output file published
    Attach,       // player: first playable output in hand -> first neural frame presented
};
inline constexpr size_t kNeuralColdStartPhaseCount = 9;

constexpr std::string_view NeuralColdStartPhaseName(NeuralColdStartPhase phase) noexcept
{
    switch (phase) {
        case NeuralColdStartPhase::Request: return "request";
        case NeuralColdStartPhase::Preflight: return "preflight";
        case NeuralColdStartPhase::Launch: return "launch";
        case NeuralColdStartPhase::HelperStart: return "helperStart";
        case NeuralColdStartPhase::RuntimeReady: return "runtimeReady";
        case NeuralColdStartPhase::NeuralInit: return "neuralInit";
        case NeuralColdStartPhase::FeatureArm: return "featureArm";
        case NeuralColdStartPhase::FirstOutput: return "firstOutput";
        case NeuralColdStartPhase::Attach: return "attach";
    }
    return "unknown";
}

// The phases only the helper can see. A timeline arriving over the metadata
// pipe may claim these and nothing else: the player's own measurements are not
// the helper's to report.
inline constexpr uint32_t kNeuralColdStartHelperPhases =
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::HelperStart)) |
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::RuntimeReady)) |
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::NeuralInit)) |
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::FeatureArm)) |
    (1u << static_cast<uint32_t>(NeuralColdStartPhase::FirstOutput));

// Durations, never stamps: the phases are measured in two processes against two
// unrelated monotonic origins, and only their lengths mean anything on another
// machine. A phase with no value did not happen - a reused preflight verdict, a
// single-file render that publishes no segment, a run that stopped before it got
// there - which is a different claim from a phase that took no measurable time,
// so the two are never collapsed into one. Same rule as NeuralTimingClearsFloor:
// a missing measurement is not a verdict.
class NeuralColdStartTimeline {
public:
    using Duration = std::chrono::microseconds;

    void Record(NeuralColdStartPhase phase, Duration elapsed) { phases_[Index(phase)] = elapsed; }
    std::optional<Duration> Phase(NeuralColdStartPhase phase) const { return phases_[Index(phase)]; }
    // Request instant to first neural frame presented: the number the toggle-to-picture
    // acceptance criterion is written against, measured end to end rather than added up.
    void RecordTotal(Duration elapsed) { total_ = elapsed; }
    std::optional<Duration> Total() const { return total_; }
    bool Empty() const
    {
        if (total_) return false;
        for (const auto& phase : phases_) if (phase) return false;
        return true;
    }
    // Adopts the phases `other` measured and leaves the rest alone, so the two
    // halves of one timeline join without either erasing the other's absences.
    void Merge(const NeuralColdStartTimeline& other)
    {
        for (size_t index = 0; index < phases_.size(); ++index)
            if (other.phases_[index]) phases_[index] = other.phases_[index];
        if (other.total_) total_ = other.total_;
    }

    friend bool operator==(const NeuralColdStartTimeline&, const NeuralColdStartTimeline&) = default;

private:
    static constexpr size_t Index(NeuralColdStartPhase phase) noexcept
    {
        return static_cast<size_t>(phase);
    }

    std::array<std::optional<Duration>, kNeuralColdStartPhaseCount> phases_{};
    std::optional<Duration> total_{};
};

// Reports the helper's share of the timeline once, as soon as the helper has
// produced something the player can show or has stopped trying.
using NeuralColdStartCallback = std::function<void(const NeuralColdStartTimeline&)>;

// What the add-on's own log has to say before a rendered frame counts as
// neural. Every member is read out of ReShade.log rather than asserted by this
// process, so a runtime that silently declined a frame cannot be published as
// verified.
//
// `nativeResolution` was `upscalingOff` while the add-on echoed an
// NREnableUpscaling setting. RenoDX 6.x deleted that key - the working
// resolution became a mode plus a scale - so the proof moved to what the
// add-on reports building and evaluating: inline NR resources at native 1:1,
// and an evaluate line that ends `[native]`. Both 4.70 and 6.5.3 print those,
// which is why the contract now holds across a lock move rather than breaking
// on one.
struct NeuralRuntimeEvidence {
    bool nativeResolution{};
    bool inlineInterceptionContract{};
    bool feature18Created{};
    bool feature18Evaluated{};
    bool laterFailure{};
    uint64_t highestObservedEvaluation{};

    bool Valid() const noexcept
    {
        return nativeResolution && inlineInterceptionContract && feature18Created && feature18Evaluated &&
               highestObservedEvaluation > 0 && !laterFailure;
    }
};

// One finalized output file produced while the job is still running. Each file
// starts at its own pts zero; firstTimestamp100ns places it on the source
// timeline and end100ns is exclusive.
struct NeuralRenderSegment {
    uint64_t index{};
    uint64_t firstFrameNumber{};
    int64_t firstTimestamp100ns{};
    int64_t end100ns{};
    uint64_t frameCount{};
    std::wstring fileName;          // relative to the staging directory
};

// Consumer of finalized segments. onSegment is called from the job's finalize
// thread, in index order, only after that file's encoder exited successfully.
struct NeuralSegmentSink {
    std::function<void(const NeuralRenderSegment&)> onSegment;
    std::function<void()> onRestart; // a from-zero relaunch invalidated every earlier segment
};
