#include "SpeechToText.h"

#include "EngineChild.h"
#include "KillOnCloseJob.h"
#include "Log.h"
#include "MediaTools.h"
#include "Utf8Text.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace subtitle_speech {
namespace {

using engine_child::Child;
using engine_child::HttpRequest;
using engine_child::OnExit;
using engine_child::Spawn;
using engine_child::WriteWholeFile;

// A boundary only has to be absent from the audio; this one cannot occur in a
// 7-bit-safe form the engine would ever send back.
constexpr std::string_view kBoundary = "----DLSS5SubtitleBoundary7f3a";

std::string ReadWholeFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The per-window transcripts of an earlier run: "<base>.<seconds>.asr.srt". The
// finished one is "<base>.asr.srt" and is never touched here - a prefix test
// alone would match it, which once deleted the file the player was about to
// open. Removing them is best effort; a version still open in the overlay's
// child cannot be deleted, and that is not worth reporting.
void RemoveVersions(const fs::path& directory, const std::string& base, const fs::path& finalTrack)
{
    std::error_code error;
    const std::string prefix = base + ".";
    const std::string finished = finalTrack.filename().string();
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (name == finished) continue;
        if (it->path().extension() != ".srt") continue;
        if (name.rfind(prefix, 0) != 0) continue;
        std::error_code ignored;
        fs::remove(it->path(), ignored);
    }
}

} // namespace

SpeechToText::SpeechToText(fs::path helperDirectory, Engine engine, fs::path cacheDirectory)
    : m_helperDirectory(std::move(helperDirectory)),
      m_engine(std::move(engine)),
      m_cacheDirectory(std::move(cacheDirectory))
{
    std::error_code error;
    const fs::path directory =
        fs::temp_directory_path(error) / (L"DLSS5-asr-" + std::to_wstring(GetCurrentProcessId()));
    m_chunkFile = directory / L"chunk.wav";
    m_worker = std::thread([this] { Run(); });
}

SpeechToText::~SpeechToText()
{
    HANDLE job = nullptr;
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        ++m_generation;
        job = m_jobObject;
    }
    if (job) TerminateJobObject(job, 1);
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
    StopServer();
    std::error_code error;
    fs::remove_all(m_chunkFile.parent_path(), error);
}

bool SpeechToText::Available(const Engine& engine)
{
    std::error_code error;
    return fs::is_regular_file(engine.runtimeDirectory / L"whisper-server.exe", error) &&
           fs::is_regular_file(engine.model, error);
}

void SpeechToText::SetState(State state, std::wstring notice)
{
    std::lock_guard lock(m_mutex);
    m_status.state = state;
    m_status.notice = std::move(notice);
}

Status SpeechToText::Poll() const
{
    std::lock_guard lock(m_mutex);
    return m_status;
}

bool SpeechToText::Cancelled(uint64_t generation) const
{
    std::lock_guard lock(m_mutex);
    return m_stop || generation != m_generation;
}

HANDLE SpeechToText::CurrentJob() const
{
    std::lock_guard lock(m_mutex);
    return m_jobObject;
}

void SpeechToText::Start(const Job& job)
{
    HANDLE jobObject = nullptr;
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_requested = true;
        m_job = job;
        m_status = {};
        m_status.state = State::Starting;
        jobObject = m_jobObject;
    }
    if (jobObject) TerminateJobObject(jobObject, 1);
    m_wake.notify_all();
}

void SpeechToText::SetPlayhead(double seconds)
{
    m_playhead.store(seconds);
}

bool SpeechToText::WaitForLead(double frontier, double lead, uint64_t generation)
{
    // Four checks a second: the transcript runs at twice real time, so a window
    // of audio is twenty seconds of work against a fifth of a second of lag.
    constexpr DWORD kSlice = 250;
    for (;;) {
        const double playhead = m_playhead.load();
        if (playhead < 0.0) return true;                 // nothing to keep up with
        if (frontier <= playhead + lead) return true;     // near enough
        if (Cancelled(generation)) return false;
        Sleep(kSlice);
    }
}

void SpeechToText::Cancel()
{
    HANDLE jobObject = nullptr;
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_requested = false;
        if (m_status.Running()) m_status.state = State::Cancelled;
        jobObject = m_jobObject;
    }
    if (jobObject) TerminateJobObject(jobObject, 1);
    m_wake.notify_all();
}

void SpeechToText::Run()
{
    for (;;) {
        Job job;
        uint64_t generation = 0;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || m_requested; });
            if (m_stop) return;
            job = m_job;
            generation = m_generation;
            m_requested = false;
        }
        RunOnce(job, generation);
    }
}

void SpeechToText::RunOnce(const Job& job, uint64_t generation)
{
    engine_child::Handle runJob(CreateKillOnCloseJob());
    if (!runJob) {
        SetState(State::Failed, L"speech recognition could not be started");
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_jobObject = runJob.Get();
    }
    auto endOfRun = OnExit([this, jobObject = runJob.Get()] {
        StopServer();
        std::lock_guard lock(m_mutex);
        if (m_jobObject == jobObject) m_jobObject = nullptr;
    });

    const size_t total = ChunkCount(job.plan, job.duration);
    if (total == 0) {
        SetState(State::Failed, L"the film has no duration to listen to");
        return;
    }
    std::error_code error;
    fs::create_directories(m_cacheDirectory, error);
    const std::string base =
        subtitle_translate::CacheKey(utf8_text::FromWide(job.identity), "asr:" + job.language,
                                     m_engine.model.filename().string());
    const fs::path finalTrack = m_cacheDirectory / (base + ".asr.srt");
    // Versions from an earlier run of this same film are not this run's. They are
    // removed here, before anything is written, rather than at the end: a version
    // removed at the end may be the one the overlay's child has just been told to
    // open, and that failure looks like the transcript itself being broken.
    RemoveVersions(m_cacheDirectory, base, finalTrack);
    {
        std::lock_guard lock(m_mutex);
        m_status.chunksTotal = total;
        m_status.finalTrack = finalTrack;
    }

    if (!StartServer(generation)) {
        if (Cancelled(generation)) SetState(State::Cancelled);
        return;
    }
    SetState(State::Listening);

    std::vector<subtitle_translate::Cue> cues;
    double frontier = 0.0;
    const auto started = std::chrono::steady_clock::now();
    for (size_t index = 0;; ++index) {
        const auto chunk = ChunkAt(job.plan, index, job.duration);
        if (!chunk) break;
        if (Cancelled(generation)) {
            SetState(State::Cancelled);
            return;
        }
        // Ahead of the viewer, the engine waits: the rest of the film is not
        // going anywhere, and sixteen cores are worth more to the picture.
        if (!WaitForLead(frontier, job.plan.leadSeconds, generation)) {
            SetState(State::Cancelled);
            return;
        }
        auto heard = Listen(job, *chunk, generation);
        if (!heard) {
            if (Cancelled(generation)) {
                SetState(State::Cancelled);
            } else {
                SetState(State::Failed, L"the speech engine stopped answering");
            }
            return;
        }
        // Split before merging: a window can hold half a minute of speech in one
        // segment, and the pieces belong to different moments.
        AppendChunk(cues, SplitLongCues(*heard), *chunk);
        frontier = std::max(frontier, chunk->end);
        // Each window is a new file rather than a rewrite of the last one: the
        // overlay is told to draw a path, and a path whose contents changed under
        // it is a path it will not reopen. A whole file per window also means a
        // half-written transcript is never opened.
        const fs::path version =
            m_cacheDirectory / (base + "." + std::to_string(size_t(frontier)) + ".asr.srt");
        WriteWholeFile(version, subtitle_translate::FormatSrt(cues, false));
        {
            std::lock_guard lock(m_mutex);
            m_status.chunksDone = index + 1;
            m_status.frontier = frontier;
            m_status.track = version;
        }
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    LOG("Speech: " << cues.size() << " cue(s) from " << total << " window(s) in " << seconds
                   << " s (RTF " << (job.duration > 0.0 ? seconds / job.duration : 0.0) << ")"
                   << " -> " << finalTrack.string());
    // The finished transcript gets the fixed name the translation is keyed on.
    WriteWholeFile(finalTrack, subtitle_translate::FormatSrt(cues, false));
    {
        std::lock_guard lock(m_mutex);
        m_status.state = State::Done;
        m_status.track = finalTrack;
        m_status.frontier = std::max(m_status.frontier, job.duration);
    }
}

bool SpeechToText::StartServer(uint64_t generation)
{
    const fs::path server = m_engine.runtimeDirectory / L"whisper-server.exe";
    Child child;
    // No capture: the engine logs every window it hears, and nobody reads it.
    if (!Spawn(server, ServerArguments(m_engine.model.wstring(), m_engine.port, m_engine.threads),
               CurrentJob(), false, child, "whisper-server")) {
        SetState(State::Failed, L"the speech engine could not be started");
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        m_server = child.process.Release();
    }

    if (!m_httpSession) {
        engine_child::InternetHandle session(
            WinHttpOpen(L"DLSSVideoPlayer/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) {
            LOG("Speech: WinHttpOpen failed winerr=" << GetLastError());
            SetState(State::Failed, L"the speech engine could not be reached");
            return false;
        }
        WinHttpSetTimeouts(session.Get(), 2000, 2000, 30000, 120000);
        m_httpSession = session.Release();
    }
    if (!m_httpConnection) {
        engine_child::InternetHandle connection(
            WinHttpConnect(m_httpSession, L"127.0.0.1", INTERNET_PORT(m_engine.port), 0));
        if (!connection) {
            LOG("Speech: WinHttpConnect(" << m_engine.port << ") failed winerr=" << GetLastError());
            SetState(State::Failed, L"the speech engine could not be reached");
            return false;
        }
        m_httpConnection = connection.Release();
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(int64_t(m_engine.startTimeoutSeconds * 1000.0));
    const auto began = std::chrono::steady_clock::now();
    for (;;) {
        if (Cancelled(generation)) return false;
        // The index page is served once the model is loaded, which is exactly
        // the readiness this needs.
        if (const auto answer = HttpRequest(m_httpConnection, L"GET", "/", {}, {}, 5000);
            answer && answer->status == 200) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            LOG("Speech: whisper-server ready in " << seconds << " s");
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            LOG("Speech: whisper-server never answered");
            SetState(State::Failed, L"the speech engine did not start");
            return false;
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_server && WaitForSingleObject(m_server, 0) == WAIT_OBJECT_0) {
                DWORD exitCode = 1;
                GetExitCodeProcess(m_server, &exitCode);
                LOG("Speech: whisper-server exited with " << exitCode);
                SetState(State::Failed, L"the speech engine failed to start");
                return false;
            }
        }
        Sleep(200);
    }
}

void SpeechToText::StopServer()
{
    HANDLE server = nullptr;
    {
        std::lock_guard lock(m_mutex);
        server = m_server;
        m_server = nullptr;
    }
    if (server) {
        TerminateProcess(server, 0);
        WaitForSingleObject(server, 5000);
        CloseHandle(server);
    }
    engine_child::InternetHandle connection(m_httpConnection);
    m_httpConnection = nullptr;
    engine_child::InternetHandle session(m_httpSession);
    m_httpSession = nullptr;
}

std::optional<std::vector<subtitle_translate::Cue>> SpeechToText::Listen(const Job& job,
                                                                        const Chunk& chunk,
                                                                        uint64_t generation)
{
    const fs::path ffmpeg =
        media_tools::FindTool(m_helperDirectory, L"ffmpeg.exe", media_tools::Fallback::SearchPath);
    if (ffmpeg.empty()) {
        LOG("Speech: ffmpeg.exe was not found");
        return std::nullopt;
    }
    std::error_code error;
    fs::create_directories(m_chunkFile.parent_path(), error);
    {
        Child child;
        if (!Spawn(ffmpeg,
                   AudioArguments(job.path, job.audioStream, chunk.start, chunk.end - chunk.start,
                                  m_chunkFile.wstring()),
                   CurrentJob(), false, child, "ffmpeg"))
            return std::nullopt;
        WaitForSingleObject(child.process.Get(), 60000);
        DWORD code = 1;
        GetExitCodeProcess(child.process.Get(), &code);
        if (Cancelled(generation)) return std::nullopt;
        if (code != 0) {
            LOG("Speech: the audio for window " << chunk.start << " s could not be extracted (exit "
                                                << code << ")");
            return std::nullopt;
        }
    }
    const std::string audio = ReadWholeFile(m_chunkFile);
    if (audio.size() < 44) {  // a WAV header and nothing else
        LOG("Speech: window " << chunk.start << " s produced no audio");
        return std::nullopt;
    }
    // The file has served its purpose; the next window overwrites it anyway.
    fs::remove(m_chunkFile, error);

    const std::string body = MultipartBody(kBoundary, "chunk.wav", audio, job.language);
    const std::string contentType = "multipart/form-data; boundary=" + std::string(kBoundary);
    const auto answer = HttpRequest(m_httpConnection, L"POST", "/inference", body, contentType, 600000);
    if (!answer) return std::nullopt;
    if (answer->status != 200) {
        LOG("Speech: the engine answered " << answer->status << " for window " << chunk.start << " s");
        return std::nullopt;
    }
    // The engine's SRT is in the window's own time; the merge puts it on the
    // film's clock.
    return subtitle_translate::ParseSrt(answer->body);
}

} // namespace subtitle_speech
