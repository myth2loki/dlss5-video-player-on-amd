#pragma once

#include "SubtitleTranslatePolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Turning speech into subtitle cues, one window of audio at a time.
//
// The engine (whisper) hears a window, not a stream, so a film is cut into
// windows that the player asks the engine about in order, and every window's
// answers are shifted onto the film's own clock and appended to what is already
// known. Everything here is that arithmetic: where the windows are, what to
// throw away because the previous window already covered it, and which of the
// engine's answers are not words at all.
//
// The numbers are the plan's (docs/REALTIME-SUBTITLE-PLAN-zh.md 7.1): a short
// first window so the first line arrives sooner, then longer ones, overlapped so
// a word split across a boundary is heard whole at least once.
namespace subtitle_speech {

// Where the windows fall. Ten seconds first, because the wait for the first line
// is what the viewer notices; twenty-four after that, because throughput is what
// matters once the transcript is running ahead of the picture.
struct ChunkPlan {
    double firstSeconds = 10.0;
    double windowSeconds = 24.0;
    double overlapSeconds = 2.0;
    // Where listening begins on the film's clock. A viewer who asks for
    // subtitles twelve minutes in must not sit through the opening titles being
    // heard again first: at a little over twice real time, the transcript would
    // take minutes to reach the picture, and nothing would be drawn until it did.
    double startSeconds = 0.0;
    // How far ahead of the viewer the transcript may run before the engine is
    // left idle. A two-hour film is an hour of cores; none of that is worth
    // spending on audio nobody will reach for another twenty minutes.
    double leadSeconds = 180.0;

    // A window is asked to hear `overlapSeconds` of audio it has already been
    // given, so the step between window starts is what is left.
    double Step() const
    {
        const double step = windowSeconds - overlapSeconds;
        return step > 1.0 ? step : 1.0;
    }
};

// A window of the film, in media time.
struct Chunk {
    double start = 0.0;  // the first sample sent to the engine
    double end = 0.0;    // one past the last
    double coveredUntil = 0.0;  // media time no other window has covered yet
};

// The window with `index`, or nothing once the film has been covered.
//
// The first window starts at zero. Every later one starts `overlapSeconds`
// before what is already covered, so consecutive windows share that much audio.
inline std::optional<Chunk> ChunkAt(const ChunkPlan& plan, size_t index, double duration)
{
    if (!(duration > 0.0) || !std::isfinite(duration)) return std::nullopt;
    const double from = std::max(0.0, plan.startSeconds);
    if (duration <= from) return std::nullopt;  // asked to start past the end
    if (index == 0) {
        Chunk chunk;
        chunk.start = from;
        chunk.end = std::min(duration, from + plan.firstSeconds);
        chunk.coveredUntil = from;
        return chunk.end > 0.0 ? std::optional<Chunk>(chunk) : std::nullopt;
    }
    // Roll the windows forward: each covers the step, and reaches back over the
    // overlap it shares with the one before.
    double covered = std::min(duration, from + plan.firstSeconds);
    for (size_t at = 1; at <= index; ++at) {
        if (covered >= duration) return std::nullopt;
        const double step = plan.Step();
        // Never reach back past where listening began: those seconds were not
        // asked for, and the two before them are outside this transcript.
        const double start = std::max(from, covered - plan.overlapSeconds);
        // start = covered - overlap and the window is windowSeconds long, so it
        // ends at start + windowSeconds == covered + step. Adding the overlap a
        // second time would make every window 26 s for a 24 s plan.
        const double end = std::min(duration, covered + step);
        if (at == index) {
            Chunk chunk;
            chunk.start = start;
            chunk.end = end;
            chunk.coveredUntil = covered;
            return chunk;
        }
        covered = end;
    }
    return std::nullopt;
}

// How many windows a film needs. Zero for a film with no duration.
inline size_t ChunkCount(const ChunkPlan& plan, double duration)
{
    if (!(duration > 0.0) || !std::isfinite(duration)) return 0;
    size_t count = 0;
    while (ChunkAt(plan, count, duration)) ++count;
    // ChunkAt walks from zero every call, which is quadratic in the number of
    // windows; a film is a few hundred, and the walk is arithmetic, not work.
    if (count > 100000) count = 100000;
    return count;
}

namespace detail {

// Words, or something that only looks like them. Whisper fills silence with
// stock phrases and with whatever it was prompted with, and a subtitle track
// that says "Thank you for watching" over a quiet scene is worse than one that
// says nothing.
inline bool LooksLikeNoise(std::string_view text)
{
    bool word = false;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            word = true;  // any non-ASCII byte is a letter in some script
            break;
        }
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            word = true;
            break;
        }
    }
    return !word;
}

inline std::string Trimmed(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n'))
        text.remove_suffix(1);
    return std::string(text);
}

} // namespace detail

// One window's answers, shifted onto the film's clock and appended to `cues`.
//
// `heard` holds the engine's cues with times relative to the window it was
// given. Answers that begin before `chunk.coveredUntil` are the overlap being
// heard twice and are dropped: keeping them would show the same sentence twice
// with slightly different wording. Returns how many were kept.
//
// A trailing cue is extended to its end, never past the window, so a line is not
// cut short by where the audio happened to be cut.
inline size_t AppendChunk(std::vector<subtitle_translate::Cue>& cues,
                          const std::vector<subtitle_translate::Cue>& heard,
                          const Chunk& chunk, size_t maxRepeats = 3)
{
    size_t kept = 0;
    size_t repeats = 0;
    std::string previous;
    for (const subtitle_translate::Cue& cue : heard) {
        subtitle_translate::Cue placed;
        placed.start = chunk.start + cue.start;
        placed.end = std::min(chunk.end, chunk.start + cue.end);
        placed.text = detail::Trimmed(cue.text);
        if (placed.end <= placed.start) continue;  // zero-length
        if (placed.text.empty() || detail::LooksLikeNoise(placed.text)) continue;
        // The overlap was heard by the previous window too. A cue that starts
        // inside it but runs past it is the same sentence heard again, so only
        // the part already covered is cut off - dropping the whole cue lost
        // every line that happened to begin in those two seconds, which was
        // every line of a film whose windows slide by less than a sentence.
        if (placed.end <= chunk.coveredUntil) continue;
        if (placed.start < chunk.coveredUntil) placed.start = chunk.coveredUntil;
        // The same line three times over is the engine stuck on a loop, not a
        // character repeating themselves.
        if (placed.text == previous) {
            if (++repeats >= maxRepeats) continue;
        } else {
            repeats = 0;
        }
        previous = placed.text;
        // Cues arrive in order, but a window can overlap the last cue the
        // previous window produced: keep the list sorted and non-overlapping.
        if (!cues.empty() && placed.start < cues.back().end) {
            if (placed.start < cues.back().start) continue;  // behind what is known
            cues.back().end = placed.start;
            if (cues.back().end <= cues.back().start) cues.back().end = placed.start + 0.05;
        }
        cues.push_back(std::move(placed));
        ++kept;
    }
    return kept;
}

namespace detail {

inline bool EndsSentence(std::string_view text, size_t at)
{
    const char c = text[at];
    if (c == '.' || c == '!' || c == '?') return true;
    // CJK full-width marks, and the ellipsis, are three bytes each.
    const std::string_view tail = text.substr(at);
    return tail.starts_with("\xE3\x80\x82") ||  // 。
           tail.starts_with("\xEF\xBC\x81") ||  // ！
           tail.starts_with("\xEF\xBC\x9F") ||  // ？
           tail.starts_with("\xE2\x80\xA6");    // …
}

// How many bytes the mark at `at` occupies.
inline size_t SentenceMarkLength(std::string_view text, size_t at)
{
    const char c = text[at];
    if (c == '.' || c == '!' || c == '?') return 1;
    const std::string_view tail = text.substr(at);
    if (tail.starts_with("\xE3\x80\x82") || tail.starts_with("\xEF\xBC\x81") ||
        tail.starts_with("\xEF\xBC\x9F"))
        return 3;
    if (tail.starts_with("\xE2\x80\xA6")) return 3;
    return 1;
}

} // namespace detail

// Breaks up a cue that would otherwise be one line across half a minute.
//
// The engine segments by what it hears, and a speaker who does not pause for
// twenty seconds gets a twenty-second cue: readable on paper, unreadable on
// screen. Time is handed out in proportion to how much of the text each piece
// carries, which is wrong by a word or two and right about where the line
// should change.
inline std::vector<subtitle_translate::Cue> SplitLongCues(
    const std::vector<subtitle_translate::Cue>& cues, double maxSeconds = 7.0,
    size_t maxChars = 48)
{
    std::vector<subtitle_translate::Cue> out;
    out.reserve(cues.size());
    for (const auto& cue : cues) {
        const size_t length = cue.text.size();
        if (cue.end - cue.start <= maxSeconds && length <= maxChars) {
            out.push_back(cue);
            continue;
        }
        // Where the sentence ends, in bytes; the mark belongs to the piece
        // before it.
        std::vector<size_t> marks;
        for (size_t at = 0; at < length;) {
            if (detail::EndsSentence(cue.text, at)) {
                marks.push_back(at + detail::SentenceMarkLength(cue.text, at));
                at += detail::SentenceMarkLength(cue.text, at);
            } else {
                ++at;
            }
        }
        if (marks.empty() || marks.back() < length) marks.push_back(length);
        // A single sentence longer than the limit is cut on spaces, and only
        // then on characters, so a line is never a wall of text.
        std::vector<size_t> cuts;
        size_t previous = 0;
        for (const size_t mark : marks) {
            while (mark - previous > maxChars) {
                size_t cut = cue.text.rfind(' ', previous + maxChars);
                if (cut == std::string::npos || cut <= previous) cut = previous + maxChars;
                cuts.push_back(cut);
                previous = cut;
            }
            cuts.push_back(mark);
            previous = mark;
        }
        if (cuts.empty()) {
            out.push_back(cue);
            continue;
        }
        const double span = cue.end - cue.start;
        const auto weight = [&](size_t from, size_t to) {
            return length > 0 ? double(to - from) / double(length) : 0.0;
        };
        size_t from = 0;
        double at = cue.start;
        for (const size_t cut : cuts) {
            subtitle_translate::Cue piece;
            piece.start = at;
            const double share = weight(from, cut) * span;
            at += share;
            piece.end = std::min(cue.end, at);
            if (piece.end - piece.start < 0.4) piece.end = std::min(cue.end, piece.start + 0.4);
            piece.text = detail::Trimmed(std::string_view(cue.text).substr(from, cut - from));
            if (!piece.text.empty() && piece.end > piece.start) out.push_back(std::move(piece));
            from = cut;
        }
    }
    return out;
}

// The newest speech the transcript covers: what the player compares its own
// clock against to say how far behind the subtitles are.
inline double TranscriptFrontier(const std::vector<subtitle_translate::Cue>& cues, double asrFrontier)
{
    double frontier = asrFrontier;
    for (const auto& cue : cues) frontier = std::max(frontier, cue.end);
    return frontier;
}

// The seconds a subtitle track is behind the picture; zero when it is not.
inline double LagSeconds(double positionSeconds, double frontierSeconds)
{
    const double lag = positionSeconds - frontierSeconds;
    return lag > 0.0 ? lag : 0.0;
}

// ---- The request the engine understands ---------------------------------------

// whisper-server takes one multipart form: the audio as a file, and the answer
// format as a field. SRT rather than JSON because it is what the rest of this
// feature already parses, and because a boundary between two lines is exactly
// what the engine's own segment timing gives.
//
// Built as bytes, not as a string of text: a window of 16 kHz PCM contains every
// byte value, including the NULs and CRLFs a text-oriented builder would mangle.
inline std::string MultipartBody(std::string_view boundary, std::string_view fileName,
                                 std::string_view audio, std::string_view language,
                                 std::string_view format)
{
    std::string body;
    body.reserve(audio.size() + 512);
    const auto field = [&body, boundary](std::string_view name, std::string_view value) {
        body += "--";
        body += boundary;
        body += "\r\nContent-Disposition: form-data; name=\"";
        body += name;
        body += "\"\r\n\r\n";
        body += value;
        body += "\r\n";
    };
    // The format is the caller's, because it is not free: a verbose_json answer
    // makes the engine do the work twice over - 9.5 s against 5.0 s for the same
    // 24 s window, measured - and it is asked for only to learn the language,
    // which is decided once for the whole film.
    field("response_format", format);
    field("language", language.empty() ? std::string_view("auto") : language);
    body += "--";
    body += boundary;
    body += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"";
    body += fileName;
    body += "\"\r\nContent-Type: audio/wav\r\n\r\n";
    body.append(audio);
    body += "\r\n--";
    body += boundary;
    body += "--\r\n";
    return body;
}

// ---- The engine's answer -----------------------------------------------------

namespace detail {

// One object of an array, from its "{" to its matching "}". Strings are skipped,
// so a brace inside a line of dialogue cannot end it early.
inline std::string_view JsonObjectAt(std::string_view text, size_t from)
{
    const size_t open = text.find('{', from);
    if (open == std::string_view::npos) return {};
    size_t depth = 0;
    bool inString = false, escaped = false;
    for (size_t at = open; at < text.size(); ++at) {
        const char c = text[at];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') { inString = true; continue; }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return text.substr(open, at - open + 1);
        } else if (c == ']' && depth == 0) {
            return {};
        }
    }
    return {};
}

// A numeric field of one object. Zero when it is absent, which is also what a
// missing start or end means to the merge.
inline double JsonNumberField(std::string_view object, std::string_view key)
{
    std::string quoted;
    quoted.reserve(key.size() + 3);
    quoted += '"';
    quoted += key;
    quoted += "\":";
    size_t at = object.find(quoted);
    if (at == std::string_view::npos) return 0.0;
    at += quoted.size();
    while (at < object.size() && (object[at] == ' ' || object[at] == '\t')) ++at;
    // Cheap and exact: copy the numeric token and let strtod do the conversion,
    // rather than reimplementing its edge cases.
    char token[40]{};
    size_t length = 0;
    if (at < object.size() && (object[at] == '-' || object[at] == '+')) token[length++] = object[at++];
    while (at < object.size() && length + 1 < sizeof(token)) {
        const char c = object[at];
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
            token[length++] = c;
            ++at;
        } else {
            break;
        }
    }
    token[length] = 0;
    return length ? std::strtod(token, nullptr) : 0.0;
}

} // namespace detail

// The lines the engine heard, and the language it heard them in.
struct Heard {
    std::vector<subtitle_translate::Cue> cues;
    std::string language;  // "english", "japanese": a name, as the engine writes it
};

// One verbose_json answer. The segment objects carry their own "text", "start"
// and "end" before the per-word array, so the first occurrence of each key is
// the one wanted; `detail::JsonObjectAt` keeps a "}" inside a line of dialogue
// from ending a segment early.
inline Heard ParseHeard(std::string_view json)
{
    Heard heard;
    if (const auto language = subtitle_translate::JsonStringField(json, "language"))
        heard.language = *language;
    const size_t key = json.find("\"segments\"");
    if (key == std::string_view::npos) return heard;
    const size_t array = json.find('[', key);
    if (array == std::string_view::npos) return heard;
    size_t at = array;
    for (;;) {
        const std::string_view object = detail::JsonObjectAt(json, at);
        if (object.empty()) break;
        subtitle_translate::Cue cue;
        cue.start = detail::JsonNumberField(object, "start");
        cue.end = detail::JsonNumberField(object, "end");
        if (const auto text = subtitle_translate::JsonStringField(object, "text")) cue.text = *text;
        if (cue.end > cue.start && !cue.text.empty()) heard.cues.push_back(std::move(cue));
        const size_t after = size_t(object.data() - json.data()) + object.size();
        const size_t next = json.find('{', after);
        const size_t close = json.find(']', after);
        if (next == std::string_view::npos || (close != std::string_view::npos && close < next)) break;
        at = next;
    }
    return heard;
}

// ---- The engine's command lines ----------------------------------------------

// ---- Which build of the engine to run ---------------------------------------

// The engine ships in two builds, side by side under the speech root:
// `runtime` computes on the cores and `runtime-gpu` carries the Vulkan backend
// beside the same tools. Recognition is the single most expensive thing the
// player asks a machine to do - measured at 4.98 core-seconds per second of film
// on the cores against 0.04 with the model on the graphics card - so which build
// it is matters more than any other number here.
inline constexpr const wchar_t* kCpuRuntimeDirectory = L"runtime";
inline constexpr const wchar_t* kGpuRuntimeDirectory = L"runtime-gpu";
inline constexpr const wchar_t* kServerFileName = L"whisper-server.exe";

// What `[Speech] Gpu=` means. The graphics card is the answer worth having, so a
// setting nobody has written yet asks for it; anything but a zero keeps asking.
inline bool SpeechPrefersGpu(int iniFlag)
{
    return iniFlag != 0;
}

// A preference is not an installation. The GPU build is the optional one: it is
// what a machine has after its owner copies the Vulkan backend beside the
// tools, or nothing at all. Wanting it without having it falls back to the cores
// rather than to silence, which is what the preference is really about - nobody
// asked for the transcript not to happen.
inline bool SpeechUsesGpu(bool prefersGpu, bool gpuInstalled)
{
    return prefersGpu && gpuInstalled;
}

// The leaf under the speech root holding the engine to run.
inline const wchar_t* SpeechRuntimeSubdirectory(bool usesGpu)
{
    return usesGpu ? kGpuRuntimeDirectory : kCpuRuntimeDirectory;
}

// whisper-server keeps the model loaded, which is the whole reason for using it
// over the one-shot CLI: a film is a hundred windows, and loading a 1.5 GB model
// for each of them would cost more than the recognition.
//
// `threads` is the plan's other half: the player needs cores of its own, so the
// engine is told how many it may have rather than being left to take them all.
inline std::wstring ServerArguments(const std::wstring& modelPath, int port, int threads)
{
    std::wstring args = L"-m \"";
    args += modelPath;
    args += L"\" --host 127.0.0.1 --port ";
    args += std::to_wstring(port);
    if (threads > 0) {
        args += L" -t ";
        args += std::to_wstring(threads);
    }
    // Transcribing is what the engine does unless it is told to translate, so
    // nothing here asks for it: the player has a model for translation and
    // whisper's own -tr would be a second, worse one.
    //
    // NOT -nt. That is --no-timestamps, not --no-translate, and it does what it
    // says: with it, a verbose_json answer carries segment text with no start or
    // end at all, every window's lines then land on the film's clock at zero and
    // all but the first are dropped as already covered. SRT output hid this,
    // because that writer needs the times and forces them back.
    return args;
}

// One window of audio as 16 kHz mono signed 16-bit PCM: what the engine was
// trained on, and the only shape that needs no resampling inside it.
//
// `-ss` before `-i` so the seek lands on a keyframe instead of decoding the film
// from the start for every window.
inline std::wstring AudioArguments(const std::wstring& input, int audioStream, double start,
                                   double seconds, const std::wstring& output)
{
    wchar_t numbers[128];
    std::swprintf(numbers, std::size(numbers), L"-ss %.3f -t %.3f", start, seconds);
    std::wstring args = L"-hide_banner -loglevel error -nostdin -y ";
    args += numbers;
    args += L" -i \"";
    args += input;
    args += L"\" -map 0:a:";
    args += std::to_wstring(audioStream);
    args += L" -ac 1 -ar 16000 -c:a pcm_s16le -f wav \"";
    args += output;
    args += L"\"";
    return args;
}

} // namespace subtitle_speech
