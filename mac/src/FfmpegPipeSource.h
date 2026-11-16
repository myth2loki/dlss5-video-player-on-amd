#pragma once

// A macOS port of the player's decoding strategy, not of its code.
//
// The Windows VideoDecoder.cpp does not link libav*: it spawns ffmpeg.exe,
// asks for headerless rawvideo on a pipe, and parses fixed-size frames out of
// it (see the "-f rawvideo pipe:1" and "-pix_fmt" assembly near its line 1024).
// That is what makes this port cheap - the arguments, the pixel layout and the
// frame arithmetic all carry over unchanged. What does not carry over is
// CreateProcessW, the inherited-handle attribute list and the job object, so
// this file is the POSIX equivalent of that one layer and nothing else.
//
// Frames are BGRA, tightly packed, top-down - the same layout the Windows
// decoder hands its upload path for a normal playback open.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct MediaInfo {
    int width = 0;
    int height = 0;
    double fps = 0.0;
    double duration = 0.0;

    bool Valid() const { return width > 0 && height > 0 && fps > 0.0; }
    size_t FrameBytes() const { return size_t(width) * size_t(height) * 4; }
};

struct VideoFrame {
    std::vector<uint8_t> bgra;
    // Position on the constant-frame-rate timeline, the same quantity the
    // Windows decoder carries as VideoFrame::frameNumber.
    int64_t number = 0;
};

class FfmpegPipeSource {
public:
    FfmpegPipeSource() = default;
    ~FfmpegPipeSource();

    FfmpegPipeSource(const FfmpegPipeSource&) = delete;
    FfmpegPipeSource& operator=(const FfmpegPipeSource&) = delete;

    // ffprobe, once. False leaves the output untouched.
    static bool Probe(const std::string& path, MediaInfo& out);

    bool Open(const std::string& path, const MediaInfo& info);
    void Close();

    // Restarts the child at `seconds`. Frames that no longer belong to the new
    // position are dropped by the producer, so a consumer only ever sees a
    // monotonic sequence.
    void Seek(double seconds);

    // Blocks until a frame is available or the source closes. False means the
    // source is finished or closed.
    bool Pop(VideoFrame& out);

    // The same without waiting. The render path needs this: it runs on the
    // window's own thread and must never block on a decoder.
    bool TryPop(VideoFrame& out);

    // The number of the frame at the head of the queue, without taking it.
    //
    // This exists because TryPop followed by "too new, put it back" is not
    // possible - TryPop has already removed it. A consumer that needs to stop
    // before a frame it is not ready for has to look first. Returns false when
    // the queue is empty.
    //
    // The number is read under the same lock the producer appends under, so a
    // caller cannot see a frame appear between asking and popping. It is stable
    // for the caller's next TryPop only if no other thread consumes; this
    // player has exactly one consumer, which is what makes the pair safe.
    bool PeekFrontNumber(int64_t& number) const;

    // Everything decoded before `number` is discarded. The consumer calls this
    // when it is behind and must catch up rather than replay.
    void DiscardBefore(int64_t number);

    // Whether the child has exited and will produce no more frames.
    //
    // This is not "the queue is empty": a decoder that is merely behind reports
    // empty without being finished, and a caller that confused the two would end a
    // film the moment its pipe ran dry. SegmentPlayback needs exactly this
    // distinction to tell a warming decoder from one at the end of its file.
    bool Finished() const
    {
        std::scoped_lock lock(m_mutex);
        return m_finished;
    }

    int width() const { return m_info.width; }
    int height() const { return m_info.height; }
    double fps() const { return m_info.fps; }

private:
    void Produce();
    bool SpawnChild(double startSeconds);
    void KillChild();

    MediaInfo m_info;
    std::string m_path;

    std::thread m_producer;
    std::atomic<bool> m_stop{false};
    std::atomic<double> m_pendingSeek{-1.0};

    // mutable because PeekFrontNumber is const and still has to lock: asking
    // what the next frame is does not change the source, but reading the queue
    // is not safe without the lock the producer appends under.
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::deque<VideoFrame> m_frames;
    // Set when the child exits, so Pop can stop waiting.
    bool m_finished = false;
    std::atomic<int64_t> m_discardBefore{-1};

    // The child's stdout. Owned by the producer thread; -1 when nothing runs.
    //
    // Guarded by m_pipeMutex because Close() has to reach them from the caller's
    // thread to release a blocked read. Everything else that touches them does so
    // on the producer, which is why the mutex is held for those two fields only
    // and never across a wait.
    mutable std::mutex m_pipeMutex;
    int m_pipe = -1;
    int m_pid = -1;

    static constexpr size_t kQueueLimit = 10;
};
