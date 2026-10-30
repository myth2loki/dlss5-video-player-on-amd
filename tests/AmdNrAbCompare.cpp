// Renders one clip through both AMD neural runtimes and dumps every frame, so
// one can be measured against the other on byte-identical input.
//
// This is not a test: it asserts nothing and exits 0 as long as both runs
// produced their frames. What it exists for is the question "which runtime is
// better", which cannot be answered from a screenshot or from the two libraries'
// own logs - both render, both look plausible, and the difference is in the
// pixels. Feeding the same N frames through both and diffing the results is the
// only honest way to find out what changed, and it is also the only way to
// measure it: a run of one runtime tells you nothing about the other.
//
// The input is a raw BGRA sequence, not a video file, on purpose. Decoding is
// done once, outside, and both runtimes then read the exact same bytes - so no
// arm can win or lose on a decoder difference, and the guide estimator sees the
// same frames in both runs.
//
//   AmdNrAbCompare.exe <runtimeDir> <input.bgra> <width> <height> <fps>
//                      <frames> <outDir>
//
// Writes <outDir>/lmxxf/NNNNN.bgra and <outDir>/mochizuki/NNNNN.bgra.
// DLSS5_MZ_OPTIONS overrides the Mochizuki panel, in the same key=value line
// the worker protocol carries; unset means the runtime's own defaults.

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "AmdLmxxfEvaluator.h"
#include "MediaPipeline.h"
#include "NeuralRenderTypes.h"
#include "OfflineNeuralRenderer.h"

#ifndef DLSS5_AMD_RUNTIME_DIR
#define DLSS5_AMD_RUNTIME_DIR "amd-neural-runtime"
#endif

namespace {

int failures = 0;

void Line(std::string_view text) { std::cout << text << '\n'; }

void Step(std::string_view text) { std::cout << "\n== " << text << " ==\n"; }

std::string Narrow(const std::filesystem::path& path)
{
    const std::wstring wide = path.wstring();
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                            nullptr, 0, nullptr, nullptr);
    std::string text(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), text.data(), needed,
                          nullptr, nullptr);
    return text;
}

// One raw BGRA file, read in order. Every frame is the same size, and the file
// is the whole sequence - no seeking, no container, no decoder. A partial tail
// is an error rather than a short frame, because a run that silently rendered
// fewer frames would make the two arms incomparable.
class RawSource final : public IFrameSource {
public:
    RawSource(std::filesystem::path path, uint32_t width, uint32_t height, uint32_t frames, double fps)
        : path_(std::move(path)), frameBytes_(static_cast<size_t>(width) * height * 4u),
          frames_(frames), fps_(fps)
    {
    }

    bool Open(const std::filesystem::path&, std::stop_token, double seekSeconds) override
    {
        // A whole-source job opens once with no seek. A seek would need this
        // source to be indexable, which it is not, and no arm here seeks.
        if (seekSeconds != 0.0) return false;
        file_.close();
        file_.clear();
        file_.open(path_, std::ios::binary);
        index_ = 0;
        return file_.good();
    }

    void Close() override { file_.close(); }

    OfflineFrameRead Read(OfflineDecodedFrame& frame, std::stop_token stop) override
    {
        if (stop.stop_requested()) return OfflineFrameRead::Cancelled;
        if (index_ >= frames_) return OfflineFrameRead::EndOfStream;
        frame.bgra.resize(frameBytes_);
        file_.read(reinterpret_cast<char*>(frame.bgra.data()), static_cast<std::streamsize>(frameBytes_));
        if (file_.gcount() != static_cast<std::streamsize>(frameBytes_)) return OfflineFrameRead::Error;
        frame.frameNumber = index_;
        frame.timestamp100ns =
            static_cast<int64_t>(static_cast<double>(index_) * 10000000.0 / fps_);
        frame.discontinuity = false;
        frame.sourceGeneration = 1;
        frame.guides = nullptr;
        ++index_;
        return OfflineFrameRead::FrameReady;
    }

    uint32_t framesRead() const { return index_; }

private:
    std::filesystem::path path_;
    size_t frameBytes_{};
    uint32_t frames_{};
    double fps_{};
    std::ifstream file_;
    uint32_t index_{};
};

// Writes every frame the job captures. Unlike the smoke test's counting encoder
// this keeps them all, because the whole point of this run is the sequence: a
// single frame cannot show temporal stability, and a pair cannot be scored
// against a reference frame by frame.
class DumpEncoder final : public IFrameEncoder {
public:
    DumpEncoder(std::filesystem::path directory, uint32_t width, uint32_t height)
        : directory_(std::move(directory)), width_(width), height_(height)
    {
    }

    EncodeError Start(const EncoderSpec& spec, const std::filesystem::path&) override
    {
        // Only the 8-bit BGRA path is dumped: a P010 capture would need an
        // unpack before it could be compared with the truth frames, and that
        // unpack would be this tool's, not the runtime's.
        if (spec.pixelFormat != EncoderPixelFormat::Bgra) return EncodeError::StartFailed;
        if (spec.width != width_ || spec.height != height_) return EncodeError::StartFailed;
        std::error_code error;
        std::filesystem::create_directories(directory_, error);
        if (error) return EncodeError::StartFailed;
        active_ = true;
        return EncodeError::None;
    }

    EncodeError WriteFrame(std::span<const uint8_t> frame, std::stop_token stop) override
    {
        if (stop.stop_requested()) return EncodeError::Cancelled;
        if (!active_) return EncodeError::WriteFailed;
        if (frame.size() != static_cast<size_t>(width_) * height_ * 4u) return EncodeError::WriteFailed;
        char name[32]{};
        std::snprintf(name, sizeof(name), "%05llu.bgra", static_cast<unsigned long long>(written_));
        std::ofstream out(directory_ / name, std::ios::binary);
        if (!out.good()) return EncodeError::WriteFailed;
        out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
        if (!out.good()) return EncodeError::WriteFailed;
        ++written_;
        return EncodeError::None;
    }

    EncodeError Finish(std::stop_token stop) override
    {
        active_ = false;
        return stop.stop_requested() ? EncodeError::Cancelled : EncodeError::None;
    }

    void Cancel() override { active_ = false; }

    uint64_t written() const { return written_; }

private:
    std::filesystem::path directory_;
    uint32_t width_{};
    uint32_t height_{};
    uint64_t written_{};
    bool active_{};
};

std::optional<MochizukiOptions> MochizukiOptionsFromEnvironment()
{
    wchar_t buffer[512]{};
    const DWORD length = ::GetEnvironmentVariableW(L"DLSS5_MZ_OPTIONS", buffer, 512);
    if (length == 0 || length >= 512) return std::nullopt;
    std::string text;
    text.reserve(length);
    for (DWORD index = 0; index < length; ++index) text.push_back(static_cast<char>(buffer[index]));
    return ParseMochizukiOptions(text);
}

// The model's style (0 standard, 1 natural, 2 cinematic) is not one of the
// Mochizuki panel's values - it is the same DLSS5_STYLE the player's menu writes
// - so a sweep over it needs its own channel.
std::optional<uint32_t> StyleFromEnvironment()
{
    wchar_t buffer[16]{};
    const DWORD length = ::GetEnvironmentVariableW(L"DLSS5_MZ_STYLE", buffer, 16);
    if (length == 0 || length >= 16) return std::nullopt;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(buffer, &end, 10);
    if (end == buffer || value > 2) return std::nullopt;
    return static_cast<uint32_t>(value);
}

// How many times the network runs per frame. It is not one of the panel's
// values - it travels in the frame struct - and every extra pass is another
// full network run, so it is the one knob that trades time for picture
// directly.
std::optional<uint32_t> PassesFromEnvironment()
{
    wchar_t buffer[16]{};
    const DWORD length = ::GetEnvironmentVariableW(L"DLSS5_MZ_PASSES", buffer, 16);
    if (length == 0 || length >= 16) return std::nullopt;
    wchar_t* end = nullptr;
    const unsigned long value = std::wcstoul(buffer, &end, 10);
    if (end == buffer || value < 1 || value > 30) return std::nullopt;
    return static_cast<uint32_t>(value);
}

std::filesystem::path ResolveRuntimeDirectory(const std::filesystem::path& argument)
{
    std::vector<std::filesystem::path> candidates;
    if (!argument.empty()) candidates.push_back(argument);
    candidates.emplace_back(DLSS5_AMD_RUNTIME_DIR);
    std::error_code cwdError;
    const std::filesystem::path cwd = std::filesystem::current_path(cwdError);
    if (!cwdError) candidates.push_back(cwd / "amd-neural-runtime");
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code existsError;
        if (std::filesystem::exists(candidate / "LmxxfNrRuntime.dll", existsError)) return candidate;
    }
    return candidates.front();
}

// One arm: one runtime, one sequence, one directory of frames.
struct ArmResult {
    std::string name;
    bool ok{};
    uint64_t frames{};
    uint64_t neuralEvaluations{};
    uint64_t written{};
    double wallMs{};
    std::string detail;
    std::string runtimeStatus;
};

ArmResult RunArm(const std::string& name, amd_lmxxf::NrKind kind,
                 const std::filesystem::path& runtimeDirectory,
                 const std::filesystem::path& inputPath,
                 const std::filesystem::path& outputDirectory, uint32_t width, uint32_t height,
                 double fps, uint32_t frames)
{
    ArmResult result;
    result.name = name;
    result.frames = frames;

    AmdLmxxfEvaluatorConfig config;
    config.runtimeDirectory = runtimeDirectory;
    config.kind = kind;
    config.log = [](std::string_view text) { Line(std::string("    ") + std::string(text)); };
    if (const auto style = StyleFromEnvironment()) {
        config.style = *style;
        Line("  style: " + std::to_string(*style));
    }
    if (const auto passes = PassesFromEnvironment()) {
        config.modelPasses = *passes;
        Line("  passes: " + std::to_string(*passes));
    }
    if (const auto options = MochizukiOptionsFromEnvironment()) {
        config.mochizuki = *options;
        Line("  mochizuki options: " + FormatMochizukiOptions(*options));
    }
    AmdLmxxfEvaluator evaluator(config);

    RawSource source(inputPath, width, height, frames, fps);
    DumpEncoder encoder(outputDirectory, width, height);
    OfflineNeuralRenderer job(source, evaluator, encoder, [] { return std::string{}; });

    NeuralRenderRequest request;
    request.sourcePath = inputPath;
    request.stagingVideoPath = outputDirectory / L"unused.partial.mkv";
    request.width = width;
    request.height = height;
    request.fps = fps;
    request.durationSeconds = static_cast<double>(frames) / fps;
    request.jobId = 1;
    request.prerollFrames = 0;
    // The NVIDIA evidence gate, not a picture gate: it asks whether feature 18's
    // inline interception was armed, which is a hook this backend has no concept
    // of, and a job that sets it captures nothing at all here. What this tool
    // needs instead is that the runtime evaluated every frame, and that is
    // checked from nativeEvaluations below - a fallback would show as zero.
    request.requireNeural = false;

    const auto started = std::chrono::steady_clock::now();
    const NeuralRenderResult run = job.Run(request);
    result.wallMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    result.ok = run.ok && !run.cancelled;
    result.frames = run.frameCount;
    result.neuralEvaluations = run.nativeEvaluations;
    result.written = encoder.written();
    result.detail = Narrow(run.detail);    result.runtimeStatus = evaluator.LiveRuntimeStatus();
    return result;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 8) {
        Line("usage: AmdNrAbCompare <runtimeDir> <input.bgra> <width> <height> <fps> <frames> <outDir>");
        Line("                       [both|lmxxf|mochizuki]   default both");
        Line("  DLSS5_MZ_STYLE=0..2      the model's style (product default 1)");
        Line("  DLSS5_MZ_OPTIONS=k=v,... the mochizuki panel, all eleven keys");
        return 2;
    }
    const std::filesystem::path runtimeArgument = argv[1];
    const std::filesystem::path inputPath = argv[2];
    const uint32_t width = static_cast<uint32_t>(std::stoul(argv[3]));
    const uint32_t height = static_cast<uint32_t>(std::stoul(argv[4]));
    const double fps = std::stod(argv[5]);
    const uint32_t frames = static_cast<uint32_t>(std::stoul(argv[6]));
    const std::filesystem::path outputRoot = argv[7];

    const std::filesystem::path runtimeDirectory = ResolveRuntimeDirectory(runtimeArgument);
    Line("runtime directory: " + Narrow(runtimeDirectory));
    Line("input            : " + Narrow(inputPath) + "  " + std::to_string(width) + "x" +
         std::to_string(height) + "  " + std::to_string(frames) + " frames");

    std::vector<ArmResult> arms;
    // A sweep runs the same arm many times over and has no use for the other
    // one, so the caller may name which to run. "both" is the default because
    // the question this tool was built for is the comparison.
    const std::string which = argc > 8 && argv[8] != nullptr ? argv[8] : "both";
    if (which != "mochizuki") {
        Step("arm 1  lmxxf");
        arms.push_back(RunArm("lmxxf", amd_lmxxf::NrKind::Lmxxf, runtimeDirectory, inputPath,
                              outputRoot / L"lmxxf", width, height, fps, frames));
    }
    if (which != "lmxxf") {
        Step("arm 2  mochizuki");
        arms.push_back(RunArm("mochizuki", amd_lmxxf::NrKind::Mochizuki, runtimeDirectory, inputPath,
                              outputRoot / L"mochizuki", width, height, fps, frames));
    }

    Line("");
    Line("================ summary ================");
    for (const ArmResult& arm : arms) {
        Line(arm.name + ": ok=" + (arm.ok ? "yes" : "no") + " frames=" + std::to_string(arm.frames) +
             " neural=" + std::to_string(arm.neuralEvaluations) +
             " written=" + std::to_string(arm.written) + " wall=" +
             std::to_string(static_cast<int>(arm.wallMs)) + "ms");
        if (!arm.detail.empty()) Line("  detail: " + arm.detail);
        if (!arm.runtimeStatus.empty()) Line("  runtime: " + arm.runtimeStatus);
    }
    // A frame count alone would pass an arm that quietly fell back to a
    // pass-through: the encoder is handed a frame either way. Requiring one
    // native evaluation per frame is what says the pixels came from the model.
    for (const ArmResult& arm : arms) {
        if (!arm.ok) Line("FAIL: " + arm.name + " reported a failed run");
        else if (arm.written != frames) Line("FAIL: " + arm.name + " wrote " + std::to_string(arm.written) +
                                             " of " + std::to_string(frames) + " frames");
        else if (arm.neuralEvaluations != frames) Line("FAIL: " + arm.name + " evaluated " +
                                                       std::to_string(arm.neuralEvaluations) + " of " +
                                                       std::to_string(frames) + " frames natively");
        else continue;
        ++failures;
    }
    if (failures == 0) Line("both arms rendered every frame through the model");
    return failures == 0 ? 0 : 1;
}
