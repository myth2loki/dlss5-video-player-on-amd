#pragma once

// The macOS counterpart of src/AudioPlayer.cpp. Same shape, same contracts,
// different platform calls:
//
//   Windows                          here
//   CreateProcessW(ffmpeg.exe)       posix_spawn(ffmpeg)
//   WASAPI / waveOut                 AudioQueue
//   IMMNotificationClient            kAudioHardwarePropertyDefaultOutputDevice
//
// The policy that decides whether the clock is still a clock is not rewritten:
// AudioClockPolicy.h is copied across unchanged, because it is arithmetic on a
// timeline and a timeline is the same on both platforms.

#include "AudioClockPolicy.h"
#include "CoreAudioSink.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

enum class AudioStartState {
    Playing,
    Paused,
};

// What the source's audio turned out to be. Empty when the file has no audio,
// which is not an error: a video-only source plays silently.
struct AudioTrackInfo {
    int streamIndex = -1;      // index among the audio streams, for -map 0:a:N
    std::string codec;
    int channels = 0;
    int sampleRate = 0;
};

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // ffprobe, once per path: a seek respawns the child but the container has
    // not changed, and re-probing it would add a probe to every seek.
    static bool Probe(const std::string& path, AudioTrackInfo& out);

    bool Start(const std::string& videoPath, double seekSeconds = 0.0,
               AudioStartState state = AudioStartState::Playing);
    bool Seek(double seconds);
    void Pause(bool paused);
    void SetVolume(float volume01);
    float Volume() const { return m_volume.load(); }
    void Stop();

    bool Active() const;
    bool Paused() const;

    // The master clock. Negative when there is no audio at all, which is the
    // caller's cue to fall back to its own timer - the same contract
    // AudioPlayer::PositionSeconds has on Windows.
    double PositionSeconds() const;

    // Seek position the current helper process was started at.
    double SeekBaseSeconds() const { return m_seekBaseSeconds; }

    // Restarts audio on the new default output device if the old one went
    // away - headphones unplugged, a default-device change. Cheap when
    // nothing happened, so the player calls it from its frame tick. Pass the
    // player's own position so a device that appears after the film started
    // is opened where the viewer actually is.
    bool ServiceDeviceChanges(double playerPositionSeconds = -1.0, bool playerPlaying = false);

    const AudioTrackInfo& Track() const { return m_track; }

private:
    struct ReaderState;

    void ReaderLoop(std::shared_ptr<ReaderState> state, double seekSeconds, bool startPaused,
                    std::string path, AudioTrackInfo track);
    void Teardown();

    std::shared_ptr<ReaderState> m_reader;
    std::thread m_thread;
    mutable std::mutex m_readerMutex;

    std::string m_path;
    AudioTrackInfo m_track;
    std::string m_probedPath;

    std::atomic<float> m_volume{1.0f};
    std::atomic<double> m_seekBaseSeconds{0.0};

    // PositionSeconds is const and is the only place that can notice the clock
    // has stopped, so the staleness it tracks is mutable.
    mutable std::mutex m_clockMutex;
    mutable audio_clock::StallState m_clock;
    mutable audio_clock::Continuity m_continuity;
    mutable bool m_clockStalled = false;
    mutable std::atomic<double> m_lastKnownPosition{0.0};
};
