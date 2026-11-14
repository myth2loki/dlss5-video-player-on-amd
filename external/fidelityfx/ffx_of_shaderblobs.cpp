// Shader-blob provider for the FidelityFX Optical Flow core.
//
// Why this file exists instead of the SDK's own ffx_opticalflow_shaderblobs.cpp:
// that file was written against the previous blob layout and reads entryName,
// constantBufferNames, numSRVTextures and so on. The headers this SDK drop ships
// under dx12/ffx_sc_output are produced by the current fx_sc tool and name the
// same fields cbvResourceNames, numSRVResources, ... so the SDK file does not
// compile against its own blobs. Nothing here invents a blob or changes what a
// pass binds: it reads the tables the SDK generated and repacks them into the
// FfxShaderBlob the DX12 backend consumes.
//
// Every field below maps one-to-one. The one that does not exist in the new
// layout is entryName, and the DX12 backend never reads it - the blob is already
// compiled, so the entry point is inside it - which is why a fixed "main" is
// enough here.
//
// Licence: FidelityFX SDK sources are MIT (see the header of
// internal/ffx_opticalflow.cpp); the signed dlls in Kits/FidelityFX/signedbin
// are the ones under the binary-only terms. This file is original glue over the
// MIT headers and carries the same notice obligation as the rest of the SDK.

#include "ffx_opticalflow_shaderblobs.h"
#include "ffx_opticalflow_private.h"

#include <ffx_opticalflow_prepare_luma_pass_permutations.h>
#include <ffx_opticalflow_prepare_luma_pass_wave64_permutations.h>
#include <ffx_opticalflow_prepare_luma_pass_16bit_permutations.h>
#include <ffx_opticalflow_prepare_luma_pass_wave64_16bit_permutations.h>

#include <ffx_opticalflow_compute_luminance_pyramid_pass_permutations.h>
#include <ffx_opticalflow_compute_luminance_pyramid_pass_wave64_permutations.h>
#include <ffx_opticalflow_compute_luminance_pyramid_pass_16bit_permutations.h>
#include <ffx_opticalflow_compute_luminance_pyramid_pass_wave64_16bit_permutations.h>

#include <ffx_opticalflow_generate_scd_histogram_pass_permutations.h>
#include <ffx_opticalflow_generate_scd_histogram_pass_wave64_permutations.h>
#include <ffx_opticalflow_generate_scd_histogram_pass_16bit_permutations.h>
#include <ffx_opticalflow_generate_scd_histogram_pass_wave64_16bit_permutations.h>

#include <ffx_opticalflow_compute_scd_divergence_pass_permutations.h>
#include <ffx_opticalflow_compute_scd_divergence_pass_wave64_permutations.h>
#include <ffx_opticalflow_compute_scd_divergence_pass_16bit_permutations.h>
#include <ffx_opticalflow_compute_scd_divergence_pass_wave64_16bit_permutations.h>

#include <ffx_opticalflow_compute_optical_flow_advanced_pass_v5_permutations.h>
#include <ffx_opticalflow_compute_optical_flow_advanced_pass_v5_wave64_permutations.h>
#include <ffx_opticalflow_compute_optical_flow_advanced_pass_v5_16bit_permutations.h>
#include <ffx_opticalflow_compute_optical_flow_advanced_pass_v5_wave64_16bit_permutations.h>

#include <ffx_opticalflow_filter_optical_flow_pass_v5_permutations.h>
#include <ffx_opticalflow_filter_optical_flow_pass_v5_wave64_permutations.h>
#include <ffx_opticalflow_filter_optical_flow_pass_v5_16bit_permutations.h>
#include <ffx_opticalflow_filter_optical_flow_pass_v5_wave64_16bit_permutations.h>

#include <ffx_opticalflow_scale_optical_flow_advanced_pass_v5_permutations.h>
#include <ffx_opticalflow_scale_optical_flow_advanced_pass_v5_wave64_permutations.h>
#include <ffx_opticalflow_scale_optical_flow_advanced_pass_v5_16bit_permutations.h>
#include <ffx_opticalflow_scale_optical_flow_advanced_pass_v5_wave64_16bit_permutations.h>

namespace {

// Repack one generated table entry into the blob the backend reads. The buffer
// and acceleration-structure groups have no counterpart in the new layout - the
// fx_sc output calls every binding a resource - and Optical Flow binds textures
// only, so zero counts are the truthful value rather than a placeholder.
template <typename Info>
FfxShaderBlob MakeBlob(const Info& info)
{
    FfxShaderBlob blob{};
    blob.data = info.blobData;
    blob.size = info.blobSize;
    blob.entryName = "main";

    blob.cbvCount = info.numCBVResources;
    blob.srvTextureCount = info.numSRVResources;
    blob.uavTextureCount = info.numUAVResources;
    blob.srvBufferCount = 0;
    blob.uavBufferCount = 0;
    blob.samplerCount = info.numSamplerResources;
    blob.rtAccelStructCount = info.numRTAccelerationStructureResources;

    blob.boundConstantBufferNames = info.cbvResourceNames;
    blob.boundConstantBuffers = info.cbvResourceBindings;
    blob.boundConstantBufferCounts = info.cbvResourceCounts;
    blob.boundConstantBufferSpaces = info.cbvResourceSpaces;

    blob.boundSRVTextureNames = info.srvResourceNames;
    blob.boundSRVTextures = info.srvResourceBindings;
    blob.boundSRVTextureCounts = info.srvResourceCounts;
    blob.boundSRVTextureSpaces = info.srvResourceSpaces;

    blob.boundUAVTextureNames = info.uavResourceNames;
    blob.boundUAVTextures = info.uavResourceBindings;
    blob.boundUAVTextureCounts = info.uavResourceCounts;
    blob.boundUAVTextureSpaces = info.uavResourceSpaces;

    blob.boundSRVBufferNames = nullptr;
    blob.boundSRVBuffers = nullptr;
    blob.boundSRVBufferCounts = nullptr;
    blob.boundSRVBufferSpaces = nullptr;

    blob.boundUAVBufferNames = nullptr;
    blob.boundUAVBuffers = nullptr;
    blob.boundUAVBufferCounts = nullptr;
    blob.boundUAVBufferSpaces = nullptr;

    blob.boundSamplerNames = info.samplerResourceNames;
    blob.boundSamplers = info.samplerResourceBindings;
    blob.boundSamplerCounts = info.samplerResourceCounts;
    blob.boundSamplerSpaces = info.samplerResourceSpaces;

    blob.boundRTAccelerationStructureNames = info.rtAccelerationStructureResourceNames;
    blob.boundRTAccelerationStructures = info.rtAccelerationStructureResourceBindings;
    blob.boundRTAccelerationStructureCounts = info.rtAccelerationStructureResourceCounts;
    blob.boundRTAccelerationStructureSpaces = info.rtAccelerationStructureResourceSpaces;

    return blob;
}

// Each generated table is indexed by the permutation key, which carries one bit
// in this release: whether the input is HDR. Wave width and fp16 are separate
// tables, not additional key bits, which is why they pick the table instead of
// indexing it.
#define OF_BLOB_RESOLVER(NAME)                                                                     \
    FfxShaderBlob Resolve_##NAME(bool wave64, bool fp16, uint32_t hdr)                             \
    {                                                                                              \
        if (wave64 && fp16)                                                                         \
            return MakeBlob(g_ffx_opticalflow_##NAME##_wave64_16bit_PermutationInfo[               \
                g_ffx_opticalflow_##NAME##_wave64_16bit_IndirectionTable[hdr]]);                    \
        if (wave64)                                                                                 \
            return MakeBlob(g_ffx_opticalflow_##NAME##_wave64_PermutationInfo[                     \
                g_ffx_opticalflow_##NAME##_wave64_IndirectionTable[hdr]]);                          \
        if (fp16)                                                                                   \
            return MakeBlob(g_ffx_opticalflow_##NAME##_16bit_PermutationInfo[                      \
                g_ffx_opticalflow_##NAME##_16bit_IndirectionTable[hdr]]);                           \
        return MakeBlob(g_ffx_opticalflow_##NAME##_PermutationInfo[                                \
            g_ffx_opticalflow_##NAME##_IndirectionTable[hdr]]);                                    \
    }

OF_BLOB_RESOLVER(prepare_luma_pass)
OF_BLOB_RESOLVER(compute_luminance_pyramid_pass)
OF_BLOB_RESOLVER(generate_scd_histogram_pass)
OF_BLOB_RESOLVER(compute_scd_divergence_pass)
OF_BLOB_RESOLVER(compute_optical_flow_advanced_pass_v5)
OF_BLOB_RESOLVER(filter_optical_flow_pass_v5)
OF_BLOB_RESOLVER(scale_optical_flow_advanced_pass_v5)

#undef OF_BLOB_RESOLVER

} // namespace

extern "C" FfxErrorCode opticalflowGetPermutationBlobByIndex(
    FfxOpticalflowPass passId, uint32_t permutationOptions, FfxShaderBlob* outBlob)
{
    if (!outBlob) return FFX_ERROR_INVALID_POINTER;

    const bool wave64 = (permutationOptions & OPTICALFLOW_SHADER_PERMUTATION_FORCE_WAVE64) != 0;
    const bool fp16 = (permutationOptions & OPTICALFLOW_SHADER_PERMUTATION_ALLOW_FP16) != 0;
    const uint32_t hdr = (permutationOptions & OPTICALFLOW_HDR_COLOR_INPUT) ? 1u : 0u;

    switch (passId)
    {
    case FFX_OPTICALFLOW_PASS_PREPARE_LUMA:
        *outBlob = Resolve_prepare_luma_pass(wave64, fp16, hdr);
        return FFX_OK;
    case FFX_OPTICALFLOW_PASS_GENERATE_OPTICAL_FLOW_INPUT_PYRAMID:
        *outBlob = Resolve_compute_luminance_pyramid_pass(wave64, fp16, hdr);
        return FFX_OK;
    case FFX_OPTICALFLOW_PASS_GENERATE_SCD_HISTOGRAM:
        *outBlob = Resolve_generate_scd_histogram_pass(wave64, fp16, hdr);
        return FFX_OK;
    case FFX_OPTICALFLOW_PASS_COMPUTE_SCD_DIVERGENCE:
        *outBlob = Resolve_compute_scd_divergence_pass(wave64, fp16, hdr);
        return FFX_OK;
    case FFX_OPTICALFLOW_PASS_COMPUTE_OPTICAL_FLOW_ADVANCED_V5:
        *outBlob = Resolve_compute_optical_flow_advanced_pass_v5(wave64, fp16, hdr);
        return FFX_OK;
    case FFX_OPTICALFLOW_PASS_FILTER_OPTICAL_FLOW_V5:
        *outBlob = Resolve_filter_optical_flow_pass_v5(wave64, fp16, hdr);
        return FFX_OK;
    case FFX_OPTICALFLOW_PASS_SCALE_OPTICAL_FLOW_ADVANCED_V5:
        *outBlob = Resolve_scale_optical_flow_advanced_pass_v5(wave64, fp16, hdr);
        return FFX_OK;
    default:
        return FFX_ERROR_INVALID_ARGUMENT;
    }
}

extern "C" FfxErrorCode opticalflowIsWave64(uint32_t permutationOptions, bool& isWave64)
{
    isWave64 = (permutationOptions & OPTICALFLOW_SHADER_PERMUTATION_FORCE_WAVE64) != 0;
    return FFX_OK;
}
