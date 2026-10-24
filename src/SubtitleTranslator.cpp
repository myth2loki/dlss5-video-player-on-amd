#include "SubtitleTranslator.h"

#include "InheritedHandles.h"
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

// A Win32 handle that closes itself, so every early return in this file is also
// a correct one. Deliberately not shared_ptr: nothing here outlives its scope.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : m_value(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : m_value(other.Release()) {}
    Handle& operator=(Handle&& other) noexcept
    {
        if (this != &other) {
            Reset();
            m_value = other.Release();
        }
        return *this;
    }

    HANDLE Get() const { return m_value; }
    explicit operator bool() const { return m_value != nullptr && m_value != INVALID_HANDLE_VALUE; }
    HANDLE Release()
    {
        HANDLE value = m_value;
        m_value = nullptr;
        return value;
    }
    void Reset(HANDLE value = nullptr)
    {
        if (m_value && m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value);
        m_value = value;
    }

private:
    HANDLE m_value = nullptr;
};

class InternetHandle {
public:
    InternetHandle() = default;
    explicit InternetHandle(HINTERNET value) : m_value(value) {}
    ~InternetHandle() { Reset(); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    InternetHandle(InternetHandle&& other) noexcept : m_value(other.Release()) {}
    InternetHandle& operator=(InternetHandle&& other) noexcept
    {
        if (this != &other) {
            Reset();
            m_value = other.Release();
        }
        return *this;
    }

    HINTERNET Get() const { return m_value; }
    explicit operator bool() const { return m_value != nullptr; }
    HINTERNET Release()
    {
        HINTERNET value = m_value;
        m_value = nullptr;
        return value;
    }
    void Reset(HINTERNET value = nullptr)
    {
        if (m_value) WinHttpCloseHandle(m_value);
        m_value = value;
    }

private:
    HINTERNET m_value = nullptr;
};

// A child in a kill-on-close job. With `capture`, its stdout and stderr are one
// pipe the player drains; without, they go to NUL - an engine that logs
// progress for minutes would otherwise fill a pipe nobody reads and block.
struct Child {
    Handle process;
    Handle output;
};

bool Spawn(const fs::path& executable, const std::wstring& arguments, HANDLE job, bool capture,
           Child& child, const char* what)
{
    SECURITY_ATTRIBUTES inheritPipe{};
    inheritPipe.nLength = sizeof(inheritPipe);
    inheritPipe.bInheritHandle = TRUE;

    Handle pipeRead, pipeWrite;
    if (capture) {
        HANDLE readRaw = nullptr, writeRaw = nullptr;
        if (!CreatePipe(&readRaw, &writeRaw, &inheritPipe, 0)) {
            LOG("Translate: CreatePipe failed winerr=" << GetLastError());
            return false;
        }
        pipeRead.Reset(readRaw);
        pipeWrite.Reset(writeRaw);
        // Only the writing end is the child's; the player's read end must not be
        // inherited, or the pipe never reports end-of-file.
        SetHandleInformation(pipeRead.Get(), HANDLE_FLAG_INHERIT, 0);
    }

    Handle nulIn(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritPipe,
                             OPEN_EXISTING, 0, nullptr));
    Handle nulOut;
    if (!nulIn) {
        LOG("Translate: opening NUL failed winerr=" << GetLastError());
        return false;
    }
    if (!capture) {
        nulOut.Reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritPipe,
                                 OPEN_EXISTING, 0, nullptr));
        if (!nulOut) {
            LOG("Translate: opening NUL for writing failed winerr=" << GetLastError());
            return false;
        }
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags = STARTF_USESTDHANDLES;
    startupInfo.hStdInput = nulIn.Get();
    startupInfo.hStdOutput = capture ? pipeWrite.Get() : nulOut.Get();
    startupInfo.hStdError = startupInfo.hStdOutput;

    // The handle list, not bInheritHandles alone: this is what stops a child of
    // the overlay from inheriting this pipe and holding its end open.
    InheritedHandles inherited{startupInfo.hStdInput, startupInfo.hStdOutput, startupInfo.hStdError};
    if (!inherited.Ready()) {
        LOG("Translate: the inherited-handle list could not be built");
        return false;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo = startupInfo;
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = inherited.AttributeList();

    std::wstring command = L"\"" + executable.wstring() + L"\" " + arguments;
    PROCESS_INFORMATION info{};
    const BOOL started =
        CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, inherited.InheritHandles(),
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | inherited.CreationFlags(), nullptr, nullptr,
                       &startup.StartupInfo, &info);
    const DWORD startError = GetLastError();
    // The child owns its copies now; the player's writing end must close or the
    // read never ends.
    pipeWrite.Reset();
    nulIn.Reset();
    nulOut.Reset();
    if (!started) {
        LOG("Translate: CreateProcess(" << what << ") failed winerr=" << startError);
        return false;
    }
    Handle thread(info.hThread);
    // Assigned before it runs, so there is no window in which it is unowned.
    if (!job || !AssignProcessToJobObject(job, info.hProcess) ||
        ResumeThread(thread.Get()) == DWORD(-1)) {
        LOG("Translate: " << what << " could not be put in its job winerr=" << GetLastError());
        TerminateProcess(info.hProcess, 1);
        CloseHandle(info.hProcess);
        return false;
    }
    child.process.Reset(info.hProcess);
    child.output = std::move(pipeRead);
    return true;
}

// Everything the child wrote, up to `cap`, or as much as arrived before `cancel`
// said stop or the deadline passed. Reading in this loop rather than with one
// blocking ReadFile is what makes a cancel take effect while a child is quiet.
std::string Drain(HANDLE pipe, size_t cap, const std::function<bool()>& cancel, DWORD timeoutMillis)
{
    std::string text;
    const ULONGLONG deadline = GetTickCount64() + timeoutMillis;
    char buffer[8192];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;  // closed
        if (available) {
            DWORD got = 0;
            if (!ReadFile(pipe, buffer, std::min<DWORD>(available, DWORD(sizeof(buffer))), &got, nullptr) ||
                !got)
                break;
            if (text.size() + got > cap) {
                text.append(buffer, cap - text.size());
                break;
            }
            text.append(buffer, got);
            continue;
        }
        if ((cancel && cancel()) || GetTickCount64() >= deadline) break;
        Sleep(2);
    }
    return text;
}

// Runs a lambda when it goes out of scope. Every early return in RunOnce has to
// stop the server and release the job, and a guard is the only form of that
// which cannot be forgotten on one path.
template <class Function>
class ScopeExit {
public:
    explicit ScopeExit(Function function) : m_function(std::move(function)) {}
    ~ScopeExit() { m_function(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    Function m_function;
};

template <class Function>
ScopeExit<Function> OnExit(Function function)
{
    return ScopeExit<Function>(std::move(function));
}

bool WriteWholeFile(const fs::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), std::streamsize(text.size()));
    // Close before reporting: a partial that another process is about to open
    // must be complete on disk, not merely written.
    out.close();
    return out.good();
}

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
        std::error_code ignored;
        fs::remove(it->path(), ignored);
    }
}

int HttpStatus(HINTERNET request)
{
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return int(status);
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

Status Translator::Poll() const
{
    std::lock_guard lock(m_mutex);
    return m_status;
}

bool Translator::Cancelled(uint64_t generation) const
{
    std::lock_guard lock(m_mutex);
    return m_stop || generation != m_generation;
}

void Translator::Start(const Job& job)
{
    HANDLE jobObject = nullptr;
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;  // a run in progress is now stale
        m_requested = true;
        m_job = job;
        m_status = {};
        m_status.state = State::Extracting;
        jobObject = m_jobObject;
    }
    // A running child is ended so its blocked read returns; the worker then
    // picks up the new job.
    if (jobObject) TerminateJobObject(jobObject, 1);
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
    // One job object per run: closing it is what guarantees nothing survives,
    // even a child that was still spawning when the run ended.
    Handle runJob(CreateKillOnCloseJob());
    if (!runJob) {
        SetState(State::Failed, L"the translation could not be started");
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        m_jobObject = runJob.Get();
    }
    // Runs on every return path below; the handle above outlives it, so the job
    // is closed after its children have been stopped.
    auto endOfRun = OnExit([this, job = runJob.Get()] {
        StopServer();
        std::lock_guard lock(m_mutex);
        if (m_jobObject == job) m_jobObject = nullptr;
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
    LOG("Translate: " << translated << " cue(s) in " << seconds << " s -> " << path.string());
    {
        std::lock_guard lock(m_mutex);
        m_status.state = State::Done;
        m_status.result = path;
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
    if (!m_httpConnection) return std::nullopt;
    const std::wstring widePath = utf8_text::ToWide(path);
    InternetHandle request(WinHttpOpenRequest(m_httpConnection, body.empty() ? L"GET" : L"POST",
                                              widePath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request) return std::nullopt;
    WinHttpSetTimeouts(request.Get(), 2000, 2000, 30000, timeoutMillis);
    std::wstring headers;
    if (!contentType.empty()) headers = L"Content-Type: " + utf8_text::ToWide(contentType) + L"\r\n";
    const DWORD length = DWORD(body.size());
    // WinHttpSendRequest takes a mutable buffer it only reads; const_cast keeps
    // the caller's string const rather than copying a prompt for every cue.
    void* payload = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    if (!WinHttpSendRequest(request.Get(),
                            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : DWORD(headers.size()), payload, length, length, 0))
        return std::nullopt;
    if (!WinHttpReceiveResponse(request.Get(), nullptr)) return std::nullopt;
    const int status = HttpStatus(request.Get());
    if (status != 200) return std::nullopt;
    std::string text;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &available) || available == 0) break;
        const size_t offset = text.size();
        const size_t chunk = std::min<size_t>(available, 64u << 10);
        text.resize(offset + chunk);
        DWORD read = 0;
        const BOOL ok = WinHttpReadData(request.Get(), text.data() + offset, DWORD(chunk), &read);
        text.resize(offset + read);
        // A reply is a few hundred bytes; anything past this is not a reply.
        if (!ok || read == 0 || text.size() > (4u << 20)) break;
    }
    return text;
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
