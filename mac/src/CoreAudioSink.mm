#include "CoreAudioSink.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

// Three buffers of ~20 ms each. The Windows renderer's engine period is 22 ms
// in shared mode, so this is the same amount of lead: enough that a scheduling
// hiccup does not underrun, short enough that a seek does not have to throw
// away a long tail of already-decoded sound.
constexpr int kBufferCount = 3;
constexpr double kBufferSeconds = 0.02;

// The pause ramp. A hard stop clicks; WasapiRenderer's fade is the same shape
// for the same reason. 8 ms is below the threshold at which a fade is heard as
// a fade rather than as a clean stop.
constexpr double kFadeSeconds = 0.008;

const char* Describe(OSStatus status)
{
    // The four-character-code errors are the ones worth reading; a bare
    // integer tells nobody anything.
    static thread_local char buffer[32];
    const uint32_t value = uint32_t(status);
    const char a = char((value >> 24) & 0xff);
    const char b = char((value >> 16) & 0xff);
    const char c = char((value >> 8) & 0xff);
    const char d = char(value & 0xff);
    if (a >= 32 && a < 127 && b >= 32 && b < 127 && c >= 32 && c < 127 && d >= 32 && d < 127)
        std::snprintf(buffer, sizeof(buffer), "%d ('%c%c%c%c')", int(status), a, b, c, d);
    else
        std::snprintf(buffer, sizeof(buffer), "%d", int(status));
    return buffer;
}

} // namespace

struct CoreAudioSink::Impl {
    AudioQueueRef queue = nullptr;
    std::vector<AudioQueueBufferRef> buffers;
    size_t bufferBytes = 0;

    // Which buffers the queue owns right now. The callback clears a slot; Write
    // waits for one.
    std::mutex mutex;
    std::condition_variable freeBuffer;
    std::vector<bool> inFlight;

    // Sum of the byte counts of every buffer handed to the queue, used only to
    // size the copy. The clock comes from the queue's own timeline.
    std::atomic<bool> failed{false};
    std::atomic<bool> running{false};

    // Frames the device has consumed, accumulated across discontinuities: a
    // stop/start pair resets the queue's own timeline, so keeping the running
    // total here is what makes PlayedFrames monotonic across a pause.
    std::atomic<uint64_t> framesBase{0};
    uint64_t framesAtLastStart = 0;

    Format format;

    static void Callback(void* userData, AudioQueueRef queue, AudioQueueBufferRef buffer)
    {
        (void)queue;  // only the owning Impl is needed; the queue is its own
        auto* self = static_cast<Impl*>(userData);
        {
            std::scoped_lock lock(self->mutex);
            for (size_t index = 0; index < self->buffers.size(); ++index) {
                if (self->buffers[index] == buffer) {
                    self->inFlight[index] = false;
                    break;
                }
            }
        }
        self->freeBuffer.notify_one();
    }
};

CoreAudioSink::CoreAudioSink() : m_impl(std::make_unique<Impl>()) {}

CoreAudioSink::~CoreAudioSink() { Close(); }

CoreAudioSink::Status CoreAudioSink::Open(const Format& format)
{
    Close();
    if (!format.Valid()) return Status::Failed;

    Impl& state = *m_impl;
    state.format = format;
    state.failed = false;
    state.framesBase = 0;
    state.framesAtLastStart = 0;

    AudioStreamBasicDescription description{};
    description.mSampleRate = double(format.sampleRate);
    description.mFormatID = kAudioFormatLinearPCM;
    // Interleaved, packed, and in the same sample format ffmpeg was told to
    // write: pcm_f32le becomes float, pcm_s16le becomes signed integer. Native
    // endianness on purpose - macOS is little-endian on both architectures, so
    // "le" needs no swap on the way in.
    description.mFormatFlags = kAudioFormatFlagIsPacked;
    if (format.isFloat) {
        description.mFormatFlags |= kAudioFormatFlagIsFloat;
    } else {
        description.mFormatFlags |= kAudioFormatFlagIsSignedInteger;
    }
    description.mChannelsPerFrame = format.channels;
    description.mBitsPerChannel = format.bitsPerSample;
    description.mFramesPerPacket = 1;
    description.mBytesPerFrame = format.BytesPerFrame();
    description.mBytesPerPacket = description.mBytesPerFrame;

    OSStatus status = AudioQueueNewOutput(&description, &Impl::Callback, &state,
                                          nullptr, nullptr, 0, &state.queue);
    if (status != noErr) {
        std::fprintf(stderr, "audio: AudioQueueNewOutput failed: %s\n", Describe(status));
        state.queue = nullptr;
        return Status::Failed;
    }

    const double framesPerBuffer = format.sampleRate * kBufferSeconds;
    state.bufferBytes = size_t(framesPerBuffer) * format.BytesPerFrame();
    // A buffer has to be a whole number of frames and at least one.
    if (state.bufferBytes == 0) state.bufferBytes = format.BytesPerFrame();

    state.buffers.assign(kBufferCount, nullptr);
    state.inFlight.assign(kBufferCount, false);
    for (int index = 0; index < kBufferCount; ++index) {
        status = AudioQueueAllocateBuffer(state.queue, UInt32(state.bufferBytes), &state.buffers[index]);
        if (status != noErr) {
            std::fprintf(stderr, "audio: AudioQueueAllocateBuffer failed: %s\n", Describe(status));
            Close();
            return Status::Failed;
        }
        state.buffers[index]->mAudioDataByteSize = 0;
    }

    // The volume lives on the queue, so SetVolume is a parameter rather than a
    // per-sample multiply.
    AudioQueueSetParameter(state.queue, kAudioQueueParam_Volume, m_volume);

    m_format = format;
    m_open = true;
    return Status::Ok;
}

void CoreAudioSink::Wake()
{
    // The wait predicate reads the abort flags, which the caller has already
    // stored by the time it calls this; notifying is all that is left.
    m_impl->freeBuffer.notify_all();
}

CoreAudioSink::WriteResult CoreAudioSink::Write(const uint8_t* bytes, size_t count,
                                                const AbortFlags& abort)
{
    Impl& state = *m_impl;
    if (!m_open || !state.queue || !bytes || count == 0) return WriteResult::Failed;
    if (state.failed.load()) return WriteResult::Failed;

    // A write larger than one buffer is split, which is what the reader thread
    // naturally produces: it reads whatever the pipe had, not a fixed size.
    size_t offset = 0;
    while (offset < count) {
        // A pause takes effect between chunks, not mid-chunk: the buffer
        // already being built is finished so the queue is never handed a
        // partial frame, and the fade that follows is not fighting a writer
        // that is still enqueueing.
        if (abort.Aborted()) return WriteResult::Aborted;

        size_t slot = 0;
        {
            std::unique_lock lock(state.mutex);
            // Predicate rather than an unbounded wait: the queue only calls
            // back when a buffer drains, so a device that has stopped asking
            // for data would otherwise block this thread forever - and with
            // it Stop()'s join.
            if (!state.freeBuffer.wait_for(lock, std::chrono::milliseconds(20), [&] {
                    for (size_t index = 0; index < state.inFlight.size(); ++index)
                        if (!state.inFlight[index]) { slot = index; return true; }
                    return state.failed.load() || abort.Aborted();
                })) {
                continue;  // timed out: loop and re-check the abort flags
            }
            if (state.failed.load()) return WriteResult::Failed;
            if (abort.Aborted()) return WriteResult::Aborted;
            state.inFlight[slot] = true;
        }

        const size_t chunk = std::min(count - offset, state.bufferBytes);
        AudioQueueBufferRef buffer = state.buffers[slot];
        std::memcpy(buffer->mAudioData, bytes + offset, chunk);
        buffer->mAudioDataByteSize = UInt32(chunk);

        if (AudioQueueEnqueueBuffer(state.queue, buffer, 0, nullptr) != noErr) {
            state.failed = true;
            state.freeBuffer.notify_all();
            return WriteResult::Failed;
        }
        offset += chunk;

        // Enqueueing does not start the queue; the first buffer does. Doing it
        // here rather than in Open means a source with no audio never opens a
        // device that then sits silent.
        if (!state.running.load()) {
            if (AudioQueueStart(state.queue, nullptr) == noErr) {
                state.running = true;
            } else {
                state.failed = true;
                state.freeBuffer.notify_all();
                return WriteResult::Failed;
            }
        }
    }
    return WriteResult::Ok;
}

bool CoreAudioSink::PlayedFrames(uint64_t& out) const
{
    const Impl& state = *m_impl;
    if (!m_open || !state.queue) return false;

    AudioTimeStamp stamp{};
    // Only the sample time: the host time would have to be converted through
    // the device's own clock and answers a different question - when the
    // sound was made rather than how much of it has been heard.
    if (AudioQueueGetCurrentTime(state.queue, nullptr, &stamp, nullptr) != noErr) {
        // A stopped queue reports nothing, which is not an error: the frames
        // already played are still the answer.
        out = state.framesBase.load();
        return true;
    }
    if ((stamp.mFlags & kAudioTimeStampSampleTimeValid) == 0) {
        out = state.framesBase.load();
        return true;
    }
    out = state.framesBase.load() + uint64_t(stamp.mSampleTime > 0 ? stamp.mSampleTime : 0);
    return true;
}

void CoreAudioSink::SetVolume(float volume01)
{
    m_volume = std::clamp(volume01, 0.0f, 1.0f);
    Impl& state = *m_impl;
    if (!m_open || !state.queue) return;
    AudioQueueSetParameter(state.queue, kAudioQueueParam_Volume, m_volume);
}

bool CoreAudioSink::Start()
{
    Impl& state = *m_impl;
    if (!m_open || !state.queue) return false;
    if (state.running.load()) return true;
    if (AudioQueueStart(state.queue, nullptr) != noErr) return false;
    state.running = true;
    return true;
}

bool CoreAudioSink::FadeOutAndStop()
{
    Impl& state = *m_impl;
    if (!m_open || !state.queue) return false;

    // The ramp is applied by the device, over what it has left to play, so the
    // queue drains without a click and the position lands at the end of the
    // queue rather than mid-waveform.
    AudioQueueSetParameter(state.queue, kAudioQueueParam_VolumeRampTime, kFadeSeconds);
    AudioQueueSetParameter(state.queue, kAudioQueueParam_Volume, 0.0f);
    const OSStatus status = AudioQueueStop(state.queue, false);
    if (status != noErr) return false;

    // Everything that was queued has now been consumed. Fold it into the base
    // so the clock stays monotonic across the pause.
    AudioTimeStamp stamp{};
    if (AudioQueueGetCurrentTime(state.queue, nullptr, &stamp, nullptr) == noErr &&
        (stamp.mFlags & kAudioTimeStampSampleTimeValid) != 0 && stamp.mSampleTime > 0) {
        state.framesBase += uint64_t(stamp.mSampleTime);
    }
    state.running = false;

    {
        std::scoped_lock lock(state.mutex);
        std::fill(state.inFlight.begin(), state.inFlight.end(), false);
    }
    state.freeBuffer.notify_all();

    // Restore the volume for the resume, after the ramp has been undone.
    AudioQueueSetParameter(state.queue, kAudioQueueParam_Volume, m_volume);
    return true;
}

void CoreAudioSink::Close()
{
    Impl& state = *m_impl;
    if (state.queue) {
        AudioQueueStop(state.queue, true);
        AudioQueueDispose(state.queue, true);
        state.queue = nullptr;
    }
    state.buffers.clear();
    state.inFlight.clear();
    state.running = false;
    m_open = false;
}
