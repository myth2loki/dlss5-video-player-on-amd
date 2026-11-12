#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Translation of a subtitle track into Chinese, and the ASS file that shows the
// result.
//
// The decisions live here, free of processes, HTTP and GPUs, so they can be
// tested: parsing the cues out of an SRT, wording the prompt the translation
// model was trained on, cleaning what it answers, and laying both languages out
// as one ASS file. SubtitleOverlay already draws a single ASS file through
// libass, so a translated track needs no new rendering path at all - which is
// why this is a file format problem rather than a compositor problem.
//
// Why one file rather than two overlay layers: SubtitleOverlay holds exactly one
// source, one child, one frame queue and one generation. Two simultaneous
// sources would mean two children, two queues, two pixel boxes and a doubled
// Settled(); the whole of that machinery already works, and none of it needs to
// change to show two languages.
namespace subtitle_translate {

// ---- Cues -------------------------------------------------------------------

// One subtitle event. `text` and `translation` are UTF-8 with '\n' between
// lines; `translation` stays empty until the model has answered for this cue,
// which is also what makes a half-finished translation resumable.
struct Cue {
    double start = 0.0;
    double end = 0.0;
    std::string text;
    std::string translation;

    bool Translated() const { return !translation.empty(); }
};

namespace detail {

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

inline std::string_view Trim(std::string_view text)
{
    while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
    return text;
}

// "<digits>" with an optional leading sign; false on anything else, so a
// malformed timestamp falls back instead of reading as zero.
inline bool ParseUnsigned(std::string_view text, int64_t& value)
{
    if (text.empty() || text.size() > 9) return false;
    value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
    }
    return true;
}

} // namespace detail

// "HH:MM:SS,mmm" (SRT) or "HH:MM:SS.mmm" (VTT); seconds may carry one or two
// digits. Anything unreadable is worth zero rather than a rejected cue: a
// subtitle with one broken line still has a hundred good ones.
inline double ParseTimestamp(std::string_view text)
{
    text = detail::Trim(text);
    int64_t hours = 0, minutes = 0, seconds = 0, fraction = 0;
    const size_t first = text.find(':');
    if (first == std::string_view::npos) return 0.0;
    const size_t second = text.find(':', first + 1);
    if (second == std::string_view::npos) return 0.0;
    if (!detail::ParseUnsigned(text.substr(0, first), hours)) return 0.0;
    if (!detail::ParseUnsigned(text.substr(first + 1, second - first - 1), minutes)) return 0.0;
    std::string_view tail = text.substr(second + 1);
    const size_t dot = tail.find_first_of(".,");
    if (dot == std::string_view::npos) {
        if (!detail::ParseUnsigned(tail, seconds)) return 0.0;
    } else {
        if (!detail::ParseUnsigned(tail.substr(0, dot), seconds)) return 0.0;
        // "5" means 500 ms, so the fraction is right-padded. A fixed buffer, not
        // a std::string: assigning a temporary back into a string_view would
        // leave it pointing at freed storage.
        std::string_view digits = tail.substr(dot + 1);
        char fractionText[4] = {'0', '0', '0', '\0'};
        for (size_t index = 0; index < 3 && index < digits.size(); ++index)
            fractionText[index] = digits[index];
        if (!detail::ParseUnsigned(std::string_view(fractionText, 3), fraction)) return 0.0;
    }
    return double(hours * 3600 + minutes * 60 + seconds) + double(fraction) / 1000.0;
}

// The cues of an SRT file, in file order. BOM, CRLF, numbering and per-cue
// settings after the end timestamp (VTT's X1/Y1) are all tolerated. A block
// without a timing line is skipped; a cue with no text is skipped, because
// libass would draw an empty event and the translator would be sent a blank
// line.
inline std::vector<Cue> ParseSrt(std::string_view text)
{
    std::string owned(text);
    if (owned.size() >= 3 && static_cast<unsigned char>(owned[0]) == 0xEF &&
        static_cast<unsigned char>(owned[1]) == 0xBB && static_cast<unsigned char>(owned[2]) == 0xBF)
        owned.erase(0, 3);
    // One normalisation, so block splitting only has to know about '\n'.
    owned.erase(std::remove(owned.begin(), owned.end(), '\r'), owned.end());

    std::vector<Cue> cues;
    size_t position = 0;
    while (position < owned.size()) {
        size_t end = owned.find("\n\n", position);
        if (end == std::string::npos) end = owned.size();
        const std::string_view block(owned.data() + position, end - position);
        position = end + 2;

        const size_t arrow = block.find("-->");
        if (arrow == std::string_view::npos) continue;
        const size_t lineStart = block.rfind('\n', arrow);
        const size_t start = lineStart == std::string_view::npos ? 0 : lineStart + 1;
        const size_t lineEnd = block.find('\n', arrow);
        const std::string_view timing = block.substr(
            start, (lineEnd == std::string_view::npos ? block.size() : lineEnd) - start);
        const size_t localArrow = timing.find("-->");
        if (localArrow == std::string_view::npos) continue;

        Cue cue;
        cue.start = ParseTimestamp(timing.substr(0, localArrow));
        // Trim first: SRT writes a space right after "-->" and VTT adds its
        // settings after another one, so searching for a space before trimming
        // would find the arrow's own separator and read the end time as empty.
        std::string_view rest = detail::Trim(timing.substr(localArrow + 3));
        const size_t space = rest.find(' ');
        if (space != std::string_view::npos) rest = rest.substr(0, space);
        cue.end = ParseTimestamp(rest);
        if (cue.end < cue.start) cue.end = cue.start;

        if (lineEnd != std::string_view::npos) {
            for (size_t at = lineEnd + 1; at < block.size();) {
                size_t next = block.find('\n', at);
                if (next == std::string_view::npos) next = block.size();
                const std::string_view line = detail::Trim(block.substr(at, next - at));
                if (!line.empty()) {
                    if (!cue.text.empty()) cue.text += '\n';
                    cue.text.append(line);
                }
                at = next + 1;
            }
        }
        if (cue.text.empty()) continue;
        cues.push_back(std::move(cue));
    }
    return cues;
}

// Translations saved by an interrupted run, put back into the cues they belong
// to. Both lists are in file order, so one pass matches them; a cue is matched
// by its start time within `tolerance` seconds, which is well under the 10 ms a
// re-serialised SRT can drift. Returns how many cues gained a translation.
//
// Text is not compared as well: the saved file was written from these same
// cues, and a retimed subtitle ("same line, 100 ms later") is exactly the case
// worth recovering rather than throwing away.
inline size_t ApplySavedTranslations(std::vector<Cue>& cues, const std::vector<Cue>& saved,
                                     double tolerance = 0.02)
{
    size_t restored = 0;
    size_t at = 0;
    for (Cue& cue : cues) {
        if (!cue.translation.empty()) continue;
        while (at < saved.size() && saved[at].start < cue.start - tolerance) ++at;
        if (at >= saved.size()) break;
        if (std::abs(saved[at].start - cue.start) > tolerance) continue;
        if (saved[at].text.empty()) continue;
        cue.translation = saved[at].text;
        ++restored;
    }
    return restored;
}

// ---- One cue per sentence ---------------------------------------------------
//
// Whisper cuts a sentence wherever the speaker paused, and the text stage writes
// one cue per cut. Measured on a three-minute interview: 109 cues over 163 s, a
// median of 1.43 s and eight characters, 34 of them four characters or less.
// Nobody reads that, and nothing can translate it. "Until last year, this figure
// in Britain was the highest it had ever been" / "from now on" / "temperature" /
// "3 degrees" is one sentence the ASR took apart, and every piece was then
// translated on its own - which is how "a professor of hydrology at the
// University of Reading" lost its last three words to the next cue and came back
// as "阅读大学", the university of reading (books).
//
// So the cues are joined back into sentences here, once, before either the
// translation or the voice sees them. The two used to disagree: the dub stage
// merged what it read, the subtitle track kept the fragments, and the viewer got
// a voice that spoke whole sentences under subtitles that flashed three words at
// a time - seen and heard as the two drifting apart.
//
// The rule is the one the dub stage had already measured: a cue that stops
// mid-sentence and is followed within `gapSeconds` by the cue that goes on is one
// line, capped at `maxSeconds` so a monologue does not become a single cue
// nobody can read. Both numbers are the dub's own (DubFitPolicy.h), because the
// two paths must agree: every merged cue ends where a sentence ends, and then the
// dub has nothing left to merge, so one cue is exactly one spoken line.
struct SentenceRules {
    // Only ever applied to a cue that stops mid-sentence: one that ends with a
    // full stop never takes the next one, whatever the gap. So this is "how long
    // a pause in the middle of a sentence may be", and 0.6 is what the excerpt
    // measured - at 0.45 "climate" / "change." stayed two cues 0.48 s apart and
    // the viewer got 气候 on screen on its own, then 改变。 three seconds later.
    // The alternative was worse than the long cues it buys: a longer gap merges
    // nothing more (0.8 measured the same) and a shorter one leaves fragments.
    double gapSeconds = 0.6;
    double maxSeconds = 16.0;   // dub_fit::kUtteranceMaxSeconds
    // A cue nobody can finish reading is not a subtitle. A line that lands in
    // 0.2 s keeps the screen at least this long - but never into the next cue,
    // which would show two lines at once.
    double minSeconds = 1.2;
};

// Trailing punctuation, spaces and closing marks are not part of the words, so
// "Celia," and "Celia" are the same word as far as a sentence end goes.
inline std::string_view TrimmedTail(std::string_view text)
{
    const auto endsWith = [](std::string_view piece, std::string_view suffix) {
        return piece.size() >= suffix.size() &&
               piece.substr(piece.size() - suffix.size()) == suffix;
    };
    static constexpr std::string_view kMarks[] = {" ", "\t", ".", "…", "。", "！", "？", "!", "?",
                                                  "\"", "'", "”", "’", "）", ")"};
    for (int guard = 0; guard < 8; ++guard) {
        bool trimmed = false;
        for (const auto mark : kMarks) {
            if (endsWith(text, mark)) {
                text.remove_suffix(mark.size());
                trimmed = true;
                break;
            }
        }
        if (!trimmed) break;
    }
    return text;
}

// Whether this is the end of a sentence rather than a pause in the middle of
// one. Only the closing punctuation counts: a comma or a dash is the speaker
// breathing, and joining across those is the whole point.
inline bool EndsSentence(std::string_view text)
{
    const auto endsWith = [](std::string_view piece, std::string_view suffix) {
        return piece.size() >= suffix.size() &&
               piece.substr(piece.size() - suffix.size()) == suffix;
    };
    static constexpr std::string_view kClosers[] = {"\"", "'", ")", "]", "”", "’", "）"};
    static constexpr std::string_view kTerminals[] = {".", "!", "?", "。", "！", "？", "…"};
    for (int guard = 0; guard < 3; ++guard) {
        bool trimmed = false;
        for (const auto closer : kClosers) {
            if (endsWith(text, closer)) {
                text.remove_suffix(closer.size());
                trimmed = true;
                break;
            }
        }
        if (!trimmed) break;
        while (!text.empty() && text.back() == ' ') text.remove_suffix(1);
    }
    for (const auto terminal : kTerminals)
        if (endsWith(text, terminal)) return true;
    return false;
}

namespace detail {

// The next fragment, appended the way that language writes two halves of one
// sentence: a space between Latin words ("Celia," + "we have" -> "Celia, we
// have"), nothing between CJK ones, where a space would be a typo. A fragment
// that opens with its own punctuation takes no space either.
inline void AppendFragment(std::string& into, const std::string& fragment)
{
    if (fragment.empty()) return;
    bool space = !into.empty();
    if (space) {
        const unsigned char last = static_cast<unsigned char>(into.back());
        const unsigned char first = static_cast<unsigned char>(fragment.front());
        if (last >= 0x80 || first >= 0x80) {
            space = false;  // CJK on either side: words run together
        } else if (into.back() == ' ' || into.back() == '\t' || into.back() == '\n') {
            space = false;
        } else if (fragment.front() == ',' || fragment.front() == '.' || fragment.front() == '!' ||
                   fragment.front() == '?' || fragment.front() == ';' || fragment.front() == ':' ||
                   fragment.front() == '\'' || fragment.front() == '"' || fragment.front() == '%') {
            space = false;  // "50" + "%" is not "50 %"
        }
    }
    if (space) into += ' ';
    into += fragment;
}

} // namespace detail

// The cues of a track, one per sentence. Order is preserved; a cue that carries
// no translation yet keeps none, and the merge never invents text: it only
// concatenates what the track already said, and the timeline stays the union of
// the cues that went into it.
inline std::vector<Cue> MergeIntoSentences(std::vector<Cue> cues,
                                           const SentenceRules& rules = {})
{
    // A cue whose words open the next one is the same speech written twice:
    // whisper emits a truncated segment and then the full one over it, and both
    // were being spoken. The longer reading keeps the earlier start.
    for (size_t index = 0; index + 1 < cues.size();) {
        const std::string_view first = TrimmedTail(cues[index].text);
        const std::string_view second = TrimmedTail(cues[index + 1].text);
        const bool opens = !first.empty() && second.size() > first.size() &&
                           second.substr(0, first.size()) == first;
        if (opens && cues[index + 1].start - cues[index].end <= 0.5) {
            cues[index + 1].start = cues[index].start;
            cues.erase(cues.begin() + std::ptrdiff_t(index));
            continue;
        }
        ++index;
    }

    std::vector<Cue> merged;
    merged.reserve(cues.size());
    for (Cue& cue : cues) {
        if (!merged.empty()) {
            Cue& last = merged.back();
            const double gap = cue.start - last.end;
            const double span = cue.end - last.start;
            // Only a cue that is still mid-sentence and close enough to be the
            // same breath continues the line; a sound the ASR wrote on its own
            // ("(laughs)") has no words to join and stays separate by its own
            // punctuation, which is the caller's business, not this one's.
            const bool continues = gap <= rules.gapSeconds && gap > -0.5 &&
                                   span <= rules.maxSeconds && !EndsSentence(last.text);
            if (continues) {
                last.end = std::max(last.end, cue.end);
                detail::AppendFragment(last.text, cue.text);
                if (!cue.translation.empty()) {
                    if (last.translation.empty()) last.translation = cue.translation;
                    else detail::AppendFragment(last.translation, cue.translation);
                }
                continue;
            }
        }
        merged.push_back(std::move(cue));
    }
    // Nothing on screen flashes: a line too short to read is held for at least
    // `minSeconds`, or until the next cue arrives, whichever comes first. Only
    // ever lengthened - a cue that already covers its window keeps it.
    for (size_t index = 0; index < merged.size(); ++index) {
        const double wanted = merged[index].start + rules.minSeconds;
        if (merged[index].end >= wanted) continue;
        const double beforeNext = index + 1 < merged.size()
                                      ? merged[index + 1].start - 0.04
                                      : wanted;
        merged[index].end = std::max(merged[index].end, std::min(wanted, beforeNext));
    }
    return merged;
}

// ---- Cutting a long line for the screen --------------------------------------
//
// A sentence can be more than a screen holds: fifteen seconds and eighty
// characters is three rows of Chinese with the source under them, and what the
// viewer reads is a paragraph, not a subtitle. The voice cannot be cut - it is
// one utterance in one window, and splitting it would mean two synthetic
// readings with a seam between them - so the cut is made on the display side
// only: two dialogue lines carrying the two halves of the same sentence back to
// back.
//
// The halves stay one line to whoever speaks them. They meet with no gap, the
// first does not close the sentence (the cut is after a clause mark, never after
// a full stop), and the text stage's own merge - which the dub runs on exactly
// these cues - joins them again, so what is spoken is the sentence that was
// translated rather than two fragments spoken apart. That is the whole contract:
// **split for the eye, never for the ear.**

struct DisplayRules {
    // Six seconds is the usual ceiling for one subtitle. A line that runs longer
    // is read twice before it changes, which reads as the track being stuck.
    double maxSeconds = 6.5;
    // Two rows at the default 62 px on a 1080p frame, with room for the wider
    // punctuation. Three rows of Chinese plus three of source is the case this
    // exists to prevent.
    std::size_t maxCharacters = 48;
    // A half that flashes is worse than the long line it came from.
    double minSeconds = 1.6;
};

namespace detail {

inline std::size_t CodePointWidth(unsigned char lead)
{
    return lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
}

inline std::size_t CodePoints(std::string_view text)
{
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); i += CodePointWidth(static_cast<unsigned char>(text[i])))
        ++count;
    return count;
}

// Where a reader accepts a break: after a comma or a semicolon, in either
// script. Full stops are not offered - see the note above on why the two halves
// must not look like two sentences to the stage that speaks them.
inline bool IsClauseMark(std::string_view piece)
{
    return piece == "," || piece == ";" || piece == ":" || piece == "\xEF\xBC\x8C" /* ， */ ||
           piece == "\xE3\x80\x81" /* 、 */ || piece == "\xEF\xBC\x9B" /* ； */ ||
           piece == "\xEF\xBC\x9A" /* ： */;
}

struct ClauseCut {
    std::size_t at = 0;          // byte offset just past the mark
    std::size_t codepoints = 0;  // characters up to `at`
};

// Every place the line can be broken, in order.
inline std::vector<ClauseCut> ClauseCuts(std::string_view text)
{
    std::vector<ClauseCut> cuts;
    std::size_t seen = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t width = CodePointWidth(static_cast<unsigned char>(text[i]));
        if (i + width > text.size()) break;
        const std::string_view piece(text.data() + i, width);
        ++seen;
        if (IsClauseMark(piece)) cuts.push_back(ClauseCut{i + width, seen});
        i += width;
    }
    return cuts;
}

inline bool EndsWithStop(char c)
{
    return c == '.' || c == '!' || c == '?' || c == ';';
}

// The same break on the other language, which has no clause mark to trust: the
// point is taken at the same fraction of the line and moved to the nearest word
// boundary. Moved back while the first half would end on a full stop, because
// that is exactly what the merge reads as "the sentence is over".
inline std::size_t SourceCutNear(std::string_view text, double fraction)
{
    if (text.empty()) return 0;
    const auto target = std::size_t(std::lround(fraction * double(text.size())));
    if (target < 8 || target + 8 > text.size()) return 0;
    std::size_t cut = 0;
    bool found = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != ' ') continue;
        if (!found || std::abs(double(i) - double(target)) < std::abs(double(cut) - double(target))) {
            found = true;
            cut = i;
        }
    }
    if (!found) {
        // No spaces at all: a CJK source, cut by character count instead.
        std::size_t seen = 0;
        for (std::size_t i = 0; i < text.size();) {
            const std::size_t width = CodePointWidth(static_cast<unsigned char>(text[i]));
            seen += width;
            i += width;
            if (seen >= target) return i < text.size() ? i : 0;
        }
        return 0;
    }
    for (int guard = 0; guard < 4; ++guard) {
        std::size_t tail = cut;
        while (tail > 0 && text[tail - 1] == ' ') --tail;
        if (tail == 0) return 0;
        if (!EndsWithStop(text[tail - 1])) return cut;
        const std::size_t previous = cut > 0 ? text.rfind(' ', cut - 1) : std::string_view::npos;
        if (previous == std::string_view::npos) return 0;
        cut = previous;
    }
    return 0;
}

inline std::string_view Trimmed(std::string_view text)
{
    while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
    while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
    return text;
}

} // namespace detail

// The screen, not the ear. A sentence the screen cannot hold in one go becomes
// two or three dialogue lines, cut at clause marks and spread evenly over the
// sentence's own window; a cue that already fits is left exactly as it was.
//
// Three lines is the limit on purpose. Beyond it the sentence is a paragraph
// being paged rather than a subtitle, and the right answer is a shorter
// translation - which is what the rewrite round in the text stage is for.
//
// The parts come back in order and meet with no gap, and none of them ends on a
// full stop: MergeIntoSentences, which the dub runs on exactly these cues, joins
// them straight back into the sentence that was translated. That is the whole
// contract - **split for the eye, never for the ear** - and the test pins it.
inline std::vector<Cue> CutOneForDisplay(const Cue& cue, const DisplayRules& rules)
{
    const std::string& primary = cue.translation.empty() ? cue.text : cue.translation;
    const double span = cue.end - cue.start;
    const double characters = double(detail::CodePoints(primary));
    if (span <= 0.0 || characters <= 0.0) return {};
    const double byTime = span / rules.maxSeconds;
    const double byLength = characters / double(rules.maxCharacters);
    const int wanted = std::min(3, int(std::ceil(std::max(byTime, byLength))));
    if (wanted < 2) return {};

    const std::vector<detail::ClauseCut> cuts = detail::ClauseCuts(primary);
    if (cuts.empty()) return {};
    // Fewer parts is always allowed: a line with one clause mark can only be cut
    // in two, and two is still an improvement.
    for (int parts = std::min<int>(wanted, int(cuts.size()) + 1); parts >= 2; --parts) {
        // The break nearest each even share. Greedy is enough: the cuts are
        // ordered, and every share is asked for in increasing order.
        std::vector<std::size_t> chosen;
        std::size_t previous = 0;
        bool found = true;
        for (int piece = 1; piece < parts && found; ++piece) {
            const double target = characters * double(piece) / double(parts);
            std::size_t best = cuts.size();
            for (std::size_t index = 0; index < cuts.size(); ++index) {
                if (cuts[index].codepoints <= previous) continue;
                if (best == cuts.size() ||
                    std::abs(double(cuts[index].codepoints) - target) <
                        std::abs(double(cuts[best].codepoints) - target))
                    best = index;
            }
            found = best != cuts.size();
            if (found) {
                chosen.push_back(best);
                previous = cuts[best].codepoints;
            }
        }
        if (!found || chosen.empty()) continue;

        // The edges of every piece in both currencies: characters and seconds.
        std::vector<std::size_t> edgeChar{0};
        std::vector<std::size_t> edgeByte{0};
        std::vector<double> edgeTime{cue.start};
        for (const std::size_t index : chosen) {
            edgeChar.push_back(cuts[index].codepoints);
            edgeByte.push_back(cuts[index].at);
            edgeTime.push_back(cue.start + span * (double(cuts[index].codepoints) / characters));
        }
        edgeChar.push_back(std::size_t(characters));
        edgeByte.push_back(primary.size());
        edgeTime.push_back(cue.end);

        bool usable = true;
        for (std::size_t piece = 0; piece + 1 < edgeChar.size() && usable; ++piece) {
            if (edgeChar[piece + 1] - edgeChar[piece] < 3) usable = false;
            if (edgeTime[piece + 1] - edgeTime[piece] < rules.minSeconds) usable = false;
        }
        if (!usable) continue;

        // The source is cut at the same fractions, at word boundaries. A source
        // that has no boundary there stops the split: repeating the whole line
        // under the second piece is worse than one long line.
        std::vector<std::size_t> sourceEdge{cue.translation.empty() ? cue.text.size() : 0};
        sourceEdge.resize(edgeByte.size(), 0);
        if (!cue.translation.empty()) {
            for (std::size_t piece = 1; piece + 1 < edgeTime.size(); ++piece) {
                const double fraction = (edgeTime[piece] - cue.start) / span;
                const std::size_t at = detail::SourceCutNear(cue.text, fraction);
                if (at == 0 || at >= cue.text.size() || at <= sourceEdge[piece - 1]) {
                    usable = false;
                    break;
                }
                sourceEdge[piece] = at;
            }
            sourceEdge.back() = cue.text.size();
        }
        if (!usable) continue;

        std::vector<Cue> pieces;
        for (std::size_t piece = 0; piece + 1 < edgeChar.size(); ++piece) {
            Cue part = cue;
            part.start = edgeTime[piece];
            part.end = edgeTime[piece + 1];
            const std::string_view spoken = detail::Trimmed(std::string_view(primary).substr(
                edgeByte[piece], edgeByte[piece + 1] - edgeByte[piece]));
            if (cue.translation.empty()) {
                part.text = std::string(spoken);
                part.translation.clear();
            } else {
                part.translation = std::string(spoken);
                part.text = std::string(detail::Trimmed(std::string_view(cue.text).substr(
                    sourceEdge[piece], sourceEdge[piece + 1] - sourceEdge[piece])));
            }
            if (part.text.empty() || part.translation.empty() && !cue.translation.empty()) {
                pieces.clear();
                break;
            }
            pieces.push_back(std::move(part));
        }
        if (pieces.size() < 2) continue;
        return pieces;
    }
    return {};
}

inline std::vector<Cue> SplitForDisplay(std::vector<Cue> cues, const DisplayRules& rules = {})
{
    std::vector<Cue> out;
    out.reserve(cues.size() + cues.size() / 2);
    for (Cue& cue : cues) {
        std::vector<Cue> pieces = CutOneForDisplay(cue, rules);
        if (pieces.empty()) {
            out.push_back(std::move(cue));
        } else {
            for (Cue& piece : pieces) out.push_back(std::move(piece));
        }
    }
    return out;
}


// ---- Time formatting ---------------------------------------------------------

// Timestamps are built from integers on purpose: snprintf with "%f" writes the
// decimal separator of the current locale, and a subtitle file with "00:00:01,5"
// in a comma-decimal locale is a file libass cannot read.
inline void AppendAssTime(std::string& out, double seconds)
{
    if (!(seconds > 0.0)) seconds = 0.0;  // also catches NaN
    int64_t centiseconds = int64_t(std::llround(seconds * 100.0));
    const int64_t hours = centiseconds / 360000;
    centiseconds -= hours * 360000;
    const int64_t minutes = centiseconds / 6000;
    centiseconds -= minutes * 6000;
    const int64_t whole = centiseconds / 100;
    const int64_t fraction = centiseconds % 100;
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld.%02lld",
                  static_cast<long long>(hours), static_cast<long long>(minutes),
                  static_cast<long long>(whole), static_cast<long long>(fraction));
    out += buffer;
}

inline void AppendSrtTime(std::string& out, double seconds)
{
    if (!(seconds > 0.0)) seconds = 0.0;
    int64_t milliseconds = int64_t(std::llround(seconds * 1000.0));
    const int64_t hours = milliseconds / 3600000;
    milliseconds -= hours * 3600000;
    const int64_t minutes = milliseconds / 60000;
    milliseconds -= minutes * 60000;
    const int64_t whole = milliseconds / 1000;
    const int64_t fraction = milliseconds % 1000;
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld,%03lld",
                  static_cast<long long>(hours), static_cast<long long>(minutes),
                  static_cast<long long>(whole), static_cast<long long>(fraction));
    out += buffer;
}

// The whole track as SRT. With `translatedOnly`, cues the model has not answered
// for are dropped - that is the file handed to the player mid-translation, and a
// line that falls back to the source is not the same thing as a line that is
// unnecessary.
inline std::string FormatSrt(const std::vector<Cue>& cues, bool useTranslation)
{
    std::string out;
    size_t index = 0;
    for (const Cue& cue : cues) {
        const std::string& text = useTranslation && cue.Translated() ? cue.translation : cue.text;
        if (text.empty()) continue;
        ++index;
        out += std::to_string(index);
        out += '\n';
        AppendSrtTime(out, cue.start);
        out += " --> ";
        AppendSrtTime(out, cue.end);
        out += '\n';
        out += text;
        out += "\n\n";
    }
    return out;
}

// ---- The ASS this player draws -----------------------------------------------

// Which of the two languages a generated track shows. Off and Source never
// generate a track at all: the source already has one, drawn with the file's own
// styling, and replacing it would throw away the typesetting an ASS release
// carries.
enum class Mode { Off, Source, Translation, Dual };

// Layout in ASS points at the video's own size. The numbers are the ones the
// slice settled on against real frames: 62 for the translation, 42 for the
// source, 44 points of bottom margin, and a deliberately visible weight
// difference so the eye reads the Chinese first.
struct Layout {
    int translationSize = 62;
    int sourceSize = 42;
    int marginVertical = 44;
    int marginHorizontal = 80;
    // A blank line at this size between the two languages. ASS has no line
    // spacing, so without it the source sits tight under the translation:
    // measured, this is the smallest gap that reads as a separation.
    int gapSize = 18;
    int translationOutline = 3;
    int sourceOutline = 2;
    const char* font = "Microsoft YaHei";
    // &HBBGGRR: white for the translation, a dimmer white for the source.
    const char* translationColour = "&H00FFFFFF";
    const char* sourceColour = "&H00E8E8E8";
};

namespace detail {

// A subtitle line may contain characters that mean something to libass: an
// override block starts at '{', and '\N' is a line break. Both are escaped so a
// line of dialogue can never become a tag. A literal backslash is left alone on
// purpose - escaping it is the one case where a wrong guess turns text into
// something worse, and "\\N" inside a subtitle line does not happen.
inline std::string Escape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '{': out += "\\{"; break;
        case '}': out += "\\}"; break;
        case '\n': out += "\\N"; break;
        case '\r': break;
        default: out += c; break;
        }
    }
    return out;
}

// Chinese has no spaces, and this libass does not break a space-less run: the
// line is drawn wider than the frame and clipped off both edges (WrapStyle 0
// only ever breaks at word boundaries). So the translation is wrapped here, at
// the widest row the frame holds. A line with spaces is left alone - that is
// Latin, and the renderer already breaks it correctly.
inline bool NeedsWrapping(std::string_view text)
{
    bool wide = false;
    for (const char c : text) {
        if (c == ' ') return false;
        if (static_cast<unsigned char>(c) >= 0x80) wide = true;
    }
    return wide;
}

// One row holds `usable width / font size` characters. Measured on this build:
// forty characters of the translation style render 1878 px wide on a 1920 px
// frame, so an advance is about 0.76 em and the frame would hold 37 of them. The
// count here assumes a full em and so wraps a little earlier than it must - the
// block comes out narrower than the frame allows and can never be clipped, which
// is the failure this exists to stop (see WrapRows).
inline int RowWidth(int frameWidth, const Layout& layout, int fontSize)
{
    const int usable = frameWidth - 2 * layout.marginHorizontal;
    if (usable <= 0 || fontSize <= 0) return 0;
    return std::max(1, usable / fontSize);
}

// Laid out rows of at most `perRow` characters, escaped, breaks marked with \N.
// A break prefers a mark: Chinese subtitle practice puts the mark at the end of
// a row rather than splitting a word, so the reader gets "…报道中，" / "我们看到…"
// and not "…全球范围" / "内发生了…". When no mark sits close enough to the even
// share, the share wins - a row of three characters is worse than a split word.
inline std::string WrapRows(std::string_view text, int perRow)
{
    if (perRow <= 0 || !NeedsWrapping(text)) return Escape(text);
    const std::size_t total = CodePoints(text);
    if (total <= std::size_t(perRow)) return Escape(text);
    const std::size_t rows = (total + std::size_t(perRow) - 1) / std::size_t(perRow);

    // Byte offset of every code point, so the breaks can be chosen by count and
    // applied by offset.
    std::vector<std::size_t> offsets;
    offsets.reserve(total + 1);
    for (std::size_t i = 0; i < text.size();) {
        offsets.push_back(i);
        i += CodePointWidth(static_cast<unsigned char>(text[i]));
    }
    offsets.push_back(text.size());

    const auto marksAt = [&](std::size_t at) {
        if (at >= total) return false;
        const std::size_t at_byte = offsets[at];
        return IsClauseMark(std::string_view(text.data() + at_byte, offsets[at + 1] - at_byte)) ||
               text[at_byte] == '.' || text[at_byte] == ';' || text[at_byte] == ':' ||
               text[at_byte] == '!' || text[at_byte] == '?' ||
               text.compare(at_byte, 3, "\xE3\x80\x82") == 0;  // 。
    };

    std::string out;
    out.reserve(text.size() + rows * 2);
    std::size_t from = 0;
    for (std::size_t row = 1; row < rows; ++row) {
        const std::size_t even = total * row / rows;
        std::size_t cut = even;
        const std::size_t slack = std::size_t(perRow) / 2;
        const std::size_t floor = std::size_t(perRow) / 3;
        bool marked = false;
        for (std::size_t at = even > slack ? even - slack : 1;
             at <= std::min(total - 1, even + slack); ++at) {
            if (at <= from || at - from < floor || at - from > std::size_t(perRow)) continue;
            if (row + 1 == rows && total - at > std::size_t(perRow)) continue;
            if (!marksAt(at - 1)) continue;  // the row would end on the mark
            if (!marked ||
                std::abs(std::ptrdiff_t(at) - std::ptrdiff_t(even)) <
                    std::abs(std::ptrdiff_t(cut) - std::ptrdiff_t(even))) {
                cut = at;
                marked = true;
            }
        }
        // No usable mark: the even share, stepped back over a mark that would
        // open the next row.
        for (int guard = 0; guard < 3 && !marked && cut > from && marksAt(cut); ++guard) --cut;
        if (cut <= from) continue;
        out += Escape(std::string_view(text).substr(offsets[from], offsets[cut] - offsets[from]));
        out += "\\N";
        from = cut;
    }
    out += Escape(std::string_view(text).substr(offsets[from]));
    return out;
}

inline void AppendStyle(std::string& out, const char* name, const char* font, int size,
                        const char* colour, int outline, int shadow, const Layout& layout)
{
    char buffer[320];
    // BorderStyle 1 = outline + drop shadow, Alignment 2 = bottom centre.
    std::snprintf(buffer, sizeof(buffer),
                  "Style: %s,%s,%d,%s,&H00FFFFFF,&H00000000,&H80000000,"
                  "0,0,0,0,100,100,0,0,1,%d,%d,2,%d,%d,%d,1\n",
                  name, font, size, colour, outline, shadow,
                  layout.marginHorizontal, layout.marginHorizontal, layout.marginVertical);
    out += buffer;
}

} // namespace detail

// One ASS file carrying `mode`'s language(s). Empty when there is nothing to
// draw - the caller then keeps the source's own track rather than swapping in a
// blank file.
inline std::string BuildAss(const std::vector<Cue>& cues, Mode mode, const Layout& layout,
                            uint32_t width, uint32_t height)
{
    if (mode == Mode::Off || width == 0 || height == 0) return {};
    // How many characters of each language the frame holds in one row: a CJK
    // character is about one em wide, and nothing else breaks it (see WrapRows).
    const int translationPerRow = detail::RowWidth(int(width), layout, layout.translationSize);
    const int sourcePerRow = detail::RowWidth(int(width), layout, layout.sourceSize);
    std::string body;
    for (const Cue& cue : cues) {
        const bool hasTranslation = !cue.translation.empty();
        const std::string& primary = mode == Mode::Source ? cue.text
                                   : hasTranslation    ? cue.translation
                                                       : cue.text;
        if (primary.empty()) continue;
        body += "Dialogue: 0,";
        AppendAssTime(body, cue.start);
        body += ',';
        AppendAssTime(body, cue.end);
        body += ",Dual,,0,0,0,,";
        if (mode == Mode::Dual && !cue.translation.empty() && !cue.text.empty()) {
            body += "{\\rZh}";
            body += detail::WrapRows(cue.translation, translationPerRow);
            body += "\\N";
            if (layout.gapSize > 0) {
                char gap[48];
                std::snprintf(gap, sizeof(gap), "{\\fs%d}\\h\\N", layout.gapSize);
                body += gap;
            }
            body += "{\\rSrc}";
            body += detail::WrapRows(cue.text, sourcePerRow);
        } else {
            const bool translation = hasTranslation && mode != Mode::Source;
            body += translation ? "{\\rZh}" : "{\\rSrc}";
            body += detail::WrapRows(primary, translation ? translationPerRow : sourcePerRow);
        }
        body += '\n';
    }
    if (body.empty()) return {};

    std::string out;
    out.reserve(body.size() + 1024);
    out += "[Script Info]\nScriptType: v4.00+\n";
    // 0 wraps at the file's own width rather than libass's default estimate;
    // ScaledBorderAndShadow keeps the outline the same weight at 720p and 4K.
    out += "WrapStyle: 0\nScaledBorderAndShadow: yes\n";
    out += "PlayResX: " + std::to_string(width) + "\n";
    out += "PlayResY: " + std::to_string(height) + "\n\n";
    out += "[V4+ Styles]\n";
    out += "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, "
           "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, "
           "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n";
    // "Dual" carries the alignment and margins the events are drawn with; the
    // two per-language styles are only ever reached through \r switches.
    detail::AppendStyle(out, "Dual", layout.font, layout.translationSize,
                        layout.translationColour, layout.translationOutline, 1, layout);
    detail::AppendStyle(out, "Zh", layout.font, layout.translationSize,
                        layout.translationColour, layout.translationOutline, 1, layout);
    detail::AppendStyle(out, "Src", layout.font, layout.sourceSize,
                        layout.sourceColour, layout.sourceOutline, 1, layout);
    out += "\n[Events]\n";
    out += "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n";
    out += body;
    return out;
}

// ---- One language out of a bilingual track -----------------------------------
//
// A bilingual track keeps both languages in one dialogue line: the translation
// first, the source under it. The two halves are marked rather than guessed -
// the translation opens with the \rZh switch and the source with \rSrc - and
// neither can appear inside a subtitle line, because the escape above turns a
// brace into \{ . So the single-language track is this same file with the
// source half taken out.
//
// A cue that never got a translation is left out altogether: a viewer who asked
// for Chinese alone does not read the English underneath it, and a lone
// English line under a file of Chinese is worse than a gap. This is the one
// place the result differs from BuildAss(cues, Mode::Translation), which falls
// back to the source for such a cue - here the cue is dropped, which is what
// "Chinese only" means. Every cue that does have a translation comes out the
// same as that builder would have made it.
inline std::string SingleLanguageAss(const std::string& dualAss)
{
    const std::string kZhSwitch="{\\rZh}";
    const std::string kSrcSwitch="{\\rSrc}";
    std::string out;
    out.reserve(dualAss.size());
    size_t pos=0;
    bool inEvents=false;
    while(true){
        const size_t eol=dualAss.find('\n',pos);
        const bool last=(eol==std::string::npos);
        std::string line=dualAss.substr(pos,last?std::string::npos:eol-pos);
        bool keep=true;
        if(!line.empty()&&line[0]=='['){
            inEvents=(line.rfind("[Events]",0)==0);
        }else if(inEvents&&line.rfind("Dialogue:",0)==0){
            // Nine commas put us at the text: the first nine fields are the
            // layer, the timing, the style, the name, the three margins and the
            // effect, and the text is whatever is left - commas included.
            size_t textStart=std::string::npos;
            int commas=0;
            for(size_t i=0;i<line.size();++i){
                if(line[i]==','){
                    if(++commas==9){textStart=i+1;break;}
                }
            }
            if(textStart!=std::string::npos){
                std::string text=line.substr(textStart);
                if(text.rfind(kZhSwitch,0)==0){
                    const size_t src=text.find(kSrcSwitch);
                    if(src!=std::string::npos){
                        std::string head=text.substr(0,src);
                        // The blank line between the two languages goes with
                        // the source half: left behind, it would hold the
                        // translation up by one line of the source's size.
                        if(head.size()>=2&&head.compare(head.size()-2,2,"\\N")==0){
                            const size_t gap=head.rfind("\\N{");
                            if(gap!=std::string::npos&&
                               head.size()>=4&&head.compare(head.size()-4,4,"\\h\\N")==0)
                                head.erase(gap);
                            else
                                head.erase(head.size()-2);
                        }
                        text=head;
                    }
                    line=line.substr(0,textStart)+text;
                }else if(text.rfind(kSrcSwitch,0)==0){
                    keep=false;
                }
            }
        }
        if(keep){
            out+=line;
            if(!last)out+='\n';
        }
        if(last)break;
        pos=eol+1;
    }
    return out;
}

// ---- The prompt the model was trained on -------------------------------------

// A pair of language names and the codes the model's own template uses. The
// template inside the GGUF accepts a structured content item carrying both
// codes; llama.cpp's OpenAI endpoint cannot express that, so the instruction is
// reproduced as text (the same thing Transub-Player does with
// --no-jinja --chat-template gemma). Wording copied from the template's text
// branch, because it is what the model was trained to answer.
// The pair the prompt names, because the model was trained to be told it. Owned
// rather than viewed: the tags come from a container, an ini file and a table,
// and a view into any of those outlives nothing.
struct Languages {
    std::string sourceName = "Japanese";
    std::string sourceCode = "ja";
    std::string targetName = "Chinese";
    std::string targetCode = "zh-Hans";
};

// The targets a dialog can offer, in one fixed order: exactly the languages the
// table below can name, so anything the UI shows is something the prompt can ask
// for by name AND code rather than by a tag the model has never seen. `tag` is
// what the ini file keeps; LanguagesFor resolves it straight back.
//
// Chinese leads because it is the pair this feature was measured on. Adding a
// language is one row in both lists - and a check on real material, because the
// prompt is only as good as the model's coverage of that pair.
struct TargetLanguage {
    std::string_view tag;
    std::string_view name;
};

inline const std::vector<TargetLanguage>& TargetLanguages()
{
    static const std::vector<TargetLanguage> languages = {
        {"zh", "Chinese"},
        {"zh-hant", "Traditional Chinese"},
        {"en", "English"},
        {"ja", "Japanese"},
        {"ko", "Korean"},
        {"yue", "Cantonese"},
        {"fr", "French"},
        {"de", "German"},
        {"es", "Spanish"},
        {"it", "Italian"},
        {"pt", "Portuguese"},
        {"ru", "Russian"},
        {"th", "Thai"},
        {"vi", "Vietnamese"},
        {"id", "Indonesian"},
        {"ar", "Arabic"},
        {"hi", "Hindi"},
    };
    return languages;
}

// What a stored tag means to a viewer, or empty when it is not one this build
// offers. Used for the dialog's own words about the choice.
inline std::string_view TargetLanguageName(std::string_view tag)
{
    std::string lowered;
    lowered.reserve(tag.size());
    for (const char c : tag) lowered.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    for (const TargetLanguage& language : TargetLanguages())
        if (language.tag == lowered) return language.name;
    return {};
}

// A pair built from tags. `sourceTag` is what the subtitle track says ("jpn",
// "eng", "zh-Hans"), empty when it says nothing; `targetTag` is the ini file's
// word, empty for Simplified Chinese.
//
// An unrecognised source is passed through as its own tag instead of being
// guessed at: telling the model "Japanese (ja) to Chinese" about English lines
// is worse than telling it "eng (eng)". Empty means the pair this feature was
// measured on.
inline Languages LanguagesFor(std::string_view sourceTag, std::string_view targetTag = {})
{
    struct Entry {
        std::string_view tag;   // as a container or a file name writes it
        std::string_view code;  // as the model's own template writes it
        std::string_view name;
    };
    static constexpr Entry kEntries[] = {
        {"ja", "ja", "Japanese"},       {"jpn", "ja", "Japanese"},
        {"en", "en", "English"},        {"eng", "en", "English"},
        {"ko", "ko", "Korean"},         {"kor", "ko", "Korean"},
        {"zh", "zh-Hans", "Chinese"},   {"zho", "zh-Hans", "Chinese"},
        {"chi", "zh-Hans", "Chinese"},  {"cmn", "zh-Hans", "Chinese"},
        {"zh-hans", "zh-Hans", "Chinese"}, {"zh-cn", "zh-Hans", "Chinese"},
        {"zh-hant", "zh-Hant", "Traditional Chinese"},
        {"zh-tw", "zh-Hant", "Traditional Chinese"},
        {"zh-hk", "zh-Hant", "Traditional Chinese"},
        {"yue", "yue", "Cantonese"},
        {"fr", "fr", "French"},         {"fra", "fr", "French"},
        {"fre", "fr", "French"},
        {"de", "de", "German"},         {"deu", "de", "German"},
        {"ger", "de", "German"},
        {"es", "es", "Spanish"},        {"spa", "es", "Spanish"},
        {"it", "it", "Italian"},        {"ita", "it", "Italian"},
        {"pt", "pt", "Portuguese"},     {"por", "pt", "Portuguese"},
        {"ru", "ru", "Russian"},        {"rus", "ru", "Russian"},
        {"th", "th", "Thai"},           {"tha", "th", "Thai"},
        {"vi", "vi", "Vietnamese"},     {"vie", "vi", "Vietnamese"},
        {"id", "id", "Indonesian"},     {"ind", "id", "Indonesian"},
        {"ar", "ar", "Arabic"},         {"ara", "ar", "Arabic"},
        {"hi", "hi", "Hindi"},          {"hin", "hi", "Hindi"},
    };
    const auto lookup = [](std::string_view tag) -> const Entry* {
        if (tag.empty()) return nullptr;
        // Containers write lowercase three-letter tags; a file name may not.
        std::string lowered;
        lowered.reserve(tag.size());
        for (const char c : tag) lowered.push_back(char(std::tolower(static_cast<unsigned char>(c))));
        for (const Entry& entry : kEntries)
            if (entry.tag == lowered) return &entry;
        // The speech engine names the language rather than tagging it: "english",
        // not "eng". The name is as good an answer as the tag.
        for (const Entry& entry : kEntries) {
            std::string name;
            name.reserve(entry.name.size());
            for (const char c : entry.name) name.push_back(char(std::tolower(static_cast<unsigned char>(c))));
            if (name == lowered) return &entry;
        }
        return nullptr;
    };
    Languages languages;
    if (const Entry* target = lookup(targetTag)) {
        languages.targetCode = target->code;
        languages.targetName = target->name;
    } else if (!targetTag.empty()) {
        languages.targetCode = targetTag;
        languages.targetName = targetTag;
    }
    if (const Entry* source = lookup(sourceTag)) {
        languages.sourceCode = source->code;
        languages.sourceName = source->name;
    } else if (!sourceTag.empty() && sourceTag != "und") {
        languages.sourceCode = sourceTag;
        languages.sourceName = sourceTag;
    }
    return languages;
}

// The ISO 639-2/B code a container tags a subtitle stream with, from the tag the
// language table is written in.
//
// B, not T: the mov muxer writes the bibliographic set and drops a T-code tag
// silently, so "zho" leaves the finished track with no language at all and a
// player choosing subtitles by language cannot see it. The two sets differ in
// exactly the languages a dub is made in - chi/zho, fre/fra, ger/deu - which is
// why the pair is spelled out here rather than derived from the code above.
//
// Empty for a tag with no B code, which is not the same as unknown: an untagged
// track is a track a player still offers, and a mis-tagged one is a track it
// offers to the wrong reader.
inline std::string MuxLanguageCode(std::string_view tag)
{
    static constexpr std::pair<std::string_view, std::string_view> kBibliographic[] = {
        {"zh", "chi"}, {"zh-hans", "chi"}, {"zh-cn", "chi"}, {"zho", "chi"}, {"cmn", "chi"},
        {"zh-hant", "chi"}, {"zh-tw", "chi"}, {"zh-hk", "chi"},
        {"en", "eng"}, {"ja", "jpn"}, {"ko", "kor"}, {"yue", "chi"},
        {"fr", "fre"}, {"de", "ger"}, {"es", "spa"}, {"it", "ita"}, {"pt", "por"},
        {"ru", "rus"}, {"th", "tha"}, {"vi", "vie"}, {"id", "ind"}, {"ar", "ara"},
        {"hi", "hin"},
    };
    if (tag.empty()) return {};
    std::string lowered;
    lowered.reserve(tag.size());
    for (const char c : tag) lowered.push_back(char(std::tolower(static_cast<unsigned char>(c))));
    for (const auto& entry : kBibliographic)
        if (entry.first == lowered) return std::string(entry.second);
    return {};
}

// ---- The film's own names ---------------------------------------------------
//
// A translation model knows the language, not the film. "University of Reading"
// came back as 阅读大学 - the university of reading, as in the act of reading a
// book - because nothing in one subtitle line says that Reading is an English
// town. Merging the cues first fixes most of the wrong translations; it cannot
// fix this one. The sentence was complete and the model still did not know the
// place.
//
// The glossary is a file, not a guess. The conversion reads a list of
// "English=Chinese" lines from beside the media - `film.mkv` -> `film.names.txt`
// - and every line that mentions one of those names is translated with it named.
// That is the one part of this that cannot be inferred: a name is a fact about
// the world, and the engine has no world.
//
// A model-written glossary was measured and rejected. Asked for the established
// name, this engine answers University of Reading=牛津大学 and Dartmoor=达特茅斯
// - it does not know the places and reaches for the famous one it does know, and
// 牛津大学 is a different university. Asked to transliterate the same names it
// answers 阅读大学 and 达特穆尔, and for names it does know it answers in pinyin
// ("伦敦=lún dūn") or in Traditional characters. The two answers agree only on
// names that are pure sound, which is exactly the case the model already gets
// right line by line. A wrong name written into every line that mentions it is
// worse than the literal one it replaces, so this stays the user's to supply.
struct GlossaryEntry {
    std::string term;
    std::string rendering;
};

// What a glossary file holds: one "English=Chinese" per line, blank lines and
// lines starting with '#' ignored. Anything that does not parse is dropped
// rather than guessed at - a file that is half right is still usable, and a line
// that is empty on one side would delete a name from a subtitle.
inline std::vector<GlossaryEntry> ParseGlossaryFile(std::string_view text)
{
    std::vector<GlossaryEntry> entries;
    size_t at = 0;
    while (at <= text.size()) {
        size_t eol = text.find('\n', at);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = detail::Trim(text.substr(at, eol - at));
        at = eol + 1;
        if (line.empty() || line.front() == '#') continue;
        // Both "English=Chinese" and "English:Chinese", because a colon is what
        // a person writes when they are not being told a format.
        size_t separator = line.find('=');
        if (separator == std::string_view::npos) separator = line.find(':');
        if (separator == std::string_view::npos) continue;
        const std::string_view term = detail::Trim(line.substr(0, separator));
        std::string_view rendering = detail::Trim(line.substr(separator + 1));
        while (!rendering.empty() && (rendering.back() == '.' || rendering.back() == '。'))
            rendering.remove_suffix(1);
        if (term.empty() || term.size() > 96 || rendering.empty() || rendering.size() > 96) continue;
        bool nonAscii = false;
        for (const char c : rendering)
            if (static_cast<unsigned char>(c) >= 0x80) nonAscii = true;
        if (!nonAscii) continue;  // an English term for an English term is not a glossary
        entries.push_back({std::string(term), std::string(rendering)});
    }
    return entries;
}


// The ones this line mentions, worded the way the prompt takes them. Case is
// ignored and nothing else is: a term matches where it appears as written.
inline std::string RenderingsFor(std::string_view text, const std::vector<GlossaryEntry>& entries,
                                 size_t limit = 6)
{
    if (entries.empty()) return {};
    std::string lowered(text);
    for (char& c : lowered) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string note;
    size_t used = 0;
    for (const GlossaryEntry& entry : entries) {
        if (used >= limit) break;
        std::string needle = entry.term;
        for (char& c : needle) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (lowered.find(needle) == std::string::npos) continue;
        if (!note.empty()) note += "; ";
        note += entry.term;
        note += " = ";
        note += entry.rendering;
        ++used;
    }
    return note;
}

// Bumped when the wording below changes: it is part of the cache key, so a
// prompt change must not read translations produced by the old one.
inline constexpr std::string_view kPromptVersion = "t2";

// `renderings` is the glossary note for this line, empty when it names nobody.
inline std::string OfficialPrompt(std::string_view text, const Languages& languages = {},
                                  std::string_view renderings = {})
{
    std::string prompt;
    prompt.reserve(text.size() + 512);
    prompt += "You are a professional ";
    prompt += languages.sourceName;
    prompt += " (";
    prompt += languages.sourceCode;
    prompt += ") to ";
    prompt += languages.targetName;
    prompt += " (";
    prompt += languages.targetCode;
    prompt += ") translator. Your goal is to accurately convey the meaning and nuances of the "
              "original ";
    prompt += languages.sourceName;
    prompt += " text while adhering to ";
    prompt += languages.targetName;
    prompt += " grammar, vocabulary, and cultural sensitivities.\nProduce only the ";
    prompt += languages.targetName;
    prompt += " translation, without any additional explanations or commentary.";
    if (!renderings.empty()) {
        // Stated before the line, never inside it: a name explained in brackets
        // in the middle of a sentence is a bracket the model then translates.
        prompt += "\nThis line names ";
        prompt += renderings;
        prompt += ". Use exactly those renderings for those names.";
    }
    prompt += " Please translate the following ";
    prompt += languages.sourceName;
    prompt += " text into ";
    prompt += languages.targetName;
    prompt += ":\n\n\n";
    prompt += text;
    return prompt;
}

// The same line, asked for a size. Only asked when the first rendering cannot be
// reached by tempo at all (dub_fit::BudgetForLine): the line would otherwise be
// compressed to the ceiling and then cut by the mix, and a listener hears both as
// the voice breaking rather than as a translation being too long.
//
// The budget is stated in characters because that is what the pace model was fit
// on - 7.02 characters a second, and a comma costs 0.665 s of its own - so a
// number of characters is a duration this code can predict and a model can obey.
inline std::string ShorterPrompt(std::string_view text, const Languages& languages,
                                 std::size_t characters, std::string_view renderings = {})
{
    std::string prompt;
    prompt.reserve(text.size() + 512);
    prompt += "You are a professional ";
    prompt += languages.sourceName;
    prompt += " (";
    prompt += languages.sourceCode;
    prompt += ") to ";
    prompt += languages.targetName;
    prompt += " (";
    prompt += languages.targetCode;
    prompt += ") translator dubbing a film.\nTranslate the line below into ";
    prompt += languages.targetName;
    prompt += " that can be spoken in ";
    prompt += std::to_string(characters);
    prompt += " characters or fewer. Keep the meaning and the tone; leave out what the line can "
              "do without.\nProduce only the ";
    prompt += languages.targetName;
    prompt += " translation, without any additional explanations or commentary.";
    if (!renderings.empty()) {
        prompt += "\nThis line names ";
        prompt += renderings;
        prompt += ". Use exactly those renderings for those names.";
    }
    prompt += " Please translate the following ";
    prompt += languages.sourceName;
    prompt += " text into ";
    prompt += languages.targetName;
    prompt += ":\n\n\n";
    prompt += text;
    return prompt;
}

// What the model answered, as one subtitle line. It obeys "produce only the
// translation" almost always; the cases handled here are the ones measured:
// a trailing <end_of_turn>, wrapped quotes, and a leading "1." when the model
// treats the text as a list item.
inline std::string CleanTranslation(std::string_view raw)
{
    const size_t turn = raw.find("<end_of_turn>");
    if (turn != std::string_view::npos) raw = raw.substr(0, turn);
    raw = detail::Trim(raw);

    while (raw.size() >= 2 && ((raw.front() == '"' && raw.back() == '"'))) {
        raw = detail::Trim(raw.substr(1, raw.size() - 2));
    }
    // “ ” are three bytes each in UTF-8; only strip them when both ends match.
    constexpr std::string_view kOpen = "\xE2\x80\x9C";
    constexpr std::string_view kClose = "\xE2\x80\x9D";
    if (raw.size() > kOpen.size() + kClose.size() && raw.substr(0, kOpen.size()) == kOpen &&
        raw.substr(raw.size() - kClose.size()) == kClose)
        raw = detail::Trim(raw.substr(kOpen.size(), raw.size() - kOpen.size() - kClose.size()));

    // A list-style prefix the model sometimes keeps: "1. ", "2) ", "12、".
    // Only one or two digits and only a real separator sequence, because
    // stripping a year and its punctuation off the front of a subtitle line
    // would silently delete part of the translation. "、" is three bytes in
    // UTF-8, so a byte-at-a-time strip would leave the rest of the character
    // behind and the line would no longer be valid text.
    size_t digits = 0;
    while (digits < raw.size() && raw[digits] >= '0' && raw[digits] <= '9') ++digits;
    if (digits >= 1 && digits <= 2 && digits < raw.size()) {
        std::string_view separator;
        const std::string_view tail = raw.substr(digits);
        if (tail.starts_with(".") || tail.starts_with(")") || tail.starts_with(":") ||
            tail.starts_with(","))
            separator = tail.substr(0, 1);
        else if (tail.starts_with("\xE3\x80\x81"))   // 、
            separator = tail.substr(0, 3);
        else if (tail.starts_with("\xEF\xBC\x8E"))   // ．
            separator = tail.substr(0, 3);
        else if (tail.starts_with("\xEF\xBC\x8C"))   // ，
            separator = tail.substr(0, 3);
        if (!separator.empty()) raw = detail::Trim(raw.substr(digits + separator.size()));
    }
    return std::string(raw);
}

// ---- JSON, only as much of it as one endpoint needs --------------------------

// The inside of a JSON string: quotes, backslashes and control characters
// escaped, everything else - including every byte of UTF-8 - passed through.
// A Chinese translation is not ASCII, and a JSON writer that escaped it as
// \uXXXX would triple the request for no benefit; the server reads UTF-8.
inline std::string JsonEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size() + 16);
    char escape[8];
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned char>(c));
                out += escape;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

namespace detail {

inline void AppendUtf8(std::string& out, uint32_t code)
{
    if (code < 0x80) {
        out += char(code);
    } else if (code < 0x800) {
        out += char(0xC0 | (code >> 6));
        out += char(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += char(0xE0 | (code >> 12));
        out += char(0x80 | ((code >> 6) & 0x3F));
        out += char(0x80 | (code & 0x3F));
    } else {
        out += char(0xF0 | (code >> 18));
        out += char(0x80 | ((code >> 12) & 0x3F));
        out += char(0x80 | ((code >> 6) & 0x3F));
        out += char(0x80 | (code & 0x3F));
    }
}

inline bool ParseHex4(std::string_view text, uint32_t& value)
{
    if (text.size() < 4) return false;
    value = 0;
    for (size_t index = 0; index < 4; ++index) {
        const char c = text[index];
        uint32_t digit = 0;
        if (c >= '0' && c <= '9') digit = uint32_t(c - '0');
        else if (c >= 'a' && c <= 'f') digit = uint32_t(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = uint32_t(c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    return true;
}

} // namespace detail

// The string value of the first `"key"` in `json`, decoded; nothing when the key
// is missing or its value is not a string.
//
// Deliberately not a JSON parser: one endpoint under our control answers, and
// the shape of its reply is fixed. It scans a string properly, though - escapes
// decoded, \uXXXX (with surrogate pairs) turned into UTF-8 - so a translation
// containing a quote cannot truncate the reply, which is the one way a
// substring search would have been wrong.
inline std::optional<std::string> JsonStringField(std::string_view json, std::string_view key)
{
    std::string needle;
    needle.reserve(key.size() + 2);
    needle += '"';
    needle += key;
    needle += '"';
    for (size_t at = json.find(needle); at != std::string_view::npos; at = json.find(needle, at + 1)) {
        size_t cursor = at + needle.size();
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\n' ||
                                        json[cursor] == '\r'))
            ++cursor;
        if (cursor >= json.size() || json[cursor] != ':') continue;  // a key-like word inside a value
        ++cursor;
        while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\n' ||
                                        json[cursor] == '\r'))
            ++cursor;
        if (cursor >= json.size() || json[cursor] != '"') continue;

        std::string out;
        for (++cursor; cursor < json.size();) {
            const char c = json[cursor];
            if (c == '"') return out;
            if (c != '\\') {
                out += c;
                ++cursor;
                continue;
            }
            if (++cursor >= json.size()) break;
            const char escape = json[cursor++];
            switch (escape) {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                uint32_t code = 0;
                if (!detail::ParseHex4(json.substr(cursor, 4), code)) return std::nullopt;
                cursor += 4;
                // A surrogate pair is two \u escapes for one character.
                if (code >= 0xD800 && code <= 0xDBFF && cursor + 1 < json.size() &&
                    json[cursor] == '\\' && json[cursor + 1] == 'u') {
                    uint32_t low = 0;
                    if (detail::ParseHex4(json.substr(cursor + 2, 4), low) && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        cursor += 6;
                    }
                }
                detail::AppendUtf8(out, code);
                break;
            }
            default: out += escape; break;  // \" \\ \/ and anything unknown
            }
        }
        // An unterminated string is a truncated reply, not an empty translation.
        return std::nullopt;
    }
    return std::nullopt;
}

// ---- Layout arithmetic --------------------------------------------------------

// Points to pixels for a CJK font, measured on the staged FFmpeg's libass with
// DirectWrite: Fontsize 100 renders ten full-width characters 748 px wide, so a
// glyph advances 0.748 of the size. Any width budget that assumes one em per
// character over-estimates the wrap point by a third.
inline constexpr double kAdvancePerPoint = 0.748;

// The default layout scaled to a picture of `height` lines. The numbers were
// settled at 1080p; a 720p window drawn with the same point sizes would show
// text a third larger relative to the frame, so every size and margin moves with
// the height. Floors at a size still legible rather than at zero.
inline Layout LayoutFor(uint32_t height)
{
    Layout layout;
    if (height == 0) return layout;
    const double scale = double(height) / 1080.0;
    const auto scaled = [scale](int value, int minimum) {
        const int result = int(std::lround(double(value) * scale));
        return result < minimum ? minimum : result;
    };
    layout.translationSize = scaled(layout.translationSize, 12);
    layout.sourceSize = scaled(layout.sourceSize, 10);
    layout.marginVertical = scaled(layout.marginVertical, 4);
    layout.marginHorizontal = scaled(layout.marginHorizontal, 8);
    layout.gapSize = scaled(layout.gapSize, 0);
    return layout;
}

inline size_t CharsPerLine(uint32_t width, const Layout& layout, int size)
{
    if (width == 0 || size <= 0) return 0;
    const double usable = double(width) - 2.0 * double(layout.marginHorizontal);
    if (usable <= 0.0) return 0;
    return size_t(usable / (kAdvancePerPoint * double(size)));
}

// ---- Cache identity ----------------------------------------------------------

// 64-bit FNV-1a. Not cryptographic: this names a directory of translations the
// viewer already has, and a collision costs a re-translation rather than a wrong
// subtitle - the file is rebuilt whenever the cue text it was made from differs.
inline uint64_t Fnv1a64(std::string_view text)
{
    uint64_t hash = 1469598103934665603ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

inline std::string CacheKey(std::string_view mediaIdentity, std::string_view trackIdentity,
                            std::string_view modelFile, const Languages& languages = {})
{
    std::string joined;
    joined += mediaIdentity;
    joined += '\x1f';
    joined += trackIdentity;
    joined += '\x1f';
    joined += modelFile;
    joined += '\x1f';
    joined += languages.targetCode;
    joined += '\x1f';
    joined += languages.sourceCode;
    joined += '\x1f';
    joined += kPromptVersion;
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(Fnv1a64(joined)));
    return std::string(buffer);
}

// ---- The ffmpeg command that gets the cues out -------------------------------

// Text of one subtitle stream, as SRT on stdout. Every format the player can
// draw converts to SRT (an ASS becomes plain text and loses its typesetting,
// which is exactly right: the translation is made from the words, and the
// generated track has its own styling). Reading the whole thing is what makes
// the cue list available for translation - libass reads it too, but nothing can
// ask libass what it read.
//
// `charenc` is the code page an external text file needs (subtitle::CharencFor);
// ffmpeg refuses such a file without being told ("Invalid UTF-8 in decoded
// subtitles text"), which is a hard failure rather than a garbled one.
inline std::wstring SrtArguments(const std::wstring& input, int stream, const std::wstring& charenc = {})
{
    // -copyts: the cues keep the container's own timestamps, so the generated
    // ASS is on the same clock as the track it was made from and the overlay
    // maps both with the same origin.
    std::wstring args = L"-hide_banner -loglevel error -nostdin -copyts ";
    if (!charenc.empty()) {
        args += L"-sub_charenc ";
        args += charenc;
        args += L' ';
    }
    args += L"-i \"";
    args += input;
    args += L"\" -map 0:s:";
    args += std::to_wstring(stream);
    args += L" -c:s srt -f srt pipe:1";
    return args;
}

// ---- The engine's own command line -------------------------------------------

// Which chat template llama-server must be told to use. A model whose GGUF
// carries a template llama.cpp cannot render (TranslateGemma's wants a
// structured content item with the two language codes) fails to initialise
// outright, so the server is started with a built-in template instead - the same
// workaround Transub-Player applies. Decided by the file name because that is
// the only identification llama-server gets.
enum class ChatTemplate { Jinja, Gemma, Chatml };

inline ChatTemplate ChatTemplateFor(std::string_view modelFileName)
{
    std::string lower;
    lower.reserve(modelFileName.size());
    for (const char c : modelFileName)
        lower += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
    if (lower.find("translategemma") != std::string::npos) return ChatTemplate::Gemma;
    if (lower.find("murasaki") != std::string::npos) return ChatTemplate::Chatml;
    return ChatTemplate::Jinja;
}

inline std::wstring ServerArguments(const std::wstring& modelPath, int port, int gpuLayers, int threads,
                                    ChatTemplate chatTemplate)
{
    std::wstring args = L"-m \"";
    args += modelPath;
    args += L"\" --host 127.0.0.1 --port ";
    args += std::to_wstring(port);
    // One slot: the player sends one request at a time, and a second slot only
    // splits the context between them.
    args += L" -ngl ";
    args += std::to_wstring(gpuLayers);
    args += L" -c 4096 -np 1";
    if (threads > 0) {
        args += L" -t ";
        args += std::to_wstring(threads);
    }
    switch (chatTemplate) {
    case ChatTemplate::Gemma: args += L" --no-jinja --chat-template gemma"; break;
    case ChatTemplate::Chatml: args += L" --no-jinja --chat-template chatml"; break;
    case ChatTemplate::Jinja: args += L" --jinja"; break;
    }
    return args;
}

// The body of one /v1/chat/completions request. Temperature 0 and a fixed seed
// make a re-run reproduce the same subtitles, which is what lets the cache be
// keyed on the cue text alone.
inline std::string ChatRequestBody(std::string_view prompt, int maxTokens = 384)
{
    std::string body;
    body.reserve(prompt.size() + 160);
    body += "{\"messages\":[{\"role\":\"user\",\"content\":\"";
    body += JsonEscape(prompt);
    body += "\"}],\"temperature\":0,\"top_p\":1,\"seed\":42,\"max_tokens\":";
    body += std::to_string(maxTokens);
    body += ",\"stream\":false}";
    return body;
}

} // namespace subtitle_translate
