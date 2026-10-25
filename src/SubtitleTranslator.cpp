#include "SubtitleTranslator.h"

#include "EngineChild.h"

#include "KillOnCloseJob.h"
#include "Log.h"
#include "MediaTools.h"
#include "Utf8Text.h"

#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <functional>
#include <system_error>

namespace fs = std::filesystem;

namespace subtitle_translate {
namespace {

// The first partial covers this many cues: at the measured 0.1 s a cue that is
// about ten seconds of film by the time the viewer has watched the opening
// titles, which is early enough that the wait is not what they remember.
inline constexpr size_t kFirstPartial = 8;
// After that, one every this many cues. Each one restarts the overlay's child,
// so they are batched rather than written per cue.
inline constexpr size_t kPartialEvery = 24;

using engine_child::Child;
using engine_child::Drain;
using engine_child::Handle;
using engine_child::HttpStatus;
using engine_child::InternetHandle;
using engine_child::OnExit;
using engine_child::Spawn;
using engine_child::WriteWholeFile;

// The versions written while translating sit beside the finished file and share
// its stem: "<key>.8.ass", "<key>.32.ass". Removing them is best effort - a
// version the overlay's child still has open cannot be deleted on Windows, and
// failing to is not worth reporting.
void RemovePartials(const fs::path& finalPath)
{
    std::error_code error;
    const std::string stem = finalPath.stem().string() + ".";
    for (fs::directory_iterator it(finalPath.parent_path(), error), end; !error && it != end;
         it.increment(error)) {
        const std::string name = it->path().filename().string();
        if (it->path().extension() != ".ass") continue;
        // The finished track is "<key>.ass", which also starts with "<key>." -
        // matching on the prefix alone deleted the very file the player was
        // about to open, and the swap failed with "No such file or directory".
        if (name == finalPath.filename().string()) continue;
        if (name.rfind(stem, 0) != 0) continue;  // a different source's track
        // The versions written on the way here are "<key>.<count>.ass". A file
        // that merely shares the prefix is not one of them: the one-language
        // track derived from the finished file is "<key>.zh.ass", and taking it
        // with the versions left the single-language view holding a path to
        // nothing - the child then failed with "No such file or directory".
        const std::string version =
            name.substr(stem.size(), name.size() - stem.size() - 4);  // ".ass" is four
        if (version.empty() || version.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::error_code ignored;
        fs::remove(it->path(), ignored);
    }
}

} // namespace

Translator::Translator(fs::path helperDirectory, Engine engine, fs::path cacheDirectory)
    : m_helperDirectory(std::move(helperDirectory)),
      m_engine(std::move(engine)),
      m_cacheDirectory(std::move(cacheDirectory)),
      m_chatTemplate(ChatTemplateFor(m_engine.model.filename().string()))
{
    m_worker = std::thread([this] { Run(); });
}

Translator::~Translator()
{
    HANDLE job = nullptr;
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
        ++m_generation;  // makes the running job's next checkpoint a cancel
        job = m_jobObject;
    }
    if (job) TerminateJobObject(job, 1);
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
    // The worker closes its own children on the way out; this covers a run that
    // never started.
    StopServer();
}

bool Translator::Available(const Engine& engine)
{
    std::error_code error;
    return fs::is_regular_file(engine.runtimeDirectory / L"llama-server.exe", error) &&
           fs::is_regular_file(engine.model, error);
}

void Translator::SetState(State state, std::wstring notice)
{
    std::lock_guard lock(m_mutex);
    m_status.state = state;
    m_status.notice = std::move(notice);
}

Status Translator::Poll()
{
    Status status;
    bool reap = false;
    {
        std::lock_guard lock(m_mutex);
        status = m_status;
        reap = m_serverIdle && m_server != nullptr &&
               std::chrono::steady_clock::now() - m_serverIdleSince > std::chrono::seconds(120);
    }
    if (reap) {
        LOG("Translate: the engine has been idle; stopping it.");
        StopServer();
        std::lock_guard lock(m_mutex);
        m_serverIdle = false;
        if (m_jobObject) {
            CloseHandle(m_jobObject);
            m_jobObject = nullptr;
        }
    }
    return status;
}

bool Translator::Cancelled(uint64_t generation) const
{
    std::lock_guard lock(m_mutex);
    return m_stop || generation != m_generation;
}

void Translator::Start(const Job& job)
{
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;  // a run in progress is now stale
        m_requested = true;
        m_job = job;
        m_status = {};
        m_status.state = State::Extracting;
    }
    // Deliberately no TerminateJobObject here. It used to be what ended a run in
    // flight, and it worked while a job existed for exactly one run. Now the job
    // lives as long as the translator, so terminating it kills the engine and
    // every child in it - including the ffmpeg that the run this call is asking
    // for is about to start, which then cannot even be put in the job
    // (AssignProcessToJobObject, winerr 5, reported as "the subtitle stream could
    // not be read"). A run in flight stops at its own next checkpoint instead:
    // this bumps the generation and every checkpoint checks it. Cancel still
    // terminates, because that is a viewer walking away, not the next batch.
    m_wake.notify_all();
}

void Translator::Cancel()
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

void Translator::Run()
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

void Translator::RunOnce(const Job& job, uint64_t generation)
{
    // The job object outlives a single run, so the engine it holds survives from
    // one batch to the next: it costs about five seconds to start, and those five
    // seconds come out of the speech engine's cores.
    {
        std::lock_guard lock(m_mutex);
        if (!m_jobObject) {
            Handle created(CreateKillOnCloseJob());
            if (!created) {
                SetState(State::Failed, L"the translation could not be started");
                return;
            }
            m_jobObject = created.Release();
        }
    }
    // Runs on every return path below. A finished or failed run leaves the engine
    // up for the next batch; an abandoned one takes it down, because nothing is
    // waiting for it any more.
    auto endOfRun = OnExit([this] {
        bool keep = false;
        {
            std::lock_guard lock(m_mutex);
            keep = m_status.state == State::Done || m_status.state == State::Failed;
            if (keep) {
                m_serverIdle = true;
                m_serverIdleSince = std::chrono::steady_clock::now();
            }
        }
        if (keep) return;
        StopServer();
        std::lock_guard lock(m_mutex);
        if (m_jobObject) {
            CloseHandle(m_jobObject);
            m_jobObject = nullptr;
        }
    });

    SetState(State::Extracting);
    auto cues = ReadCues(job, generation);
    if (!cues) {
        if (Cancelled(generation)) SetState(State::Cancelled);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_status.total = int(cues->size());
        m_status.done = int(std::count_if(cues->begin(), cues->end(),
                                         [](const Cue& cue) { return cue.Translated(); }));
    }

    // Where the track goes, known before the loop: a partial is the same file
    // with a version suffix.
    std::error_code error;
    fs::create_directories(m_cacheDirectory, error);
    const fs::path path =
        m_cacheDirectory / (CacheKey(utf8_text::FromWide(job.identity), std::to_string(job.stream),
                                     m_engine.model.filename().string(), job.languages) +
                            ".ass");
    // Versions an interrupted run left behind are not this run's.
    RemovePartials(path);

    // What an interrupted run finished, in the same folder under a name that is
    // not an ASS so the version cleanup never touches it.
    const fs::path progressPath =
        m_cacheDirectory / (path.stem().string() + ".part.srt");
    {
        std::ifstream in(progressPath, std::ios::binary);
        if (in) {
            std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const size_t restored = ApplySavedTranslations(*cues, ParseSrt(saved));
            if (restored > 0) {
                LOG("Translate: resumed " << restored << " cue(s) from " << progressPath.string());
                std::lock_guard lock(m_mutex);
                m_status.done = int(std::count_if(cues->begin(), cues->end(),
                                                  [](const Cue& cue) { return cue.Translated(); }));
            }
        }
    }

    SetState(State::Starting);
    if (!StartServer(generation)) {
        if (Cancelled(generation)) SetState(State::Cancelled);
        return;
    }

    SetState(State::Translating);
    const auto started = std::chrono::steady_clock::now();
    size_t translated = 0;
    for (Cue& cue : *cues) {
        if (cue.Translated()) continue;
        if (Cancelled(generation)) {
            SetState(State::Cancelled);
            return;
        }
        const auto answer = TranslateOne(cue.text, job.languages);
        if (!answer) {
            if (Cancelled(generation)) {
                SetState(State::Cancelled);
            } else {
                SetState(State::Failed, L"the translation engine stopped answering");
            }
            return;
        }
        cue.translation = *answer;
        ++translated;
        {
            std::lock_guard lock(m_mutex);
            ++m_status.done;
        }
        // Publish while the engine works: the viewer is reading Chinese long
        // before the last cue is answered, and each version is a whole track so
        // the overlay can swap it in without waiting.
        if (translated == kFirstPartial || translated % kPartialEvery == 0) {
            const std::string partial =
                BuildAss(*cues, Mode::Dual, LayoutFor(job.height), job.width, job.height);
            if (!partial.empty()) {
                const fs::path partialPath = path.parent_path() /
                    (path.stem().string() + "." + std::to_string(translated) + ".ass");
                if (WriteWholeFile(partialPath, partial)) {
                    std::lock_guard lock(m_mutex);
                    m_status.partial = partialPath;
                }
                // The same moment is the cheapest one to also save what would be
                // lost if this run ended here.
                WriteWholeFile(progressPath, FormatSrt(*cues, true));
            }
            if (Cancelled(generation)) {
                SetState(State::Cancelled);
                return;
            }
        }
    }

    const std::string ass = BuildAss(*cues, Mode::Dual, LayoutFor(job.height), job.width, job.height);
    if (ass.empty()) {
        SetState(State::Failed, L"there was nothing to draw");
        return;
    }
    if (!WriteWholeFile(path, ass)) {
        SetState(State::Failed, L"the translated track could not be written");
        return;
    }
    // The finished file supersedes every version written on the way here, and
    // nothing is left to resume.
    RemovePartials(path);
    std::error_code ignored;
    fs::remove(progressPath, ignored);
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    double coverage = 0.0;
    for (const Cue& cue : *cues) coverage = std::max(coverage, cue.end);
    LOG("Translate: " << translated << " cue(s) in " << seconds << " s -> " << path.string()
                      << " (covers up to " << coverage << " s)");
    {
        std::lock_guard lock(m_mutex);
        m_status.state = State::Done;
        m_status.result = path;
        m_status.coverage = coverage;
    }
}

std::optional<std::vector<Cue>> Translator::ReadCues(const Job& job, uint64_t generation)
{
    const fs::path ffmpeg =
        media_tools::FindTool(m_helperDirectory, L"ffmpeg.exe", media_tools::Fallback::SearchPath);
    if (ffmpeg.empty()) {
        SetState(State::Failed, L"ffmpeg.exe was not found");
        return std::nullopt;
    }
    Child child;
    if (!Spawn(ffmpeg, SrtArguments(job.path, job.stream, job.charenc), CurrentJob(), true, child,
               "ffmpeg")) {
        SetState(State::Failed, L"the subtitle stream could not be read");
        return std::nullopt;
    }
    const std::string srt = Drain(child.output.Get(), 8u << 20,
                                 [this, generation] { return Cancelled(generation); }, 120000);
    WaitForSingleObject(child.process.Get(), 30000);
    DWORD code = 1;
    GetExitCodeProcess(child.process.Get(), &code);
    if (Cancelled(generation) || code != 0 || srt.empty()) {
        LOG("Translate: no cues (ffmpeg exit " << code << ", " << srt.size() << " bytes)");
        if (!Cancelled(generation)) SetState(State::Failed, L"the subtitle stream has no text");
        return std::nullopt;
    }
    std::vector<Cue> cues = ParseSrt(srt);
    if (cues.empty()) {
        SetState(State::Failed, L"the subtitle stream has no text to translate");
        return std::nullopt;
    }
    LOG("Translate: " << cues.size() << " cue(s) read from the subtitle stream");
    return cues;
}

HANDLE Translator::CurrentJob() const
{
    std::lock_guard lock(m_mutex);
    return m_jobObject;
}

bool Translator::StartServer(uint64_t generation)
{
    {
        // Still running from the last batch, and still answering: nothing to do.
        std::lock_guard lock(m_mutex);
        if (m_server && m_httpConnection && WaitForSingleObject(m_server, 0) == WAIT_TIMEOUT) {
            m_serverIdle = false;
            return true;
        }
    }
    // A server that died between batches is not reusable, and its handles are
    // stale: drop them before another is started.
    StopServer();
    const fs::path server = m_engine.runtimeDirectory / L"llama-server.exe";
    Child child;
    // No capture: the engine logs for the whole run, and nothing reads it.
    if (!Spawn(server,
               ServerArguments(m_engine.model.wstring(), m_engine.port, m_engine.gpuLayers,
                               m_engine.threads, m_chatTemplate),
               CurrentJob(), false, child, "llama-server")) {
        SetState(State::Failed, L"the translation engine could not be started");
        return false;
    }
    {
        std::lock_guard lock(m_mutex);
        m_server = child.process.Release();  // owned by m_server from here
    }

    if (!m_httpSession) {
        InternetHandle session(WinHttpOpen(L"DLSSVideoPlayer/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                           WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) {
            LOG("Translate: WinHttpOpen failed winerr=" << GetLastError());
            SetState(State::Failed, L"the translation engine could not be reached");
            return false;
        }
        // One timeout per stage; the health loop below is what bounds start-up.
        WinHttpSetTimeouts(session.Get(), 2000, 2000, 30000, 120000);
        m_httpSession = session.Release();
    }
    if (!m_httpConnection) {
        InternetHandle connection(WinHttpConnect(m_httpSession, L"127.0.0.1",
                                                 INTERNET_PORT(m_engine.port), 0));
        if (!connection) {
            LOG("Translate: WinHttpConnect(" << m_engine.port << ") failed winerr=" << GetLastError());
            SetState(State::Failed, L"the translation engine could not be reached");
            return false;
        }
        m_httpConnection = connection.Release();
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(int64_t(m_engine.startTimeoutSeconds * 1000.0));
    const auto began = std::chrono::steady_clock::now();
    for (;;) {
        if (Cancelled(generation)) return false;
        // A model still loading answers 503; that is waiting, not failing.
        if (const auto answer = Post("/health", {}, {}, 4000)) {
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            LOG("Translate: llama-server ready in " << seconds << " s");
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            LOG("Translate: llama-server never answered /health");
            SetState(State::Failed, L"the translation engine did not start");
            return false;
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_server && WaitForSingleObject(m_server, 0) == WAIT_OBJECT_0) {
                DWORD exitCode = 1;
                GetExitCodeProcess(m_server, &exitCode);
                LOG("Translate: llama-server exited with " << exitCode);
                SetState(State::Failed, L"the translation engine failed to start");
                return false;
            }
        }
        Sleep(200);
    }
}

void Translator::StopServer()
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
    InternetHandle connection(m_httpConnection);
    m_httpConnection = nullptr;
    InternetHandle session(m_httpSession);
    m_httpSession = nullptr;
}

std::optional<std::string> Translator::Post(const std::string& path, const std::string& body,
                                            const std::string& contentType, int timeoutMillis)
{
    const auto answer = engine_child::HttpRequest(m_httpConnection, body.empty() ? L"GET" : L"POST",
                                                  path, body, contentType, timeoutMillis);
    if (!answer || answer->status != 200) return std::nullopt;
    return answer->body;
}

std::optional<std::string> Translator::TranslateOne(const std::string& text, const Languages& languages)
{
    const std::string body = ChatRequestBody(OfficialPrompt(text, languages));
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto answer = Post("/v1/chat/completions", body, "application/json", 120000);
        if (!answer) continue;
        const auto content = JsonStringField(*answer, "content");
        if (!content) continue;
        std::string cleaned = CleanTranslation(*content);
        if (!cleaned.empty()) return cleaned;
    }
    return std::nullopt;
}

} // namespace subtitle_translate
