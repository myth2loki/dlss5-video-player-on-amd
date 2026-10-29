#pragma once
// Who is speaking, measured rather than guessed.
//
// The Python pipeline settled this with an ECAPA-TDNN embedding per line and a
// greedy cosine clustering at 0.55 (tools/dubbing/step1_align.py), on audio it
// had already separated into a vocal stem. The numbers below are that file's,
// because a different embedding model would silently move both thresholds; the
// graph is the same one, exported once to ONNX by tools/export_ecapa_onnx.py.
//
// ONNX Runtime is loaded from disk instead of linked. That keeps the player's
// link line unchanged and, more to the point, keeps a machine without the DLL
// or the model working: Ready() is false, the caller falls back to the pitch
// track it had before, and the log says which file was missing.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace speaker {

// Where the two files live. Both under the dub root, beside the synthesizer
// they exist to serve.
struct Engine {
    std::filesystem::path library;  // onnxruntime.dll
    std::filesystem::path model;    // ecapa_voxceleb.onnx
    bool Complete() const { return !library.empty() && !model.empty(); }
};

// The clip length below which there is no timbre to measure (spk.py's
// MIN_SECONDS). Shorter lines are clustered by their neighbours instead.
inline constexpr double kMinimumSeconds = 0.5;
inline constexpr int kSampleRate = 16000;

// The width of the embedding this graph produces. Only used to size buffers
// and to sanity-check what came back.
inline constexpr size_t kDimensions = 192;

class Embedder {
public:
    explicit Embedder(Engine engine);
    ~Embedder();
    Embedder(const Embedder&) = delete;
    Embedder& operator=(const Embedder&) = delete;

    bool Ready() const { return m_impl != nullptr; }
    // Why it is not ready, for the log: spelled as the file that was missing,
    // or the reason the graph refused to load.
    const std::string& Notice() const { return m_notice; }

    // 16 kHz mono in [-1, 1] -> a unit-length vector, or empty when the clip is
    // too short or the graph refused it.
    std::vector<float> Embed(const std::vector<float>& samples);

private:
    struct Impl;
    Impl* m_impl{};
    std::string m_notice;
};

// Cosine similarity of two unit-length vectors: +1 same voice, 0 unrelated.
// Zero when either is empty, so a line with no measurement never joins a
// cluster.
double Similarity(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace speaker
