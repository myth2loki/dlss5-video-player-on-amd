#pragma once

// The casting encoder: a ffmpeg child that turns captured RGB frames fed on a
// pipe into an HLS live stream in a fixed directory, plus the audio-decode
// child that turns the current audio track into s16le for muxing.
//
// Design (doc 14.3 / 14.4):
//   - h264_amf first (RX 9070 XT native, R3-verified), libx264 ultrafast +
//     zerolatency fallback. This is a cast-specific realtime path; it does NOT
//     touch EncoderKind (a wire value for offline exports).
//   - The child is spawned suspended into a kill-on-close job with an explicit
//     inherited-handle list (engine_child::Spawn), so a player crash cannot
//     leave ffmpeg holding the GPU or a pipe.
//   - Wall-clock timebase: frames are fed as captured; ffmpeg timestamps them
//     with -use_wallclock_as_timestamps so the HLS playhead follows real time.
//
// Threading: CastEncoder owns its child and a feeding thread. Start/Stop/
// PushFrame are called from the cast session; PushFrame is non-blocking (drops
// on backpressure) so it is safe to call from the render thread.

#include "CastDevicePolicy.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cast_encoder {

struct Settings {
    std::filesystem::path hlsDirectory;   // encoder writes live.m3u8 + seg-N.ts here
    std::wstring ffmpegPath;              // empty -> find next to module / PATH
    uint32_t width{1920};
    uint32_t height{1080};
    int fps{30};
    // Prefer AMD hardware H.264; false forces libx264 (also the fallback when
    // AMF is unavailable — detected by a dry-run probe at Start).
    bool preferAmf{true};
    int bitrateKbps{8000};
    std::string audioSource;              // path to current media; empty -> silent audio
    double audioSeekSeconds{0.0};         // start audio at the player's position
};

struct StartResult {
    bool started{false};
    std::string encoderUsed;              // "h264_amf" or "libx264"
    std::string error;
};

// A frame in native BGRA, row-major, width*height*4 bytes. Owned (moved) so the
// render thread never waits on the encoder.
struct Frame {
    std::vector<uint8_t> bgra;
    uint32_t width{0};
    uint32_t height{0};
    uint64_t id{0};                       // render/source identity for drain checks
};

class Encoder {
public:
    Encoder();
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    StartResult Start(Settings settings);
    // Non-blocking. Returns false when the feed queue is full (frame dropped) or
    // the encoder is not running. Never blocks the caller on the GPU or pipe.
    bool PushFrame(Frame frame);
    // Stop feeding, close stdin (lets ffmpeg finalize HLS), then terminate the
    // child if it does not exit within the timeout. Idempotent.
    void Stop();
    bool Running() const { return m_running.load(std::memory_order_acquire); }
    std::string EncoderName() const { return m_encoderUsed; }
    // Errors captured from ffmpeg stderr tail, for the status line / toast.
    std::string LastError() const;

private:
    void FeedLoop();
    bool SpawnFfmpeg(const Settings& settings, bool useAmf);
    // Decide AMF vs software: prefer the flag unless a probe says AMF is absent.
    bool AmfAvailable(const std::wstring& ffmpeg) const;

    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_running{false};
    std::string m_encoderUsed;
    Settings m_settings;
    std::thread m_feedThread;
};

} // namespace cast_encoder
