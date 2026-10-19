# FidelityFX compatibility bits

Two small things the build needs only when `FFX_SDK_ROOT` points at a FidelityFX
SDK **and** the optical flow shader blobs have been generated. Without both, the
player builds exactly as it always did and `OpticalFlowFfx` compiles to a class
that is never available.

## Why this directory exists

Optical flow is not like the other FidelityFX effects this player uses. FSR
upscaling and frame generation are `ffx_api` effects with an effect id and a
signed DLL (`amd_fidelityfx_upscaler_dx12.dll` and friends), so they are loaded
at run time and need nothing from the SDK at build time. Optical flow has
neither:

* there is no `FFX_API_EFFECT_ID_OPTICALFLOW`, and no optical flow DLL in the
  SDK's `signedbin` - only the five DLLs above;
* the driver's own `amd_fidelityfx_dx12.dll` exports just the five generic
  `ffx_api` entry points, so the effect cannot be reached through it either.

So it exists only as SDK source, and that source does not compile on its own:
`ffx_opticalflow_shaderblobs.cpp` includes a `*_permutations.h` per shader pass
that holds the compiled DXIL, and those headers are generated, not shipped.

## Generating the blobs

The generator is `FidelityFX_SC.exe`. **The SDK repository does not contain it** -
it was dropped after SDK 1.1.2 - and the batch file that drives it
(`Kits/FidelityFX/framegeneration/fsr3/dx12/BuildOpticalFlowShaders.bat`) points
at a `tools/ffx_sc` path that no longer exists. Two things follow from that, and
both are easy to get wrong:

1. Recover the compiler from the SDK's own git history, where it is still there:

   ```sh
   cd "$FFX_SDK_ROOT"
   git show c16b1d2:sdk/tools/binary_store/FidelityFX_SC.exe > FidelityFX_SC.exe
   git show c16b1d2:sdk/tools/binary_store/dxcompiler.dll   > dxcompiler.dll
   git show c16b1d2:sdk/tools/binary_store/dxil.dll         > dxil.dll
   ```

   (`c16b1d2` is SDK 1.1.2. Do **not** take the one in
   `GPUOpen-Effects/FidelityFX-FSR2` at `tools/sc`: it is from 2022 and emits
   `numSRVResources`, where SDK 2.3's `POPULATE_SHADER_BLOB_FFX` reads
   `numSRVTextures` / `numSRVBuffers`. The blob header then does not compile,
   and the error looks like a missing struct member rather than a wrong
   compiler.)

2. Patch the headers it produces before compiling. SDK 2.3 reads an `entryName`
   field that no compiler in the repository's history emits; it arrived with the
   version AMD stopped shipping. Every other field 2.3 reads - including
   `numRTAccelerationStructures` - is already there.

`tools/ffx-opticalflow/` has both steps, in order:

```sh
export FFX_SDK_ROOT=/f/dlss5-video/FidelityFX-SDK
bash tools/ffx-opticalflow/build-shader-blobs.sh     # 28 headers into ffx_sc_output2
python tools/ffx-opticalflow/patch-permutation-headers.py
```

Then reconfigure:

```sh
cmake -B build-ninja -S . -DFFX_SDK_ROOT="$FFX_SDK_ROOT"
```

Configure-time output says which way it went:
`FidelityFX Optical Flow: enabled` or `disabled`.

## pix3.h

A macro-only stand-in for the PIX event runtime header. FidelityFX's DX12 backend
defines `ENABLE_PIX_CAPTURES` itself and then includes `<pix3.h>`, so the header
is required to compile it; what the backend takes from it is `PIX_COLOR`, because
the three event functions it calls are resolved with `GetProcAddress` at run time
and are never referenced at link time. Supplying this instead of the NuGet
package keeps PIX instrumentation compiled in - the backend still looks for the
DLL and no-ops without it - without adding a dependency.

It is only on the include path of the FidelityFX sources, so a real `pix3.h`
anywhere in a build is never shadowed by it.

## What is deliberately not here

No FidelityFX source. The licence permits binary redistribution, not source
redistribution, which is why the rest of this repository declares the ABI it uses
(`FsrFrameGenAbi.h`, `FsrUpscaleAbi.h`) instead of vendoring headers, and why the
optical flow sources here are compiled from `FFX_SDK_ROOT` at build time rather
than copied in.
