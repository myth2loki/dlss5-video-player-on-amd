#pragma once
// Saying a line in fewer words, for the one case where nothing else works.
//
// In a two-handed exchange the transcript splits a breath into three cues half a
// second apart, and half a second cannot hold a Chinese sentence. Tempo cannot
// close that gap (it is capped, and 1.5 is already fast), and letting the lines
// queue up does not either - measured on this film, the dense 13 s exchange
// holds 17 s of Chinese, so the queue arrived 4 s late and drifted the whole
// scene. The only move left is to say it shorter, which is the step
// step12_fit_auto.py asked the translator for.
//
// The engine is the translation model, started for this stage and stopped with
// it: loading a 4 GB gguf costs seconds, and a rewrite is wanted only for the
// lines that overflow their window - a handful per film.
//
// Deliberately not trusted: a rewrite that comes back longer, empty, or with the
// model's own commentary in it is discarded and the original line is spoken.
// Losing a second to a slightly hurried line is recoverable; losing the line is
// not.

#include "SubtitleTranslator.h"
#include "SubtitleTranslatePolicy.h"

#include <filesystem>
#include <string>

namespace dub {

class Rewriter {
public:
    // `enabled` false, or an engine that is not installed, leaves Ready() false
    // and every line overflowing to the old path (tempo, then a cut).
    Rewriter(subtitle_translate::Engine engine, bool enabled);
    ~Rewriter();
    Rewriter(const Rewriter&) = delete;
    Rewriter& operator=(const Rewriter&) = delete;

    bool Ready() const { return m_impl != nullptr; }

    // Chinese that should be speakable in `seconds`, or empty when the engine is
    // not running or answered nothing usable.
    std::string Shorten(const std::string& text, double seconds);

private:
    struct Impl;
    Impl* m_impl{};
};

}  // namespace dub
