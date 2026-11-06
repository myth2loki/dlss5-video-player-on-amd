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
#include <utility>
#include <vector>

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
// One later pass's own controls. The runtime takes two of these beside the
// global block (its pass[0] is pass 2, pass[1] is pass 3), which is what lets a
// stack carry a different look per pass instead of repeating one. Follow keeps
// the runtime's own default: that pass inherits pass 1's look and its tone drops
// to 0, because re-applying the same tone on every pass compounds.
struct MochizukiPassOptions {
    bool followPass1{true};
    // 0..2, as MochizukiOptions::style. Standard, which is what the dialog's
    // first entry says: a later pass that is given its own look should start
    // from the ungraded one, not from Natural.
    uint32_t style{0};
    // 0..2, default 1.
    float intensity{1.0f};
    // 0..2, default 0 for a later pass: see above.
    float localTone{0.0f};
    // 0..2, default 1.
    float localStructure{1.0f};
    // -1..2, default -1: negative follows the structure value.
    float skinStructure{-1.0f};
    // Default on: skin structure is inert without it.
    bool autoMask{true};

    friend bool operator==(const MochizukiPassOptions&, const MochizukiPassOptions&) = default;
};

// The five values that are also `NeuralSettings` names - Intensity, Local tone,
// Local structure, Skin structure, Automatic mask - are filled from it when the
// job is built, so a preset moves them and the panel keeps one place per knob.
struct MochizukiOptions {
    // 0..2, default 1: above 1 raises the luminance ratio rather than "more NR".
    float intensity{1.0f};
    // 0..2, default 1: tone re-applied per frame. It matches
    // NeuralSettings::localTone, which is where the value actually comes from
    // (main.cpp copies it into this struct when a job is built) - the two are the
    // same knob, and a default that disagreed would make MochizukiOptionsAreDefault
    // say "not default" about the product's own default.
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
    // 0..1, default 0.70: how much of the previous frame's answer is kept. With
    // motion vectors only, so on this package it is the one temporal knob that
    // reaches the network at all. Measured: 1.00 -> 0.70 moves the trailing
    // residue by about 1% (0.590 -> 0.584 on the ghosting score) while leaving
    // the frame's texture alone, so it is a small trim rather than the lever
    // the pass count and the per-pass look are.
    float historyStrength{0.70f};
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
    // Which of the two networks this runtime carries. 0 is the 310.8.0 network
    // (599 entries here, the picture every earlier render was made with); 1 is
    // the 310.9.16 small one (255 entries), a differently trained network, not
    // a cheaper setting of the same one: shipped as `dlssnr-small.bin` beside
    // `dlssnr.bin` and selected by the runtime, so the two files must both be
    // present for the switch to mean anything. Rebuilds the network, and it is
    // a key term because the same settings make a different frame.
    uint32_t networkModel{0};
    // Passes 2 and 3, each optionally carrying its own look instead of
    // inheriting pass 1. Only the ones the frame's pass count reaches run: at
    // two passes pass3 is inert, at one neither is.
    MochizukiPassOptions pass2;
    MochizukiPassOptions pass3;
    // Named fields of the control struct this host has no panel for, in the
    // header's own member names (see amd_mochizuki::kControlFields). Empty is the
    // package's own defaults, which is what a job that moved nothing sends. This
    // is how a knob with no dialog entry still reaches a pixel without a new
    // field on the wire, a new ini key per knob, or a rebuilt runtime.
    std::vector<std::pair<std::string, double>> overrides;

    // The runtime carries INT4 records/weights for the ORIGINAL network only
    // (MochizukiNrRuntime.cpp: `key.int4 = int4 && networkModel == 0`). On the
    // small network it clears the request without telling anyone, so leaving the
    // pair on the wire would claim a setting the render does not have - and
    // would give one byte-identical picture two different cache keys. Collapse
    // the impossible pair here, on the one type every path passes through:
    // saved ini, the dialog, the worker's command line and the cache key.
    // Returns true when a requested value had to be dropped, so a caller that
    // has a log can say so instead of silently rendering something else.
    bool NormaliseInt4() noexcept
    {
        if (networkModel == 0 || !int4Mixed) return false;
        int4Mixed = false;
        return true;
    }

    friend bool operator==(const MochizukiOptions&, const MochizukiOptions&) = default;
};

// The RenoDX grading controls the player exposes, owned solely by the [RenoDX]
// settings section and its dialog. There are twelve per-knob grades (detail,
// shadows, ...) plus reno_look_mode, which the package carries as a bool
// (NRLookMode): the grade itself, on or off. With it off every look knob is an
// identity gain and the picture does not move, which is why the dialog shows
// it as a switch rather than a number. Every other knob the package carries
// that this host does not surface stays at the runtime's own defaults (see
// kControlFields in AmdMochizukiApi.h).
//
// The defaults are the package's own (MochizukiRenoOptions.inc), so a knob the
// user never moved is not written into the controls the render reads and the
// package stays free to change it. The shipped look - reno_look_detail left at the package's neutral 1.00 and
// reno_look_shadows 0.90 - is the look this renderer was A/B'd at and the
// migration target the first time RenoDX is enabled.
struct RenoDxSettings {
    enum Field : int {
        Detail,            // reno_look_detail       0.00 .. 2.00
        DetailRadius,     // reno_look_detail_radius 0.00 .. 4.00
        Shadows,          // reno_look_shadows       0.00 .. 2.00
        Highlights,       // reno_look_highlights    0.00 .. 2.00
        Colour,           // reno_look_colour        0.00 .. 2.00
        Tone,             // reno_look_tone          0.00 .. 2.00
        DetailStability,  // reno_detail_stability    0 .. 2 (uint)
        LookStabilize,    // reno_look_stabilize      0 .. 1 (uint)
        StabilizeMs,      // reno_look_stabilize_ms   0.0 .. 500.0
        RenoPasses,       // reno_passes              1 .. 30 (uint)
        MvecScaleX,       // reno_mvec_scale_x       -4.0 .. 4.0
        MvecScaleY,       // reno_mvec_scale_y       -4.0 .. 4.0
        LookMode,         // reno_look_mode           0 .. 1 (bool: the grade on/off)
        // Whether passes 2+ keep their own recursion between frames. Off, each
        // later pass restarts its history every frame, which is what takes the
        // cross-pass residue out of motion - at the price of a little of the
        // temporal steadiness the stack buys.
        ChainedHistory,   // reno_chained_history     0 .. 1 (bool)
        // What the second, third and fourth pass of the RenoDX stack does on
        // its own. The package carries three such slots and a fifth pass takes
        // the fourth's, so these cover every count the stack can reach. Left
        // alone a later pass inherits pass 1's look entirely, which is why a
        // pass count changed the work done and not the picture.
        Pass2Follow,      // reno_pass_follow_0       0 .. 1 (bool)
        Pass2Style,       // reno_pass_style_0        0 .. 2
        Pass2Intensity,   // reno_pass_intensity_0    0 .. 2
        Pass2Tone,        // reno_pass_local_tone_0   0 .. 2
        Pass3Follow,      // reno_pass_follow_1       0 .. 1 (bool)
        Pass3Style,       // reno_pass_style_1        0 .. 2
        Pass3Intensity,   // reno_pass_intensity_1    0 .. 2
        Pass3Tone,        // reno_pass_local_tone_1   0 .. 2
        Pass4Follow,      // reno_pass_follow_2       0 .. 1 (bool)
        Pass4Style,       // reno_pass_style_2        0 .. 2
        Pass4Intensity,   // reno_pass_intensity_2    0 .. 2
        Pass4Tone,        // reno_pass_local_tone_2   0 .. 2
        // Passes (fast): one network pass, strengthened afterwards, instead of
        // running the network once per pass. The strength is still referred to
        // legacy pass counts, hence the float range 1..8. Two things read
        // differently from the legacy knobs and both are in the runtime:
        // reno_passes_mode = 1 makes stack_passes() return 1, so RenoPasses is
        // dead there; and reno_fast_passes_N = 1 means "no strengthening",
        // i.e. off, not "one pass". Three methods carry their own settings,
        // Extrapolation (0) / Feedback (1) / Hybrid (2); method 3 (Edit
        // strength) has no slot and uses FastEditStrength instead.
        PassesMode,       // reno_passes_mode         0 .. 1 (0 legacy, 1 fast)
        FastMethod,       // reno_fast_method         0 .. 3
        FastPasses0,      // reno_fast_passes_0       1 .. 8 (extrapolation)
        FastHalo0,        // reno_fast_halo_limit_0   0 .. 1 (bool)
        FastFine0,        // reno_fast_fine_0         0 .. 2
        FastPasses1,      // reno_fast_passes_1       1 .. 8 (feedback)
        FastHalo1,        // reno_fast_halo_limit_1   0 .. 1 (bool)
        FastFine1,        // reno_fast_fine_1         0 .. 2
        FastPasses2,      // reno_fast_passes_2       1 .. 8 (hybrid)
        FastHalo2,        // reno_fast_halo_limit_2   0 .. 1 (bool)
        FastFine2,        // reno_fast_fine_2         0 .. 2
        FastEditStrength, // reno_fast_edit_strength  0 .. 4
        Count
    };

    double value[Count];

    RenoDxSettings() {
        for (int i = 0; i < Count; ++i) value[i] = kStart[i];
    }

    // True when every field is at the look a fresh profile starts at, i.e. the
    // user has never moved anything in [RenoDX] - the signal to migrate the
    // measured group in, and the reference the ini's sparseness is kept
    // against. Judged against kStart, not kDefault: the starting look is a real
    // look, so a profile sitting on it has nothing of its own to record.
    bool IsEmpty() const {
        for (int i = 0; i < Count; ++i)
            if (std::fabs(value[i] - kStart[i]) > 1e-6) return false;
        return true;
    }

    // Equality within the override precision (two decimals), so the RenoDX
    // Settings window can show an unsaved mark only when a knob actually moved.
    bool operator==(const RenoDxSettings& o) const {
        for (int i = 0; i < Count; ++i)
            if (std::fabs(value[i] - o.value[i]) > 1e-6) return false;
        return true;
    }
    bool operator!=(const RenoDxSettings& o) const { return !(*this == o); }

    // The look this renderer was measured at, which is also where a fresh
    // profile starts and what Reset returns to: shadows 0.90 keeps the dark end
    // from lifting, the stabiliser is Always so stacked passes do not flicker,
    // and the stack runs two passes. Detail stays at the package's neutral 1.00
    // - the stabiliser already softens the picture and pushing detail up at the
    // same time over-corrects.
    //
    // One set for all three - the first time RenoDX is enabled, Reset, and a
    // profile that never touched the section - because three answers to "what
    // does RenoDX look like" is how the panel and the render drift apart. It is
    // deliberately NOT the package's identity: kDefault is what the render uses
    // when the host says nothing, and every knob where this look differs from it
    // is written into the controls, so the picture matches the panel.
    static RenoDxSettings MeasuredBaseline() { return RenoDxSettings{}; }

    // The recommended fast-mode configuration, used by the RenoDX window's
    // "restore recommended" button and by Reset while fast is selected. It only
    // touches the fast section: the look knobs the user tuned (shadows, detail,
    // stabiliser, ...) are not the fast mode's to reset. The mode switch itself
    // is deliberately left alone by the caller when it just wants the values.
    static void ApplyRecommendedFast(RenoDxSettings& settings) {
        for (int i = FastMethod; i < Count; ++i) settings.value[i] = kStart[i];
        settings.value[PassesMode] = 1.0;
    }

    // Whether the fast knobs are still the recommended ones. Drives the note
    // under the fast section, so a profile carrying hand-edited values says so
    // rather than claiming to be at the measured configuration.
    static bool FastSectionAtRecommended(const RenoDxSettings& settings) noexcept {
        for (int i = FastMethod; i < Count; ++i)
            if (std::fabs(settings.value[i] - kStart[i]) > 1e-6) return false;
        return true;
    }

    // Where a later pass's four fields live: slot 1 is the second pass of the
    // stack, slot 3 the fourth. The dialog edits one slot at a time, so its
    // Reset has to restore one slot - and it restores kStart, not kDefault: the
    // starting look gives every later pass its own style, so resetting a slot to
    // the package's "follow pass 1 / standard style / tone 1" would quietly undo
    // the look the rest of the panel claims to be at (and tone 1 would compound
    // across passes on the next Apply).
    static constexpr int PassSlotBase(int slot) { return Pass2Follow + (slot - 1) * 4; }
    void ResetPassSlot(int slot) {
        const int base = PassSlotBase(slot);
        for (int k = 0; k < 4; ++k) value[base + k] = kStart[base + k];
    }

    // The [RenoDX] ini key for each knob (one key per control, the dialog's
    // sole owner) and the matching control-struct member name ApplyControlFields
    // writes into.
    static constexpr const char* kIniKey[Count] = {
        "RenoDetail", "RenoDetailRadius", "RenoShadows", "RenoHighlights",
        "RenoColour", "RenoTone", "RenoDetailStability", "RenoLookStabilize",
        "RenoStabilizeMs", "RenoPasses", "RenoMvecScaleX", "RenoMvecScaleY",
        "RenoLookMode", "RenoChainedHistory",
        "RenoPass2Follow", "RenoPass2Style", "RenoPass2Intensity", "RenoPass2Tone",
        "RenoPass3Follow", "RenoPass3Style", "RenoPass3Intensity", "RenoPass3Tone",
        "RenoPass4Follow", "RenoPass4Style", "RenoPass4Intensity", "RenoPass4Tone",
        "RenoPassesMode", "RenoFastMethod", "RenoFastExtrapolationPasses",
        "RenoFastExtrapolationHaloLimit", "RenoFastExtrapolationFine",
        "RenoFastFeedbackPasses", "RenoFastFeedbackHaloLimit", "RenoFastFeedbackFine",
        "RenoFastHybridPasses", "RenoFastHybridHaloLimit", "RenoFastHybridFine",
        "RenoFastEditStrength"};
    static constexpr const char* kControlName[Count] = {
        "reno_look_detail", "reno_look_detail_radius", "reno_look_shadows",
        "reno_look_highlights", "reno_look_colour", "reno_look_tone",
        "reno_detail_stability", "reno_look_stabilize", "reno_look_stabilize_ms",
        "reno_passes", "reno_mvec_scale_x", "reno_mvec_scale_y",
        "reno_look_mode", "reno_chained_history",
        "reno_pass_follow_0", "reno_pass_style_0", "reno_pass_intensity_0", "reno_pass_local_tone_0",
        "reno_pass_follow_1", "reno_pass_style_1", "reno_pass_intensity_1", "reno_pass_local_tone_1",
        "reno_pass_follow_2", "reno_pass_style_2", "reno_pass_intensity_2", "reno_pass_local_tone_2",
        "reno_passes_mode", "reno_fast_method", "reno_fast_passes_0",
        "reno_fast_halo_limit_0", "reno_fast_fine_0",
        "reno_fast_passes_1", "reno_fast_halo_limit_1", "reno_fast_fine_1",
        "reno_fast_passes_2", "reno_fast_halo_limit_2", "reno_fast_fine_2",
        "reno_fast_edit_strength"};
    // The runtime's own table, copied from MochizukiRenoOptions.inc in the
    // package (the RDX_ lines name every field with its default and its range):
    // the look floats 1.00, detail stability 0 (auto), the stabilisers 0,
    // stabilise 60 ms, passes 1, motion-vector scale 1.00. It used to be a
    // different table, and the difference was not cosmetic: detail stability 1
    // ("off") is not the runtime's default, and a stabilise window of 0 is
    // clamped up to the 10 ms floor, so the panel said one thing and the render
    // did another. Copying the package's numbers is the only version of this
    // table that cannot drift from it.
    //
    // reno_look_mode is a bool in the package (NRLookMode, 0..1), not the
    // 0..3 selector this once assumed; it defaults to 1 here because the route
    // has always forced it on, and 1 is what the panel's switch shows.
    static constexpr double kDefault[Count] = {
        1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 0.00, 0.00, 60.00, 1.00, 1.00, 1.00,
        1.00,
        1.00, // chained history: on, the package's own
        // The per-pass slots, three times over: a later pass follows pass 1
        // (true), in the standard style (0), at pass 1's strength (1.00) and
        // pass 1's tone (1.00). The package's own defaults, so a profile that
        // never touched them keeps the picture it had.
        1.00, 0.00, 1.00, 1.00,
        1.00, 0.00, 1.00, 1.00,
        1.00, 0.00, 1.00, 1.00,
        // Passes (fast), the package's own (MochizukiRenoOptions.inc): legacy
        // mode, Extrapolation, 3 legacy passes, no halo limit, Fine 0 / 1 / 1,
        // Edit strength 2.00. Fine 0 on Extrapolation is the package's choice,
        // not an omission: the guided filter already carries the fine band.
        0.00, 0.00, 3.00, 0.00, 0.00, 3.00, 0.00, 1.00, 3.00, 0.00, 1.00, 2.00};
    // The look a fresh profile starts at and Reset returns to - the set that was
    // tuned in the player and measured: detail 1.00 (neutral), shadows 0.90,
    // detail stability Always, two passes.
    // kDefault above stays the package's identity and is still the reference a
    // knob is judged against when deciding whether it is worth writing into the
    // render; this table is what the panel shows and what [RenoDX] records
    // deviations from. Splitting them is what lets the starting look be a real
    // look: if kDefault were the start, shadows 0.90 would equal "unset" and
    // would never reach the runtime, and the panel would show a grade the
    // picture never had.
    static constexpr double kStart[Count] = {
        //            detail  radius  shadows highlights colour tone
        // Detail is the package's own 1.00, i.e. neutral. 2.00 was the value the
        // first sweep picked (it buys back the texture the grading layer smooths
        // away), but with the stabiliser on by default the picture is already
        // softer, and stacking both left it over-corrected: the two knobs pull
        // the same way, so the shipped look keeps the sharpening at neutral and
        // lets the stabiliser do the whole job. Raise it to taste - it is one
        // slider, and it is still written into the render when it differs from
        // the package (see ApplyNeuralRoute).
        1.00, 1.00, 0.90, 1.00, 1.00, 1.00,
        //            stability stabilise ms  passes
        // Stability is Always (2), not the package's Auto (0): Auto only engages
        // when the host marks a frame AFTER_UPSCALE, and this player never does
        // (the flag is for NR-after-upscaler game paths), so Auto is a measured
        // no-op here - bit-identical to Off. Always is the one setting that
        // actually damps the flicker stacked passes produce: frame-to-frame
        // change 3.84 -> 3.22 (-16%) for texture 1.76 -> 1.45 on the
        // calibration clip, and 1.45 is still the source's own 1.38 rather than
        // below it. It sits in kStart (not kDefault) so it is still written into
        // the render: kDefault is the package's identity, kStart is this look.
        2.00, 0.00, 60.00, 2.00,
        //            mvec x  mvec y  look mode chained
        1.00, 1.00, 1.00, 1.00,
        // The per-pass slots, swept on the project's calibration clip
        // (face_clip, 1920x800, 48 frames) through `--render` and measured as
        // frame-to-frame change against the source's own (ghosting) and
        // high-frequency energy (texture recovered):
        //   follow, std 1.00   temporal 4.66 texture 2.86  - within 35 dB of
        //     following, i.e. turning follow off with the same style changes
        //     nothing: the style is what a later pass actually owns;
        //   natural 1.00       temporal 3.84 texture 1.76  - clearly its own
        //     picture (30.8 dB from the follow arm), structure without the
        //     crunch;
        //   cinematic 1.00     temporal 3.34 texture 1.51  - closest to the
        //     source on both, the calmest, and the least recovered;
        //   std 0.50           temporal 4.02 texture 2.23.
        // Natural at full strength is the middle of that trade-off and the one
        // a later pass can call its own. Tone 0 follows the project's own rule
        // for stacked passes (tone re-applied on every pass compounds), and the
        // third and fourth run at half strength, which is where the measured
        // ghosting stops being worth the extra structure.
        0.00, 1.00, 1.00, 0.00,
        0.00, 1.00, 0.50, 0.00,
        0.00, 1.00, 0.50, 0.00,
        // Passes (fast) starts at the measured RECOMMENDED configuration, with
        // the mode itself still on legacy: switching to fast is a different
        // picture by construction (one network pass, the edit amplified k
        // times), so it stays a thing the user asks for. HaloLimit=1 / Fine=0.24
        // are not cosmetic: at the same 3 passes they buy +0.50 dB / +0.0063
        // SSIM against the source and take the high-frequency ratio 1.114 ->
        // 1.107 (docs/RENODX-FAST-MODE-INTEGRATION-zh.md section 12). They
        // differ from kDefault on purpose - kStart is the look this player
        // shows and writes, kDefault stays the runtime's identity - and
        // ApplyNeuralRoute drops the whole fast section in legacy mode so no
        // existing profile's cache/retention key changes just because of this.
        0.00, 0.00, 3.00, 1.00, 0.24, 3.00, 0.00, 1.00, 3.00, 0.00, 1.00, 2.00};
    // Clamp ranges, copied from the same table so a hand-edited or out-of-range
    // [RenoDX] value is pinned rather than refused - and pinned to what the
    // runtime accepts, not past it.
    static constexpr double kLow[Count] = {
        0.0, 0.25, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 10.0, 1.0, -2.0, -2.0,
        0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0,
        // Passes (fast): mode 0..1, method 0..3, passes 1..8, halo 0..1,
        // fine 0..2, edit strength 0..4 (the .inc's own ranges).
        0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0};
    static constexpr double kHigh[Count] = {
        2.0, 4.0, 2.0, 2.0, 2.0, 2.0, 2.0, 1.0, 250.0, 30.0, 2.0, 2.0,
        1.0, 1.0,
        1.0, 2.0, 2.0, 2.0,
        1.0, 2.0, 2.0, 2.0,
        1.0, 2.0, 2.0, 2.0,
        1.0, 3.0, 8.0, 1.0, 2.0, 8.0, 1.0, 2.0, 8.0, 1.0, 2.0, 4.0};
};

// Whether every value is the runtime's own default.
inline bool MochizukiOptionsAreDefault(const MochizukiOptions& options) noexcept
{
    return options == MochizukiOptions{};
}

// The panel's processing route, spelled as control overrides so the dialog, a
// live session and `--render` all mean the same thing by "RenoDX": the route
// owns the switch and the look this renderer was A/B'd at (shadows 0.90 keeps the
// dark end from lifting, detail left neutral at 1.00 so the Always stabiliser is
// the only thing softening the picture, both measured on moving and still
// content).
//
// Route 0 removes the keys rather than writing renodx_enabled=0: off is the
// runtime's own default, and an override list that only says "off" would make
// every job carry a "mochizuki=" term in its cache key - a key that claims a
// different picture for one that is byte-identical to the default panel's.
// A profile written before the route existed asked for the grading layer by
// putting renodx_enabled=1 in MochizukiOverride rather than by picking a route.
// Reading that back is the migration: without it the new default (0) would
// silently switch the layer off on the next start, changing a picture the user
// never asked to change. Only consulted when the file has no route key at all,
// so a real choice is never overridden by a leftover override.
inline int MigrateRouteFromOverrides(int route, bool routeKeyPresent,
                                    const MochizukiOptions& options)
{
    if (routeKeyPresent) return route;
    for (const auto& entry : options.overrides)
        if (entry.first == "renodx_enabled" && entry.second != 0.0) return 1;
    return route;
}

// The panel's processing route, spelled as control overrides so the dialog, a
// live session and `--render` all mean the same thing by "RenoDX": the route
// owns the switch and the look this renderer was A/B'd at. When the route is on,
// renodx_enabled is forced on and reno_look_mode is written from the panel's
// switch, then every RenoDX knob the user has moved from its package default -
// so a knob left at its default is not pinned into the controls the render
// reads. When it is off, every one of these keys is cleared so the render falls
// back to the runtime's own defaults.
inline void ApplyNeuralRoute(MochizukiOptions& options, bool renoDx, const RenoDxSettings& reno)
{
    auto& overrides = options.overrides;
    static constexpr const char* kRouteKeys[]{
        "renodx_enabled", "reno_look_mode", "reno_look_detail", "reno_look_shadows",
        "reno_look_detail_radius", "reno_look_highlights", "reno_look_colour", "reno_look_tone",
        "reno_detail_stability", "reno_look_stabilize", "reno_look_stabilize_ms", "reno_passes",
        "reno_unlock_passes", "reno_mvec_scale_x", "reno_mvec_scale_y", "reno_chained_history",
        // The later passes' own look. They are the route's too: a route turned
        // off must not leave a per-pass override behind, and a second Apply must
        // not append them twice - a doubled key would double the cache term.
        "reno_pass_follow_0", "reno_pass_style_0", "reno_pass_intensity_0", "reno_pass_local_tone_0",
        "reno_pass_follow_1", "reno_pass_style_1", "reno_pass_intensity_1", "reno_pass_local_tone_1",
        "reno_pass_follow_2", "reno_pass_style_2", "reno_pass_intensity_2", "reno_pass_local_tone_2",
        // Passes (fast) is the route's too. A route turned off must leave none
        // of it behind: a stale reno_passes_mode = 1 would survive into the next
        // session and make the panel's Passes slider dead without saying so.
        "reno_passes_mode", "reno_fast_method", "reno_fast_passes_0",
        "reno_fast_halo_limit_0", "reno_fast_fine_0",
        "reno_fast_passes_1", "reno_fast_halo_limit_1", "reno_fast_fine_1",
        "reno_fast_passes_2", "reno_fast_halo_limit_2", "reno_fast_fine_2",
        "reno_fast_edit_strength"};
    std::erase_if(overrides, [](const std::pair<std::string, double>& entry) {
        for (const char* key : kRouteKeys)
            if (entry.first == key) return true;
        return false;
    });
    if (!renoDx) return;
    overrides.emplace_back("renodx_enabled", 1.0);
    // reno_look_mode is written unconditionally from the user's selection. The
    // runtime's own default is 0, and the route used to force 1, so leaving it
    // out here would change the picture for existing profiles; defaulting the
    // field to 1 keeps that forced-1 behaviour until the user picks otherwise.
    overrides.emplace_back("reno_look_mode", reno.value[RenoDxSettings::LookMode]);
    const bool fastMode = reno.value[RenoDxSettings::PassesMode] != 0.0;
    for (int i = 0; i < RenoDxSettings::Count; ++i) {
        if (i == RenoDxSettings::LookMode) continue; // written above, always
        // In legacy mode stack_passes() runs the legacy count, so every fast
        // key is dead. The starting look deliberately carries the recommended
        // fast values (kStart), and writing those here would lengthen the
        // override list for every existing profile - changing its retention
        // and cache keys without changing a single pixel. The erase above
        // still removes any fast key left by a previous fast session.
        if (!fastMode && i >= RenoDxSettings::PassesMode) continue;
        if (std::fabs(reno.value[i] - RenoDxSettings::kDefault[i]) > 1e-6)
            overrides.emplace_back(RenoDxSettings::kControlName[i], reno.value[i]);
    }
    // A RenoDX stack runs at most four passes unless the runtime's own unlock is
    // set, and it clamps the count itself rather than refusing it: the panel
    // offers 1..30 and a count of eight would quietly be four. Asking for the
    // unlock with the count is what makes the number on the panel the number
    // that runs - the same switch the package's own UI calls NRUnlockPasses.
    if (reno.value[RenoDxSettings::RenoPasses] > 4.0)
        overrides.emplace_back("reno_unlock_passes", 1.0);
}

// One line, so the helper's command line stays one argument: `key=value` pairs
// separated by commas, floats at two decimals (the runtime's own precision).
//
// An empty string is the runtime's defaults, which is what a job that changed
// nothing sends: the helper then takes no argument at all and an older helper -
// one that has never heard of this key - renders the same picture.
// By value on purpose: what goes on the wire is the NORMALISED value. A caller
// must not be able to hand the helper a pair the runtime refuses (INT4 + small
// network) while the cache key still claims INT4 was on.
inline std::string FormatMochizukiOptions(MochizukiOptions options)
{
    options.NormaliseInt4();
    if (MochizukiOptionsAreDefault(options)) return {};
    // Two lines' worth of fields: a pass block is seven values and there are
    // two of them, so 256 was no longer enough once they existed. The override
    // list is open-ended, so the buffer grows with it rather than capping it.
    std::string overrides;
    for (size_t index = 0; index < options.overrides.size(); ++index) {
        if (index != 0) overrides.push_back(';');
        overrides += options.overrides[index].first;
        char value[32]{};
        std::snprintf(value, sizeof(value), "=%.2f", options.overrides[index].second);
        overrides += value;
    }
    if (overrides.empty()) overrides = "none";
    std::string line;
    line.resize(512 + overrides.size() * 2);
    // `|` inside a pass block: the outer separator is the comma, and a nested
    // comma would split a pass into two fields that parse as neither.
    const auto pass = [](const MochizukiPassOptions& p) {
        char block[96]{};
        std::snprintf(block, sizeof(block), "%d|%u|%.2f|%.2f|%.2f|%.2f|%d",
                      p.followPass1 ? 1 : 0, p.style, double(p.intensity), double(p.localTone),
                      double(p.localStructure), double(p.skinStructure), p.autoMask ? 1 : 0);
        return std::string(block);
    };
    std::snprintf(line.data(), line.size(),
                  "intensity=%.2f,tone=%.2f,structure=%.2f,skin=%.2f,mask=%d,maxratio=%.2f,"
                  "history=%.2f,apply=%d,pre=%d,int4=%d,drs=%u,network=%u,pass2=%s,pass3=%s,mz=%s",
                  double(options.intensity), double(options.localTone),
                  double(options.localStructure), double(options.skinStructure),
                  options.autoMask ? 1 : 0, double(options.maxRatio),
                  double(options.historyStrength), options.applyModel ? 1 : 0,
                  options.preprocess ? 1 : 0, options.int4Mixed ? 1 : 0, options.drsMode,
                  options.networkModel, pass(options.pass2).c_str(), pass(options.pass3).c_str(),
                  overrides.c_str());
    // snprintf leaves the rest of the resized buffer as NUL, and a command-line
    // argument cannot carry one: the child would see a truncated or split value
    // and refuse the whole line. Trim to what was written.
    line.resize(std::strlen(line.c_str()));
    return line;
}

// The part of the picture the add-on's canonical settings string cannot say: the
// Mochizuki controls (where the RenoDX route puts its whole look) plus the five
// look values the job borrows from the panel rather than carrying in the struct.
//
// Apply restarts a live session when any of these moves, so two looks must not
// share a retention key: a restart that finds the key it had before adopts the
// segments the OLD look produced, and the change reaches no frame that was
// already rendered. Measured on 2026-11-05, before this term existed: switching
// detail stability Always -> Off restarted the session at 28.9 s and the next
// stop still reported coverage from 4.13 s, i.e. the 19 segments rendered under
// the old look were adopted and the new one was written only into new ground.
//
// Empty when the merged controls are the runtime's own defaults, so a profile
// that never touched them keeps byte-identical retention keys.
inline std::string MochizukiLookTerm(const MochizukiOptions& options,
                                     float intensity, float localTone, float localStructure,
                                     float skinStructure, bool autoMask)
{
    MochizukiOptions merged = options;
    merged.intensity = intensity;
    merged.localTone = localTone;
    merged.localStructure = localStructure;
    merged.skinStructure = skinStructure;
    merged.autoMask = autoMask;
    return FormatMochizukiOptions(merged);
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
    // A pass block: seven fields separated by `|` - follow, style, intensity,
    // tone, structure, skin, mask - every one of them required, because the
    // writer always emits all seven. Anything else (an eighth field, a missing
    // one, a value outside the runtime's range) means the line did not come
    // from this writer, and a half-read look is worse than no answer.
    const auto passBlock = [&inRange](std::string_view block) -> std::optional<MochizukiPassOptions> {
        std::array<double, 7> values{};
        for (size_t index = 0; index < values.size(); ++index) {
            const size_t bar = block.find('|');
            const std::string_view field = block.substr(0, bar);
            block = bar == std::string_view::npos ? std::string_view{} : block.substr(bar + 1);
            if (field.empty() || field.size() >= 32) return std::nullopt;
            char scratch[32]{};
            std::memcpy(scratch, field.data(), field.size());
            char* end = nullptr;
            const double parsed = std::strtod(scratch, &end);
            if (end == scratch || *end != '\0') return std::nullopt;
            values[index] = parsed;
        }
        if (!block.empty()) return std::nullopt;   // an eighth field
        const auto whole = [](double value, double low, double high) {
            return std::isfinite(value) && value >= low && value <= high && value == std::floor(value);
        };
        if (!whole(values[0], 0.0, 1.0) || !whole(values[1], 0.0, 2.0)) return std::nullopt;
        if (!inRange(values[2], 0.0, 2.0) || !inRange(values[3], 0.0, 2.0) ||
            !inRange(values[4], 0.0, 2.0) || !inRange(values[5], -1.0, 2.0))
            return std::nullopt;
        if (!whole(values[6], 0.0, 1.0)) return std::nullopt;
        MochizukiPassOptions out;
        out.followPass1 = values[0] != 0.0;
        out.style = uint32_t(values[1]);
        out.intensity = float(values[2]);
        out.localTone = float(values[3]);
        out.localStructure = float(values[4]);
        out.skinStructure = float(values[5]);
        out.autoMask = values[6] != 0.0;
        return out;
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
        // The `mz` list is the one value that is not a single number: it is a
        // `;`-separated list whose length grows with the number of overrides the
        // job carries, so the per-number bound below does not apply to it. It is
        // bounded on its own in that branch.
        if (key != "mz" && value.size() >= sizeof(buffer)) return std::nullopt;
        std::memcpy(buffer, value.data(), std::min(value.size(), sizeof(buffer) - 1));
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
        else if (key == "network") {
            // Two networks, not a range: anything else names a file the host
            // cannot check for, so it is refused rather than clamped to 0.
            const auto n = number();
            if (!inRange(n, 0.0, 1.0) || *n != std::floor(*n)) return std::nullopt;
            options.networkModel = uint32_t(*n);
        }
        else if (key == "pass2" || key == "pass3") {
            const auto block = passBlock(value);
            if (!block) return std::nullopt;
            (key == "pass2" ? options.pass2 : options.pass3) = *block;
        }
        else if (key == "mz") {
            // The override list: `name=value` pairs separated by `;`, or `none`
            // for "the package's own defaults". Only the shape is checked here -
            // a name this build does not know still parses, so the evaluator can
            // log it by name instead of the whole line failing on a typo.
            if (value != "none") {
                if (value.size() > 1024) return std::nullopt;   // 1024 is ~40 pairs, far past any panel
                std::string_view list = value;
                while (!list.empty()) {
                    const size_t semi = list.find(';');
                    const std::string_view pair = list.substr(0, semi);
                    list = semi == std::string_view::npos ? std::string_view{} : list.substr(semi + 1);
                    const size_t eq = pair.find('=');
                    if (eq == std::string_view::npos || eq == 0 || eq + 1 >= pair.size())
                        return std::nullopt;
                    const std::string_view name = pair.substr(0, eq);
                    const std::string_view valueText = pair.substr(eq + 1);
                    if (name.size() >= 48 || valueText.size() >= 32) return std::nullopt;
                    for (const char c : name)
                        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
                            return std::nullopt;
                    char scratch[32]{};
                    std::memcpy(scratch, valueText.data(), valueText.size());
                    char* end = nullptr;
                    const double parsed = std::strtod(scratch, &end);
                    if (end == scratch || *end != '\0' || !std::isfinite(parsed)) return std::nullopt;
                    options.overrides.emplace_back(std::string(name), parsed);
                }
            }
        }
        else return std::nullopt;
        ++seen;
    }
    if (seen != 15) return std::nullopt;
    // An older host (or a hand-edited command line) can still send the pair the
    // runtime refuses. Normalise on the way in too, so the worker's request and
    // the job's cache key cannot disagree about what was asked for.
    options.NormaliseInt4();
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
