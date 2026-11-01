#pragma once

// AmdLmxxfApi.h is the only place this repository declares TemporalGuides, the
// struct both backends take their guides in; the mochizuki header adds the frame
// contract that does not exist on the lmxxf side.
#include "AmdLmxxfApi.h"
#include "../third_party/mochizuki/MochizukiNrApi.h"

#include <cstdint>
#include <string_view>

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

} // namespace amd_mochizuki
