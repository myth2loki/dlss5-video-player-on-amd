#pragma once

// AmdLmxxfApi.h is the only place this repository declares TemporalGuides, the
// struct both backends take their guides in; the mochizuki header adds the frame
// contract that does not exist on the lmxxf side.
#include "AmdLmxxfApi.h"
#include "../third_party/mochizuki/MochizukiNrApi.h"

#include <cstdint>
#include <string_view>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace amd_mochizuki {

// The two exports a Mochizuki session cannot work without. The rest of the call
// order is the LmxxfNrApi table this runtime hands back from MochizukiNrGetApi -
// the same 21 entries src/AmdLmxxfApi.h validates, so nothing here duplicates
// that validation.
inline constexpr std::string_view kGetApiExport = "MochizukiNrGetApi";
inline constexpr std::string_view kPrepareFrameExport = "MochizukiNrPrepareFrame";

// The module name a Mochizuki runtime tree holds. The lmxxf tree holds
// LmxxfNrRuntime.dll beside it, which is what makes coexistence a matter of
// resolving a kind rather than choosing a directory.
inline constexpr std::wstring_view kModuleFileName = L"MochizukiNrRuntime.dll";
// The model the runtime needs, and the shader/ACO bundles beside it. Their
// absence is reported by Create/PrepareSession, not by the loader, but the
// failure is much easier to read when the loader says which one is missing.
inline constexpr std::wstring_view kModelRelativePath = L"dlssnr-amd\\dlssnr.bin";
inline constexpr std::wstring_view kShadersRelativePath = L"dlssnr-amd\\shaders";

inline bool HasPrepareExport(PFN_MochizukiNrPrepareFrame prepare) noexcept
{
    return prepare != nullptr;
}

// Builds one frame's contract. Deliberately mirrors amd_lmxxf::MakeFrameInfo so
// the two call sites in AmdLmxxfEvaluator stay one branch apart, and leaves
// every guide field null - a spatial frame is the truthful answer for a host
// that has no motion/depth, and the runtime reports history=off for it instead
// of failing.
inline MochizukiNrFrameInfo MakeFrameInfo(void* color, uint32_t width, uint32_t height,
                                          uint32_t colorState, uint64_t frameId,
                                          uint32_t passes, float transferStrength = 1.0f,
                                          float colorStrength = 1.0f,
                                          float modelScale = 1.0f) noexcept
{
    MochizukiNrFrameInfo info{};
    info.struct_size = sizeof(info);
    info.frame_id = frameId;
    info.color = color;
    info.color_width = width;
    info.color_height = height;
    info.color_state = colorState;
    // STRENGTH is the one lmxxf flag this struct also honours: without it the
    // runtime ignores transfer_strength/color_strength rather than failing, so
    // a host that forgets it silently renders at 1.0/1.0.
    info.flags = LMXXF_NR_FRAME_FLAG_STRENGTH;
    info.transfer_strength = transferStrength;
    info.color_strength = colorStrength;
    info.model_scale = modelScale;
    // 0 means 1 in the runtime; spelling 1 makes the log line and the struct
    // agree without relying on that.
    info.passes = passes < 1u ? 1u : passes;
    return info;
}

// Fills the guide half of `info` from the same TemporalGuides the lmxxf path
// hands over, so the estimator keeps producing one field for both backends.
//
// Two fields do not survive the trip and are dropped on purpose:
//   * jitter_x/jitter_y - Mochizuki has no per-frame jitter input. A decoded
//     video frame has no engine jitter, so the value was always zero here.
//   * LMXXF_NR_TEMPORAL_MV_JITTERED - no equivalent, and its absence is correct
//     for a source with no jitter.
// Everything that does exist is carried: reset (a plain u32, not a temporal
// flag), depth inversion, the motion field's own extent and its scale factors.
inline void ApplyTemporalGuides(MochizukiNrFrameInfo& info,
                                const amd_lmxxf::TemporalGuides& guides,
                                uint32_t colorWidth, uint32_t colorHeight) noexcept
{
    info.motion = guides.motion;
    info.motion_state = guides.motionState;
    info.motion_width = guides.motionWidth;
    info.motion_height = guides.motionHeight;
    info.motion_scale_x = guides.motionScaleX;
    info.motion_scale_y = guides.motionScaleY;
    info.depth = guides.depth;
    info.depth_state = guides.depthState;
    // LmxxfNrFrameInfo has no depth extent: it requires the colour to fit inside
    // the depth texture and reads the colour's own size. Here the extent is
    // per-frame state, so it is the colour's - the runtime clamps it to the
    // texture anyway.
    info.depth_width = colorWidth;
    info.depth_height = colorHeight;
    info.depth_inverted =
        (guides.flags & LMXXF_NR_TEMPORAL_DEPTH_INVERTED) ? 1u : 0u;
    info.reset = guides.reset ? 1u : 0u;
    // History is engaged by the frame flag + a non-null motion texture, not by a
    // bit in a temporal_flags word.
    if (guides.motion != nullptr && (guides.flags & LMXXF_NR_TEMPORAL_MODEL_HISTORY))
        info.flags |= MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL;
}

// Converts a frame already built for lmxxf into the Mochizuki contract. This is
// the whole difference between the two backends' prepare calls: the evaluator
// keeps filling one LmxxfNrFrameInfo (it owns the colour texture, the upload and
// the guides), and the branch is this function plus which export it calls.
//
// Fields with no home - session_id, list_generation, the exposure block,
// paper_white, debug_view, the temporal_flags word - are dropped rather than
// approximated:
//   * session identity is the context pointer on this ABI;
//   * exposure is session state (MochizukiNrSetExposure / SetExposureTexture),
//     and the video path is SDR with pre_exposure 1.0, so there is nothing to
//     forward;
//   * debug_view has no counterpart.
// `passes` is the caller's because the lmxxf side carries the count in its
// runtime configuration file rather than the frame.
inline MochizukiNrFrameInfo ConvertFrame(const LmxxfNrFrameInfo& source,
                                         uint32_t passes) noexcept
{
    MochizukiNrFrameInfo info = MakeFrameInfo(source.color, source.color_width,
                                              source.color_height, source.color_state,
                                              source.frame_id, passes,
                                              source.transfer_strength, source.color_strength,
                                              source.model_scale);
    amd_lmxxf::TemporalGuides guides{};
    guides.motion = source.motion;
    guides.depth = source.depth;
    guides.motionState = source.motion_state;
    guides.depthState = source.depth_state;
    guides.motionWidth = source.motion_width;
    guides.motionHeight = source.motion_height;
    guides.motionScaleX = source.motion_scale_x;
    guides.motionScaleY = source.motion_scale_y;
    guides.jitterX = source.jitter_x;
    guides.jitterY = source.jitter_y;
    guides.flags = source.temporal_flags;
    guides.reset = (source.temporal_flags & LMXXF_NR_TEMPORAL_RESET) != 0;
    ApplyTemporalGuides(info, guides, source.color_width, source.color_height);
    // The strength flags are the caller's choice on the lmxxf struct; this
    // struct can only express them as a word, so it says exactly what the source
    // struct said.
    if ((source.flags & LMXXF_NR_FRAME_FLAG_STRENGTH) == 0)
        info.flags &= ~uint32_t(LMXXF_NR_FRAME_FLAG_STRENGTH);
    return info;
}

/* Host-side overrides of the control struct.
 *
 * The 532-byte struct carries every knob the package has; this host has a panel
 * for a dozen of them and leaves the rest at whatever the payload carried. That
 * "whatever" used to be zero, which is not the package's default for most
 * fields - RenoDX's look controls default to 1.0, not 0 - so the job now starts
 * from GetControlDefaults (Runtime::MochizukiControlDefaults) and these entries
 * move single fields by their header name afterwards.
 *
 * The names are the struct's own member names, so an override list reads as the
 * struct it writes and there is no second spelling to drift. The ranges are the
 * ones the header documents (and the ones MochizukiRenoOptions.inc states
 * upstream, for the RenoDX block). A value outside its range is clamped here
 * rather than refused: the runtime clamps the same way, and a hand-edited INI
 * should not fail a render over a decimal.
 *
 * `name` 0 or a value that is not finite is ignored, and the field keeps the
 * default it was seeded with. */
struct ControlField {
    const char* name;
    size_t offset;
    enum class Kind { Float, Uint, Bool };
    Kind kind;
    float low, high;
};

// Nothing here is a new ABI: every entry is a member of the struct the package
// already validates by size, at the offset this build's header gives it.
inline constexpr ControlField kControlFields[] = {
    // The RenoDX layer and its own NR: this is the block a film's grade comes
    // from, and it is off by default in the package (renodx_enabled = 0).
    {"renodx_enabled", offsetof(MochizukiNrControls, renodx_enabled), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_nr_enabled", offsetof(MochizukiNrControls, reno_nr_enabled), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_intensity", offsetof(MochizukiNrControls, reno_intensity), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_style", offsetof(MochizukiNrControls, reno_style), ControlField::Kind::Uint, 0.f, 2.f},
    {"reno_local_tone", offsetof(MochizukiNrControls, reno_local_tone), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_local_structure", offsetof(MochizukiNrControls, reno_local_structure), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_skin_structure", offsetof(MochizukiNrControls, reno_skin_structure), ControlField::Kind::Float, -1.f, 1.f},
    {"reno_auto_mask", offsetof(MochizukiNrControls, reno_auto_mask), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_skin_independent", offsetof(MochizukiNrControls, reno_skin_independent), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_passes", offsetof(MochizukiNrControls, reno_passes), ControlField::Kind::Uint, 1.f, 30.f},
    {"reno_unlock_passes", offsetof(MochizukiNrControls, reno_unlock_passes), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_chained_history", offsetof(MochizukiNrControls, reno_chained_history), ControlField::Kind::Bool, 0.f, 1.f},
    // What a later pass of the RenoDX stack does on its own. The package
    // carries three slots - the second, third and fourth pass - and a fifth
    // pass takes the fourth's, so three slots cover every count the stack can
    // reach. Without these in the table a per-pass setting would be named in an
    // override and dropped as unknown, and every extra pass would keep
    // inheriting pass 1's look: the count would change and the picture would
    // not.
    {"reno_pass_follow_0", offsetof(MochizukiNrControls, reno_pass_follow_0), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_pass_style_0", offsetof(MochizukiNrControls, reno_pass_style_0), ControlField::Kind::Uint, 0.f, 2.f},
    {"reno_pass_intensity_0", offsetof(MochizukiNrControls, reno_pass_intensity_0), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_pass_local_tone_0", offsetof(MochizukiNrControls, reno_pass_local_tone_0), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_pass_follow_1", offsetof(MochizukiNrControls, reno_pass_follow_1), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_pass_style_1", offsetof(MochizukiNrControls, reno_pass_style_1), ControlField::Kind::Uint, 0.f, 2.f},
    {"reno_pass_intensity_1", offsetof(MochizukiNrControls, reno_pass_intensity_1), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_pass_local_tone_1", offsetof(MochizukiNrControls, reno_pass_local_tone_1), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_pass_follow_2", offsetof(MochizukiNrControls, reno_pass_follow_2), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_pass_style_2", offsetof(MochizukiNrControls, reno_pass_style_2), ControlField::Kind::Uint, 0.f, 2.f},
    {"reno_pass_intensity_2", offsetof(MochizukiNrControls, reno_pass_intensity_2), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_pass_local_tone_2", offsetof(MochizukiNrControls, reno_pass_local_tone_2), ControlField::Kind::Float, 0.f, 2.f},
    // The grade: the knobs that decide whether skin reads as skin or as wax.
    {"reno_look_mode", offsetof(MochizukiNrControls, reno_look_mode), ControlField::Kind::Uint, 0.f, 3.f},
    {"reno_look_strength", offsetof(MochizukiNrControls, reno_look_strength), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_brighten", offsetof(MochizukiNrControls, reno_look_brighten), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_darken", offsetof(MochizukiNrControls, reno_look_darken), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_colour", offsetof(MochizukiNrControls, reno_look_colour), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_hue", offsetof(MochizukiNrControls, reno_look_hue), ControlField::Kind::Float, -0.5f, 0.5f},
    {"reno_look_shadows", offsetof(MochizukiNrControls, reno_look_shadows), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_midtones", offsetof(MochizukiNrControls, reno_look_midtones), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_highlights", offsetof(MochizukiNrControls, reno_look_highlights), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_tone", offsetof(MochizukiNrControls, reno_look_tone), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_detail", offsetof(MochizukiNrControls, reno_look_detail), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_detail_radius", offsetof(MochizukiNrControls, reno_look_detail_radius), ControlField::Kind::Float, 0.f, 4.f},
    {"reno_look_halo", offsetof(MochizukiNrControls, reno_look_halo), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_halo_bright", offsetof(MochizukiNrControls, reno_look_halo_bright), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_look_stabilize", offsetof(MochizukiNrControls, reno_look_stabilize), ControlField::Kind::Uint, 0.f, 1.f},
    {"reno_look_stabilize_ms", offsetof(MochizukiNrControls, reno_look_stabilize_ms), ControlField::Kind::Float, 0.f, 500.f},
    // 0 auto, 1 off (the runtime's own default), 2 always. The clamp was 0..1,
    // which made "always" - the only setting that forces the stabiliser on -
    // unreachable from here.
    {"reno_detail_stability", offsetof(MochizukiNrControls, reno_detail_stability), ControlField::Kind::Uint, 0.f, 2.f},
    // 0 from the format, 1 SDR, 2 linear, 3 PQ. A film that reads as linear when
    // it is SDR is what an over-bright, waxy picture looks like from the inside.
    {"reno_source_encoding", offsetof(MochizukiNrControls, reno_source_encoding), ControlField::Kind::Uint, 0.f, 3.f},
    {"reno_color_strength", offsetof(MochizukiNrControls, reno_color_strength), ControlField::Kind::Float, 0.f, 2.f},
    {"reno_transfer_strength", offsetof(MochizukiNrControls, reno_transfer_strength), ControlField::Kind::Float, 0.f, 1.f},
    {"reno_chroma_clamp_stops", offsetof(MochizukiNrControls, reno_chroma_clamp_stops), ControlField::Kind::Float, 0.f, 8.f},
    {"reno_resolution_scale", offsetof(MochizukiNrControls, reno_resolution_scale), ControlField::Kind::Float, 0.25f, 1.f},
    {"reno_codec_mode", offsetof(MochizukiNrControls, reno_codec_mode), ControlField::Kind::Uint, 0.f, 2.f},
    {"reno_neural_floor_guard", offsetof(MochizukiNrControls, reno_neural_floor_guard), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_norm_governor", offsetof(MochizukiNrControls, reno_norm_governor), ControlField::Kind::Uint, 0.f, 2.f},
    {"reno_norm_cut_aware", offsetof(MochizukiNrControls, reno_norm_cut_aware), ControlField::Kind::Bool, 0.f, 1.f},
    {"reno_mvec_scale_x", offsetof(MochizukiNrControls, reno_mvec_scale_x), ControlField::Kind::Float, -4.f, 4.f},
    {"reno_mvec_scale_y", offsetof(MochizukiNrControls, reno_mvec_scale_y), ControlField::Kind::Float, -4.f, 4.f},
    // What the network is shown, which is where an over-bright film turns waxy.
    {"preprocess", offsetof(MochizukiNrControls, preprocess), ControlField::Kind::Uint, 0.f, 1.f},
    {"preprocess_exposure", offsetof(MochizukiNrControls, preprocess_exposure), ControlField::Kind::Uint, 0.f, 2.f},
    {"preprocess_curve", offsetof(MochizukiNrControls, preprocess_curve), ControlField::Kind::Uint, 0.f, 6.f},
    {"preprocess_contrast", offsetof(MochizukiNrControls, preprocess_contrast), ControlField::Kind::Float, 0.5f, 2.f},
    {"preprocess_saturation", offsetof(MochizukiNrControls, preprocess_saturation), ControlField::Kind::Float, 0.05f, 2.f},
    {"preprocess_bias_ev", offsetof(MochizukiNrControls, preprocess_bias_ev), ControlField::Kind::Float, -8.f, 8.f},
    // The model's own picture controls, repeated here so one override list can
    // carry a whole experiment instead of half of it.
    {"intensity", offsetof(MochizukiNrControls, intensity), ControlField::Kind::Float, 0.f, 2.f},
    {"style", offsetof(MochizukiNrControls, style), ControlField::Kind::Uint, 0.f, 2.f},
    {"local_tone", offsetof(MochizukiNrControls, local_tone), ControlField::Kind::Float, 0.f, 2.f},
    {"local_structure", offsetof(MochizukiNrControls, local_structure), ControlField::Kind::Float, 0.f, 2.f},
    {"skin_structure", offsetof(MochizukiNrControls, skin_structure), ControlField::Kind::Float, -1.f, 2.f},
    {"automatic_mask", offsetof(MochizukiNrControls, automatic_mask), ControlField::Kind::Bool, 0.f, 1.f},
    {"max_ratio", offsetof(MochizukiNrControls, max_ratio), ControlField::Kind::Float, 1.f, 8.f},
    {"history_strength", offsetof(MochizukiNrControls, history_strength), ControlField::Kind::Float, 0.f, 1.f},
    {"white_point", offsetof(MochizukiNrControls, white_point), ControlField::Kind::Float, 0.01f, 100.f},
    {"noise_every_frame", offsetof(MochizukiNrControls, noise_every_frame), ControlField::Kind::Bool, 0.f, 1.f},
    {"max_passes", offsetof(MochizukiNrControls, max_passes), ControlField::Kind::Uint, 0.f, 30.f},
    {"linear_input", offsetof(MochizukiNrControls, linear_input), ControlField::Kind::Uint, 0.f, 2.f},
    {"compact_transfer", offsetof(MochizukiNrControls, compact_transfer), ControlField::Kind::Bool, 0.f, 1.f},
    {"classic_scaler", offsetof(MochizukiNrControls, classic_scaler), ControlField::Kind::Uint, 0.f, 3.f},
};

// Applies the `name=value` pairs to a struct that has already been seeded (from
// GetControlDefaults, or zeroed). Returns the names it did not know, in the order
// they were given, so the caller can say so once: a typo that silently rendered
// the default is worse than a line in the log.
inline std::vector<std::string> ApplyControlFields(
    MochizukiNrControls& controls, const std::vector<std::pair<std::string, double>>& values)
{
    std::vector<std::string> unknown;
    for (const auto& [name, value] : values) {
        const ControlField* field = nullptr;
        for (const ControlField& candidate : kControlFields)
            if (name == candidate.name) { field = &candidate; break; }
        if (field == nullptr) {
            unknown.push_back(name);
            continue;
        }
        if (!std::isfinite(value)) continue;
        auto* base = reinterpret_cast<uint8_t*>(&controls) + field->offset;
        const double clamped = std::clamp(value, double(field->low), double(field->high));
        switch (field->kind) {
        case ControlField::Kind::Float:
            *reinterpret_cast<float*>(base) = static_cast<float>(clamped);
            break;
        case ControlField::Kind::Uint:
            *reinterpret_cast<uint32_t*>(base) = static_cast<uint32_t>(std::llround(clamped));
            break;
        case ControlField::Kind::Bool:
            *reinterpret_cast<uint32_t*>(base) = clamped != 0.0 ? 1u : 0u;
            break;
        }
    }
    return unknown;
}

} // namespace amd_mochizuki
