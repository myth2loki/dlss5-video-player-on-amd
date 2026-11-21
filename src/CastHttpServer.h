#pragma once

// Tiny static HLS file server for the DLNA cast session, run inside a child
// process (doc 14.1: network-input parsing is isolated out of the player).
// It serves ONE fixed directory and nothing else.
//
// Security model (why no path-traversal surface exists at all):
//   - The only accepted request targets are the two names the encoder emits:
//     "live.m3u8" and "seg-NNNNNN.ts" (N = decimal digits). Anything else —
//     "..", a slash, a backslash, a query, an absolute path — is rejected
//     before any filesystem call. There is no general path resolver to attack.
//   - Every response carries Access-Control-Allow-Origin: * (the DMR's CAF-like
//     clients and the POC receiver need CORS) and Range/Content-Range support,
//     matching the verified serve_hls.py behaviour.
//
// The pure functions (IsAllowedName / ParseRange) are side-effect-free and
// unit-tested in CastHttpServerTests.cpp. RunServer is the Winsock loop the
// headless child process calls; it returns when `stop` is set or on fatal error.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace cast_http_server {

// Accept only the exact names the HLS encoder writes. No '.' segments, no
// separators, no query strings — a strict allowlist, not a "reject ../" list
// (denylists are how path traversal bugs survive).
inline bool IsAllowedName(std::string_view target)
{
    if (target == "live.m3u8") return true;
    constexpr std::string_view prefix = "seg-";
    constexpr std::string_view suffix = ".ts";
    if (target.size() <= prefix.size() + suffix.size()) return false;
    if (target.rfind(prefix, 0) != 0) return false;
    if (target.substr(target.size() - suffix.size()) != suffix) return false;
    const std::string_view digits = target.substr(prefix.size(), target.size() - prefix.size() - suffix.size());
    if (digits.empty()) return false;
    for (char c : digits) if (c < '0' || c > '9') return false;
    return true;
}

// DLNA DMRs choose an HLS demuxer from the HTTP Content-Type. Serving every
// file as application/octet-stream makes them refuse the clip with "content
// not playable" even though the SOAP control and every GET succeed. The
// Python POC the TV actually played used mimetypes.guess_type(); match it.
inline std::string_view ContentTypeFor(std::string_view target)
{
    auto endsWith = [&](std::string_view suffix) {
        if (target.size() < suffix.size()) return false;
        auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
        return std::equal(suffix.rbegin(), suffix.rend(), target.rbegin(),
                          [&](char a, char b) { return lower(a) == lower(b); });
    };
    if (endsWith(".m3u8")) return "application/vnd.apple.mpegurl";
    if (endsWith(".ts")) return "video/mp2t";
    return "application/octet-stream";
}

// A DMR issues its first GET the instant SetAVTransportURI returns. If the
// playlist or the first segment is not on disk yet, that GET is a 404 and the
// TV declares "content not playable" without ever retrying — even though the
// very same segments play fine once they exist. Start() gates the URL push on
// this predicate (with a bounded wait so a broken encoder cannot hang UI).
// Side-effect-free so the exact rule is unit-tested in CastHttpServerTests.
inline bool HlsReady(const std::filesystem::path& dir)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(dir / "live.m3u8", ec) || ec) return false;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) return false;
        const std::string name = entry.path().filename().string();
        if (name == "live.m3u8" || !IsAllowedName(name)) continue; // segments only
        std::error_code sizeEc;
        const auto size = entry.file_size(sizeEc);
        if (!sizeEc && size > 0) return true;
    }
    return false;
}

struct ByteRange {
    uint64_t first{0};
    uint64_t last{0}; // inclusive
};

// Parse "bytes=START-END" / "bytes=START-" / "bytes=-SUFFIX". Returns nullopt
// for malformed or unsatisfiable ranges; the caller then serves the whole file
// (200) or 416 as appropriate. Overlong numbers overflow -> nullopt, never wrap.
inline std::optional<ByteRange> ParseRange(std::string_view header, uint64_t fileSize)
{
    constexpr std::string_view unit = "bytes=";
    if (header.rfind(unit, 0) != 0) return std::nullopt;
    const std::string_view spec = header.substr(unit.size());
    const std::size_t dash = spec.find('-');
    if (dash == std::string_view::npos) return std::nullopt;
    const std::string_view lhs = spec.substr(0, dash);
    const std::string_view rhs = spec.substr(dash + 1);
    auto toU64 = [](std::string_view text, uint64_t& out) -> bool {
        if (text.empty()) return false;
        uint64_t value = 0;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
            const uint64_t digit = static_cast<uint64_t>(c - '0');
            if (value > (UINT64_MAX - digit) / 10) return false; // overflow guard
            value = value * 10 + digit;
        }
        out = value;
        return true;
    };
    if (lhs.empty()) {
        // Suffix form: last N bytes.
        uint64_t count = 0;
        if (!toU64(rhs, count) || count == 0) return std::nullopt;
        if (count >= fileSize) return ByteRange{0, fileSize - 1};
        return ByteRange{fileSize - count, fileSize - 1};
    }
    uint64_t first = 0;
    if (!toU64(lhs, first) || first >= fileSize) return std::nullopt;
    uint64_t last = fileSize - 1;
    if (!rhs.empty()) {
        if (!toU64(rhs, last) || last < first) return std::nullopt;
        if (last >= fileSize) last = fileSize - 1;
    }
    return ByteRange{first, last};
}

struct Settings {
    std::filesystem::path directory;
    std::string bindIp{"0.0.0.0"};
    unsigned short port{8080};
    std::function<bool()> stop; // checked between accepts; null => run forever
};

// Blocking accept/serve loop. One connection at a time is plenty: a DMR opens
// a handful of sequential GETs, and HLS segments are a few MB. Returns false
// only on a socket/init failure that makes serving impossible.
bool RunServer(const Settings& settings);

} // namespace cast_http_server
