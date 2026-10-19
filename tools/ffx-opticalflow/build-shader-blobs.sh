#!/usr/bin/env bash
# Compile the FidelityFX Optical Flow HLSL into the permutation headers that
# ffx_opticalflow_shaderblobs.cpp includes.
#
# The SDK ships the shaders but not the compiler: FidelityFX_SC.exe was dropped
# from the repository after SDK 1.1.2, and the batch file that drives it points
# at a tools/ffx_sc path that no longer exists. So the compiler is recovered
# from the SDK's own git history first (see README in third_party/ffx-compat for
# the exact commands and for why the FSR2-era one must not be used).
#
#   FFX_SDK_ROOT=/path/to/FidelityFX-SDK
#   FFX_SC=/path/to/FidelityFX_SC.exe          # recovered from git history
#   bash tools/ffx-opticalflow/build-shader-blobs.sh
#   python tools/ffx-opticalflow/patch-permutation-headers.py
set -u

FFX_SDK_ROOT="${FFX_SDK_ROOT:?set FFX_SDK_ROOT to a FidelityFX SDK checkout}"
SC="${FFX_SC:-$FFX_SDK_ROOT/Kits/FidelityFX/framegeneration/tools/ffx_sc/bin/FidelityFX_SC.exe}"

SDK="$FFX_SDK_ROOT/Kits/FidelityFX"
OUT="${FFX_SC_OUT:-$SDK/framegeneration/fsr3/dx12/ffx_sc_output2}"
SHADERS="$SDK/framegeneration/fsr3/internal/shaders"

if [ ! -x "$SC" ]; then
    echo "no shader compiler at $SC"
    echo "recover one from the SDK's history: git show c16b1d2:sdk/tools/binary_store/FidelityFX_SC.exe"
    exit 1
fi
mkdir -p "$OUT"

PASSES=(
  ffx_opticalflow_prepare_luma_pass
  ffx_opticalflow_compute_luminance_pyramid_pass
  ffx_opticalflow_generate_scd_histogram_pass
  ffx_opticalflow_compute_scd_divergence_pass
  ffx_opticalflow_compute_optical_flow_advanced_pass_v5
  ffx_opticalflow_filter_optical_flow_pass_v5
  ffx_opticalflow_scale_optical_flow_advanced_pass_v5
)

BASE_ARGS=(-Zs -reflection -deps=gcc -DFFX_GPU=1 -DFFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL=0
  -embed-arguments -E CS -Wno-for-redefinition -Wno-ambig-lit-shift -DFFX_HLSL=1
  -DFFX_OPTICALFLOW_EMBED_ROOTSIG=0 '-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}'
  -I "$SDK/api/internal/gpu" -I "$SDK/framegeneration/fsr3/include/gpu")

W32=(-DFFX_HALF=0 -DFFX_HLSL_SM=62 -T cs_6_2)
W64=(-DFFX_HALF=0 '-DFFX_PREFER_WAVE64=[WaveSize(64)]' -DFFX_HLSL_SM=66 -T cs_6_6)
B16=(-DFFX_HALF=1 -enable-16bit-types)

missing=0
for p in "${PASSES[@]}"; do
  for suffix in "" _wave64 _16bit _wave64_16bit; do
    case "$suffix" in
      "")        EXTRA=("${W32[@]}") ;;
      _wave64)   EXTRA=("${W64[@]}") ;;
      _16bit)    EXTRA=("${B16[@]}" "${W32[@]}") ;;
      *)         EXTRA=("${B16[@]}" "${W64[@]}") ;;
    esac
    "$SC" "${BASE_ARGS[@]}" "-name=${p}${suffix}" "${EXTRA[@]}" "-output=$OUT" \
        "$SHADERS/$p.hlsl" >/dev/null 2>&1
    if [ ! -f "$OUT/${p}${suffix}_permutations.h" ]; then
      echo "MISSING ${p}${suffix}"
      missing=$((missing+1))
    fi
  done
done
echo "missing: $missing"
echo "permutation headers: $(ls "$OUT"/*_permutations.h 2>/dev/null | wc -l) (expect 28)"
