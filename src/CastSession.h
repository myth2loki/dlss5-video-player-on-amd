#pragma once

// DLNA cast session: the state machine that owns the whole casting pipeline.
// It is the single object the player UI talks to — the toolbar button, device
// menu, status line and WM_CLOSE all call into here. All six grilled decisions
// (doc 14.1–14.6) and the multi-device / liveness fixes (14.8/14.9) land here.
//
// Ownership / threads:
//   - CastSession lives on the player thread. Its methods are cheap state
//     transitions plus starting/stopping owned children and threads.
//   - The camera feed calls SubmitFrame every rendered frame; it is non-blocking
//     and safe from the render thread (drops on backpressure).
//   - A background thread runs the HLS HTTP server; a DeferredCapture-style
//     feed carries frames to the encoder child.
//
// Lifecycle (14.6):
//   - Pause: the render loop stops, so CastSession caches the last frame and
//     keeps re-feeding it (wall-clock timestamps keep advancing; HLS never
//     breaks). The player's own PlayPause state is untouched (14.4).
//   - End of stream: keep the last frame + silence, never auto-disconnect.
//   - Failure: restart the children and re-push once from the latest frame; a
//     second failure reports and stops. No infinite retry.
//   - Source change: Stop() drops the session but remembers the chosen UDN.
//   - Close window: Stop() is idempotent and joins every owned thread/child.
//   - Toggle: selecting the active device again stops casting.

#include "CastControl.h"
#include "CastDevicePolicy.h"
#include "CastDiscovery.h"
#include "CastEncoder.h"
#include "CastHttpServer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cast_session {

enum class State {
    Idle,
    Scanning,
    Connecting,
    Streaming,
    Reconnecting,
    Error,
};

inline const char* StateName(State state)
{
    switch (state) {
        case State::Idle: return "Idle";
        case State::Scanning: return "Scanning";
        case State::Connecting: return "Connecting";
        case State::Streaming: return "Streaming";
        case State::Reconnecting: return "Reconnecting";
        case State::Error: return "Error";
    }
    return "Unknown";
}

struct Status {
    State state{State::Idle};
    std::string deviceName;    // display name (disambiguated) of the active device
    std::string deviceUdn;
    std::string streamUrl;
    std::string encoder;       // "h264_amf" / "libx264"
    uint32_t width{0}, height{0};
    int fps{0};
    std::string detail;        // last event / error for the status line
};

struct Config {
    unsigned short httpPort{8080};
    std::string httpBindIp{"0.0.0.0"};
    uint32_t width{1920};
    uint32_t height{1080};
    int fps{30};
    bool preferAmf{true};
    int bitrateKbps{8000};
    std::wstring ffmpegPath;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // --- discovery (control-plane, any thread) ---
    // Run an SSDP scan, then probe every DMR it can see so the menu reports
    // liveness rather than a stale flag. Returns the number of devices seen.
    // Safe to call from a background thread; registry access is serialized.
    int Scan(std::chrono::seconds listen = std::chrono::seconds{6});
    // Ask a running Scan to return early. Bounded: the caller still has to join
    // its own scan thread; this only shortens the wait.
    void CancelScan();

    // Why a device carries a caveat, kept as a code instead of a sentence so
    // the UI owns the words. The backend must not decide the interface language.
    enum class DeviceLabel {
        None,           // castable, nothing to warn about
        DialOnly,       // answers, but is a DIAL endpoint and not a DMR
        Offline,        // did not answer, or dropped past the TTL
        IdleReachable,  // answered a GetTransportInfo with a 501: alive, idle
        Detail,         // backend detail worth showing verbatim (ASCII)
    };

    // A snapshot for the device menu, with display names disambiguated.
    struct MenuEntry {
        std::string key;         // UDN (identity)
        std::string displayName;
        bool online{false};
        bool dmr{false};
        bool reachable{false};
        DeviceLabel label{DeviceLabel::None};
        std::string detail;      // only meaningful when label == Detail
    };
    std::vector<MenuEntry> DeviceMenu() const;

    // --- session control ---
    // Cast to one device. `mediaPath` is the current video (for the audio
    // decode branch); `seekSeconds` the player position; `paused` whether the
    // player is paused. The stream URL is built from the local LAN IP + port.
    bool Start(const std::string& deviceUdn, const std::string& mediaPath,
               double seekSeconds, bool paused, uint64_t startRenderId = 0,
               std::string* error = nullptr);
    // Stop casting; idempotent. Remembers the device UDN for "last device".
    void Stop();
    // Feed one evaluated (upscaled) frame. Non-blocking; call every rendered
    // frame. `renderId` is the id at capture time for the drain hard check.
    void SubmitFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint64_t renderId);
    // Tell the session the player paused/resumed so it can hold/replay a frame.
    void SetPaused(bool paused);
    // Called when the source changes: stop casting, keep the device choice.
    void OnSourceChanged();

    Status GetStatus() const;
    State GetState() const;
    bool Casting() const;
    std::string LastDeviceUdn() const;

    // Mutable configuration (set before Start; read at Start).
    Config& Config_();

    // Persist / recall the last successful device (writes next to the log).
    void SaveLastDevice() const;
    std::string LoadLastDevice() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace cast_session
