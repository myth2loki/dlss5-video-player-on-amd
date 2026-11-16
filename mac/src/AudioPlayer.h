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
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class AudioStartState {
    Playing,
    Paused,
};

// What one of the source's audio streams turned out to be. `streamIndex` is the
// index AMONG AUDIO STREAMS, which is exactly what ffmpeg's `-map 0:a:N` takes.
// The extra fields are what AudioTrackPolicy needs to choose a default and name
// the list: a disc rip that puts the commentary first is the reason they exist.
struct AudioTrackInfo {
    int streamIndex = -1;
    std::string codec;
    int channels = 0;
    int sampleRate = 0;
    std::string language;
    std::string title;
    bool isDefault = false;
    bool comment = false;
    bool visualImpaired = false;
    bool descriptions = false;
    bool hearingImpaired = false;
    std::optional<double> startSeconds;
};

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // ffprobe, once per path: a seek respawns the child but the container has
    // not changed, and re-probing it would add a probe to every seek.
    // `Probe` keeps the old one-track contract; `ProbeTracks` is what the
    // player's Audio menu uses.
    static bool Probe(const std::string& path, AudioTrackInfo& out);
    static bool ProbeTracks(const std::string& path, std::vector<AudioTrackInfo>& out);

    // `audioIndex` is the position among audio streams. -1 asks the ported
    // AudioTrackPolicy to choose the container's default (or the first ordinary
    // track), which is what a viewer expects when a film carries several.
    bool Start(const std::string& videoPath, double seekSeconds = 0.0,
               AudioStartState state = AudioStartState::Playing, int audioIndex = -1);
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
    const std::vector<AudioTrackInfo>& Tracks() const { return m_tracks; }
    int TrackIndex() const { return m_audioIndex; }

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
    std::vector<AudioTrackInfo> m_tracks;
    int m_audioIndex = -1;
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
