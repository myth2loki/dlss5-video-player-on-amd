// Minimal stand-in for the PIX event runtime header (WinPixEventRuntime).
//
// FidelityFX's DX12 backend includes <pix3.h> under ENABLE_PIX_CAPTURES, a
// symbol it defines itself at the top of ffx_dx12.cpp, so the header is
// required to compile that backend at all. What the backend actually takes
// from it is the PIX_COLOR macro: the three event functions it calls are
// resolved with GetProcAddress against the PIX DLL at run time, so no PIX
// symbol is referenced at link time and a macro-only header is enough.
//
// Supplying this instead of the NuGet package keeps PIX instrumentation
// compiled in - the backend still looks for the DLL and no-ops when it is
// absent - without adding a dependency to the build. It is deliberately NOT on
// the normal include path: only the FidelityFX sources in CMakeLists see it, so
// a real pix3.h elsewhere in a build is never shadowed by this one.
#pragma once

#include <cstdint>

#ifndef PIX_COLOR
#define PIX_COLOR(r, g, b)                                                              \
    (static_cast<uint32_t>((r) & 0xFF) | (static_cast<uint32_t>((g) & 0xFF) << 8) |     \
     (static_cast<uint32_t>((b) & 0xFF) << 16))
#endif
