# Third-Party Notices

Two kinds of FidelityFX code are involved, and their terms differ. The effects
loaded at run time come from AMD's signed binaries under the SDK's binary-only
licence. The Optical Flow technique used as a motion guide on cards with no flow
engine is compiled from AMD's MIT-licensed sources and linked into the player, so
that one is a build-time dependency - optional, and absent from a build that
points `FFX_SDK` at nothing.

## AMD FidelityFX Optical Flow (optional, linked at build time)

- Component: `ffx_opticalflow.cpp`, `ffx_dx12.cpp`, `ffx_backends_dx12.cpp`,
  `ffx_message.cpp` and `ffx_object_management.cpp` from the SDK's
  `Kits/FidelityFX`, plus this repository's own shader-blob glue in
  `external/fidelityfx/ffx_of_shaderblobs.cpp`.
- Source: <https://github.com/GPUOpen-Libraries-AndSDKs/FidelityFX-SDK>
  (`Kits/FidelityFX/api`, `backend/dx12`, `framegeneration/fsr3`).
- Copyright (C) Advanced Micro Devices, Inc.
- Licence: MIT, as stated in the header of every one of those source files. It
  permits use, modification and redistribution provided the copyright notice and
  permission notice travel with the copies, which this section and the file
  headers satisfy. The binary-only terms in the SDK's `docs/license.md` govern
  the signed dlls in `Kits/FidelityFX/signedbin`, not these sources.
- The SDK is not vendored: it is a multi-hundred-megabyte tree of samples and
  tools around five translation units, so `CMakeLists.txt` takes its path as the
  `FFX_SDK` cache variable and compiles without it (the class then reports itself
  unavailable and the CPU block matcher remains the motion source).
- `external/fidelityfx/pix3.h` is a stand-in for the WinPixEventRuntime header
  the SDK's DX12 backend includes. It defines the one macro that backend uses;
  nothing links against PIX.
- Used for: the temporal motion guide on a machine with no flow engine
  (`src/OpticalFlowFfx.cpp`). It produces a motion field and a scene-change flag
  and nothing else - frame generation and upscaling stay with the two effects
  below.

## AMD FidelityFX (optional, loaded at run time)

- Component: `amd_fidelityfx_framegeneration_dx12.dll` and
  `amd_fidelityfx_upscaler_dx12.dll`, and the `amd_fidelityfx_loader_dx12.dll`
  that loads either effect beside itself.
- Source: <https://github.com/GPUOpen-Libraries-AndSDKs/FidelityFX-SDK>
  (`Kits/FidelityFX/signedbin`).
- Copyright (C) Advanced Micro Devices, Inc.
- Licence: the FidelityFX SDK licence (`docs/license.md` in that repository).
  It grants redistribution **in binary form only**, on three conditions this
  project satisfies:
  1. No reverse engineering, decompilation or disassembly of the binary. This
     project reaches it only through its published C API
     (`ffxCreateContext` / `ffxDispatch` / `ffxDestroyContext`) and its published
     descriptor structures; nothing inspects or modifies the binary itself. The
     descriptor layouts are transcribed in `src/FsrFrameGenAbi.h` from the
     public headers, and the version constant is checked by the runtime, so a
     version mismatch surfaces as an error rather than as a patched call.
  2. Redistributions must reproduce the copyright notice, the permission notice
     and the disclaimers - that is what this file is.
  3. The software is provided "AS IS", with all warranties disclaimed.

  The licence's source-redistribution term does not apply here: no FidelityFX
  source is vendored into this repository, which is why the ABI is declared
  locally rather than included from the SDK.

- Used for: two effects, each optional on its own. AMD FidelityFX Frame
  Generation, the AMD GPU frame-generation backend
  (`src/FsrFrameGenBackend.cpp`); absent, the player falls back to its own
  optical-flow backend (`src/AmdFrameGenBackend.cpp`), which needs no runtime.
  And FSR 4, the top tier of the playback upscaler
  (`src/FsrUpscaleBackend.cpp`); absent, the upscale falls to FSR 1's EASU and
  then to Catmull-Rom with CAS, both the player's own shaders
  (`src/AmdUpscaleShader.h`), so upscaling and every other feature work without
  the runtime either way.

- FSR, FidelityFX and AMD are trademarks of Advanced Micro Devices, Inc.

## What this is not

The `MPCVR-FSR-FrameGen` project (a DirectShow video renderer with FSR Frame
Generation support) pointed at this same DLL while this player was being written,
and it is GPL v3. That licence covers **its** video-renderer code, not the DLL,
which is AMD's and separately licensed. No code was taken from that project; the
only thing reused was the observation that the DLL can be loaded by name and
driven through the FFX C API.

## NVIDIA components (not redistributed)

`nvngx_dlss.dll` and `nvngx_dlssg.dll` are produced by this repository's build
from the NVIDIA DLSS SDK, which is not redistributed here. They are required only
by the NVIDIA NGX paths (DLSS-G frame generation and DLSS super-resolution);
neither is loaded on an AMD adapter.