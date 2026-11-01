#pragma once

// The Metal counterpart of what D3D12Renderer does when neural rendering is
// off: upload one BGRA frame to a texture and draw it letterboxed into the
// drawable. Nothing else.
//
// This is deliberately the small half. D3D12Renderer.cpp is 5875 lines, and
// only about 8% of it names neural/FSR/DLSS - but the other 92% is not all
// presentation either: it carries the guide estimator, the compare views, the
// live session's frame pair wiring and the capture path. What a "play the
// video" port needs is the upload-and-present tail of it, which is what this
// class is.
//
// Metal.h cannot be named from this header: it pulls in Foundation, which is
// Objective-C and does not compile in a C++ translation unit. So the Metal
// objects live behind an opaque implementation, and the only thing this header
// exposes that is not plain C++ is the CAMetalLayer the caller already owns,
// passed as a void* and cast on the other side.

#include <cstdint>
#include <memory>
#include <string>

class MetalVideoPresenter {
public:
    struct InitResult {
        bool ok = false;
        std::string error;
    };

    MetalVideoPresenter();
    ~MetalVideoPresenter();

    MetalVideoPresenter(const MetalVideoPresenter&) = delete;
    MetalVideoPresenter& operator=(const MetalVideoPresenter&) = delete;

    // `layer` is a CAMetalLayer*, which only the Objective-C++ side may name.
    InitResult Init(void* layer);

    // Rebuilds the source texture for a new size. Cheap; it does not touch the
    // device.
    void SetSourceSize(int width, int height);

    // Uploads `bgra` (tightly packed, top-down, width*height*4 bytes) and
    // presents it letterboxed. `fill` crops instead of fitting, which is the
    // Windows build's A key.
    void Present(const uint8_t* bgra, int width, int height,
                 double drawableWidth, double drawableHeight, bool fill);

    void Shutdown();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
