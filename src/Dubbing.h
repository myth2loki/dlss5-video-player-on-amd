#pragma once
// The dub stage of a conversion: the translated lines spoken into a second
// audio track.
//
// The measuring stick is src/DubFitPolicy.h - the window a line gets, the order
// in which it gives ground, and the levels the mix is judged by are all that
// file's constants, measured by the Python pipeline this stage replaces. The
// prose behind them is docs/DUBBING-FIT-RULES-zh.md.
//
// The probe is the Python pipeline this stage replaces, end to end: the same
// stem separator under its own vocals, the same ECAPA embedding and 0.55 cosine
// clustering to decide who speaks, one reference clip per voice cut from the
// vocal stem, and the backing stem under the Chinese lines so the score
// survives. What is still not here: a rewrite round (a line the tempo cannot
// absorb is reported and cut by the mix, not re-asked of the translator).

#include "DubFitPolicy.h"
#include "SpeakerEmbedding.h"
#include "SubtitleTranslator.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace dub {

// One line the dub speaks. `start`/`end` are the source cue's own times, on the
// film's timeline; the window the line fits into is measured from `start` to the
// next line's `start`, never to this `end` (DubFitPolicy.h says why).
struct Line {
    double start{}, end{};
    std::string text;    // UTF-8, what the voice says
    std::string source;  // UTF-8, the same line in the source language
};

// The lines a dub needs, read out of the translated ASS the text stage wrote.
// That file is bilingual ({\rZh} the translation, {\rSrc} the source), and a
// line carries a dub only when it has a translation to speak: a cue the model
// left untranslated has nothing to say, and speaking its English at it is not
// a translation. Overlay tags ({\...}), hard breaks (\N) and non-breaking
// spaces (\h) are not words and come out, and so does a cue that is a stage
// direction - *Gunshot*, [door slams] - because reading one out says the word
// over the sound it describes. The subtitle keeps those; the voice skips them.
std::vector<Line> LinesFromAss(const std::string& ass);

struct Engine {
    std::filesystem::path runner;   // llama-tts.exe
    std::filesystem::path model;    // the backbone gguf
    std::filesystem::path mmproj;   // the projector gguf
};

struct Plan {
    Engine engine;
    std::filesystem::path helpers;    // the directory carrying ffmpeg.exe
    std::filesystem::path work;       // scratch for the per-line files; the caller sweeps it
    std::filesystem::path mediaPath;  // the source file, for the bed under the voice
    std::filesystem::path track;      // the finished dub track: 48 kHz stereo, 16-bit
    // Where the children's chatter goes, appended. Empty sends it to NUL. An
    // ffmpeg that refuses a fit puts its reason on stderr, and without this
    // file that reason is unreadable.
    std::filesystem::path logFile;
    std::vector<Line> lines;
    double durationSeconds{};         // the film's length; the track is exactly that long
    std::string language{"zh"};       // the code the synthesizer is asked for
    // A clip of the voice to speak in (--tts-speaker-file): Qwen3-TTS clones the
    // timbre from it. Empty picks one out of the film's own audio - the longest
    // cue's stretch, which is one person talking - because the engine's default
    // is a fresh sample per line at --temp 0.7: measured as a different timbre
    // on every line, and unrelated to the actors on screen. Both the Python
    // pipeline (10 s per speaker, refs_auto) and the engine's own reference
    // window say 10-15 s is the useful length.
    std::filesystem::path voiceReference;
    // Where voices are told apart. Empty - or a machine missing the DLL or the
    // model - falls back to the pitch track, which cannot separate two men and
    // reports every woman as whoever talks longest. The embedding is the answer
    // the Python pipeline calibrated: 0.55 cosine, greedy, cue order.
    speaker::Engine voiceEngine;
    // The stem separator and its model (dub/stems). With them the vocal stem is
    // what the embeddings and the reference clips are measured on, and the
    // backing stem is what the dub can carry under the Chinese voice. A
    // reference cut from the full mix inherits the music under it, which the
    // engine answers by repeating itself (measured: one line seven times).
    std::filesystem::path stemSeparator;
    std::filesystem::path stemModel;
    // The translation engine, when the caller has one: a line that overflows its
    // window is re-asked for in fewer words before a cut is accepted. The dense
    // part of an exchange has more Chinese than the cues hold, and neither tempo
    // nor queueing closes that - see TextRewriter.h.
    subtitle_translate::Engine rewriter;
    bool rewrite{true};
    // Lay the film's music and effects under the voice, at bed level. Only the
    // separated backing stem is ever used: the source's own mix contains its
    // English dialogue, so laying that under the dub plays the original a
    // second time and louder than the voice. Without a separator this is
    // skipped (and the log says so) rather than fallen back to.
    bool bed{false};
};

struct Progress {
    size_t done{}, total{};  // lines finished; the mix is the last one
};

struct Result {
    bool ok{}, cancelled{};
    std::wstring detail;
    size_t lines{}, compressed{}, cut{};  // given ground by tempo / reported and trimmed
    size_t rewritten{};                   // lines re-asked for in fewer words
};

Result RunDub(const Plan& plan, std::stop_token stop,
              const std::function<void(const Progress&)>& report);

}  // namespace dub
