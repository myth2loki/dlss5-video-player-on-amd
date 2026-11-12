#pragma once
// Saying a line in different words, for the two cases where nothing else works.
//
// Too long: in a two-handed exchange the transcript splits a breath into three
// cues half a second apart, and half a second cannot hold a Chinese sentence.
// Tempo cannot close that gap (it is capped, and 1.5 is already fast), and
// letting the lines queue up does not either - measured on this film, the dense
// 13 s exchange holds 17 s of Chinese, so the queue arrived 4 s late and drifted
// the whole scene. The only move left is to say it shorter, which is the step
// step12_fit_auto.py asked the translator for.
//
// Too short: the same exchange the other way round. Chinese is terse, and a line
// that finishes in 11 s of a 15 s slot leaves the actor's mouth moving over
// music - heard as "两句之间没接上". Tempo gives back at most 15%, so the words
// have to grow. Both directions are the same call with a different prompt, and
// both keep the subtitle out of it: the track has already been written when this
// stage runs, so a rewrite changes what is heard and never what is read.
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

#include <cstddef>
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

    // Chinese that says the same thing in about `characters` characters, for a
    // line whose slot would otherwise end in silence under a moving mouth. The
    // mirror of Shorten, and the same contract: empty means "keep the original".
    //
    // Why this is a rewrite and not a slower tempo: tempo can only give 15%
    // (kMinTempo), and a line that says its piece in 60% of the slot needs 40%.
    // Measured on the Python chain, filling the shortfall with words took one
    // 240 s interview from 82.5% of its running time spoken to 93.6%.
    std::string Lengthen(const std::string& text, std::size_t characters);

private:
    struct Impl;
    Impl* m_impl{};
};

}  // namespace dub
