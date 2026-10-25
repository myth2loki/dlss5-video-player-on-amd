#pragma once

#include "SubtitleTranslatePolicy.h"

#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// The translation of one subtitle track, from end to end, on its own thread:
// ffmpeg writes the stream's cues out as SRT, llama-server translates them one
// at a time, and the result lands as a single ASS file that SubtitleOverlay
// draws like any other subtitle file.
//
// Nothing here blocks its caller. Start() posts a job and returns; the UI thread
// polls Status() every tick, exactly as it polls SubtitleOverlay::FrameAt. Every
// child - ffmpeg and llama-server - is put in a kill-on-close job object, so a
// cancelled translation, a crashed player or a killed process cannot leave an
// engine holding the GPU.
//
// Cancellation is immediate rather than cooperative: the running job object is
// terminated, which closes the server's socket under the blocked HTTP read. The
// worker then unwinds through its normal error path.
namespace subtitle_translate {

// Where the engine is and how it may run. Read from the settings file by the
// caller: nothing in here decides policy about using the GPU.
struct Engine {
    std::filesystem::path runtimeDirectory;  // holds llama-server.exe
    std::filesystem::path model;             // the .gguf
    int port = 8127;
    int gpuLayers = 99;                      // 0 runs on the CPU and never touches the GPU
    int threads = 0;                         // 0 lets the server choose
    double startTimeoutSeconds = 120.0;      // a cold 2.5 GB model on a slow disk
};

// What to translate.
struct Job {
    // The file holding the subtitle stream: the video itself, or a subtitle file
    // the viewer picked, in which case `stream` is 0 and `external` is set.
    std::wstring path;
    int stream = 0;
    bool external = false;
    // A legacy code page the file needs telling about; empty for everything
    // else (see subtitle::CharencFor).
    std::wstring charenc;
    // What the cache key is built from: the media's own identity and the track.
    std::wstring identity;
    // The picture's size, which the ASS is laid out for.
    uint32_t width = 1920;
    uint32_t height = 1080;
    Languages languages;
};

enum class State {
    Idle,        // nothing has been asked for
    Extracting,  // reading the cues out of the stream
    Starting,    // waiting for llama-server to answer /health
    Translating, // cue by cue
    Done,        // `result` is ready
    Failed,      // `notice` says why
    Cancelled,
};

struct Status {
    State state = State::Idle;
    int total = 0;  // cues found
    int done = 0;   // cues answered
    std::wstring notice;
    std::filesystem::path result;  // the finished ASS, once Done
    // Where that track ends, in film seconds. A translated track is a snapshot
    // of the transcript at the moment its batch ran, and past this the player is
    // looking at a file with nothing in it for that moment.
    double coverage = 0.0;
    // A usable track written while the engine is still working, for the player
    // to swap in without waiting for the end. Each one is a whole ASS of its
    // own, named by how many cues it covers, so the overlay can hot-reload it.
    std::filesystem::path partial;

    bool Running() const
    {
        return state == State::Extracting || state == State::Starting || state == State::Translating;
    }
};

class Translator {
public:
    // `helperDirectory` is where the player looks for ffmpeg.exe, as for the
    // overlay. `cacheDirectory` holds the generated tracks: one file per source,
    // named by the cache key, never inside the media's own folder.
    Translator(std::filesystem::path helperDirectory, Engine engine, std::filesystem::path cacheDirectory);
    ~Translator();

    Translator(const Translator&) = delete;
    Translator& operator=(const Translator&) = delete;

    // Whether the engine could run at all: both files are where the settings say
    // they are. The menu greys its entries out on this rather than on a failure.
    static bool Available(const Engine& engine);

    // Begins translating `job`. A running translation is cancelled first.
    void Start(const Job& job);
    // Ends the running translation. The status becomes Cancelled.
    void Cancel();
    // Also the moment the engine is put away: it is kept between batches, and
    // this is what stops it once nothing has needed it for a while.
    Status Poll();

private:
    void Run();
    // One run, from extraction to the finished ASS. Sets the status on every
    // path; `generation` is the run's, so a superseded one stops at its next
    // checkpoint.
    void RunOnce(const Job& job, uint64_t generation);
    std::optional<std::vector<Cue>> ReadCues(const Job& job, uint64_t generation);
    bool StartServer(uint64_t generation);
    void StopServer();
    std::optional<std::string> Post(const std::string& path, const std::string& body,
                                    const std::string& contentType, int timeoutMillis);
    std::optional<std::string> TranslateOne(const std::string& text, const Languages& languages);
    void SetState(State state, std::wstring notice = {});
    bool Cancelled(uint64_t generation) const;
    // The job object of the run in progress, or null. Safe to hold across a
    // blocking call: only a spawn and a cancel read it.
    HANDLE CurrentJob() const;

    const std::filesystem::path m_helperDirectory;
    const Engine m_engine;
    const std::filesystem::path m_cacheDirectory;
    const ChatTemplate m_chatTemplate;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false;
    // Bumped by every Start and Cancel: a run whose generation is stale stops
    // at its next checkpoint and leaves nothing behind.
    uint64_t m_generation = 0;
    bool m_requested = false;
    Job m_job;
    Status m_status;

    // Worker-only: the server and the handles it needs. Kept out of the mutex'
    // users' reach so no lock is held across a blocking call.
    // One job object for the life of the translator, not one per run: the engine
    // it holds takes about five seconds to start and a transcript is translated
    // in batches a minute apart, so paying that every batch starved the speech
    // engine of the very cores it needs to stay ahead of the picture. Still
    // kill-on-close, so nothing outlives the player.
    HANDLE m_jobObject = nullptr;
    HANDLE m_server = nullptr;
    // Set when a run ends with the engine still up, cleared when it is used or
    // stopped. Poll() stops it once it has been idle this long.
    bool m_serverIdle = false;
    std::chrono::steady_clock::time_point m_serverIdleSince{};
    HINTERNET m_httpSession = nullptr;
    HINTERNET m_httpConnection = nullptr;
    std::thread m_worker;
};

} // namespace subtitle_translate
