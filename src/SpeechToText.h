#pragma once

#include "SubtitleSpeechPolicy.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Speech to subtitles, one window of audio at a time, on its own thread.
//
// The shape is the translator's: a child engine started once, requests made over
// loopback, a status the UI thread polls, and a cancel that terminates the job
// object so a blocked read returns immediately. What differs is the unit of
// work - a window of audio rather than a line of text - and that the track grows
// while it runs, so the player can draw what is already known.
//
// Measured on a 9070 XT's cores with large-v3-turbo q5_0 and 16 threads: 33.9 s
// to transcribe 132 s of audio (RTF 0.26), about 4.8 s per 30 s window. The
// windows are the reason: whisper always encodes a full 30 s of mel, so a short
// window costs what a long one does.
namespace subtitle_speech {

struct Engine {
    std::filesystem::path runtimeDirectory;  // holds whisper-server.exe
    std::filesystem::path model;             // the ggml .bin
    int port = 8137;
    // Cores the engine may have. Zero lets it take as many as it likes, which
    // starves the player: recognition and rendering on the same machine.
    int threads = 0;
    double startTimeoutSeconds = 120.0;
};

struct Job {
    std::wstring path;  // the media file
    int audioStream = 0;
    double duration = 0.0;  // of the media, which decides how many windows there are
    std::string language;   // "en", "ja", ...; empty asks the engine to decide
    std::wstring identity;  // what the cache key is built from
    ChunkPlan plan;
};

enum class State {
    Idle,
    Starting,   // waiting for the engine to answer
    Listening,  // transcribing windows
    Done,
    Failed,
    Cancelled,
};

struct Status {
    State state = State::Idle;
    size_t chunksDone = 0;
    size_t chunksTotal = 0;
    // Media seconds transcribed so far. Compared against the playhead it is how
    // far behind the subtitles are, which is a number worth showing.
    double frontier = 0.0;
    // The newest whole transcript, for the player to draw like any other
    // subtitle file. Every window publishes a new one under a new name, so the
    // overlay sees a different file and hot-reloads it.
    std::filesystem::path track;
    // Where the finished transcript goes. Fixed before the first window, so a
    // player can key a translation of it on a name that does not change while
    // the transcript is still growing.
    std::filesystem::path finalTrack;
    // What the engine decided the audio was in, as it names it ("english"). The
    // translation is told to translate FROM this, so an English film is not
    // handed a prompt that says Japanese.
    std::string language;
    std::wstring notice;

    bool Running() const { return state == State::Starting || state == State::Listening; }
};

class SpeechToText {
public:
    SpeechToText(std::filesystem::path helperDirectory, Engine engine,
                 std::filesystem::path cacheDirectory);
    ~SpeechToText();

    SpeechToText(const SpeechToText&) = delete;
    SpeechToText& operator=(const SpeechToText&) = delete;

    static bool Available(const Engine& engine);

    void Start(const Job& job);
    void Cancel();
    Status Poll() const;
    // Where the viewer is, so the engine can keep a lead rather than transcribe
    // the second half of a film nobody has reached. Negative means nothing is
    // playing and the lead does not apply.
    void SetPlayhead(double seconds);

private:
    void Run();
    void RunOnce(const Job& job, uint64_t generation);
    bool StartServer(uint64_t generation);
    void StopServer();
    HANDLE CurrentJob() const;
    bool Cancelled(uint64_t generation) const;
    void SetState(State state, std::wstring notice = {});
    // One window: extract the audio, ask the engine, hand back its cues in the
    // window's own time.
    std::optional<std::vector<subtitle_translate::Cue>> Listen(const Job& job, const Chunk& chunk,
                                                               uint64_t generation,
                                                               std::string& language);
    // Waits until the transcript is within `lead` seconds of the playhead. False
    // when the run was cancelled while waiting.
    bool WaitForLead(double frontier, double lead, uint64_t generation);

    const std::filesystem::path m_helperDirectory;
    const Engine m_engine;
    const std::filesystem::path m_cacheDirectory;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    uint64_t m_generation = 0;
    bool m_requested = false;
    std::atomic<double> m_playhead{-1.0};
    Job m_job;
    Status m_status;

    // Worker-only, kept out of the UI thread's reach.
    HANDLE m_jobObject = nullptr;
    HANDLE m_server = nullptr;
    HINTERNET m_httpSession = nullptr;
    HINTERNET m_httpConnection = nullptr;
    std::filesystem::path m_chunkFile;
    std::thread m_worker;
};

} // namespace subtitle_speech
