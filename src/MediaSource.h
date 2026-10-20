#pragma once

// Describes where media comes from and which decoder backend that permits.
// These live here rather than in UiLayout so the decode layer does not
// depend on the UI layer.

enum class MediaSourceKind {
    LocalFile,
    YouTube,
};

enum class DecoderOpenPolicy {
    FfmpegThenMediaFoundation,
    FfmpegOnly,
};

DecoderOpenPolicy DecoderPolicyForSource(MediaSourceKind sourceKind);

// What a source stream declares about its own colour: ffprobe's color_space,
// color_range, color_primaries and color_transfer, mapped onto the values this
// pipeline can act on. Every field has an explicit Unspecified because an
// undeclared stream is the case these types exist to name - the GPU NV12 source
// conversion needs a stated matrix and a stated range, and Unspecified is never
// read as "BT.709 limited", which is what it used to be assumed to be.
//
// Other means "declared, and not one of the listed values". It is kept apart
// from Unspecified so a refusal can say which of the two it saw: "this file
// says BT.2020" and "this file says nothing" are different diagnoses.
enum class ColorMatrix {
    Unspecified,
    Bt709,
    // ffprobe's bt470bg and smpte170m: the same coefficients, 625- and
    // 525-line, and the shader has one set for both.
    Bt601,
    // ffprobe's bt2020nc, the HDR10 and HLG matrix. Named because the decoder's
    // own tone map (HdrToneMap.h) implements exactly this one; the NV12 pass
    // does not, so it refuses it like Other.
    Bt2020Ncl,
    Other,
};

enum class ColorRange {
    Unspecified,
    Limited,  // ffprobe "tv": Y in 16..235, chroma in 16..240
    Full,     // ffprobe "pc": Y in 0..255, chroma centred on 128 over 0..255
};

// Bt2020, Pq (ffprobe "smpte2084") and Hlg ("arib-std-b67") are the three
// values that make a stream HDR; they are named so the decoder can tone map
// exactly those streams (HdrPolicy.h) instead of lumping them into Other.
enum class ColorPrimaries { Unspecified, Bt709, Bt470bg, Smpte170m, Other, Bt2020 };
enum class ColorTransfer { Unspecified, Bt709, Smpte170m, Other, Pq, Hlg };

struct SourceColorDescription {
    ColorMatrix matrix{ColorMatrix::Unspecified};
    ColorRange range{ColorRange::Unspecified};
    ColorPrimaries primaries{ColorPrimaries::Unspecified};
    ColorTransfer transfer{ColorTransfer::Unspecified};
};

// The conversions D3D12Renderer's NV12 source pass implements: one per (matrix,
// range) pair it has coefficients and a range mapping for. This enum IS the gate
// on the GPU source-conversion path - Unsupported means the decoder delivers BGRA
// and ffmpeg converts on the CPU instead, which costs pipe bandwidth and never
// costs correctness. There is deliberately no "nearest variant": a source that
// declares a matrix the pass cannot honour is refused, not rounded to BT.709.
//
// Primaries and transfer are carried in the description and named in the refusal
// log line, but they do not select a conversion. The pass produces R'G'B' from
// Y'CbCr, which is exactly a matrix and a range mapping; neither it nor the CPU
// conversion it falls back to re-maps primaries or re-applies a transfer, so
// gating on those two would refuse sources the fallback handles no better.
enum class SourceNv12Conversion {
    Unsupported,
    Bt709Limited,
    Bt709Full,
    Bt601Limited,
    Bt601Full,
};

inline SourceNv12Conversion SourceNv12ConversionFor(const SourceColorDescription& color)
{
    const bool full = color.range == ColorRange::Full;
    if (!full && color.range != ColorRange::Limited) return SourceNv12Conversion::Unsupported;
    switch (color.matrix) {
        case ColorMatrix::Bt709:
            return full ? SourceNv12Conversion::Bt709Full : SourceNv12Conversion::Bt709Limited;
        case ColorMatrix::Bt601:
            return full ? SourceNv12Conversion::Bt601Full : SourceNv12Conversion::Bt601Limited;
        case ColorMatrix::Unspecified:
        case ColorMatrix::Bt2020Ncl:
        case ColorMatrix::Other:
            break;
    }
    return SourceNv12Conversion::Unsupported;
}

// The conversions' numbers as the tokens each backend pastes into its shader
// text, one set per conversion SourceNv12Conversion names. They are strings
// because a float cannot be turned back into a token, and tokens are what keeps
// the BT.709 limited-range arm compiling to the byte-identical program every
// cached render on disk was made with.
//
// Luma is (Y*255 - lumaOffset) / lumaScale and chroma is (UV*255 - 128) /
// chromaScale; the four coefficients are the standard inverse matrix, R from V,
// G from U and V, B from U.
//
// It lives beside the enum rather than in one backend because both backends
// implement exactly this list: the NVIDIA renderer compiles PSSourceNv12 from
// it and the AMD evaluator compiles its own copy of the same algebra, and a
// source that plays on one has to decode identically on the other.
struct SourceNv12Constants {
    const char* lumaOffset;
    const char* lumaScale;
    const char* chromaScale;
    const char* redV;
    const char* greenU;
    const char* greenV;
    const char* blueU;
};

// Null for Unsupported, which is the whole gate: there is no nearest-variant
// branch to fall into.
//
// BT.709 limited range is the program every cached render on disk was made
// with, so its seven tokens are frozen: 2(1-Kr) and 2(1-Kb) for Kr=0.2126,
// Kb=0.0722, the two green terms, and the 8-bit studio-swing scalings. BT.601 is
// the same algebra at Kr=0.299, Kb=0.114. Full range drops the 16 offset and
// spans the whole byte in both planes.
inline const SourceNv12Constants* SourceNv12ConstantsFor(SourceNv12Conversion conversion)
{
    static constexpr SourceNv12Constants kBt709Limited{
        "16.0", "219.0", "224.0", "1.5748", "0.187324", "0.468124", "1.8556"};
    static constexpr SourceNv12Constants kBt709Full{
        "0.0", "255.0", "255.0", "1.5748", "0.187324", "0.468124", "1.8556"};
    static constexpr SourceNv12Constants kBt601Limited{
        "16.0", "219.0", "224.0", "1.402", "0.344136", "0.714136", "1.772"};
    static constexpr SourceNv12Constants kBt601Full{
        "0.0", "255.0", "255.0", "1.402", "0.344136", "0.714136", "1.772"};
    switch (conversion) {
        case SourceNv12Conversion::Bt709Limited: return &kBt709Limited;
        case SourceNv12Conversion::Bt709Full: return &kBt709Full;
        case SourceNv12Conversion::Bt601Limited: return &kBt601Limited;
        case SourceNv12Conversion::Bt601Full: return &kBt601Full;
        case SourceNv12Conversion::Unsupported: break;
    }
    return nullptr;
}
