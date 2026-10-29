// A headless drive of the Convert video pipeline's text and dub stages.
//
// The Convert video dialog is the only front end for these stages, and a
// dialog cannot be run from a script or a test. This console tool starts the
// same functions the dialog's worker starts - RunConvertVideoText,
// dub::RunDub and RunStageExport - with no picture stage, so the source's
// picture and its own streams are copied as they stand and the finished file
// carries the translated subtitle track and the dubbed second audio track.
// One pipeline, one set of refusals, whichever way a conversion is started.
//
// Not a ctest: a run is minutes of engine work and needs the three engines
// (asr/, translation/, dub/) installed beside the player. Run it manually:
//
//   DubConvertE2E.exe <source media> <output.mkv> [subtitle target tag]
//                     [--text-only <ass out>] [--from-ass <ass in>]
//
// The subtitle target tag is what LanguagesFor resolves ("zh-Hans" by
// default); the dub speaks Chinese either way, as the dialog does.
//
// --text-only stops after the text stage and writes the translated ASS to the
// named file; --from-ass starts there and skips it. A whole film is more than
// one sitting of engine work on some machines, and the two halves are the
// same halves the dialog's worker runs in sequence - the file in between is
// the one the mux carries.

#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "ConvertTextStage.h"
#include "Dubbing.h"
#include "StageExport.h"
#include "SubtitleTranslatePolicy.h"
#include "ToolCapture.h"
#include "Utf8Text.h"

namespace {

namespace fs = std::filesystem;

fs::path ExecutableDirectory()
{
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    return length ? fs::path(path).parent_path() : fs::current_path();
}

// The player's rule: beside the executable, or one level up, where run-fg/
// sits next to the repository root.
fs::path HelperRoot(const fs::path& base, const wchar_t* name)
{
    std::error_code error;
    if (!fs::exists(base / name, error) && fs::exists(base.parent_path() / name, error))
        return base.parent_path() / name;
    return base / name;
}

fs::path FirstFileOf(const fs::path& directory, const wchar_t* extension,
                     const wchar_t* prefixExcluded = nullptr)
{
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (it->path().extension() != extension) continue;
        if (prefixExcluded && it->path().filename().wstring().rfind(prefixExcluded, 0) == 0) continue;
        return it->path();
    }
    return {};
}

// Four small questions ffprobe answers in a line each. The decoder would
// answer them too, and a conversion the dialog runs asks it instead; here one
// child process per number keeps this file free of the decoder's device work.
std::optional<std::string> Probe(const fs::path& helpers, const fs::path& media,
                                 const wchar_t* entries)
{
    // ffprobe has no -nostdin; it never reads stdin in the first place.
    return RunToolCapture(helpers / L"ffprobe.exe",
        {L"-hide_banner", L"-loglevel", L"error",
         L"-show_entries", entries, L"-of", L"default=noprint_wrappers=1:nokey=1",
         media.wstring()},
        std::stop_token {}, std::chrono::seconds(30), 4096);
}

std::optional<double> ProbeSeconds(const fs::path& helpers, const fs::path& media)
{
    const auto answer = Probe(helpers, media, L"format=duration");
    if (!answer || answer->empty()) return std::nullopt;
    try { return std::stod(*answer); } catch (...) { return std::nullopt; }
}

std::optional<uint32_t> ProbeDimension(const fs::path& helpers, const fs::path& media,
                                       const wchar_t* entry)
{
    const auto answer = Probe(helpers, media, entry);
    if (!answer || answer->empty()) return std::nullopt;
    try { return static_cast<uint32_t>(std::stoul(*answer)); } catch (...) { return std::nullopt; }
}

std::optional<double> ProbeFps(const fs::path& helpers, const fs::path& media)
{
    const auto answer = Probe(helpers, media, L"stream=r_frame_rate");
    if (!answer || answer->empty()) return std::nullopt;
    const size_t slash = answer->find('/');
    try {
        if (slash == std::string::npos) return std::stod(*answer);
        const double numerator = std::stod(answer->substr(0, slash));
        const double denominator = std::stod(answer->substr(slash + 1));
        return denominator > 0.0 ? numerator / denominator : std::optional<double>{};
    } catch (...) { return std::nullopt; }
}

void PrintProgress(const wchar_t* key, uint64_t done, uint64_t total)
{
    std::wcout << L"  [" << key << L"] " << done << L" / " << total << std::endl;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) {
        std::wcerr << L"Usage: DubConvertE2E <source media> <output.mkv> [subtitle target tag]"
                      L" [--text-only <ass out>] [--from-ass <ass in>] [--burn]"
                   << std::endl;
        return 2;
    }
    const fs::path source = argv[1];
    const fs::path destination = argv[2];
    std::string targetTag = "zh-Hans";
    bool burn = false;
    fs::path textOnly, fromAss;
    for (int i = 3; i < argc; ++i) {
        const std::wstring argument = argv[i];
        const auto value = [&]() -> std::optional<fs::path> {
            return i + 1 < argc ? std::optional<fs::path>(argv[++i]) : std::nullopt;
        };
        if (argument == L"--text-only") {
            const auto given = value();
            if (!given) return 2;
            textOnly = *given;
        } else if (argument == L"--from-ass") {
            const auto given = value();
            if (!given) return 2;
            fromAss = *given;
        } else if (argument == L"--burn") {
            // The text is drawn into every frame at the mux rather than carried
            // as a stream: the dialog's third choice, which re-encodes.
            burn = true;
        } else if (!argument.empty() && argument[0] == L'-') {
            std::wcerr << L"Unknown option: " << argument << std::endl;
            return 2;
        } else {
            targetTag = utf8_text::FromWide(argument);
        }
    }

    const fs::path helpers = ExecutableDirectory();
    std::wcout << L"Helpers: " << helpers.wstring() << std::endl;
    const subtitle_translate::Languages languages = subtitle_translate::LanguagesFor("", targetTag);

    // ---- Engines -----------------------------------------------------------
    const fs::path speechRoot = HelperRoot(helpers, L"asr");
    fs::path speechRuntime = speechRoot / L"runtime";
    if (!fs::is_directory(speechRuntime)) speechRuntime = speechRoot / L"runtime-gpu";
    subtitle_speech::Engine speech;
    speech.runtimeDirectory = speechRuntime;
    speech.model = FirstFileOf(speechRoot / L"models", L".bin");
    speech.threads = 8;
    const fs::path translationRoot = HelperRoot(helpers, L"translation");
    subtitle_translate::Engine translation;
    translation.runtimeDirectory = translationRoot / L"runtime";
    translation.model = FirstFileOf(translationRoot / L"models", L".gguf");
    translation.gpuLayers = 99;
    const fs::path dubRoot = HelperRoot(helpers, L"dub");
    dub::Plan dubPlan;
    dubPlan.engine.runner = dubRoot / L"runtime" / L"llama-tts.exe";
    dubPlan.engine.model = FirstFileOf(dubRoot / L"models", L".gguf", L"mmproj");
    dubPlan.engine.mmproj = FirstFileOf(dubRoot / L"models", L".gguf");
    // The projector's name starts with mmproj-; the backbone is everything else.
    if (dubPlan.engine.mmproj == dubPlan.engine.model) dubPlan.engine.mmproj.clear();

    const auto refuse = [](const wchar_t* what) {
        std::wcerr << L"Refused: " << what << std::endl;
        return 3;
    };
    // Only the dub's four files are needed either way: a resumed run skips the
    // text stage, and with it both of its engines.
    if (!fs::exists(dubPlan.engine.runner)) return refuse(L"llama-tts.exe is missing (dub/runtime).");
    if (dubPlan.engine.model.empty() || !fs::exists(dubPlan.engine.model)) return refuse(L"the dub backbone model is missing (dub/models).");
    if (dubPlan.engine.mmproj.empty() || !fs::exists(dubPlan.engine.mmproj)) return refuse(L"the dub projector model is missing (dub/models).");

    // ---- What the mux needs to know about the source -----------------------
    const auto duration = ProbeSeconds(helpers, source);
    const auto width = ProbeDimension(helpers, source, L"stream=width");
    const auto height = ProbeDimension(helpers, source, L"stream=height");
    const auto fps = ProbeFps(helpers, source);
    if (!duration || !width || !height || !fps) return refuse(L"the source could not be probed.");
    std::wcout << L"Source: " << source.wstring() << L"  " << *width << L"x" << *height
               << L"  " << *fps << L" fps  " << *duration << L" s" << std::endl;

    std::error_code error;
    const fs::path scratch = destination.parent_path() / (destination.stem().wstring() + L".e2e");
    fs::create_directories(scratch, error);
    if (error) return refuse(L"the scratch directory could not be created.");
    const fs::path cacheDirectory = helpers / L"subtitles";
    fs::create_directories(cacheDirectory, error);

    // ---- The text stage ----------------------------------------------------
    // A source with text of its own is translated from it; this one has none,
    // so the film is listened to first - the dialog's transcribe path.
    std::filesystem::path textFile = fromAss;
    if (textFile.empty()) {
        if (!subtitle_speech::SpeechToText::Available(speech)) return refuse(L"the speech engine is not installed (asr/).");
        if (!subtitle_translate::Translator::Available(translation)) return refuse(L"the translation engine is not installed (translation/).");
        ConvertTextPlan text;
        text.needed = true;
        text.transcribe = true;
        text.mediaPath = source.wstring();
        text.helpers = helpers;
        text.cacheDirectory = cacheDirectory;
        text.speech = speech;
        text.translation = translation;
        text.speechJob.path = source.wstring();
        text.speechJob.audioStream = 0;
        text.speechJob.duration = *duration;
        text.speechJob.plan.startSeconds = 0.0;
        text.identity = L"e2e|" + source.wstring() + L"|speech";
        text.languages = languages;
        text.width = *width;
        text.height = *height;
        std::wcout << L"Text stage: transcribe, then translate to "
                   << utf8_text::ToWide(text.languages.targetCode) << std::endl;
        const ConvertTextResult produced = RunConvertVideoText(text, std::stop_token {}, PrintProgress);
        if (!produced.ok) {
            std::wcerr << L"Text stage failed: " << produced.detail << std::endl;
            return produced.cancelled ? 5 : 4;
        }
        textFile = produced.file;
        std::wcout << L"Text stage wrote: " << textFile.wstring() << std::endl;
        if (!textOnly.empty()) {
            std::error_code copyError;
            fs::copy_file(textFile, textOnly, fs::copy_options::overwrite_existing, copyError);
            if (copyError) return refuse(L"the translated ASS could not be copied to --text-only.");
            std::wcout << L"Stopped after the text stage: " << textOnly.wstring() << std::endl;
            return 0;
        }
    } else {
        std::wcout << L"Text stage: skipped, using " << textFile.wstring() << std::endl;
    }

    // ---- The dub -----------------------------------------------------------
    // The lines the text stage wrote are the lines the dub speaks, as in the
    // dialog's worker: the file the container carries and the voice cannot
    // tell different stories.
    std::string ass;
    {
        std::ifstream file(textFile, std::ios::binary);
        ass.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    dubPlan.helpers = helpers;
    dubPlan.work = scratch / L"dub";
    dubPlan.logFile = dubPlan.work / L"dub.log";
    dubPlan.mediaPath = source.wstring();
    dubPlan.track = dubPlan.work / L"dub-track.wav";
    dubPlan.durationSeconds = *duration;
    dubPlan.language = "zh";
    // Who says what, and in what voice: the embedding decides, the separator
    // supplies the clean stem to clone from and the backing to lay under the
    // Chinese. Both are optional here for the same reason they are in the
    // dialog - a machine without them still gets a dub, just a plainer one.
    dubPlan.voiceEngine.library = dubRoot / L"runtime" / L"onnxruntime.dll";
    dubPlan.voiceEngine.model = dubRoot / L"models" / L"ecapa_voxceleb.onnx";
    if (!fs::exists(dubPlan.voiceEngine.library) || !fs::exists(dubPlan.voiceEngine.model))
        dubPlan.voiceEngine = speaker::Engine{};
    dubPlan.stemSeparator = dubRoot / L"stems" / L"stems-split.exe";
    dubPlan.stemModel = FirstFileOf(dubRoot / L"stems" / L"models", L".gguf", {});
    if (!fs::exists(dubPlan.stemSeparator) || dubPlan.stemModel.empty()) {
        dubPlan.stemSeparator.clear();
        dubPlan.stemModel.clear();
    }
    // The bed is the separated backing stem, so the track keeps the film's score
    // under the Chinese voice. With no separator there is nothing to lay down.
    dubPlan.bed = !dubPlan.stemSeparator.empty();
    // The rewrite engine: the one the text stage used, started again for the
    // lines that do not fit. Without it they take the old path and get cut.
    dubPlan.rewriter = translation;
    dubPlan.rewrite = subtitle_translate::Translator::Available(translation);
    dubPlan.lines = dub::LinesFromAss(ass);
    if (dubPlan.lines.empty()) {
        std::wcerr << L"Refused: the translation has no lines a voice can speak." << std::endl;
        return 3;
    }
    std::wcout << L"Dub stage: " << dubPlan.lines.size() << L" lines" << std::endl;
    {
        const dub::Result dubbed = dub::RunDub(dubPlan, std::stop_token {}, [](const dub::Progress& p) {
            std::wcout << L"  [convert.progress.dub] " << p.done << L" / " << p.total << std::endl;
        });
        if (!dubbed.ok) {
            std::wcerr << L"Dub stage failed: " << dubbed.detail << std::endl;
            return dubbed.cancelled ? 5 : 4;
        }
        std::wcout << L"Dub stage: " << dubbed.lines << L" spoken, " << dubbed.compressed
                   << L" given ground by tempo, " << dubbed.cut << L" cut by the mix" << std::endl;
    }

    // ---- The mux -----------------------------------------------------------
    // No picture stage: the plan reads "nothing selected", which the dialog
    // also accepts for a conversion of the sound and the text alone - the
    // picture is copied as it stands.
    StageExportJob job;
    job.source = source;
    job.destination = destination;
    job.scratch = scratch;
    job.helpers = helpers;
    job.sourceWidth = *width;
    job.sourceHeight = *height;
    job.fps = *fps;
    job.duration = *duration;
    job.subtitleFile = textFile;
    job.subtitleLanguage = utf8_text::ToWide(subtitle_translate::MuxLanguageCode(languages.targetCode));
    job.burnSubtitles = burn;
    job.dubTrack = dubPlan.track;
    job.dubLanguage = job.subtitleLanguage;
    std::wcout << L"Mux: writing " << destination.wstring()
               << (burn ? L" (text burned into the picture)" : L" (text carried as a track)")
               << std::endl;
    const StageExportOutcome outcome = RunStageExport(job, std::stop_token {}, [](const StageExportUpdate& u) {
        if (u.passKey)
            std::wcout << L"  [" << u.passKey << L"] " << u.completedFrames << L" / " << u.totalFrames << std::endl;
    });
    if (outcome.status != StageExportStatus::Done) {
        std::wcerr << L"Mux failed: " << outcome.detail << std::endl;
        return outcome.status == StageExportStatus::Cancelled ? 5
             : outcome.status == StageExportStatus::Refused ? 3 : 4;
    }
    std::wcout << L"Done: " << destination.wstring() << std::endl;
    return 0;
}
