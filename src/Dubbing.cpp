#include "Dubbing.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>

#include "Log.h"
#include "InheritedHandles.h"
#include "TextRewriter.h"
#include "Utf8Text.h"

namespace dub {
namespace {

// ---------------------------------------------------------------------------
// A child process, the way every other engine call in this codebase runs one:
// a job object so cancellation kills the tree, output to NUL because the
// synthesizer logs its progress and nobody reads it, and a poll loop so the
// dialog's Cancel is honoured within a tenth of a second.
// ---------------------------------------------------------------------------

bool RunProcess(const std::filesystem::path& executable, const std::vector<std::wstring>& arguments,
                std::stop_token stop, std::chrono::milliseconds timeout,
                const std::filesystem::path& log = {})
{
    std::wstring commandLine = L"\"" + executable.wstring() + L"\"";
    for (const std::wstring& argument : arguments) {
        commandLine += L" \"" + argument + L"\"";
    }
    // The child's chatter goes to the run's log when there is one, NUL when
    // there is not - the synthesizer logs its progress and nobody reads it,
    // but an ffmpeg that fails has its reason on stderr, and a stage that
    // swallows it leaves nobody any way to say why.
    SECURITY_ATTRIBUTES inheritSecurity{sizeof(inheritSecurity), nullptr, TRUE};
    HANDLE sink = CreateFileW(log.empty() ? L"NUL" : log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &inheritSecurity, OPEN_ALWAYS, 0, nullptr);
    if (!log.empty() && sink != INVALID_HANDLE_VALUE)
        SetFilePointer(sink, 0, nullptr, FILE_END);
    InheritedHandles inherit{sink};
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = sink;
    startup.StartupInfo.hStdError = sink;
    startup.lpAttributeList = inherit.AttributeList();
    PROCESS_INFORMATION process{};
    const BOOL created = inherit.Ready() &&
        CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
                       inherit.InheritHandles(),
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | inherit.CreationFlags(),
                       nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process);
    if (sink) CloseHandle(sink);
    if (!created) return false;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, process.hProcess);
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool ok = false;
    for (;;) {
        const DWORD wait = WaitForSingleObject(process.hProcess, 100);
        if (wait == WAIT_OBJECT_0) {
            DWORD code = 1;
            GetExitCodeProcess(process.hProcess, &code);
            ok = code == 0;
            break;
        }
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) break;
    }
    if (!ok) {
        if (job) TerminateJobObject(job, 1);
        else TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 2000);
    }
    CloseHandle(process.hProcess);
    if (job) CloseHandle(job);
    return ok;
}

// ---------------------------------------------------------------------------
// RIFF/WAVE, only the two shapes this stage actually moves: 16-bit PCM in from
// the synthesizer and the resampler, 32-bit float out to the limiter. A 24-bit
// master is deliberately not a shape here - the deliverable is encoded by the
// mux, and the audit tooling reads what it produces.
// ---------------------------------------------------------------------------

struct Pcm {
    std::vector<int16_t> samples;  // interleaved
    int rate{};
    int channels{};
    double Seconds() const { return rate && channels ? double(samples.size()) / double(rate * channels) : 0.0; }
};

struct F32 {
    std::vector<float> samples;  // interleaved stereo
    int rate{48000};
};

std::optional<Pcm> ReadWav(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    char header[12];
    if (!file.read(header, sizeof(header)) || std::memcmp(header, "RIFF", 4) != 0 ||
        std::memcmp(header + 8, "WAVE", 4) != 0)
        return std::nullopt;
    Pcm pcm;
    bool haveFormat = false;
    char chunkHeader[8];
    while (file.read(chunkHeader, sizeof(chunkHeader))) {
        const uint32_t size = uint32_t(uint8_t(chunkHeader[4])) | uint32_t(uint8_t(chunkHeader[5])) << 8 |
                              uint32_t(uint8_t(chunkHeader[6])) << 16 | uint32_t(uint8_t(chunkHeader[7])) << 24;
        if (std::memcmp(chunkHeader, "fmt ", 4) == 0) {
            char format[16];
            if (size < 16 || !file.read(format, 16)) return std::nullopt;
            const uint16_t tag = uint16_t(uint8_t(format[0])) | uint16_t(uint8_t(format[1])) << 8;
            pcm.channels = uint16_t(uint8_t(format[2])) | uint16_t(uint8_t(format[3])) << 8;
            pcm.rate = uint32_t(uint8_t(format[4])) | uint32_t(uint8_t(format[5])) << 8 |
                       uint32_t(uint8_t(format[6])) << 16 | uint32_t(uint8_t(format[7])) << 24;
            const uint16_t bits = uint16_t(uint8_t(format[14])) | uint16_t(uint8_t(format[15])) << 8;
            if (tag != 1 || bits != 16 || pcm.channels < 1 || pcm.rate < 1) return std::nullopt;
            haveFormat = true;
            if (size > 16) file.seekg(std::streamoff(size - 16), std::ios::cur);
        } else if (std::memcmp(chunkHeader, "data", 4) == 0) {
            if (!haveFormat) return std::nullopt;
            pcm.samples.resize(size / sizeof(int16_t));
            file.read(reinterpret_cast<char*>(pcm.samples.data()), std::streamsize(size));
            if (size & 1) file.seekg(1, std::ios::cur);
        } else {
            file.seekg(std::streamoff(size) + (size & 1), std::ios::cur);
        }
    }
    if (!haveFormat || pcm.samples.empty()) return std::nullopt;
    return pcm;
}

bool WriteWav16(const std::filesystem::path& path, const std::vector<int16_t>& samples,
                int rate, int channels)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    const uint32_t dataBytes = uint32_t(samples.size() * sizeof(int16_t));
    const uint32_t riffBytes = 36 + dataBytes;
    const uint16_t blockAlign = uint16_t(channels * sizeof(int16_t));
    char header[44];
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 4, &riffBytes, 4);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    const uint32_t formatSize = 16;
    std::memcpy(header + 16, &formatSize, 4);
    const uint16_t tag = 1, channelCount = uint16_t(channels);
    std::memcpy(header + 20, &tag, 2);
    std::memcpy(header + 22, &channelCount, 2);
    std::memcpy(header + 24, &rate, 4);
    const uint32_t byteRate = uint32_t(rate) * blockAlign;
    std::memcpy(header + 28, &byteRate, 4);
    std::memcpy(header + 32, &blockAlign, 2);
    const uint16_t bits = 16;
    std::memcpy(header + 34, &bits, 2);
    std::memcpy(header + 36, "data", 4);
    std::memcpy(header + 40, &dataBytes, 4);
    file.write(header, sizeof(header));
    file.write(reinterpret_cast<const char*>(samples.data()), std::streamsize(samples.size() * sizeof(int16_t)));
    return bool(file);
}

bool WriteWavFloat(const std::filesystem::path& path, const F32& mix)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    const uint32_t dataBytes = uint32_t(mix.samples.size() * sizeof(float));
    const uint32_t riffBytes = 36 + dataBytes;
    const uint16_t blockAlign = 2 * sizeof(float);
    char header[44];
    std::memcpy(header, "RIFF", 4);
    std::memcpy(header + 4, &riffBytes, 4);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    const uint32_t formatSize = 16;
    std::memcpy(header + 16, &formatSize, 4);
    const uint16_t tag = 3, channels = 2;  // IEEE float
    std::memcpy(header + 20, &tag, 2);
    std::memcpy(header + 22, &channels, 2);
    std::memcpy(header + 24, &mix.rate, 4);
    const uint32_t byteRate = uint32_t(mix.rate) * blockAlign;
    std::memcpy(header + 28, &byteRate, 4);
    std::memcpy(header + 32, &blockAlign, 2);
    const uint16_t bits = 32;
    std::memcpy(header + 34, &bits, 2);
    std::memcpy(header + 36, "data", 4);
    std::memcpy(header + 40, &dataBytes, 4);
    file.write(header, sizeof(header));
    file.write(reinterpret_cast<const char*>(mix.samples.data()), std::streamsize(dataBytes));
    return bool(file);
}

// ---------------------------------------------------------------------------
// Trimming. Not cosmetic, budget: the engine pads a one-character "Wow!" with
// nearly a quarter second of tail, and left in, the fit spends the whole window
// compressing silence while the mix cuts real speech off the end.
// ---------------------------------------------------------------------------

double LevelOf(int16_t sample) { return std::abs(double(sample)) / 32768.0; }

void TrimSilence(Pcm& pcm)
{
    const double gate = std::pow(10.0, dub_fit::kTrimSilenceDb / 20.0);
    const size_t channels = size_t(pcm.channels);
    const size_t frames = pcm.samples.size() / channels;
    size_t first = frames, last = 0;  // [first, last) is the loud region
    for (size_t frame = 0; frame < frames; ++frame)
        for (size_t c = 0; c < channels; ++c)
            if (LevelOf(pcm.samples[frame * channels + c]) > gate) {
                if (first == frames) first = frame;
                last = frame + 1;
                break;
            }
    // All silence stays as it is: the caller reports an empty voice, which is
    // the honest version of a line the engine never spoke.
    if (first == frames) return;
    const size_t pad = size_t(dub_fit::kTrimPadSeconds * double(pcm.rate));
    const size_t lo = first > pad ? first - pad : 0;
    const size_t hi = std::min(frames, last + pad);
    if (hi <= lo) return;
    // Cut the tail, then the head: two erases, because erase removes the range
    // it is given, and what this wants to remove is everything *outside*
    // [lo, hi) - deleting that range itself would keep the silence and throw
    // the speech away.
    pcm.samples.erase(pcm.samples.begin() + std::ptrdiff_t(hi * channels), pcm.samples.end());
    pcm.samples.erase(pcm.samples.begin(), pcm.samples.begin() + std::ptrdiff_t(lo * channels));
}

// The delivered level the mixer and the audit agree on: RMS of 20 ms blocks,
// taken at the 75th percentile of the line. A whole-file RMS instead pushes a
// quiet line with a long silent tail up by whole decibels.
double DeliveredLevel(const std::vector<int16_t>& samples, int rate, int channels)
{
    const size_t block = std::max(size_t(1), size_t(dub_fit::kLevelBlockSeconds * double(rate)) * channels);
    std::vector<double> blocks;
    for (size_t at = 0; at + block <= samples.size(); at += block) {
        double energy = 0.0;
        for (size_t i = 0; i < block; ++i) {
            const double v = double(samples[at + i]) / 32768.0;
            energy += v * v;
        }
        blocks.push_back(std::sqrt(energy / double(block)));
    }
    if (blocks.empty()) return 0.0;
    std::sort(blocks.begin(), blocks.end());
    return blocks[size_t(double(blocks.size() - 1) * dub_fit::kLevelPercentile)];
}

std::wstring WideFromUtf8(const std::string& text)
{
    if (text.empty()) return {};
    const int bytes = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0);
    std::wstring wide(size_t(bytes), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), int(text.size()), wide.data(), bytes);
    return wide;
}

std::string Utf8FromWide(const std::wstring& wide)
{
    if (wide.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string narrow(size_t(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), int(wide.size()), narrow.data(), bytes, nullptr, nullptr);
    return narrow;
}

// ---------------------------------------------------------------------------
// The bilingual ASS the text stage wrote.
// ---------------------------------------------------------------------------

double ParseAssTime(const std::string& text)
{
    // H:MM:SS.cc, the timestamps AppendAssTime writes: four runs of digits
    // with the separators between them. Read by hand - the scanf family trips
    // this build's warnings-as-errors on nothing.
    int numbers[4]{};
    int filled = 0;
    for (size_t i = 0; i < text.size() && filled < 4;) {
        if (text[i] >= '0' && text[i] <= '9') {
            int value = 0;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9')
                value = value * 10 + (text[i++] - '0');
            numbers[filled++] = value;
        } else {
            ++i;
        }
    }
    if (filled != 4) return 0.0;
    return double(numbers[0]) * 3600.0 + double(numbers[1]) * 60.0 +
           double(numbers[2]) + double(numbers[3]) / 100.0;
}

// The words under the translation switch, and nothing else. A line with no
// translation has no dub - speaking the source's language at it would not be
// one. A line with no switches at all is a file that was never bilingual, and
// its words are taken as they stand.
std::string DubTextOf(const std::string& raw)
{
    static constexpr std::string_view kZh = "{\\rZh}";
    static constexpr std::string_view kSrc = "{\\rSrc}";
    std::string_view text = raw;
    if (text.rfind(kZh, 0) == 0) {
        text.remove_prefix(kZh.size());
    } else if (text.find(kZh) != std::string_view::npos) {
        text.remove_prefix(text.find(kZh) + kZh.size());
    } else if (text.find(kSrc) != std::string_view::npos) {
        return {};  // the source's own words only: nothing to speak
    }
    // The translation ends where the source's switch begins; everything after
    // it is the other language's words, not this line's.
    if (const size_t src = text.find(kSrc); src != std::string_view::npos)
        text = text.substr(0, src);
    std::string words;
    words.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        // The writer escaped literal braces as \{ and \}; unescape those first,
        // then treat any remaining brace as the start of an overlay tag.
        if (text.compare(i, 2, "\\{") == 0 || text.compare(i, 2, "\\}") == 0) {
            words += text[i + 1];
            ++i;
            continue;
        }
        if (text[i] == '{') {  // an overlay tag runs to its brace
            while (i < text.size() && text[i] != '}') ++i;
            continue;
        }
        if (text.compare(i, 2, "\\N") == 0 || text.compare(i, 2, "\\n") == 0 ||
            text.compare(i, 2, "\\h") == 0) {
            // A wrap inside Chinese is not a space in it: the text stage breaks
            // a long line to fit the screen, and the voice still has to read the
            // sentence as one run of characters.
            const bool wide = !words.empty() &&
                              static_cast<unsigned char>(words.back()) >= 0x80 &&
                              i + 2 < text.size() &&
                              static_cast<unsigned char>(text[i + 2]) >= 0x80;
            if (!wide) words += ' ';
            ++i;
            continue;
        }
        words += text[i];
    }
    // The empty run left between the two switches, and the spaces a wrap left.
    while (!words.empty() && (words.back() == ' ' || words.back() == '\r')) words.pop_back();
    size_t at = 0;
    while (at < words.size() && words[at] == ' ') ++at;
    words.erase(0, at);
    return words;
}

// The names a line addresses: "You're a jerk, Tom." addresses Tom, "Look,
// Celia, we have to..." addresses Celia. Only vocatives count - a capitalized
// word after a comma and followed by punctuation - which is what keeps "Tom is
// a jerk" (Tom as subject) out of the answer.
std::vector<std::string> AddressedNames(const std::string& text)
{
    std::vector<std::string> names;
    for (size_t at = 1; at + 2 < text.size(); ++at) {
        if (text[at - 1] != ',' && text[at - 1] != '-') continue;
        if (text[at] != ' ') continue;
        const size_t first = at + 1;
        if (!std::isupper(static_cast<unsigned char>(text[first]))) continue;
        size_t end = first + 1;  // past the capital; the rest of the name is lower case
        while (end < text.size() && std::islower(static_cast<unsigned char>(text[end]))) ++end;
        if (end - first < 3) continue;
        if (end < text.size()) {
            const char next = text[end];
            if (next != '.' && next != ',' && next != '!' && next != '?' && next != ' ' &&
                next != '\'' && next != '"')
                continue;
        }
        names.push_back(text.substr(first, end - first));
    }
    return names;
}

// Which two lines to seed the voices with, chosen from the dialogue instead of
// from the audio.
//
// A line that addresses somebody is spoken by somebody else. That is the one
// reliable fact in a conversation, and it is exactly what the embedding cannot
// supply on this material: measured, the two leads sit 0.18 apart as centroids
// while lines of one speaker sit up to 0.39 from each other, so no threshold and
// no unsupervised split reliably separates them - the search keeps landing on
// "one of them and everyone else", which renders as a woman spoken by a man.
//
// The two names addressed in the most separate lines are taken as the cast, and
// one line that addresses each is used as a seed - that line belongs to the
// other person, which is what makes the two seeds different people. A name only
// ever addressed inside one line is dropped: "Roger, Roger." is a robot
// acknowledging, not a character.
std::vector<size_t> SeedLinesByAddress(const std::vector<Line>& lines)
{
    std::vector<std::pair<std::string, std::vector<size_t>>> mentioned;
    const auto find = [&](const std::string& name) -> std::vector<size_t>* {
        for (auto& entry : mentioned)
            if (entry.first == name) return &entry.second;
        return nullptr;
    };
    for (size_t index = 0; index < lines.size(); ++index)
        for (const std::string& name : AddressedNames(lines[index].source)) {
            std::vector<size_t>* where = find(name);
            if (!where) mentioned.emplace_back(name, std::vector<size_t>{index});
            else where->push_back(index);  // repeats count: they are how a name is ruled out below
        }
    // A name addressed twice inside one line is a word being repeated, not a
    // character being spoken to: "Roger, Roger." is a robot acknowledging. A
    // character's name spreads across lines, and is remembered by how many.
    std::vector<std::pair<size_t, size_t>> ranked;  // (lines, mentions)
    for (const auto& entry : mentioned) {
        std::vector<size_t> distinct = entry.second;
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        if (entry.second.size() > distinct.size()) continue;
        ranked.emplace_back(distinct.size(), entry.second.front());
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& left, const auto& right) { return left.first > right.first; });
    std::vector<size_t> seeds;
    for (const auto& [spread, first] : ranked) {
        if (seeds.size() >= 2) break;
        if (seeds.empty() || seeds.front() != first) seeds.push_back(first);
    }
    return seeds.size() == 2 ? seeds : std::vector<size_t>{};
}

struct Spoken {
    double anchor{};               // where the voice starts (start, moved back by the shift)
    double finish{};               // where it actually stops: the next line borrows the gap after this
    double window{};               // what it had to fit
    std::vector<int16_t> samples;  // the fitted line, 48 kHz stereo
    double level{};                // delivered level before the mix aligns it
};

// The same line in the source language, without the tags: what the words were
// before translation. Only the cast is read out of it - see SeedByAddress.
std::string SourceTextOf(const std::string& raw)
{
    static constexpr std::string_view kSrc = "{\\rSrc}";
    const size_t at = raw.find(kSrc);
    std::string text = at == std::string::npos ? raw : raw.substr(at + kSrc.size());
    std::string words;
    words.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '{') {
            while (i < text.size() && text[i] != '}') ++i;
            continue;
        }
        if (text.compare(i, 2, "\\N") == 0 || text.compare(i, 2, "\\n") == 0 ||
            text.compare(i, 2, "\\h") == 0) {
            words += ' ';
            ++i;
            continue;
        }
        words += text[i];
    }
    while (!words.empty() && (words.back() == ' ' || words.back() == '\r')) words.pop_back();
    size_t start = 0;
    while (start < words.size() && words[start] == ' ') ++start;
    return words.substr(start);
}

}  // namespace

// Whether a cue is a stage direction rather than words to speak. "*Gunshot*"
// and "[door slams]" are what subtitle files use for sounds, and a dub that
// reads them out says "gunshot" over a gunshot. The source's own text decides,
// because the translation routinely drops the markers (measured: *Gunshot* came
// back as 枪声, with nothing left to recognise) - so this reads the {\rSrc} side
// and asks whether the whole run is wrapped.
bool IsStageDirection(const std::string& raw)
{
    static constexpr std::string_view kSrc = "{\\rSrc}";
    const auto wrapped = [](std::string_view text) {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\r')) text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.remove_suffix(1);
        if (text.size() < 3) return false;
        const std::string_view opens{"*[【（("};
        const std::string_view closes{"*]】）)"};
        const size_t index = opens.find(text.front());
        if (index == std::string_view::npos) return false;
        // A pair only counts when the two are the same kind: '*' closes '*',
        // '[' closes ']', and so on.
        return text.back() == closes[index];
    };
    const size_t at = raw.find(kSrc);
    if (at != std::string::npos) return wrapped(std::string_view(raw).substr(at + kSrc.size()));
    // No source side: the line is a file that was never bilingual, so all we
    // have is what would be spoken.
    return wrapped(DubTextOf(raw));
}

// A cue that names a sound instead of saying words, and that the source did not
// mark with brackets: the translation drops those markers often enough that the
// markers alone miss one every few minutes ("*Ominous music*" came back as
// "令人不安的音乐", with the asterisks gone). The Python pipeline kept a
// hand-written override table for exactly these; this is the same idea as a
// rule - the whole line is a short noun phrase ending in a sound, and nothing in
// it addresses anybody.
//
// Deliberately conservative: a line that merely mentions music while somebody
// talks about it ("我喜欢这首音乐") stays, because it contains a person and a
// verb. Missing a sound description costs one odd word in the dub; eating a
// real line costs the audience the line.
bool IsSoundDescription(const std::string& text)
{
    static constexpr std::string_view kNouns[] = {
        "音乐", "音效", "声响", "枪声", "笑声", "掌声", "脚步声", "尖叫声", "爆炸声",
        "门铃声", "铃声", "风声", "雨声", "回声", "回响", "嘈杂声", "噪音"};
    static constexpr std::string_view kPeople[] = {
        "我", "你", "他", "她", "它", "们", "说", "想", "要", "是", "在", "把", "被", "让"};
    // Counted in code points, not bytes: eight Chinese characters is a phrase,
    // twenty is a sentence.
    size_t characters = 0;
    for (const unsigned char lead : text) characters += (lead & 0xC0) != 0x80 ? 1 : 0;
    if (characters == 0 || characters > 10) return false;
    for (const auto person : kPeople)
        if (text.find(person) != std::string::npos) return false;
    for (const auto noun : kNouns)
        if (text.size() >= noun.size() && text.compare(text.size() - noun.size(), noun.size(), noun) == 0)
            return true;
    return false;
}

// ---- One line per sentence, not per cue ------------------------------------
// Whisper cuts a sentence where the speaker paused, and the text stage writes
// one cue per cut. Spoken cue by cue that is a different performance: every cut
// became its own line, so it got its own voice sample, its own window and its own
// trip through the synthesizer. Measured on the 3-minute excerpt, "Look, Celia,
// we have to follow our passions. You have your robotics and I just want to be
// awesome in space." arrived as four cues and was spoken in four voices -
// 278 Hz, 124 Hz, 107 Hz, 270 Hz off one reference clip - because four words
// carry too little timbre for the embedding to place, and because a half-second
// window cannot hold the Chinese for half a sentence, which is what a listener
// hears as the line being cut off.
//
// So a cue that stops mid-sentence and is followed within kUtteranceGap by the
// cue that goes on is one line - here, and in the text stage the subtitle comes
// from, so the two cannot drift apart.
// The rule itself is the text stage's now - subtitle_translate::MergeIntoSentences,
// applied to the cues before anything else reads them, so the subtitle track and
// the voice are cut the same way. This is the same call on this stage's own
// lines: the merge is normally a no-op by now, and keeping it means a track
// produced elsewhere (an older ASS, a release's own subtitles) is still spoken
// one sentence at a time. `text` is what is spoken, `source` what the decision
// is made on - the translation punctuates a fragment with a full stop often
// enough that the Chinese would never join.
std::vector<Line> MergeSentences(std::vector<Line> lines)
{
    std::vector<subtitle_translate::Cue> cues;
    cues.reserve(lines.size());
    for (const Line& line : lines) {
        subtitle_translate::Cue cue;
        cue.start = line.start;
        cue.end = line.end;
        cue.text = line.source.empty() ? line.text : line.source;
        cue.translation = line.source.empty() ? std::string{} : line.text;
        cues.push_back(std::move(cue));
    }
    cues = subtitle_translate::MergeIntoSentences(
        std::move(cues), subtitle_translate::SentenceRules{dub_fit::kUtteranceGap,
                                                           dub_fit::kUtteranceMaxSeconds});
    std::vector<Line> merged;
    merged.reserve(cues.size());
    for (subtitle_translate::Cue& cue : cues) {
        Line line;
        line.start = cue.start;
        line.end = cue.end;
        if (cue.translation.empty()) {
            line.text = std::move(cue.text);
        } else {
            line.text = std::move(cue.translation);
            line.source = std::move(cue.text);
        }
        merged.push_back(std::move(line));
    }
    return merged;
}

std::vector<Line> LinesFromAss(const std::string& ass)
{
    std::vector<Line> lines;
    size_t at = 0;
    while (true) {
        const size_t eol = ass.find('\n', at);
        const std::string line = ass.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (line.rfind("Dialogue:", 0) == 0) {
            // Nine commas put us at the text; the two times come before them.
            int commas = 0;
            size_t field[3] = {9, 0, 0};  // text start, then the two time fields' ends
            std::string fields[2];
            size_t position = std::string("Dialogue:").size();
            for (size_t i = std::string("Dialogue:").size(); i < line.size(); ++i) {
                if (line[i] != ',') continue;
                if (commas == 1) fields[0] = line.substr(position, i - position);
                if (commas == 2) fields[1] = line.substr(position, i - position);
                position = i + 1;
                if (++commas == 9) { field[0] = i + 1; break; }
            }
            if (commas == 9) {
                Line spokenLine;
                spokenLine.start = ParseAssTime(fields[0]);
                spokenLine.end = ParseAssTime(fields[1]);
                const std::string raw = line.substr(field[0]);
                spokenLine.text = DubTextOf(raw);
                spokenLine.source = SourceTextOf(raw);
                // A sound is not a line: the subtitle keeps it, the voice does not.
                if (!spokenLine.text.empty() && !IsStageDirection(raw) &&
                    !IsSoundDescription(spokenLine.text) && spokenLine.end > spokenLine.start)
                    lines.push_back(std::move(spokenLine));
            }
        }
        if (eol == std::string::npos) break;
        at = eol + 1;
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const Line& a, const Line& b) { return a.start < b.start; });
    return MergeSentences(std::move(lines));
}

// The line's size in the pace model's terms - one character per code point, plus
// the punctuation that costs a pause - turned into a generation cap. The engine
// is allowed the line's own predicted length plus headroom, never the flat 75 s
// ceiling: overshoot is audio the mix then has to compress, and compression is
// what a listener hears as the voice changing under them.
int NewTokensFor(const std::string& text)
{
    size_t characters = 0;
    size_t commas = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const size_t width = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : 4;
        if (width == 1) {
            if (lead == ',') ++commas;
        } else if (i + width <= text.size()) {
            const std::string_view piece(text.data() + i, width);
            if (piece == "\xEF\xBC\x8C" || piece == "\xE3\x80\x81") ++commas;  // ，、
        }
        ++characters;
        i += width;
    }
    const double predicted = dub_fit::PredictSeconds(dub_fit::kQwen3TtsPace, characters, commas);
    const double frames = std::ceil(predicted * dub_fit::kTtsFramesPerSecond * dub_fit::kTokenHeadroom);
    return int(std::clamp(frames, double(dub_fit::kMinNewTokens), double(dub_fit::kMaxNewTokens)));
}

// ---- Who speaks, and what that voice sounds like ---------------------------
// The Python pipeline answered both from an ECAPA embedding computed on a
// separated vocal stem, with a 10-15 s reference per speaker. The player has
// neither an embedder nor a separator, so the two answers come from a pitch
// track instead: the film's voices part where male and female speech part
// (165 Hz), and the clearest cue on each side becomes that side's reference.
// Two voices rather than four - but a female character stops being read by a
// man, which is what one reference per film gets wrong on every second line.
double PitchOfFrame(const int16_t* frame, size_t count, int rate)
{
    if (count < 64 || rate <= 0) return 0.0;
    const size_t tauMin = std::max<size_t>(2, size_t(double(rate) / 400.0));
    const size_t tauMax = std::min(count - 1, size_t(double(rate) / 70.0));
    if (tauMax <= tauMin + 2) return 0.0;
    std::vector<double> difference(tauMax + 1, 0.0);
    for (size_t tau = 1; tau <= tauMax; ++tau) {
        double sum = 0.0;
        for (size_t i = 0; i + tau < count; ++i) {
            const double delta = double(frame[i]) - double(frame[i + tau]);
            sum += delta * delta;
        }
        difference[tau] = sum;
    }
    // Cumulative mean normalised difference: the first dip under the threshold
    // is the period, which is why this does not report an octave up the way a
    // plain "argmax of the autocorrelation" does. Measured with the same
    // estimator the Python pipeline kept in _yin.py.
    std::vector<double> normalised(tauMax + 1, 1.0);
    double running = 0.0;
    for (size_t tau = 1; tau <= tauMax; ++tau) {
        running += difference[tau];
        normalised[tau] = running > 0.0 ? difference[tau] * double(tau) / running : 1.0;
    }
    for (size_t tau = tauMin; tau <= tauMax; ++tau) {
        if (normalised[tau] >= 0.35) continue;
        while (tau + 1 <= tauMax && normalised[tau + 1] < normalised[tau]) ++tau;
        return double(rate) / double(tau);
    }
    return 0.0;
}

// One cue's pitch: the median of the frames the tracker trusts, and how many of
// the cue's frames those were. A cue that is mostly music has a low fraction,
// and its median means nothing.
struct CuePitch {
    double f0{};
    double voiced{};
};

CuePitch MeasurePitch(const std::vector<int16_t>& mono, int rate, double start, double end)
{
    CuePitch pitch;
    if (mono.empty() || rate <= 0) return pitch;
    const size_t window = std::max<size_t>(64, size_t(0.040 * double(rate)));
    const size_t hop = std::max<size_t>(1, size_t(0.020 * double(rate)));
    const size_t first = size_t(std::max(0.0, start) * double(rate));
    const size_t span = size_t(std::min(std::max(0.0, end - start), 8.0) * double(rate));
    const size_t last = std::min(mono.size(), first + span);
    std::vector<double> found;
    size_t frames = 0;
    for (size_t at = first; at + window <= last; at += hop) {
        ++frames;
        const double f0 = PitchOfFrame(mono.data() + at, window, rate);
        if (f0 >= 70.0 && f0 <= 400.0) found.push_back(f0);
    }
    if (!frames) return pitch;
    pitch.voiced = double(found.size()) / double(frames);
    if (found.empty()) return pitch;
    std::sort(found.begin(), found.end());
    pitch.f0 = found[found.size() / 2];
    return pitch;
}

// ---- The stems -------------------------------------------------------------
// One separation, up front. Both answers the dub needs come out of it: who is
// speaking is measured on the vocal stem (a speaker embedding computed over a
// mix measures the music), and each voice's reference clip is cut from that
// stem too - a reference carrying music made the engine repeat a line seven
// times. The backing stem is the film's score without its dialogue, which is
// the only thing that may be laid under the Chinese voice.
struct Stems {
    std::filesystem::path vocals;
    std::filesystem::path backing;
    bool separated{false};
};

Stems SeparateStems(const Plan& plan, const std::filesystem::path& ffmpeg, std::stop_token stop)
{
    Stems stems;
    if (plan.stemSeparator.empty() || plan.stemModel.empty()) return stems;
    std::error_code error;
    if (!std::filesystem::exists(plan.stemSeparator, error) ||
        !std::filesystem::exists(plan.stemModel, error)) {
        LOG("Dub: no stem separator at " << utf8_text::FromWide(plan.stemSeparator.wstring())
            << "; the voices are measured on the mix and no bed is laid");
        return stems;
    }
    // The separator opens a wav, not a film.
    const auto source = plan.work / L"stem-source.wav";
    if (!RunProcess(ffmpeg, {L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror", L"-y",
                             L"-i", plan.mediaPath.wstring(), L"-vn", L"-map", L"0:a:0",
                             L"-ac", L"2", L"-ar", L"44100", L"-c:a", L"pcm_s16le", source.wstring()},
                    stop, std::chrono::minutes(30), plan.logFile))
        return stems;
    const auto directory = plan.work / L"stems";
    const auto arguments = [&](std::initializer_list<std::wstring> extra) {
        std::vector<std::wstring> command{L"--model", plan.stemModel.wstring(),
                                          L"--input", source.wstring(), L"--out", directory.wstring(),
                                          L"--two-stems", L"vocals"};
        command.insert(command.end(), extra);
        return command;
    };
    // The separator picks its own backend; the retina of this machine has a GPU
    // the player's own neural stages may be holding, in which case it falls over
    // to the CPU - so the retry names the CPU rather than assuming the failure.
    if (!RunProcess(plan.stemSeparator, arguments({}), stop, std::chrono::minutes(60), plan.logFile)) {
        if (stop.stop_requested()) return stems;
        LOG("Dub: separating on the accelerator failed; retrying on the CPU");
        if (!RunProcess(plan.stemSeparator, arguments({L"--device", L"cpu"}), stop,
                        std::chrono::minutes(120), plan.logFile))
            return stems;
    }
    stems.vocals = directory / L"vocals.wav";
    stems.backing = directory / L"no_vocals.wav";
    stems.separated = std::filesystem::exists(stems.vocals, error);
    LOG(stems.separated ? "Dub: the voice is separated from the backing"
                        : "Dub: the separator wrote no vocal stem");
    return stems;
}

// A cue's audio as the embedder eats it: 16 kHz mono in [-1, 1].
std::vector<float> Slice(const std::vector<int16_t>& mono, int rate, double start, double end)
{
    std::vector<float> samples;
    if (mono.empty() || rate <= 0) return samples;
    const size_t first = size_t(std::max(0.0, start) * double(rate));
    const size_t last = std::min(mono.size(), size_t(std::max(0.0, end) * double(rate)));
    if (last <= first) return samples;
    samples.resize(last - first);
    for (size_t index = 0; index < samples.size(); ++index)
        samples[index] = float(double(mono[first + index]) / 32768.0);
    return samples;
}

// The voices, from the embeddings.
//
// Two means, not a threshold. Every pair of lines is tried as a seed, Lloyd's
// runs from each, and the partition with the least inertia wins - which is the
// one where each line is nearer its own mean than the other. On the film's own
// dialogue that recovers the speakers the dialogue names: all four of the
// woman's lines land in one cluster and five of the man's six in the other,
// where a cosine threshold (0.125, 0.55, anything between) either put every
// line in its own voice or put her lines in his.
//
// Splitting again is a decision with a measurement behind it: two sub-means
// closer than kClusterSplitCeiling are one actor heard from two distances, and
// splitting him would hand the film two timbres for one person.
//
// A cue too short to measure, or one the graph refused, keeps -1 and takes a
// neighbour's voice below: the line is still spoken by somebody.
namespace {

// One k-means solution: which mean each point belongs to, the means themselves
// (unit length, so a comparison is a dot product), and how far the points sit
// from their own mean. Inertia is what picks between seed pairs.
struct Partition
{
    std::vector<int> assignment;
    std::vector<std::vector<float>> means;
    double inertia{0.0};
};

std::vector<float> UnitMean(const std::vector<std::vector<float>>& points, const std::vector<int>& assignment,
                            int slot)
{
    std::vector<float> mean;
    size_t count = 0;
    for (size_t index = 0; index < points.size(); ++index) {
        if (assignment[index] != slot) continue;
        if (mean.empty()) mean.assign(points[index].size(), 0.0f);
        for (size_t axis = 0; axis < mean.size(); ++axis) mean[axis] += points[index][axis];
        ++count;
    }
    double length = 0.0;
    for (const float value : mean) length += double(value) * double(value);
    length = std::sqrt(length);
    if (length > 0.0)
        for (float& value : mean) value = float(double(value) / length);
    return mean;
}

Partition Refine(const std::vector<std::vector<float>>& points, std::vector<std::vector<float>> means)
{
    Partition answer;
    answer.assignment.assign(points.size(), 0);
    for (int iteration = 0; iteration < 40; ++iteration) {
        bool moved = false;
        for (size_t index = 0; index < points.size(); ++index) {
            int best = 0;
            double bestScore = -2.0;
            for (size_t slot = 0; slot < means.size(); ++slot) {
                const double score = speaker::Similarity(points[index], means[slot]);
                if (score > bestScore) { bestScore = score; best = int(slot); }
            }
            if (answer.assignment[index] != best) { answer.assignment[index] = best; moved = true; }
        }
        bool emptied = false;
        for (size_t slot = 0; slot < means.size(); ++slot) {
            bool used = false;
            for (const int value : answer.assignment)
                if (value == int(slot)) { used = true; break; }
            if (!used) { emptied = true; break; }
        }
        if (emptied) break;  // a mean with no lines is not a voice; keep the last good one
        for (size_t slot = 0; slot < means.size(); ++slot) means[slot] = UnitMean(points, answer.assignment, int(slot));
        if (!moved) break;
    }
    answer.means = means;
    for (size_t index = 0; index < points.size(); ++index) {
        double bestScore = -2.0;
        for (size_t slot = 0; slot < means.size(); ++slot)
            bestScore = std::max(bestScore, speaker::Similarity(points[index], means[slot]));
        answer.inertia += 1.0 - bestScore;
    }
    return answer;
}

// How well a partition would be drawn as two islands: each point wants to be
// near its own cluster and far from the other. This is what picks the winner,
// not inertia.
//
// Inertia cannot do it. Measured on this film: the partition with the least
// inertia put 24 of 26 lines in one cluster and left the two odd ones alone,
// because that IS the least-total-distance answer when the two leads are 0.18
// apart and one of them speaks eleven times more than the other. It renders as
// "every line in one voice", which is the symptom a listener reported. Silhouette
// asks the question that matters - is each line closer to its own island than to
// the other - and rewards the balanced answer.
double Silhouette(const std::vector<std::vector<float>>& points, const std::vector<int>& assignment, int clusters)
{
    double total = 0.0;
    size_t counted = 0;
    for (size_t index = 0; index < points.size(); ++index) {
        double inside = 0.0, outside = 2.0;
        size_t insideCount = 0;
        for (int slot = 0; slot < clusters; ++slot) {
            double sum = 0.0;
            size_t count = 0;
            for (size_t other = 0; other < points.size(); ++other) {
                if (other == index || assignment[other] != slot) continue;
                sum += 1.0 - speaker::Similarity(points[index], points[other]);
                ++count;
            }
            if (slot == assignment[index]) { inside = count ? sum / double(count) : 0.0; insideCount = count; }
            else if (count) outside = std::min(outside, sum / double(count));
        }
        if (!insideCount || outside > 1.5) continue;
        total += (outside - inside) / std::max(inside, outside);
        ++counted;
    }
    return counted ? total / double(counted) : -1.0;
}

// k-means with k = 2, seeded by the two lines that are least like each other.
//
// The seed used to be the farthest-point sample from line 0, and line 0 is
// whoever speaks first - on this film a robot announcing an engine start. From
// there the search settled on "the robot and everything else", which is a
// two-cluster partition and also the one that puts a woman's lines in a man's
// voice. The least-similar pair cannot make that mistake: whatever those two
// lines are, they are two voices, and both are then represented.
//
// The pairs are tried in order of how unlike each other they are, and the
// winner has to be a real split - each side holding at least a fifth of the
// lines - because one voice plus its outliers is the answer that makes a film
// sound like it has one actor. Among those, silhouette decides: is every line
// nearer its own island than the other.
Partition Split(const std::vector<std::vector<float>>& points,
                const std::vector<std::vector<float>>& seeds = {})
{
    // Given two lines the dialogue itself vouches for, only one solution is
    // worth trying: those two are different people, so the split is to ask which
    // of them each line sounds like.
    //
    // And it is asked once, with no refinement. Refining on this material makes
    // it worse, measured: the embedding puts two lines of one speaker as far
    // apart as 0.05 and two different speakers as close as 0.02, so every round
    // of "recompute the mean, reassign" drags in lines that were right and moves
    // the mean onto them. Straight comparison against the two seeded lines
    // assigns 25 of this film's 26 lines to the speaker the dialogue names; with
    // refinement it assigns 14.
    if (seeds.size() == 2) {
        Partition attempt;
        attempt.means = seeds;
        for (const auto& point : points) {
            const double first = speaker::Similarity(point, seeds[0]);
            const double second = speaker::Similarity(point, seeds[1]);
            attempt.assignment.push_back(first >= second ? 0 : 1);
        }
        return attempt;
    }

    std::vector<std::pair<double, std::pair<size_t, size_t>>> pairs;
    pairs.reserve(points.size() * points.size() / 2);
    for (size_t first = 0; first < points.size(); ++first)
        for (size_t second = first + 1; second < points.size(); ++second)
            pairs.emplace_back(speaker::Similarity(points[first], points[second]),
                               std::make_pair(first, second));
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });

    const size_t floor = std::max<size_t>(2, points.size() / 5);
    const size_t tries = std::min<size_t>(pairs.size(), 24);
    Partition best;
    bool have = false;
    double bestScore = -2.0;
    for (size_t attemptIndex = 0; attemptIndex < tries; ++attemptIndex) {
        const auto& [_, who] = pairs[attemptIndex];
        Partition attempt = Refine(points, {points[who.first], points[who.second]});
        size_t left = 0, right = 0;
        for (const int value : attempt.assignment) (value == 0 ? left : right) += 1;
        if (left < floor || right < floor) continue;
        const double score = Silhouette(points, attempt.assignment, 2);
        if (!have || score > bestScore) {
            best = attempt;
            bestScore = score;
            have = true;
        }
    }
    return best;
}

}  // namespace

std::vector<int> ClusterByEmbedding(const std::vector<Line>& lines, const std::vector<int16_t>& mono,
                                    int rate, speaker::Embedder& embedder, int& voices,
                                    std::vector<std::vector<float>>& vectors,
                                    std::vector<std::vector<float>>& centroids,
                                    std::vector<size_t>& anchors)
{
    std::vector<int> label(lines.size(), -1);
    std::vector<std::vector<float>> points;
    std::vector<size_t> which;  // points[i] is lines[which[i]]
    for (size_t index = 0; index < lines.size(); ++index) {
        // The cue, or half a second of it when the cue is a two-word shout: the
        // embedding needs kMinimumSeconds of audio and a cue is allowed to be
        // shorter than that.
        const double span = std::min(std::max(lines[index].end - lines[index].start, 0.6), 8.0);
        const auto vector = embedder.Embed(Slice(mono, rate, lines[index].start, lines[index].start + span));
        if (vector.empty()) continue;
        points.push_back(vector);
        which.push_back(index);
    }
    voices = 0;
    if (points.size() < 2) {
        for (const size_t index : which) label[index] = 0;
        voices = points.empty() ? 0 : 1;
        return label;
    }
    {
        double lowest = 2.0, highest = -2.0;
        for (size_t a = 0; a < points.size(); ++a)
            for (size_t b = a + 1; b < points.size(); ++b) {
                const double score = speaker::Similarity(points[a], points[b]);
                lowest = std::min(lowest, score);
                highest = std::max(highest, score);
            }
        LOG("Dub: " << points.size() << " embeddings, pairwise cosine " << lowest << " .. " << highest);
    }

    // The film's two leads, then each lead's own cluster split again when its
    // lines really are two people. A three-hander (this film has a robot) is
    // found this way without anyone saying how many actors there are.
    //
    // When the dialogue names its characters, its own lines seed the split: a
    // line that addresses Tom is spoken by somebody else, so that line and the
    // one that addresses Celia are two different people by construction.
    std::vector<std::vector<float>> seeds;
    for (const size_t index : SeedLinesByAddress(lines)) {
        for (size_t at = 0; at < which.size(); ++at)
            if (which[at] == index) { seeds.push_back(points[at]); anchors.push_back(index); break; }
    }
    if (seeds.size() == 2) LOG("Dub: the dialogue names its cast; seeding the two voices from it");
    std::vector<Partition> settled;
    {
        Partition whole = Split(points, seeds);
        LOG("Dub: k-means settled on " << whole.means.size() << " mean(s), inertia " << whole.inertia);
        if (whole.means.size() < 2) {
            for (const size_t index : which) label[index] = 0;
            voices = 1;
            return label;
        }
        settled.push_back(whole);
    }
    // A third and fourth voice were tried here once (splitting each cluster
    // again), and they made the film worse: the second split cut one actor into
    // two timbres and left clusters of two lines that could not fill a reference
    // clip. Two means is what the dialogue can vouch for - it names two people -
    // and a robot's three clipped lines joining one of them is a smaller fault
    // than a person changing voice between scenes.

    // Every line goes to the mean it sounds most like, computed once from the
    // final means rather than stitched together from per-cluster assignments:
    // the stitching left lines with no owner at all whenever a cluster had been
    // split again, and those fell back to a neighbour's voice.
    std::vector<std::vector<float>> means;
    for (const Partition& part : settled)
        for (const std::vector<float>& mean : part.means) means.push_back(mean);
    voices = int(means.size());
    centroids = means;
    vectors.assign(lines.size(), {});
    for (size_t index = 0; index < points.size(); ++index) vectors[which[index]] = points[index];
    for (size_t index = 0; index < points.size(); ++index) {
        int best = 0;
        double bestScore = -2.0;
        for (size_t slot = 0; slot < means.size(); ++slot) {
            const double score = speaker::Similarity(points[index], means[slot]);
            if (score > bestScore) { bestScore = score; best = int(slot); }
        }
        label[which[index]] = best;
    }
    for (size_t index = 0; index < label.size(); ++index)
        LOG("Dub: line " << index << " -> voice " << label[index] << "  "
                         << lines[index].source.substr(0, 40));

    // A cue the graph refused takes its nearest neighbour's voice: the line is
    // still spoken by somebody, and the next cue is the best evidence there is
    // when this one was music.
    for (size_t index = 0; index < label.size(); ++index) {
        if (label[index] >= 0) continue;
        for (size_t distance = 1; distance < label.size() && label[index] < 0; ++distance) {
            if (index >= distance && label[index - distance] >= 0) label[index] = label[index - distance];
            else if (index + distance < label.size() && label[index + distance] >= 0)
                label[index] = label[index + distance];
        }
        if (label[index] < 0) label[index] = 0;
    }
    return label;
}

// The fallback, for a machine with no embedder: two voices, split on pitch. It
// cannot separate two men and it reads a woman as whoever spoke longest on the
// high side - which is exactly the complaint the embedding answers.
std::vector<int> ClusterByPitch(const std::vector<Line>& lines, const std::vector<int16_t>& mono, int rate,
                                int& voices)
{
    std::vector<int> label(lines.size(), -1);
    for (size_t index = 0; index < lines.size(); ++index) {
        const CuePitch pitch = MeasurePitch(mono, rate, lines[index].start, lines[index].end);
        if (pitch.f0 > 0.0 && pitch.voiced >= dub_fit::kVoiceMinVoiced)
            label[index] = pitch.f0 < dub_fit::kVoiceSplitHz ? 0 : 1;
    }
    // A cue whose pitch could not be trusted takes its nearest neighbour's
    // voice: the line is still spoken by somebody, and the next cue is the best
    // evidence there is when this one was music.
    for (size_t index = 0; index < label.size(); ++index) {
        if (label[index] >= 0) continue;
        for (size_t distance = 1; distance < label.size() && label[index] < 0; ++distance) {
            if (index >= distance && label[index - distance] >= 0) label[index] = label[index - distance];
            else if (index + distance < label.size() && label[index + distance] >= 0)
                label[index] = label[index + distance];
        }
        if (label[index] < 0) label[index] = 0;
    }
    voices = 2;
    return label;
}

// How much more energy a cue has in the vocal stem than in the backing, which
// is what step11_refs_auto.py calls separation. 1.0 means the cue is as loud in
// the backing as in the voice - music with words over it, or a watermark, not an
// actor. Both stems have to be at the same rate and mono; 10 stands in for
// "nothing under it at all".
double VoicePurity(const std::vector<int16_t>& vocal, const std::vector<int16_t>& backing, int rate,
                   double start, double end)
{
    if (vocal.empty() || backing.empty() || rate <= 0) return 1.0;
    const size_t first = size_t(std::max(0.0, start) * double(rate));
    const size_t last = std::min({vocal.size(), backing.size(), size_t(std::max(0.0, end) * double(rate))});
    if (last <= first) return 1.0;
    double voiceEnergy = 0.0;
    double backingEnergy = 0.0;
    for (size_t at = first; at < last; ++at) {
        const double voice = double(vocal[at]) / 32768.0;
        const double under = double(backing[at]) / 32768.0;
        voiceEnergy += voice * voice;
        backingEnergy += under * under;
    }
    if (!(backingEnergy > 0.0)) return 10.0;
    return std::sqrt(voiceEnergy / backingEnergy);
}

// One voice's reference clip: the single line that most sounds like this voice
// seeds it, then only the lines that measure like the seed may join, in delivery
// order, with 5 ms fades and 0.12 s between them. Under
// kReferenceMinTotal seconds the clip is refused and the caller hands the lines
// to the longest voice instead.
//
// `purity` is that file's first criterion, separation: how much more energy the
// cue has in the vocal stem than in the backing. A cue that whisper turned into
// words but that is really a music sting or a channel watermark carries as much
// backing as voice, and cloning it gives the actor the watermark's voice -
// measured: one cluster's reference transcribed as "明镜需要您的支持 欢迎订阅明镜",
// and its five lines were spoken by that announcer.
bool CutReference(const Pcm& vocal, const std::vector<double>& purity, const std::vector<Line>& lines,
                  const std::vector<int>& label, int voice, const std::filesystem::path& out,
                  const std::vector<std::vector<float>>& vectors,
                  const std::vector<std::vector<float>>& centroids, std::vector<size_t>& chosen,
                  double& total)
{
    if (vocal.rate <= 0 || vocal.channels < 1) return false;
    const int rate = vocal.rate;
    const size_t fade = size_t(dub_fit::kReferenceFade * double(rate));
    const size_t gap = size_t(dub_fit::kReferenceGap * double(rate));

    // Two passes. The first keeps only cues the separator really put the voice
    // in; the second drops that requirement. It is needed because a scene scored
    // under the whole conversation measures every line at about 1.0 against the
    // backing, so a strict-only rule produced no clip at all for one cluster -
    // and that cluster borrowed the other's voice, which is heard as a woman
    // spoken by a man. Cloning a slightly music-flavoured clip is the lesser
    // fault; having no clip of that voice is the fault being fixed.
    for (int pass = 0; pass < 2; ++pass) {
        const bool strict = pass == 0;
        // Ordered by how much the line sounds like its own cluster, then by
        // length. Length alone picks the wrong clip: a cluster holds whoever the
        // split could not separate - here a robot announcing an engine start,
        // seven and a half seconds of it - and "longest first" hands the whole
        // reference to the one voice that is not the actor. Similarity puts that
        // line last, where it belongs: measured on this film, a woman's lines
        // score 0.13-0.25 against her own seed while the robot scores 0.00.
        std::vector<std::tuple<double, double, size_t>> candidates;
        for (size_t index = 0; index < lines.size() && index < label.size(); ++index) {
            if (label[index] != voice) continue;
            if (strict && index < purity.size() && purity[index] < dub_fit::kReferenceMinPurity) continue;
            const double span = lines[index].end - lines[index].start;
            if (span < dub_fit::kReferenceMinLine) continue;
            double likeness = -1.0;
            if (voice >= 0 && size_t(voice) < centroids.size() && index < vectors.size() &&
                !vectors[index].empty())
                likeness = speaker::Similarity(vectors[index], centroids[size_t(voice)]);
            candidates.emplace_back(likeness, std::min(span, dub_fit::kReferenceMaxLine), index);
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            if (std::get<0>(left) != std::get<0>(right)) return std::get<0>(left) > std::get<0>(right);
            return std::get<1>(left) > std::get<1>(right);
        });
        chosen.clear();
        total = 0.0;
        // The seed is the line the clip is cloned from, so it is chosen the way a
        // listener would: the one that most sounds like this voice *and* sits in
        // the length that clones the steadiest (2-5 s, measured). A 1.2 s
        // fragment can outscore a full sentence on likeness and still clone
        // badly, and a 7-second one lets the engine wander inside it.
        size_t seed = lines.size();
        for (const std::pair<double, double> band :
             {std::pair<double, double>{dub_fit::kReferenceSeedBestLow, dub_fit::kReferenceSeedBestHigh},
              {dub_fit::kReferenceMinLine, dub_fit::kReferenceSeedBestLow}}) {
            for (const auto& [likeness, span, index] : candidates) {
                const double heard = lines[index].end - lines[index].start;
                if (heard < band.first || heard > band.second) continue;
                seed = index;
                break;
            }
            if (seed < lines.size()) break;
        }
        if (seed >= lines.size()) {
            for (const auto& [likeness, span, index] : candidates) {
                const double heard = lines[index].end - lines[index].start;
                if (heard < dub_fit::kReferenceMinLine) continue;
                seed = index;
                break;
            }
        }
        if (seed < lines.size()) {
            const bool measurable = seed < vectors.size() && !vectors[seed].empty();
            chosen.push_back(seed);
            total = std::min(lines[seed].end - lines[seed].start, dub_fit::kReferenceMaxLine);
            for (const auto& [likeness, span, index] : candidates) {
                if (index == seed) continue;
                if (total >= dub_fit::kReferenceTarget) break;
                if (total + span > dub_fit::kReferenceCap) continue;
                // Only lines that measure like the seed may join it. Without this
                // gate the clip is the *cluster*, and a cluster holds whoever the
                // split could not separate: one robot announcement, one "Ah.",
                // one line the other actor really said. The engine clones all of
                // them, which is heard as the timbre changing from line to line
                // off a single reference - measured as four voices on one
                // character in a single sentence.
                if (measurable) {
                    if (index >= vectors.size() || vectors[index].empty()) continue;
                    if (speaker::Similarity(vectors[index], vectors[seed]) < dub_fit::kReferenceAgree)
                        continue;
                }
                chosen.push_back(index);
                total += span;
            }
        }
        if (total < dub_fit::kReferenceMinTotal) continue;
        std::sort(chosen.begin(), chosen.end());  // delivery order, so prosody stays coherent
        // Which lines the voice was cloned from, in words: a clip is the one
        // thing this stage cannot check by listening, so the log says who it
        // heard. "The female lead's reference reads as the male lead's line"
        // is then a line of the log rather than a guess at the mix.
        std::string audit;
        for (const size_t index : chosen)
            audit += (audit.empty() ? "" : " | ") + std::to_string(index) + " " +
                     lines[index].source.substr(0, 28);
        LOG("Dub: voice " << voice + 1 << " clones from " << chosen.size() << " line(s), " << total
                          << " s: " << audit);

        std::vector<int16_t> joined;
        for (const size_t index : chosen) {
            const size_t first = size_t(std::max(0.0, lines[index].start) * double(rate));
            const size_t last = std::min(vocal.samples.size(),
                                         size_t(std::min(lines[index].end,
                                                         lines[index].start + dub_fit::kReferenceMaxLine) * double(rate)));
            if (last <= first) continue;
            const size_t begin = joined.size();
            for (size_t at = first; at < last; ++at) {
                int32_t value = 0;
                for (int channel = 0; channel < vocal.channels; ++channel)
                    value += vocal.samples[at * size_t(vocal.channels) + size_t(channel)];
                joined.push_back(int16_t(value / vocal.channels));
            }
            // The joins are inside the clip, so both ends of every piece are
            // faded: a step in the waveform is a click, and a click in a
            // reference is what the model imitates.
            const size_t length = joined.size() - begin;
            for (size_t at = 0; at < std::min(fade, length); ++at) {
                const double gain = double(at) / double(fade);
                joined[begin + at] = int16_t(double(joined[begin + at]) * gain);
                joined[begin + length - 1 - at] = int16_t(double(joined[begin + length - 1 - at]) * gain);
            }
            joined.insert(joined.end(), gap, 0);
        }
        if (joined.empty()) continue;
        if (!strict) LOG("Dub: voice " << voice + 1 << " had no clean clip; its reference is taken from lines the backing is loud under");
        return WriteWav16(out, joined, rate, 1);
    }
    return false;
}

Result RunDub(const Plan& plan, std::stop_token stop,
              const std::function<void(const Progress&)>& report)
{
    Result result;
    const std::chrono::minutes synthesisTimeout(10);
    const auto ffmpeg = plan.helpers / L"ffmpeg.exe";
    std::error_code workError;
    std::filesystem::create_directories(plan.work, workError);
    if (workError) {
        result.detail = L"The dub's scratch directory could not be created.";
        return result;
    }
    const Progress total{0, plan.lines.size() + 1};
    if (report) report(total);
    if (plan.lines.empty()) {
        result.detail = L"The translation has no lines to speak.";
        return result;
    }

    // One file per line, named by its index so a re-run overwrites cleanly.
    const auto text = [&](size_t index) { return plan.work / (L"line" + std::to_wstring(index) + L".txt"); };
    const auto raw = [&](size_t index) { return plan.work / (L"line" + std::to_wstring(index) + L".wav"); };
    const auto trimmed = [&](size_t index) { return plan.work / (L"trim" + std::to_wstring(index) + L".wav"); };
    const auto fitted = [&](size_t index) { return plan.work / (L"fit" + std::to_wstring(index) + L".wav"); };

    std::vector<Spoken> spoken(plan.lines.size());

    // ---- The voice ----------------------------------------------------------
    // Qwen3-TTS clones a timbre from a reference clip (--tts-speaker-file).
    // Without one the engine samples a fresh speaker per line at --temp 0.7,
    // which is heard as "the timbre changed" - every line somebody else. One
    // reference per film is heard as the opposite fault: every woman read by a
    // man. So the film is separated first, its cues are clustered into voices on
    // the vocal stem, and each voice gets its own clip cut from that stem.
    //
    // plan.voiceReference, when set, outranks all of it - that is the caller
    // saying it already knows the voice.
    std::vector<std::filesystem::path> lineReference(plan.lines.size(), plan.voiceReference);
    const Stems stems = SeparateStems(plan, ffmpeg, stop);
    if (stop.stop_requested()) { result.cancelled = true; return result; }

    if (!lineReference.front().empty()) {
        if (!std::filesystem::exists(lineReference.front())) {
            LOG("Dub: the given voice reference is missing; the engine's own voice is used");
            std::fill(lineReference.begin(), lineReference.end(), std::filesystem::path{});
        }
    } else {
        // 16 kHz mono of the vocal stem: what both the embedder and the pitch
        // track read, and a sixteenth of the stem's own size.
        const std::filesystem::path& listen = stems.separated ? stems.vocals : plan.mediaPath;
        const auto probeFile = plan.work / L"voice-probe.wav";
        std::vector<int16_t> mono;
        int monoRate = 16000;
        const bool probed = RunProcess(ffmpeg, {L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror",
                                                L"-y", L"-i", listen.wstring(), L"-vn", L"-map", L"0:a:0",
                                                L"-ac", L"1", L"-ar", L"16000", L"-c:a", L"pcm_s16le",
                                                probeFile.wstring()},
                                       stop, std::chrono::minutes(30), plan.logFile);
        if (stop.stop_requested()) { result.cancelled = true; return result; }
        if (probed) {
            if (const auto pcm = ReadWav(probeFile)) {
                mono = pcm->samples;
                monoRate = int(pcm->rate);
            }
        }

        speaker::Embedder embedder(plan.voiceEngine);
        if (!embedder.Ready())
            LOG("Dub: " << embedder.Notice() << "; the voices are told apart by pitch instead");

        int voices = 0;
        std::vector<int> group(plan.lines.size(), -1);
        // Per-line embeddings and per-voice means, kept so the reference clips
        // can be chosen by likeness instead of by length.
        std::vector<std::vector<float>> vectors;
        std::vector<std::vector<float>> centroids;
        // The lines the dialogue itself assigns to somebody: "You're a jerk, Tom"
        // is not spoken by Tom. They are the only evidence here that is not a
        // cosine, so the recast below may not overturn them.
        std::vector<size_t> anchors;
        if (!mono.empty())
            group = embedder.Ready()
                        ? ClusterByEmbedding(plan.lines, mono, monoRate, embedder, voices, vectors,
                                             centroids, anchors)
                        : ClusterByPitch(plan.lines, mono, monoRate, voices);

        if (voices > 0) {
            // The clips, cut from the vocal stem at the engine's own rate so the
            // clip is not resampled twice before the model hears it.
            const auto vocalFile = plan.work / L"voice-source.wav";
            const bool cut = RunProcess(ffmpeg, {L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror",
                                                 L"-y", L"-i", listen.wstring(), L"-vn", L"-map", L"0:a:0",
                                                 L"-ac", L"1", L"-ar", std::to_wstring(dub_fit::kEngineSampleRate),
                                                 L"-c:a", L"pcm_s16le", vocalFile.wstring()},
                                        stop, std::chrono::minutes(30), plan.logFile);
            if (stop.stop_requested()) { result.cancelled = true; return result; }
            const std::optional<Pcm> vocal = cut ? ReadWav(vocalFile) : std::optional<Pcm>{};

            // Separation, cue by cue: the vocal stem against the backing at the
            // same moment. This is the filter that keeps a channel watermark or
            // a music sting out of the reference clips - one such cue sang five
            // lines in an announcer's voice who is not in the film.
            std::vector<double> purity(plan.lines.size(), 10.0);
            if (stems.separated && !mono.empty()) {
                const auto backingFile = plan.work / L"voice-backing.wav";
                if (RunProcess(ffmpeg, {L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror", L"-y",
                                        L"-i", stems.backing.wstring(), L"-vn", L"-map", L"0:a:0",
                                        L"-ac", L"1", L"-ar", L"16000", L"-c:a", L"pcm_s16le",
                                        backingFile.wstring()},
                               stop, std::chrono::minutes(30), plan.logFile)) {
                    if (const auto backing = ReadWav(backingFile)) {
                        for (size_t index = 0; index < plan.lines.size(); ++index)
                            purity[index] = VoicePurity(mono, backing->samples, monoRate,
                                                        plan.lines[index].start, plan.lines[index].end);
                        for (size_t index = 0; index < purity.size(); ++index)
                            if (purity[index] < dub_fit::kReferenceMinPurity)
                                LOG("Dub: line " << index + 1 << " has only " << purity[index]
                                    << "x the energy in the voice that it has in the backing"
                                       "; it is music or a watermark, not a reference");
                    }
                }
            }

            std::vector<size_t> population(size_t(voices), 0);
            int lead = 0;

            // The clips, then the casting again against them. A cluster mean is
            // built from every line the split could not separate, so it sits
            // between the people in it; a clip is built from the lines that
            // agree with one another, so comparing a line to the clip is the
            // same question asked with a cleaner answer. A line that matches no
            // clip is somebody the film has not given a voice - an announcer, a
            // robot, a man in another scene - and it takes the longest clip
            // instead of being handed to whichever mean was nearer by 0.05.
            // A named count, not size_t(voices) in the constructor: the compiler
            // reads `vector<path> x(size_t(y))` as a function declaration.
            const size_t voiceCount = size_t(voices);
            std::vector<double> referenceSeconds(voiceCount, 0.0);
            std::vector<std::vector<float>> referenceMeans(voiceCount);
            std::vector<std::filesystem::path> referencePaths(voiceCount);
            for (int voice = 0; voice < voices; ++voice) {
                const auto reference = plan.work / (std::wstring(L"voice-") + std::to_wstring(voice + 1) + L".wav");
                std::vector<size_t> chosen;
                double seconds = 0.0;
                if (!vocal || !CutReference(*vocal, purity, plan.lines, group, voice, reference,
                                            vectors, centroids, chosen, seconds)) {
                    LOG("Dub: voice " << voice + 1 << " has no clip of its own");
                    continue;
                }
                referencePaths[size_t(voice)] = reference;
                referenceSeconds[size_t(voice)] = seconds;
                // What the clip sounds like, averaged over the lines it was cut
                // from: the clean version of that voice's mean.
                std::vector<float> mean(speaker::kDimensions, 0.0f);
                size_t counted = 0;
                for (const size_t index : chosen) {
                    if (index >= vectors.size() || vectors[index].empty()) continue;
                    for (size_t at = 0; at < mean.size(); ++at) mean[at] += vectors[index][at];
                    ++counted;
                }
                if (counted) {
                    double energy = 0.0;
                    for (const float value : mean) energy += double(value) * double(value);
                    energy = std::sqrt(energy);
                    if (energy > 0.0) {
                        for (float& value : mean) value = static_cast<float>(double(value) / energy);
                        referenceMeans[size_t(voice)] = std::move(mean);
                    }
                }
            }
            int longest = -1;
            for (int voice = 0; voice < voices; ++voice)
                if (referenceSeconds[size_t(voice)] > 0.0 &&
                    (longest < 0 || referenceSeconds[size_t(voice)] >
                                        referenceSeconds[size_t(longest)]))
                    longest = voice;

            if (!referenceMeans.empty()) {
                size_t changed = 0;
                for (size_t index = 0; index < plan.lines.size(); ++index) {
                    if (index >= vectors.size() || vectors[index].empty()) continue;
                    int best = -1;
                    double bestScore = -2.0;
                    for (int voice = 0; voice < voices; ++voice) {
                        if (referenceMeans[size_t(voice)].empty()) continue;
                        const double score = speaker::Similarity(vectors[index], referenceMeans[size_t(voice)]);
                        if (score > bestScore) { bestScore = score; best = voice; }
                    }
                    int casted = best;
                    // "Nobody matches" is decided on the best score alone. It
                    // used to be overruled by "the clip it is going to must be
                    // nearer than the one it came from" - which is the same
                    // number when the best is its own voice, so the rule never
                    // fired there and a launch countdown stayed on the female
                    // lead's clip at 0.03.
                    const bool unmatched = bestScore < dub_fit::kCastUncertain;
                    if (unmatched) casted = longest;
                    if (casted < 0 || casted == group[index]) continue;
                    if (!unmatched) {
                        const bool own = group[index] >= 0 && size_t(group[index]) < referenceMeans.size() &&
                                         !referenceMeans[size_t(group[index])].empty();
                        const double mine = own ? speaker::Similarity(vectors[index],
                                                                      referenceMeans[size_t(group[index])])
                                                : -2.0;
                        if (bestScore <= mine) continue;
                    }
                    // A line the dialogue itself vouches for never moves: the
                    // two address anchors are the only evidence here that is
                    // not a cosine, and a cosine on this material is noise.
                    if (std::find(anchors.begin(), anchors.end(), index) != anchors.end()) continue;
                    LOG("Dub: line " << index + 1 << " moves from voice " << group[index] + 1 << " to "
                                     << casted + 1 << " (" << (unmatched
                                                                   ? "no clip it matches; the longest one"
                                                                   : "nearer that clip")
                                     << ", " << bestScore << ")");
                    group[index] = casted;
                    ++changed;
                }
                if (changed) LOG("Dub: " << changed << " line(s) recast against the finished clips");
            }

            for (int voice = 0; voice < voices; ++voice) {
                if (referencePaths[size_t(voice)].empty()) continue;
                for (size_t index = 0; index < group.size(); ++index)
                    if (group[index] == voice) lineReference[index] = referencePaths[size_t(voice)];
            }

            std::fill(population.begin(), population.end(), 0);
            for (const int voice : group)
                if (voice >= 0) ++population[size_t(voice)];
            lead = 0;
            for (int voice = 1; voice < voices; ++voice)
                if (population[size_t(voice)] > population[size_t(lead)]) lead = voice;
            std::filesystem::path leadReference = referencePaths[size_t(lead)];
            if (leadReference.empty() && longest >= 0) leadReference = referencePaths[size_t(longest)];
            // A voice whose clip fell short borrows the busiest one: a character
            // heard once still has to sound like a person, and "the engine's own
            // random voice" is exactly what this stage exists to stop.
            if (!leadReference.empty())
                for (auto& reference : lineReference)
                    if (reference.empty()) reference = leadReference;

            size_t unreferenced = 0;
            for (const auto& reference : lineReference)
                if (reference.empty()) ++unreferenced;
            std::string census;
            for (int voice = 0; voice < voices; ++voice)
                census += (census.empty() ? "" : ", ") + std::to_string(population[size_t(voice)]);
            LOG("Dub: " << voices << " voice(s) over " << plan.lines.size() << " line(s) [" << census
                        << "], " << unreferenced << " without a reference");

            // The cast, in a file. Every complaint this stage has drawn - "the
            // woman is read by a man", "the timbre changes between lines" - was
            // answerable from this table, and cost a run of guesswork without it.
            {
                std::ofstream cast(plan.work / L"cast.txt", std::ios::binary);
                for (size_t index = 0; index < plan.lines.size() && cast; ++index) {
                    cast << index << '\t' << (group[index] >= 0 ? group[index] + 1 : 0) << '\t'
                         << plan.lines[index].start << '\t' << plan.lines[index].end << '\t'
                         << lineReference[index].filename().string() << '\t'
                         << plan.lines[index].source << '\n';
                }
            }
        }
    }

    // The rewrite engine, when the caller supplied one. Started here and stopped
    // with the stage: the model takes seconds to load, and only the lines that
    // their window cannot hold - too long, or too short - ever ask it anything.
    Rewriter rewriter(plan.rewriter, plan.rewrite);

    for (size_t index = 0; index < plan.lines.size(); ++index) {
        if (stop.stop_requested()) { result.cancelled = true; return result; }
        const Line& line = plan.lines[index];

        // The window first: it is what a rewrite aims at, and unlike everything
        // else below it does not depend on what the engine produced.
        const double nextStart = index + 1 < plan.lines.size() ? plan.lines[index + 1].start : 0.0;
        const double window = dub_fit::WindowSeconds(line.start, line.end, nextStart);

        // The words, and the synthesizer. A line that comes back longer than its
        // window is asked for again in fewer words before a cut is accepted: in a
        // dense exchange the Chinese runs about 30% longer than the cues hold,
        // tempo is capped at 1.25, and queueing the lines up drifted a whole
        // scene 4 s late. Rewriting is the only concession left that keeps both
        // the sentence and its place.
        std::string spokenText = line.text;
        int rewrites = 0;
        // The opposite fault, and the one heard first: a Chinese line that has
        // said its piece in 11 s of a 15 s slot leaves the actor's mouth moving
        // over music. Slowing covers 15% at most (kMinTempo), so a line short by
        // more than that is asked for in more words before it is spoken - the
        // invisible concession, and the one the subtitle does not share: the
        // track was written before this stage ran, so this changes what is heard
        // and never what is read.
        if (rewriter.Ready()) {
            const auto fill = dub_fit::FillForLine(line.start, line.end, spokenText);
            if (fill.characters > 0) {
                const std::string longer = rewriter.Lengthen(spokenText, fill.characters);
                if (dub_fit::FillIsSafe(spokenText, longer, fill.characters)) {
                    LOG("Dub: line " << index + 1 << " would speak " << fill.predicted
                                     << " s of a " << fill.slot << " s line; filling it to "
                                     << fill.characters << " characters");
                    spokenText = longer;
                    ++result.filled;
                }
            }
        }
        Pcm voice;
        wchar_t tempoFlag[16];
        std::swprintf(tempoFlag, 16, L"%.2f", dub_fit::kDefaultTemperature);
        for (int attempt = 0;;) {
            // The synthesizer reads its words from a file: a command line would
            // put the text through two layers of quoting, and the model reads
            // UTF-8. Rewritten, the file is written again.
            {
                std::ofstream file(text(index), std::ios::binary);
                file.write(spokenText.data(), std::streamsize(spokenText.size()));
                if (!file) {
                    result.detail = L"The line's text could not be written for the synthesizer.";
                    return result;
                }
            }
            // The timbre, when there is one to clone. Without this flag the
            // engine invents a speaker per line; with it, this line's own
            // speaker - the clusters above, or the caller's one voice.
            std::vector<std::wstring> arguments{
                L"-m", plan.engine.model.wstring(), L"-mm", plan.engine.mmproj.wstring(),
                L"-f", text(index).wstring(), L"-o", raw(index).wstring(),
                L"--tts-lang", WideFromUtf8(plan.language),
                L"-n", std::to_wstring(NewTokensFor(spokenText)),
                L"--temp", tempoFlag};
            if (!lineReference[index].empty())
                arguments.insert(arguments.end(), {L"--tts-speaker-file", lineReference[index].wstring()});
            if (!RunProcess(plan.engine.runner, arguments, stop, synthesisTimeout, plan.logFile)) {
                if (stop.stop_requested()) { result.cancelled = true; return result; }
                result.detail = L"The synthesizer failed on line " + std::to_wstring(index + 1) +
                                L" of " + std::to_wstring(plan.lines.size()) + L".";
                return result;
            }
            const auto produced = ReadWav(raw(index));
            if (!produced || produced->rate == 0) {
                result.detail = L"The synthesizer wrote no audio for line " + std::to_wstring(index + 1) + L".";
                return result;
            }
            voice = *produced;
            const double spokenSeconds = voice.Seconds();
            TrimSilence(voice);
            LOG("Dub: line " << index + 1 << ": " << spokenSeconds << " s spoken, "
                             << voice.Seconds() << " s after trim");
            // A line that came back empty is the model wandering off, not a
            // line that cannot be said; the second draw is independent.
            if (voice.samples.empty()) {
                if (attempt > 0) {
                    result.detail = L"The synthesizer wrote silence for line " + std::to_wstring(index + 1) +
                                    L", twice.";
                    return result;
                }
                LOG("Dub: line " << index + 1 << " came back silent; synthesizing it again");
                ++attempt;
                continue;
            }
            // Overflowing, and there is an engine to ask: fewer words beats a
            // cut, and beats arriving late in somebody else's line.
            if (rewrites < dub_fit::kMaxRewriteRounds && voice.Seconds() > window * 1.02) {
                const std::string shorter = rewriter.Shorten(spokenText, window);
                if (!shorter.empty() && shorter != spokenText) {
                    LOG("Dub: line " << index + 1 << " is " << voice.Seconds() << " s in a " << window
                                     << " s window; speaking it as \"" << shorter << "\"");
                    spokenText = shorter;
                    ++rewrites;
                    ++result.rewritten;
                    continue;
                }
            }
            break;
        }

        // The silence in front of the line is what the previous line left: from
        // where it actually stopped to here. The first line owns the film's own
        // lead-in.
        const double room = index == 0 ? line.start : line.start - spoken[index - 1].finish;
        const double natural = voice.Seconds();
        // Starting early is a concession, not a default. ShiftFor answers "how
        // far back could this line move", and using that answer unconditionally
        // moved every line that had silence in front of it: a cue at 60.00 s was
        // spoken at 58.00 s, 117.00 at 115.04 - the voice spoke before its
        // subtitle appeared, which is exactly "对不上". So the line moves back
        // only by what it actually needs, and not at all when it fits.
        const double shift = std::min(dub_fit::ShiftFor(room), std::max(0.0, natural - window));
        double tempo = 1.0;
        bool cut = false;
        const double available = window + shift;
        if (natural > available) {
            // Overflow: accelerate to the ceiling, and report what remains.
            tempo = natural / available;
            const double ceiling = dub_fit::TempoCeilingFor(window);
            if (tempo > ceiling) { tempo = ceiling; cut = true; }
        } else if (natural < line.end - line.start) {
            // The dub would stop while the mouth is still moving; slow down to
            // the line's own length, within the floor the ear was measured at.
            tempo = std::max(dub_fit::kMinTempo, natural / std::max(0.001, line.end - line.start));
        }

        // Through ffmpeg once either way: the tempo when there is one, and the
        // resample and upmix to the working format regardless. One -af chain -
        // a second would silently replace the first - with the resampler named
        // inside it, because the global -resampler form exits non-zero here.
        if (!WriteWav16(trimmed(index), voice.samples, voice.rate, voice.channels)) {
            result.detail = L"The trimmed line could not be written.";
            return result;
        }
        std::string filter;
        if (std::abs(tempo - 1.0) > 0.001) {
            char tempoText[16];
            std::snprintf(tempoText, sizeof(tempoText), "%.4f", tempo);
            filter += std::string("atempo=") + tempoText + ",";
        }
        filter += "aresample=" + std::to_string(dub_fit::kWorkingSampleRate) +
                  ":resampler=soxr:precision=28";
        if (!RunProcess(ffmpeg,
                        {L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror", L"-y",
                         L"-i", trimmed(index).wstring(), L"-af", WideFromUtf8(filter),
                         L"-ac", std::to_wstring(dub_fit::kWorkingChannels),
                         L"-c:a", L"pcm_s16le", fitted(index).wstring()},
                        stop, std::chrono::minutes(5), plan.logFile)) {
            if (stop.stop_requested()) { result.cancelled = true; return result; }
            result.detail = L"The line could not be fitted (" + std::to_wstring(index + 1) + L").";
            return result;
        }
        const auto fittedPcm = ReadWav(fitted(index));
        if (!fittedPcm || fittedPcm->rate != dub_fit::kWorkingSampleRate || fittedPcm->channels != 2) {
            result.detail = L"The fitted line came back wrong (" + std::to_wstring(index + 1) + L").";
            return result;
        }
        Spoken& entry = spoken[index];
        entry.anchor = std::max(0.0, line.start - shift);
        entry.finish = entry.anchor + fittedPcm->Seconds();
        entry.window = window;
        entry.samples = std::move(fittedPcm->samples);
        entry.level = DeliveredLevel(entry.samples, dub_fit::kWorkingSampleRate, 2);
        if (cut) ++result.cut;
        if (std::abs(tempo - 1.0) > 0.001) ++result.compressed;
        if (report) report({index + 1, plan.lines.size() + 1});
    }

    // ---- The floor ------------------------------------------------------------
    // A cue boundary is a subtitle cut, not a breath. In a two-handed exchange
    // the transcript splits one line into cues half a second apart, and half a
    // second cannot hold a Chinese sentence: measured on this film, 8 of 24
    // lines were cut mid-word for 2.7 s of lost speech - which a listener
    // reports, correctly, as "话没说完".
    //
    // So the lines take the floor in order. A line that is still speaking keeps
    // it, and the next one begins when it is free, instead of being squeezed
    // into whatever gap its own subtitle left. What this costs is a line
    // arriving late inside a dense exchange; what it buys is every sentence
    // finishing. A late line is recovered at the next real pause, and the drift
    // never leaves the scene because the exchange has more room than a per-cue
    // reading suggests.
    const double workingRate = double(dub_fit::kWorkingSampleRate);
    for (size_t index = 1; index < spoken.size(); ++index) {
        const double previous = double(spoken[index - 1].samples.size()) / (workingRate * 2.0);
        const double earliest = spoken[index - 1].anchor + previous + dub_fit::kMinGapSeconds;
        if (spoken[index].anchor >= earliest) continue;
        // Capped: an unbounded queue drifted a dense scene four seconds late, and
        // a scene that agrees with nothing on screen is worse than a short line.
        const double latest = plan.lines[index].start + dub_fit::kMaxLineDelay;
        const double target = std::min(earliest, std::max(spoken[index].anchor, latest));
        if (target <= spoken[index].anchor) continue;
        LOG("Dub: line " << index + 1 << " waits " << target - spoken[index].anchor
                         << " s for the line before it to finish");
        spoken[index].anchor = target;
        spoken[index].finish = target + double(spoken[index].samples.size()) / (workingRate * 2.0);
    }

    // The mix. Every line's level is aligned to the median of the lines'
    // delivered levels, clamped: the engine's own loudness varies by 20 dB, and
    // one line amplified past the clamp would come back as noise.
    if (stop.stop_requested()) { result.cancelled = true; return result; }
    std::vector<double> levels;
    levels.reserve(spoken.size());
    for (const Spoken& entry : spoken)
        if (entry.level > 0.0) levels.push_back(entry.level);
    std::sort(levels.begin(), levels.end());
    const double median = levels.empty() ? 0.0 : levels[levels.size() / 2];
    const double gainClamp = std::pow(10.0, dub_fit::kGainClampDb / 20.0);

    const size_t frames = std::max(size_t(1), size_t(plan.durationSeconds * double(dub_fit::kWorkingSampleRate)));
    F32 mix;
    mix.samples.assign(frames * 2, 0.0f);
    // What each line peaks at once its own gain is applied: the median of these
    // is what kDialoguePeak is measured against below.
    std::vector<double> linePeaks;
    linePeaks.reserve(spoken.size());
    for (size_t index = 0; index < spoken.size(); ++index) {
        const Spoken& entry = spoken[index];
        const double gain = entry.level > 0.0 && median > 0.0
                                ? std::clamp(median / entry.level, 1.0 / gainClamp, gainClamp)
                                : 1.0;
        // Where the voice must end: the next line's start, minus the gap two
        // voices need. Trimming here is what "cut" means - the line is spoken
        // as far as it fits and no further, and the count says so.
        // The next line, plus the overhang a sentence is allowed to spill into
        // so it can finish its last word - see kOverhangSeconds.
        const double limit = index + 1 < spoken.size()
                                 ? spoken[index + 1].anchor + dub_fit::kOverhangSeconds
                                 : plan.durationSeconds;
        const size_t begin = std::min(frames, size_t(entry.anchor * double(dub_fit::kWorkingSampleRate)));
        const size_t end = std::min(frames, size_t(limit * double(dub_fit::kWorkingSampleRate)));
        size_t at = 0;
        double peak = 0.0;
        for (size_t frame = begin; frame < end && at + 2 <= entry.samples.size(); ++frame, at += 2) {
            const double left = double(entry.samples[at]) / 32768.0 * gain;
            const double right = double(entry.samples[at + 1]) / 32768.0 * gain;
            mix.samples[frame * 2] += float(left);
            mix.samples[frame * 2 + 1] += float(right);
            peak = std::max({peak, std::abs(left), std::abs(right)});
        }
        if (peak > 0.0) linePeaks.push_back(peak);
    }

    // The bed: the film's score and effects under the Chinese voice, from the
    // separated backing stem. Never the source's own mix - that carries the
    // English dialogue, so laying it under a dub plays the original a second
    // time, and at nearly full level it plays it louder than the voice.
    //
    // Its level follows the Python mix: down to kBackingGain while a Chinese
    // line plays, back to kBackingRest between them, ramped so the step does not
    // click. Holding it down for the whole film made the film's real pauses read
    // as the audio dropping out - a listener reported exactly that at 5:06.
    if (plan.bed) {
        if (!stems.separated) {
            LOG("Dub: no backing stem, so the track carries the voice only");
        } else {
            const auto bedFile = plan.work / L"bed.wav";
            if (RunProcess(ffmpeg, {L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror", L"-y",
                                    L"-i", stems.backing.wstring(), L"-vn", L"-map", L"0:a:0",
                                    L"-af", L"aresample=" + std::to_wstring(dub_fit::kWorkingSampleRate) +
                                                 L":resampler=soxr:precision=28",
                                    L"-ac", std::to_wstring(dub_fit::kWorkingChannels),
                                    L"-c:a", L"pcm_s16le", bedFile.wstring()},
                           stop, std::chrono::minutes(30), plan.logFile)) {
                if (const auto bed = ReadWav(bedFile)) {
                    // GAIN_BACKING was tuned when the bed was the mono sum, so a
                    // stereo stem is scaled by the ratio that keeps the
                    // voice-to-music balance the mix was accepted at.
                    double monoEnergy = 0.0;
                    double channelEnergy = 0.0;
                    const size_t bedFrames = bed->samples.size() / 2;
                    for (size_t frame = 0; frame < bedFrames; ++frame) {
                        const double left = double(bed->samples[frame * 2]) / 32768.0;
                        const double right = double(bed->samples[frame * 2 + 1]) / 32768.0;
                        monoEnergy += ((left + right) * 0.5) * ((left + right) * 0.5);
                        channelEnergy += (left * left + right * right) * 0.5;
                    }
                    const double correction = bedFrames && channelEnergy > 0.0
                        ? std::sqrt(monoEnergy / channelEnergy) : 1.0;
                    const double bedGain = std::clamp(dub_fit::kBackingGain * correction, 0.5, 1.5);

                    // The duck, drawn at its steps: a per-sample curve is a 48 kHz
                    // array of 1.0 with a few ramps in it, and the ramps are what
                    // matter.
                    const double rate = double(dub_fit::kWorkingSampleRate);
                    std::vector<double> envelope(frames, dub_fit::kBackingRest);
                    for (const Spoken& entry : spoken) {
                        const size_t begin = std::min(frames, size_t(std::max(0.0, entry.anchor - dub_fit::kDuckPadSeconds) * rate));
                        const size_t end = std::min(frames, size_t((entry.finish + dub_fit::kDuckPadSeconds) * rate));
                        for (size_t frame = begin; frame < end; ++frame)
                            envelope[frame] = std::min(envelope[frame], bedGain);
                    }
                    const size_t ramp = std::max<size_t>(1, size_t(dub_fit::kDuckRampSeconds * rate));
                    for (size_t frame = 1; frame < frames; ++frame) {
                        if (envelope[frame] == envelope[frame - 1]) continue;
                        const size_t low = frame > ramp ? frame - ramp : 0;
                        const size_t high = std::min(frames, frame + ramp);
                        const double from = envelope[low];
                        const double to = envelope[high - 1];
                        for (size_t at = low; at < high; ++at)
                            envelope[at] = from + (to - from) * (double(at - low) / double(std::max<size_t>(1, high - 1 - low)));
                    }

                    const size_t count = std::min(mix.samples.size(), bed->samples.size());
                    for (size_t sample = 0; sample < count; ++sample)
                        mix.samples[sample] += float(double(bed->samples[sample]) / 32768.0 *
                                                     envelope[sample / 2]);
                    LOG("Dub: the bed is the backing stem at " << 20.0 * std::log10(bedGain) << " dB under "
                        << spoken.size() << " line(s)");
                }
            } else {
                // A bed that fails is reported, not fatal: the dub is still the
                // track, and the log now says why it is dry.
                LOG("Dub: the backing stem could not be read; the track carries the voice only");
            }
        }
    }

    // Aim the track at a level, on the median line rather than the loudest one.
    // A dry mix has nothing under it to bring the peaks up to the ceiling, and
    // the source track beside it does: measured 7.8 dB low, heard as "the
    // Chinese is quieter than the English". Scaling to the loudest line instead
    // would leave every normal line ~6 dB down, because one hot line is 17 dB
    // above the rest (measured: peaks -20.5 .. -3.5 dB on one 3 min run).
    if (dub_fit::kFillHeadroom && !linePeaks.empty()) {
        std::sort(linePeaks.begin(), linePeaks.end());
        const double medianPeak = linePeaks[linePeaks.size() / 2];
        if (medianPeak > 0.0) {
            const double gain = std::min(dub_fit::kDialoguePeak / medianPeak, dub_fit::kMaxNormalizeGain);
            for (float& sample : mix.samples) sample = float(double(sample) * gain);
            LOG("Dub: median line peak " << 20.0 * std::log10(medianPeak) << " dB, aimed by "
                << 20.0 * std::log10(gain) << " dB at "
                << 20.0 * std::log10(dub_fit::kDialoguePeak) << " dBFS");
        }
    }

    // The limiter, at four times the rate: a limiter at 48 kHz cannot see the
    // peak between two samples, so it does not limit the true peak.
    const auto floatFile = plan.work / L"master-float.wav";
    if (!WriteWavFloat(floatFile, mix)) {
        result.detail = L"The mixed track could not be written.";
        return result;
    }
    if (!RunProcess(ffmpeg,
                    {L"-hide_banner", L"-nostdin", L"-loglevel", L"error", L"-xerror", L"-y",
                     L"-i", floatFile.wstring(),
                     L"-af", L"aresample=176400,alimiter=limit=0.794:level=disabled:latency=true,aresample=48000",
                     L"-c:a", L"pcm_s16le", plan.track.wstring()},
                    stop, std::chrono::minutes(30), plan.logFile)) {
        if (stop.stop_requested()) { result.cancelled = true; return result; }
        result.detail = L"The dub track could not be limited.";
        return result;
    }
    if (report) report({plan.lines.size() + 1, plan.lines.size() + 1});
    result.ok = true;
    result.lines = plan.lines.size();
    LOG("Dub: " << plan.lines.size() << " lines, " << result.compressed << " by tempo, "
                << result.rewritten << " re-worded shorter, " << result.filled
                << " re-worded longer, " << result.cut << " still cut by the mix");
    return result;
}

}  // namespace dub
