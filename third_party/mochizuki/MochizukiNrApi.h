#pragma once

/*
 * Vendored from TheAutomatic/dlss-5-amd-project @ main (2026-10-28),
 * OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/mochizuki_runtime/MochizukiNrApi.h.
 *
 * MochizukiNrRuntime.dll does NOT carry its own API table. It hands back the
 * *Lmxxf* table (LmxxfNrApi, ABI 6, 176 bytes, 21 entries) from
 * MochizukiNrGetApi(), and replaces exactly one entry: the table's
 * `PrepareFrame` is a rejection stub ("Use MochizukiNrPrepareFrame from the
 * current package"), and the real prepare call is the standalone
 * MochizukiNrPrepareFrame() export below, taking its own frame struct.
 *
 * Measured against the packaged 1.12.1 runtime (MochizukiNrRuntime.dll,
 * 1,465,632 bytes) on 2026-10-30:
 *   MochizukiNrGetApi(6, table{struct_size=176}) -> OK, abi_version 6, 21/21 non-null
 *   MochizukiNrGetApi(1..5, 7..8, 176)           -> UNSUPPORTED_ABI
 *   MochizukiNrGetApi(6, table{struct_size!=176})-> INVALID_ARGUMENT
 *   table.QueryCapabilities(sizeof 32)           -> OK, box 16384x16384, history=1, gfx1201=1
 *   table.PrepareFrame(...)                      -> INVALID_ARGUMENT
 *                                                   "Use MochizukiNrPrepareFrame from the current package"
 * That is the same version negotiation src/AmdLmxxfApi.h already performs, so a
 * Mochizuki host reuses kAcceptedAbiVersions and ValidateApiTable unchanged.
 *
 * Everything below (the frame struct, its flag bits and the standalone exports)
 * is the only part of the contract that differs from lmxxf. There are no STL
 * types, no exceptions and no CRT allocations across this boundary.
 */

#include "../lmxxf/LmxxfNrApi.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A frame that carries motion vectors, i.e. that may continue history. Set it
 * only when `motion` is non-null: the runtime runs spatially without it and
 * reports history=off rather than failing. */
#define MOCHIZUKI_NR_FRAME_FLAG_TEMPORAL (1u << 8)
/* Record into the host's current native Vulkan command list. Only for an
 * adopted vkd3d device; on Windows (D3D12 host, D3D12->Vulkan bridge) it must
 * stay clear - PrepareFrame rejects the frame with INVALID_ARGUMENT otherwise. */
#define MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION (1u << 9)
/* SR -> NR placement. The video host always runs NR before SR, so it stays clear. */
#define MOCHIZUKI_NR_FRAME_FLAG_AFTER_UPSCALE (1u << 10)
/* Inline-path conversions on the compute family instead of graphics blits.
 * Inline frames only; harmless (ignored) on the split path this host uses. */
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_CONVERT (1u << 11)
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_MOTION (1u << 12)
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_DOWNSCALE (1u << 13)
#define MOCHIZUKI_NR_FRAME_FLAG_COMPUTE_EXPORT (1u << 14)
/* Diagnostic GPU timestamp pairs, reported through MochizukiNrInfo. */
#define MOCHIZUKI_NR_FRAME_FLAG_STAGE_TIMING (1u << 15)
/* Adopted vkd3d only: record into the game's native Vulkan command buffer.
 * Mutually exclusive with INLINE_EXECUTION; Windows keeps the bridge path. */
#define MOCHIZUKI_NR_FRAME_FLAG_DIRECT_RECORDING (1u << 16)

/* The two frame-struct sizes this family of runtimes has shipped, measured rather
 * than assumed. PrepareFrame rejects every struct_size but its own, and it says
 * so with the same sentence a null argument gets, so the size has to be found by
 * calling it:
 *
 *   OptScaler-NR 1.12.1 (MochizukiNrRuntime.dll sha256 176bc308...ae751):
 *     job 32 + frame  88 -> UNAVAILABLE "building the network ..."  (accepted)
 *     job 32 + frame 112 -> INVALID_ARGUMENT "PrepareFrame: bad arguments"
 *     job 24/40/48       -> INVALID_ARGUMENT for every frame size
 *
 * 88 is the struct up to and including `reset`: this package has no depth block.
 * 112 is the 0.0.5/upstream form below, which added depth/depth_state/
 * depth_width/depth_height/depth_inverted. Both are ABI 6; the difference is the
 * package revision, not the version integer, so a host stamps the size it was
 * told by the runtime rather than one it assumed.
 */
#define MOCHIZUKI_NR_FRAME_INFO_SIZE_LEGACY 88u
#define MOCHIZUKI_NR_FRAME_INFO_SIZE_FULL 112u

/* 112 bytes. NOT the 168-byte LmxxfNrFrameInfo: this struct has no
 * session_id/list_generation (the runtime identifies the session by the context
 * pointer), no exposure block (MochizukiNrSetExposure / SetExposureTexture owns
 * that), no paper_white, and it carries the pass count and depth extent per
 * frame instead of baking them into a configuration file.
 *
 * struct_size must be exactly sizeof(MochizukiNrFrameInfo); PrepareFrame answers
 * "PrepareFrame: bad arguments" for any other value. Fields are read only when
 * struct_size covers them (the runtime's MZ_HOLDS), so this header is the whole
 * contract for the version it was taken from. */
typedef struct MochizukiNrFrameInfo
{
    uint32_t struct_size;
    uint64_t frame_id;   /* the host's monotonic frame counter; history is crossed in from it */
    void *color;         /* ID3D12Resource*; required, with its valid extent */
    uint32_t color_width, color_height, color_state, flags;
    /* Read only when LMXXF_NR_FRAME_FLAG_STRENGTH is set; 1.0 is "unchanged". */
    float transfer_strength, color_strength, model_scale;
    /* Passes this frame runs (1..3, or up to 30 with the capacity allowance).
     * 0 means 1. A moved count rebuilds the network, so treat it as bring-up
     * state exactly as the lmxxf path treats its own count. */
    uint32_t passes;
    void *motion;        /* ID3D12Resource*; null = spatial frame */
    uint32_t motion_state, motion_width, motion_height;
    float motion_scale_x, motion_scale_y;
    /* 1 = this frame must not continue the previous one's history: first frame,
     * seek, cut, drop, source change, backend rebuild. */
    uint32_t reset;
    void *depth;         /* ID3D12Resource*; null = no depth guide */
    uint32_t depth_state, depth_width, depth_height;
    /* 1 = the depth texture stores reversed-Z. A video host that synthesises
     * depth (rather than a game's depth buffer) wants 0. */
    uint32_t depth_inverted;
} MochizukiNrFrameInfo;

typedef int32_t (*PFN_MochizukiNrGetApi)(uint32_t abi_version, LmxxfNrApi *out);
typedef int32_t (*PFN_MochizukiNrPrepareFrame)(void *context, const MochizukiNrFrameInfo *info,
                                               LmxxfNrJob *job);

/* The first frame pays the network build (measured: 70 s cold, 2.6 s once the
 * driver's pipeline cache is warm). While it runs, PrepareFrame answers
 * UNAVAILABLE and the host is expected to pass frames through. This is what a
 * host shows during that window: stage is a NUL-terminated, human-readable
 * phase ("Creating pipelines", "Finishing"), completed/total count the main
 * network's shaders, and active is 0 once the build has ended.
 *
 * 128 bytes; declared here because a host that waits on the build needs it and
 * nothing else from MochizukiNrControls.h. */
typedef struct MochizukiNrBuildProgress
{
    uint32_t struct_size, active, completed, total;
    uint64_t start_tick, update_tick;
    char stage[96];
} MochizukiNrBuildProgress;
typedef int32_t (*PFN_MochizukiNrGetBuildProgress)(void *context,
                                                   MochizukiNrBuildProgress *out);

#ifdef __cplusplus
}
#endif

#if defined(__cplusplus)
static_assert(sizeof(MochizukiNrFrameInfo) == 112u,
              "the runtime validates struct_size against its own sizeof(MochizukiNrFrameInfo) = 112");
/* The job object is shared with lmxxf on purpose: MochizukiNrPrepareFrame fills
 * the same 32-byte LmxxfNrJob that LmxxfNrApi::PrepareFrame fills, so
 * amd_lmxxf::MakeJob() and the session's stamping are reused unchanged. */
static_assert(sizeof(LmxxfNrJob) == 32u, "ABI 6 job is 32 bytes for both runtimes");
#endif
