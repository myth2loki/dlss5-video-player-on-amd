// Minimal stand-in for the WinPixEventRuntime header.
//
// FidelityFX's DX12 backend includes <pix3.h> only to reach the PIX_COLOR
// packing macro: every PIX entry point it uses is resolved at run time with
// LoadLibrary/GetProcAddress, and the build never links WinPixEventRuntime.lib.
// The stub keeps that include resolvable on a machine without the PIX runtime
// installed. Defining USE_PIX on the command line makes the backend skip the
// include instead; the symbol still has to exist either way, because the colour
// constant is compiled unconditionally.
#pragma once

#ifndef PIX_COLOR
#define PIX_COLOR(r, g, b) \
    ((UINT64)(((UINT32)(r)) | (((UINT32)(g)) << 8) | (((UINT32)(b)) << 16)))
#endif
