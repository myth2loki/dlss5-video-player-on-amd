#pragma once

// The playback clock.
//
// On the Windows side this role is split between AudioClockPolicy.h and the
// player's own fallback timer; here it is one class, but with the same rule:
// **audio is the master clock whenever there is audio**, and the monotonic
// timer is only a fallback for a film with no sound or no output device.
//
// That ordering is not a preference. A picture driven by its own timer drifts
// against the sound by however much the sound card's crystal differs from the
// system's, which is a frame every few minutes and audible within ten. Driving
// the picture from the frames the device has actually consumed removes the
// drift by construction, because there is only one clock.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

class PlaybackClock {
public:
    void SetFrameRate(double fps) { m_fps = fps > 0.0 ? fps : 24.0; }
    double FrameRate() const { return m_fps; }

    void SetDuration(double seconds) { m_duration = std::max(0.0, seconds); }
    double Duration() const { return m_duration; }

    // `audio` is the AudioPlayer that owns the master clock. It may outlive
    // this call and may report nothing (negative) at any moment; the fallback
    // covers both.
    void SetAudioSource(const class AudioPlayer* audio) { m_audio = audio; }

    void Play() {
        if (m_playing) return;
        m_anchor = Now();
        m_anchorSeconds = Position();
        m_playing = true;
    }

    void Pause() {
        if (!m_playing) return;
        m_position = Position();
        m_playing = false;
    }

    void Toggle() { m_playing ? Pause() : Play(); }
    bool IsPlaying() const { return m_playing; }

    void Seek(double seconds) {
        m_position = std::clamp(seconds, 0.0, m_duration > 0.0 ? m_duration : seconds);
        m_anchor = Now();
        m_anchorSeconds = m_position;
    }

    // Audio position when audio is answering, the monotonic timer otherwise.
    double Position() const {
        if (m_audio) {
            const double audio = m_audio->PositionSeconds();
            // audio_clock::Present has already carried a stalled clock forward
            // and slewed it back, so a non-negative answer is always usable
            // here - including during an underrun, which is the whole point of
            // that policy.
            if (audio >= 0.0) return Clamp(audio);
        }
        if (!m_playing) return m_position;
        const double elapsed = std::chrono::duration<double>(Now() - m_anchor).count();
        return Clamp(m_anchorSeconds + elapsed);
    }

    int64_t CurrentFrame() const {
        return static_cast<int64_t>(std::llround(Position() * m_fps));
    }

    bool AtEnd() const { return m_duration > 0.0 && Position() >= m_duration; }

    void SetFill(bool fill) { m_fill = fill; }
    bool Fill() const { return m_fill; }

    // Whether the position in use is the audio's or the fallback timer's. The
    // status line says which, because "silent film" and "audio device gone"
    // look identical otherwise.
    bool AudioDriven() const {
        return m_audio && m_audio->PositionSeconds() >= 0.0;
    }

private:
    using TimePoint = std::chrono::steady_clock::time_point;
    static TimePoint Now() { return std::chrono::steady_clock::now(); }

    double Clamp(double position) const {
        if (position < 0.0) return 0.0;
        if (m_duration > 0.0 && position > m_duration) return m_duration;
        return position;
    }

    double m_fps = 24.0;
    double m_duration = 0.0;
    double m_position = 0.0;
    double m_anchorSeconds = 0.0;
    TimePoint m_anchor{};
    bool m_playing = false;
    bool m_fill = false;
    const AudioPlayer* m_audio = nullptr;
};
