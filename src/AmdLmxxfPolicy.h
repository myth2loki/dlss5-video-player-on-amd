#pragma once

#include "TemporalStabilityPolicy.h"
#include "Utf8Text.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace amd_lmxxf {

inline constexpr uint32_t kAmdVendorId = 0x1002;
inline constexpr std::wstring_view kRuntimeDirectoryName = L"amd-neural-runtime";
inline constexpr std::wstring_view kRuntimeDllName = L"LmxxfNrRuntime.dll";
inline constexpr std::wstring_view kAssetsDirectoryName = L"native-game-tiled-assets";
inline constexpr std::wstring_view kLockFileName = L"amd-runtime-lock.json";

// The rung an AMD render takes when nothing asked for one.
//
// Two paths reach this constant and they want it for different reasons.
//
// With guides on - the default, mv=1,depth=1 - the runtime fuses history under
// the recording-lease lifecycle and the field it fuses is the player's own
// estimator (OfflineNeuralRenderer's AmdLmxxfEvaluatorAdapter). The host-side
// accumulation then runs on top of a picture that is already temporally
// resolved, where its job is the model's residual jitter rather than all of it.
//
// With guides off - DLSS5_AMD_GUIDES=0, or a job that asked for neither channel -
// the runtime relights every frame on its own and has no history to fuse with,
// so what a viewer sees between frames is the model's own frame-to-frame
// jitter and the accumulation is the only thing taking it out. That is the arm
// the numbers below were measured in, and it makes Off here not a neutral
// setting but the arm that flickers: on a still source, frame-to-frame Y PSNR
// is 49.1 dB at the model's own output against the source's own 96.6 dB, and
// Low takes the added jitter down 28.5% for 0.1 dB of distance from the source
// (docs/AMD-TEMPORAL-ACCUMULATION-zh.md). The rung was chosen for that arm and
// is not re-derived for the guided one: it is the same knob, it still trades
// steadiness for detail monotonically, and a job that wants a different rung
// says so.
//
// Off rather than Low/Medium/High, and the reason is the frame contract rather
// than a taste for detail. Every rung blends the previous output through the
// motion field, and the runtime this fork defaults to (mochizuki) carries no
// depth in its 88-byte frame struct - so at an occlusion edge a wrong vector
// puts one frame's content where the next frame's should be, and no trust band
// on this side can undo what the runtime already fused. Off is the only rung
// that cannot do that. The cost is the steadiness the pass existed for: measured
// 8.44 -> 9.45 low-frequency jitter on the A/B bench, so a viewer who prefers a
// calmer picture raises this in Neural settings, and lmxxf - which does carry
// depth - is unaffected by the reason above.
inline constexpr TemporalStability kAmdDefaultStability = TemporalStability::Off;

enum class GpuFamily
{
    Unsupported,
    Rdna4,
};

enum class KernelTarget
{
    Unsupported,
    Gfx1200,
    Gfx1201,
};

constexpr std::string_view GpuFamilyName(GpuFamily family) noexcept
{
    switch (family) {
        case GpuFamily::Unsupported: return "unsupported";
        case GpuFamily::Rdna4: return "amd-rdna4";
    }
    return "unsupported";
}

constexpr std::string_view KernelTargetName(KernelTarget target) noexcept
{
    switch (target) {
        case KernelTarget::Unsupported: return "unsupported";
        case KernelTarget::Gfx1200: return "gfx1200";
        case KernelTarget::Gfx1201: return "gfx1201";
    }
    return "unsupported";
}

constexpr GpuFamily ClassifyGpu(uint32_t vendorId, std::wstring_view description) noexcept
{
    if (vendorId != kAmdVendorId) return GpuFamily::Unsupported;
    if (description.find(L"RX 9070") != std::wstring_view::npos ||
        description.find(L"RX 9060") != std::wstring_view::npos ||
        description.find(L"Radeon AI PRO R9700") != std::wstring_view::npos)
        return GpuFamily::Rdna4;
    return GpuFamily::Unsupported;
}

constexpr KernelTarget SelectKernelTarget(GpuFamily family,
                                          std::wstring_view description) noexcept
{
    if (family != GpuFamily::Rdna4) return KernelTarget::Unsupported;
    if (description.find(L"RX 9060") != std::wstring_view::npos)
        return KernelTarget::Gfx1200;
    if (description.find(L"RX 9070") != std::wstring_view::npos ||
        description.find(L"Radeon AI PRO R9700") != std::wstring_view::npos)
        return KernelTarget::Gfx1201;
    return KernelTarget::Unsupported;
}

struct RuntimeLayout
{
    bool runtimeDll{};
    bool assetsDirectory{};
    bool shaders{};
    bool modules{};
    bool weights{};
    bool lockFile{};

    constexpr bool Complete() const noexcept
    {
        return runtimeDll && assetsDirectory && shaders && modules && weights && lockFile;
    }
    constexpr bool Absent() const noexcept
    {
        return !runtimeDll && !assetsDirectory && !shaders && !modules && !weights && !lockFile;
    }
};

enum class LayoutState
{
    Absent,
    Complete,
    Incomplete,
};

constexpr LayoutState ClassifyLayout(const RuntimeLayout& layout) noexcept
{
    if (layout.Absent()) return LayoutState::Absent;
    return layout.Complete() ? LayoutState::Complete : LayoutState::Incomplete;
}

// The panel's pass count, written where the runtime will read it: the
// `DLSS5_MULTI_PASS` key of the runtime's own custom-config.txt, under
// `DLSS5-AMD\` next to the DLL.
//
// Why a file and not the process environment. The runtime reads its
// configuration exactly once per process, when the network is created
// (LoadFlagsFileOnce is a std::call_once inside the module), and its DLL is a
// static-CRT build with no option setter in its API table - so an environment
// variable set by this process cannot reach it: this program is /MT too, and
// two static CRTs have two private copies of the environment. Measured, not
// assumed: with the count injected into this process's environment, renders at
// one pass and at two came out byte-identical. The file is the channel that
// does work, and rewriting the key is idempotent, so every AMD job states the
// count again before the network is built.
//
// Everything else in the file is left alone - the reader's rules are
// reproduced here rather than replaced (a line counts when it starts with
// DLSS5_, holds '=', and is at least 8 characters; anything else, comments
// included, passes through), because a file rewritten wholesale would take the
// user's other switches with it.
//
// The count is one key with two models behind it, and the 0.44 runtime is
// explicit about the domain: "DLSS5_MULTI_PASS=%s invalid (0.5/1/2/3), using
// 0.5". 0.5 is the 31-block Small model, 1/2/3 the Original 71-block one at
// one, two or three stacked passes - the ladder the runtime's own F9 hotkey
// cycles. 4 is NOT a value this runtime takes: it reads an unknown count as
// 0.5, which is the Small model, so the rung is clamped away here rather than
// handed over as a silently different network. Small is a differently trained
// model, so it is spelled as its own argument and never as a count: the two
// never share a line, and a caller that says Small does not also say a count.
// (The parameter is not called `small`: the Windows SDK's rpcndr.h #defines
// that word as char, and the declaration stopped compiling the day it was.)
inline std::string AmdPassesConfigLine(uint32_t passes, bool smallModel = false)
{
    if (smallModel) return "DLSS5_MULTI_PASS=0.5";
    return "DLSS5_MULTI_PASS=" + std::to_string(passes < 1 ? 1u : (passes > 3 ? 3u : passes));
}

// Whether `line` sets `key` exactly, under the reader's own rule: a line counts
// when it starts with DLSS5_, holds '=' and is long enough, and the key is
// everything before the first '='. Matching the key rather than the prefix is
// what keeps the runtime's own switches apart - DLSS5_MULTI_PASS_SKIN_PROTECT
// and DLSS5_MULTI_PASS_PREDICT share the prefix, and a prefix match swallowed
// the first of them when this writer began as a single-key one (measured, and
// the reason the tests below exist). Same key twice in one file is the last
// line that wins, so this returns true for every occurrence and the writer
// keeps the last one, which is where the reader looks.
inline bool IsAmdConfigLine(std::string_view line, std::string_view key) noexcept
{
    if (line.size() < 8 || line.substr(0, 6) != "DLSS5_") return false;
    const size_t eq = line.find('=');
    return eq != std::string_view::npos && line.substr(0, eq) == key;
}

inline bool IsAmdPassesConfigLine(std::string_view line) noexcept
{
    return IsAmdConfigLine(line, "DLSS5_MULTI_PASS");
}

// The model's own style feature (NGX "Style", preprocess feature 6 = Style/128),
// which this runtime reads from `DLSS5_STYLE` as 0, 1 or 2. It is not one of the
// add-on's NR* keys: the player's neural settings panel carries the add-on's
// NRStyle, which this backend never reads and which is a different control with
// a different meaning, so the two are kept apart here by name and by value.
//
// The runtime's built-in answer is 1 (the style the original runtime used in
// Stellar Blade, .0078125f compiled into the C32/prefix modules). 0 is NVIDIA's
// default and the setting its reference images use; 2 is the third style. A
// value that is not exactly 0, 1 or 2 is read by the runtime as 1, so the
// writer normalises instead of handing over something the reader would quietly
// reinterpret.
inline constexpr uint32_t kAmdStyleDefault = 1;
inline constexpr uint32_t kAmdStyleCount = 3;

inline uint32_t NormalizedAmdStyle(uint32_t style) noexcept
{
    return style < kAmdStyleCount ? style : kAmdStyleDefault;
}

inline std::string AmdStyleConfigLine(uint32_t style)
{
    return "DLSS5_STYLE=" + std::to_string(NormalizedAmdStyle(style));
}

inline bool IsAmdStyleConfigLine(std::string_view line) noexcept
{
    return IsAmdConfigLine(line, "DLSS5_STYLE");
}

// Whether a `history=` token from the runtime's own GetStatus line means the
// temporal model is fusing.
//
// The token is a family rather than a pair of words. 0.43 said `active` and
// `priming`; 0.44 qualifies them with what the model decided about the source -
// `enhance-active` and `enhance-priming` when the temporal enhance feature is
// on, `low-freq-active` and `low-freq-priming` on a source it reads as low
// frequency (which is also the judgement that turns the Small model back into
// the Original one, and the only place that is ever said). A qualified token is
// the same answer given differently: the history is fusing, and the prefix
// says what it is fusing under.
//
// The match is therefore on the ending and not on the whole word, because the
// endings are the part the runtime has never changed and a prefix it has not
// invented yet is not a reason to call a running history a refusal -
// misreading one is exactly the failure this replaced, where `low-freq-priming`
// and `low-freq-active` alternating frame by frame were logged as a refusal on
// every frame of a healthy session.
//
// Everything else is a refusal or a reading that went missing: `off`,
// `reset-after-discard`, `reset-requested`, `low-freq-requires-mp1`, and the
// `unreadable`/`absent` a caller substitutes for a token that never arrived.
inline bool HistoryStatusEngaged(std::string_view token) noexcept
{
    constexpr std::string_view kActive = "active";
    constexpr std::string_view kPriming = "priming";
    if (token.size() < kActive.size()) return false;
    if (token.substr(token.size() - kActive.size()) == kActive) return true;
    if (token.size() < kPriming.size()) return false;
    return token.substr(token.size() - kPriming.size()) == kPriming;
}

// The runtime's own temporal mode (DLSS5_TEMPORAL_MODE), read once per process
// when the network is built, so a change needs a restart to reach a runtime that
// is already loaded. Four rungs, the runtime's own numbering:
//
//   0  off (the runtime's built-in answer), unchanged network output
//   1  TheAutomatic Fast History, an approximation that needs the row/logit
//      exports and post70-history-head.f16 beside the weights
//   2  independently written low-frequency residual EMA: this frame's
//      high-frequency residual is kept and only the guided low-frequency one is
//      smoothed, over roughly 80 ms
//   3  mode 2 plus low/high residual enhancement (DLSS5_TEMPORAL_ENHANCE_STRENGTH)
//
// The one rule that matters here and that is not obvious from the names: every
// active mode requires DLSS5_MULTI_PASS=1 - the Original model at one pass,
// because 0.5 is the Small model and anything above 1 is stacking. A mode
// therefore lives at exactly one point of the ladder, and at the two places it
// does not the runtime says so in words that name neither the ladder nor the
// mode: asked beside the Small model it renders the Original and reports
// "low-frequency-requires-original", and above one pass it runs no mode at all
// and reports history=low-freq-requires-mp1. Both reasons read like a judgement
// about the source; both are the name of mode 2. Which is why the pairing is
// decided here rather than left to the runtime to resolve silently.
inline constexpr uint32_t kAmdTemporalModeDefault = 0;
inline constexpr uint32_t kAmdTemporalModeCount = 4;

inline uint32_t NormalizedAmdTemporalMode(uint32_t mode) noexcept
{
    return mode < kAmdTemporalModeCount ? mode : kAmdTemporalModeDefault;
}

inline std::string AmdTemporalModeConfigLine(uint32_t mode)
{
    return "DLSS5_TEMPORAL_MODE=" + std::to_string(NormalizedAmdTemporalMode(mode));
}

// The rung's name as the runtime's own status line spells it, so a log, a
// receipt and the panel agree on what ran. Mode 2's word is the one the status
// carries while it is fusing, which is also the word that reads like a
// judgement about the source - history=low-freq-active.
inline std::string_view AmdTemporalModeName(uint32_t mode) noexcept
{
    switch (NormalizedAmdTemporalMode(mode)) {
        case 1: return "fast-history";
        case 2: return "low-frequency-ema";
        case 3: return "low-frequency-ema+enhance";
        default: return "off";
    }
}

inline bool IsAmdTemporalModeConfigLine(std::string_view line) noexcept
{
    return IsAmdConfigLine(line, "DLSS5_TEMPORAL_MODE");
}

// The mode a job will actually run under, from the three things that decide it:
// the rung asked for, and the two places a rung does not run. Every active mode
// requires DLSS5_MULTI_PASS=1 - the Original model at one pass - so the Small
// model and a stacked count both take the mode away, and a job carrying a mode
// beside either is a job that renders without one while its own settings claim
// otherwise. Deciding it here is what keeps the claim and the render the same.
inline uint32_t EffectiveAmdTemporalMode(uint32_t mode, bool smallModel, uint32_t passes) noexcept
{
    if (smallModel || passes != 1) return 0;
    return NormalizedAmdTemporalMode(mode);
}

// What the file says right now, so a player taking the key over can adopt the
// value a hand edit left there instead of replacing it with its own default.
// Empty when the file has no such line, which is the runtime's own off - the
// answer a file that never mentioned the key has always given.
inline std::optional<uint32_t> ReadAmdTemporalMode(const std::filesystem::path& runtimeDirectory)
{
    const std::filesystem::path path = runtimeDirectory / L"DLSS5-AMD" / L"custom-config.txt";
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string line;
    std::optional<uint32_t> found;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!IsAmdTemporalModeConfigLine(line)) continue;
        // The reader takes the last line that sets the key, so that is the one
        // a migration has to agree with.
        const std::string value = line.substr(line.find('=') + 1);
        uint32_t parsed = 0;
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) continue;
        try {
            parsed = static_cast<uint32_t>(std::stoul(value));
        } catch (...) {
            continue;
        }
        if (parsed < kAmdTemporalModeCount) found = parsed;
    }
    return found;
}

// Writes the one key and leaves the rest of the file alone - the comments a
// hand edit wrote, and every switch of the runtime's that is not this one. The
// writer of the other keys does the same in the other direction, so neither
// takes the other's lines with it.
inline bool WriteAmdTemporalMode(const std::filesystem::path& runtimeDirectory, uint32_t mode,
                                 std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path directory = runtimeDirectory / L"DLSS5-AMD";
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        error = "the runtime's configuration folder could not be created (" + directory.string() + ")";
        return false;
    }
    const fs::path path = directory / L"custom-config.txt";
    std::vector<std::string> lines;
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream buffer;
        if (input) buffer << input.rdbuf();
        const std::string text = buffer.str();
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // Every earlier line that sets the key is dropped, the way the other
            // writer drops them: the reader takes the last one, so a file that
            // still had the earlier ones would be read the same way either way
            // and one that had comments about them would not lose the comments.
            if (!IsAmdTemporalModeConfigLine(line)) lines.push_back(std::move(line));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    lines.push_back(AmdTemporalModeConfigLine(mode));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "the runtime's configuration file could not be written (" + path.string() + ")";
        return false;
    }
    for (const std::string& line : lines) output << line << "\n";
    if (!output) {
        error = "the runtime's configuration file was not written whole (" + path.string() + ")";
        return false;
    }
    return true;
}

// What the player owns inside the runtime's configuration file: which model
// the count selects (DLSS5_MULTI_PASS: 0.5 Small, 1..3 Original stacked),
// whether later passes keep skin-coloured pixels pinned to the first pass
// (DLSS5_MULTI_PASS_SKIN_PROTECT), the third-pass forecast, the later-pass block
// list, and the style (DLSS5_STYLE). All of them are written on every AMD job -
// a key the menu has released has to be able to switch off one a hand edit
// turned on, and a runtime that reads its configuration once cannot be told
// again.
inline bool WriteAmdRuntimeConfig(const std::filesystem::path& runtimeDirectory, uint32_t passes,
                                  bool smallModel, bool skinProtect, bool multiPassPredict,
                                  bool laterPassSkip, uint32_t style, std::string& error)
{
    namespace fs = std::filesystem;
    const fs::path directory = runtimeDirectory / L"DLSS5-AMD";
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (ec) {
        error = "the runtime's configuration folder could not be created (" + directory.string() + ")";
        return false;
    }
    const fs::path path = directory / L"custom-config.txt";
    // Passes first, then the style: the order they are written in is the order
    // they appear in when neither was in the file.
    std::string keys[5];
    std::string values[5];
    keys[0] = "DLSS5_MULTI_PASS";
    values[0] = AmdPassesConfigLine(passes, smallModel).substr(keys[0].size() + 1);
    keys[1] = "DLSS5_MULTI_PASS_SKIN_PROTECT";
    values[1] = skinProtect ? "1" : "0";
    keys[2] = "DLSS5_MULTI_PASS_PREDICT";
    values[2] = multiPassPredict ? "1" : "0";
    // Off is an EMPTY value, not "0": the runtime treats an empty list as its
    // built-in default (run every block), which is the only spelling that also
    // switches off a list a hand edit left behind. The list itself is the ViT
    // and C512-uplink group the porting project measured - it costs the later
    // passes most of their reshaping, which is where the stacking's ghosting
    // comes from.
    keys[3] = "DLSS5_MULTI_PASS_SKIP_BLOCKS";
    values[3] = laterPassSkip ? "31,32,33,34,35,36,37,38,40,41,42,43,44,45,46,47" : "";
    // Written even at 1, the runtime's own default: that is what makes the menu
    // the authority over a hand-edited line, and 1 renders what an absent line
    // renders, so a machine that never opens the menu sees no change.
    keys[4] = "DLSS5_STYLE";
    values[4] = std::to_string(NormalizedAmdStyle(style));
    constexpr size_t kKeyCount = 5;
    // Where each key's last line sits, or npos when the file has none yet. Every
    // slot is spelled out: a brace list shorter than the array value-initialises
    // the rest to 0, which reads as "the key is on the first line" and silently
    // drops that key from a file that did not have it.
    size_t keepAt[kKeyCount] = {std::string::npos, std::string::npos, std::string::npos,
                                std::string::npos, std::string::npos};
    std::vector<std::string> lines;
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream buffer;
        if (input) buffer << input.rdbuf();
        const std::string text = buffer.str();
        // Two passes over the text: which lines are ours, and where the reader
        // would read each key. Only the LAST occurrence of each key is rewritten
        // and the earlier ones are dropped, so the file the runtime parses agrees
        // with the file it would have parsed before this call.
        {
            size_t start = 0;
            while (start < text.size()) {
                const size_t end = text.find('\n', start);
                std::string_view line(text.data() + start,
                                      (end == std::string::npos ? text.size() : end) - start);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                for (size_t key = 0; key < kKeyCount; ++key)
                    if (IsAmdConfigLine(line, keys[key])) keepAt[key] = start;
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        size_t start = 0;
        while (start < text.size()) {
            const size_t end = text.find('\n', start);
            std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            size_t ours = kKeyCount;
            for (size_t key = 0; key < kKeyCount; ++key)
                if (IsAmdConfigLine(line, keys[key])) ours = key;
            if (ours == kKeyCount) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                lines.push_back(std::move(line));
            } else if (keepAt[ours] == start) {
                lines.push_back(keys[ours] + "=" + values[ours]);
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    for (size_t key = 0; key < kKeyCount; ++key)
        if (keepAt[key] == std::string::npos) lines.push_back(keys[key] + "=" + values[key]);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "the runtime's configuration file could not be written (" + path.string() + ")";
        return false;
    }
    for (const std::string& line : lines) output << line << "\n";
    if (!output) {
        error = "the runtime's configuration file was not written whole (" + path.string() + ")";
        return false;
    }
    return true;
}

} // namespace amd_lmxxf
