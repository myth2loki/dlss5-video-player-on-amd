#pragma once

#include <algorithm>
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
            body += detail::Escape(cue.translation);
            body += "\\N";
            if (layout.gapSize > 0) {
                char gap[48];
                std::snprintf(gap, sizeof(gap), "{\\fs%d}\\h\\N", layout.gapSize);
                body += gap;
            }
            body += "{\\rSrc}";
            body += detail::Escape(cue.text);
        } else {
            body += hasTranslation && mode != Mode::Source ? "{\\rZh}" : "{\\rSrc}";
            body += detail::Escape(primary);
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

// ---- The prompt the model was trained on -------------------------------------

// A pair of language names and the codes the model's own template uses. The
// template inside the GGUF accepts a structured content item carrying both
// codes; llama.cpp's OpenAI endpoint cannot express that, so the instruction is
// reproduced as text (the same thing Transub-Player does with
// --no-jinja --chat-template gemma). Wording copied from the template's text
// branch, because it is what the model was trained to answer.
struct Languages {
    std::string_view sourceName = "Japanese";
    std::string_view sourceCode = "ja";
    std::string_view targetName = "Chinese";
    std::string_view targetCode = "zh-Hans";
};

// Bumped when the wording below changes: it is part of the cache key, so a
// prompt change must not read translations produced by the old one.
inline constexpr std::string_view kPromptVersion = "t1";

inline std::string OfficialPrompt(std::string_view text, const Languages& languages = {})
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
    prompt += " translation, without any additional explanations or commentary. Please translate "
              "the following ";
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
