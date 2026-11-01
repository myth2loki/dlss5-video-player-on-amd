#pragma once

// The CoreAudio counterpart of WasapiRenderer.cpp: a sink that accepts PCM
// buffers, reports which have actually been heard, and can be faded out
// without cutting a waveform in half.
//
// AudioQueue rather than an AudioUnit graph, because the thing being handed in
// is a byte stream with no framing of its own - the same shape the Windows
// renderer receives from its ffmpeg child - and AudioQueue's buffer callbacks
// map onto that one-to-one. An AUHAL graph would additionally mean owning a
// render thread and a clock, both of which AudioQueue already provides.
//
// The clock matters more than the plumbing: PositionSeconds in the player is
// the audio position whenever there is one, and everything downstream (the
// presentation gate, the frame scheduler) is slaved to it. AudioQueue reports
// frames consumed rather than frames submitted, which is exactly the quantity
// a master clock needs.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class CoreAudioSink {
public:
    enum class Status {
        Ok,
        Failed,
        Refused,
    };

    // Write's three outcomes. Aborted is not Failed: it means the caller asked
    // to stop, and the sink is still perfectly usable.
    enum class WriteResult {
        Ok,
        Aborted,
        Failed,
    };

    // Why a Write may be abandoned. Both are read on every wait, so a pipe
    // reader blocked on a full queue still notices a stop or a pause - without
    // this, Stop()'s join deadlocks and Pause() leaves a writer that restarts
    // the queue behind the fade.
    struct AbortFlags {
        const std::atomic<bool>* stop = nullptr;
        const std::atomic<bool>* paused = nullptr;

        bool Aborted() const {
            return (stop && stop->load()) || (paused && paused->load());
        }
    };

    struct Format {
        uint32_t sampleRate = 0;
        uint16_t channels = 0;
        uint16_t bitsPerSample = 0;
        bool isFloat = false;

        uint32_t BytesPerFrame() const {
            return uint32_t(channels) * uint32_t(bitsPerSample) / 8u;
        }
        bool Valid() const { return sampleRate > 0 && channels > 0 && bitsPerSample > 0; }
    };

    CoreAudioSink();
    ~CoreAudioSink();

    CoreAudioSink(const CoreAudioSink&) = delete;
    CoreAudioSink& operator=(const CoreAudioSink&) = delete;

    // Opens the default output device at `format`. On a real machine this is
    // the format ffmpeg was already told to produce, so neither side resamples.
    Status Open(const Format& format);

    // Hands `bytes` to the queue, blocking while every buffer is in flight.
    // `abort` is polled while waiting, so a stop or a pause is noticed even
    // when the device has asked for nothing for a while.
    WriteResult Write(const uint8_t* bytes, size_t count, const AbortFlags& abort);

    // Wakes any writer blocked in Write so it can see a newly set abort flag.
    // Called after the flags are stored, which is what closes the race between
    // "flag set" and "writer already waiting".
    void Wake();

    // Frames the device has actually consumed since Open. This is the clock.
    bool PlayedFrames(uint64_t& out) const;

    void SetVolume(float volume01);
    float Volume() const { return m_volume; }

    // Pause is a ramp to silence and a stop, not a cut. The frames already
    // queued drain rather than being discarded, so the position ends at the
    // end of what was queued rather than in the middle of a waveform, and
    // resuming opens with a matching ramp.
    bool Start();
    bool FadeOutAndStop();

    void Close();

    bool IsOpen() const { return m_open; }

    // What the device actually gave us, which can differ from what was asked
    // for if the system has to resample.
    const Format& OpenedFormat() const { return m_format; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    Format m_format;
    float m_volume = 1.0f;
    bool m_open = false;
};
