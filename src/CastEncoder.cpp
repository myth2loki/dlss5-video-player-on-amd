#include "CastEncoder.h"
#include "CastDevicePolicy.h"

#include "HardErrorSuppression.h"
#include "InheritedHandles.h"
#include "KillOnCloseJob.h"
#include "Log.h"
#include "MediaTools.h"
#include "Utf8Text.h"
#include "WindowsArgument.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cast_encoder {

namespace {

std::wstring Q(const std::wstring& s) { return QuoteCommandArgument(s); }

// A Win32 handle that closes itself on every path.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE v) : m_v(v) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : m_v(o.Release()) {}
    Handle& operator=(Handle&& o) noexcept
    {
        if (this != &o) { Reset(); m_v = o.Release(); }
        return *this;
    }
    HANDLE Get() const { return m_v; }
    explicit operator bool() const { return m_v && m_v != INVALID_HANDLE_VALUE; }
    HANDLE Release() { HANDLE v = m_v; m_v = nullptr; return v; }
    void Reset(HANDLE v = nullptr)
    {
        if (m_v && m_v != INVALID_HANDLE_VALUE) CloseHandle(m_v);
        m_v = v;
    }
private:
    HANDLE m_v{nullptr};
};

constexpr std::size_t kMaxQueuedFrames = 4;      // 4-slot backpressure, like the capture ring
constexpr DWORD kExitWaitMs = 1500;

} // namespace

struct Encoder::Impl {
    Handle job;
    Handle process;
    Handle stdinWrite;   // parent's write end; closing it lets ffmpeg finalize HLS
    Handle stderrRead;   // drained by the feed thread, tail kept for errors
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<Frame> queue;
    std::mutex errorMutex;
    std::string errorTail;
    ~Impl() { Stop(); }

    void Stop()
    {
        // Declared here but implemented through Encoder::Stop to keep ownership
        // of the feed thread (which touches the queue) in one place. Left empty
        // deliberately; Encoder::Stop does the work before releasing Impl.
    }
};

Encoder::Encoder() : m_impl(std::make_unique<Impl>()) {}

Encoder::~Encoder()
{
    Stop();
}

bool Encoder::AmfAvailable(const std::wstring& ffmpeg) const
{
    // A dry-run that asks ffmpeg to list the encoder. -hide_banner keeps output
    // small; the check is name presence, not a GPU capability probe (the real
    // capability is proven at Start and falls back if the child fails).
    std::wstring command = Q(ffmpeg) + L" -hide_banner -encoders";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE readRaw = nullptr, writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &sa, 0)) return false;
    SetHandleInformation(readRaw, HANDLE_FLAG_INHERIT, 0);
    Handle readHandle(readRaw), writeHandle(writeRaw);

    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr; si.hStdOutput = writeHandle.Get(); si.hStdError = writeHandle.Get();
    const InheritedHandles inherit{si.hStdInput, si.hStdOutput, si.hStdError};
    STARTUPINFOEXW startup{}; startup.StartupInfo = si; startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = inherit.AttributeList();
    PROCESS_INFORMATION pi{};
    const ScopedHardErrorSuppression noDialog;
    const BOOL ok = inherit.Ready() &&
        CreateProcessW(ffmpeg.c_str(), mutableCommand.data(), nullptr, nullptr, inherit.InheritHandles(),
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | inherit.CreationFlags(),
                       nullptr, nullptr, &startup.StartupInfo, &pi);
    writeHandle.Reset();
    if (!ok) return false;
    Handle thread(pi.hThread), process(pi.hProcess);
    ResumeThread(thread.Get());
    std::string output;
    char buffer[4096];
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(readHandle.Get(), nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) {
            if (WaitForSingleObject(process.Get(), 0) == WAIT_OBJECT_0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(readHandle.Get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || !got) break;
        output.append(buffer, got);
        if (output.size() > (1u << 20)) break;
    }
    return output.find("h264_amf") != std::string::npos;
}

bool Encoder::SpawnFfmpeg(const Settings& settings, bool useAmf)
{
    std::wstring ffmpeg = settings.ffmpegPath;
    if (ffmpeg.empty()) {
        ffmpeg = media_tools::FindTool({}, L"ffmpeg.exe", media_tools::Fallback::SearchPath).wstring();
    }
    if (ffmpeg.empty()) { LOG("Cast encoder: ffmpeg.exe not found"); return false; }

    // ffmpeg reads raw BGRA frames from stdin, muxes the current audio track
    // (seeked to the player position) and writes an HLS live playlist.
    std::wostringstream args;
    args << L"-hide_banner -loglevel error -nostdin -y ";
    args << L"-f rawvideo -pixel_format bgra -video_size " << settings.width << L"x" << settings.height
         << L" -framerate " << settings.fps << L" -i pipe:0 ";
    if (!settings.audioSource.empty()) {
        // Independent decode branch (doc 14.4): seek to the player position and
        // re-encode to AAC. This bypasses the player's passthrough path entirely.
        if (settings.audioSeekSeconds > 0.0)
            args << L"-ss " << std::fixed << std::setprecision(6) << settings.audioSeekSeconds << L" ";
        args << L"-i " << Q(utf8_text::ToWide(settings.audioSource)) << L" -map 1:a:0? ";
    }
    args << L"-map 0:v:0 ";
    if (useAmf)
        args << L"-c:v h264_amf -quality speed -b:v " << settings.bitrateKbps << L"k ";
    else
        args << L"-c:v libx264 -preset ultrafast -tune zerolatency -b:v " << settings.bitrateKbps << L"k ";
    args << L"-pix_fmt yuv420p -g " << settings.fps << L" ";
    if (!settings.audioSource.empty()) args << L"-c:a aac -b:a 128k ";
    args << L"-f hls -hls_time 2 -hls_list_size 12 -hls_flags delete_segments "
         << L"-hls_segment_type mpegts -hls_segment_filename "
         << Q((settings.hlsDirectory / L"seg-%06d.ts").wstring()) << L" "
         << Q((settings.hlsDirectory / L"live.m3u8").wstring());

    std::wstring command = Q(ffmpeg) + L" " + args.str();
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');

    // stdin pipe for raw frames + stderr pipe for errors. Read ends are
    // non-inheritable; only the child's write ends cross the process boundary.
    SECURITY_ATTRIBUTES sa{}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE stdinReadRaw = nullptr, stdinWriteRaw = nullptr;
    if (!CreatePipe(&stdinReadRaw, &stdinWriteRaw, &sa, 4u << 20)) return false;
    SetHandleInformation(stdinWriteRaw, HANDLE_FLAG_INHERIT, 0);
    Handle stdinRead(stdinReadRaw), stdinWrite(stdinWriteRaw);

    HANDLE errReadRaw = nullptr, errWriteRaw = nullptr;
    if (!CreatePipe(&errReadRaw, &errWriteRaw, &sa, 0)) return false;
    SetHandleInformation(errReadRaw, HANDLE_FLAG_INHERIT, 0);
    Handle errRead(errReadRaw), errWrite(errWriteRaw);

    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead.Get();
    si.hStdOutput = errWrite.Get();
    si.hStdError = errWrite.Get();
    const InheritedHandles inherit{si.hStdInput, si.hStdOutput, si.hStdError};
    if (!inherit.Ready()) return false;
    STARTUPINFOEXW startup{}; startup.StartupInfo = si; startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = inherit.AttributeList();

    Handle job(CreateKillOnCloseJob());
    PROCESS_INFORMATION pi{};
    const ScopedHardErrorSuppression noDialog;
    const BOOL ok = job &&
        CreateProcessW(ffmpeg.c_str(), mutableCommand.data(), nullptr, nullptr, inherit.InheritHandles(),
                       CREATE_NO_WINDOW | CREATE_SUSPENDED | inherit.CreationFlags(),
                       nullptr, nullptr, &startup.StartupInfo, &pi);
    const DWORD startError = GetLastError();
    // Child owns its copies now.
    stdinRead.Reset(); errWrite.Reset();
    if (!ok) { LOG("Cast encoder: CreateProcess failed winerr=" << startError); return false; }
    Handle thread(pi.hThread);
    if (!AssignProcessToJobObject(job.Get(), pi.hProcess) || ResumeThread(thread.Get()) == DWORD(-1)) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        LOG("Cast encoder: could not put ffmpeg in job winerr=" << GetLastError());
        return false;
    }
    // Transfer ownership to Impl.
    m_impl->job = std::move(job);
    m_impl->process.Reset(pi.hProcess);
    m_impl->stdinWrite = std::move(stdinWrite);
    m_impl->stderrRead = std::move(errRead);
    return true;
}

StartResult Encoder::Start(Settings settings)
{
    StartResult result;
    if (m_running.load(std::memory_order_acquire)) { result.error = "already running"; return result; }

    std::error_code ec;
    std::filesystem::create_directories(settings.hlsDirectory, ec);
    if (ec) { result.error = "create HLS dir failed: " + ec.message(); return result; }
    m_settings = std::move(settings);

    bool useAmf = false;
    if (m_settings.preferAmf) {
        std::wstring ffmpeg = m_settings.ffmpegPath.empty()
            ? media_tools::FindTool({}, L"ffmpeg.exe", media_tools::Fallback::SearchPath).wstring()
            : m_settings.ffmpegPath;
        useAmf = !ffmpeg.empty() && AmfAvailable(ffmpeg);
    }
    if (!SpawnFfmpeg(m_settings, useAmf)) {
        // Fallback: if AMF was requested but the child failed to start, retry
        // once with libx264 before giving up (doc 14.3).
        if (useAmf && SpawnFfmpeg(m_settings, false)) useAmf = false;
        else { result.error = "ffmpeg failed to start"; return result; }
    }
    result.started = true;
    result.encoderUsed = useAmf ? "h264_amf" : "libx264";
    m_encoderUsed = result.encoderUsed;
    m_running.store(true, std::memory_order_release);
    m_feedThread = std::thread([this] { FeedLoop(); });
    LOG("Cast encoder started: " << m_encoderUsed << " " << m_settings.width << "x" << m_settings.height
        << "@" << m_settings.fps << " -> " << m_settings.hlsDirectory.string());
    return result;
}

bool Encoder::PushFrame(Frame frame)
{
    if (!m_running.load(std::memory_order_acquire)) return false;
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        if (m_impl->queue.size() >= kMaxQueuedFrames) return false; // drop on backpressure
        m_impl->queue.push_back(std::move(frame));
    }
    m_impl->queueCv.notify_one();
    return true;
}

void Encoder::FeedLoop()
{
    std::vector<uint8_t> scratch;
    while (m_running.load(std::memory_order_acquire)) {
        Frame frame;
        {
            std::unique_lock<std::mutex> lock(m_impl->queueMutex);
            m_impl->queueCv.wait(lock, [this] {
                return !m_impl->queue.empty() || !m_running.load(std::memory_order_acquire);
            });
            if (m_impl->queue.empty() && !m_running.load(std::memory_order_acquire)) break;
            frame = std::move(m_impl->queue.front());
            m_impl->queue.pop_front();
        }
        if (frame.bgra.empty()) continue;
        const std::size_t expected = static_cast<std::size_t>(frame.width) * frame.height * 4;
        if (frame.bgra.size() < expected) continue; // never read past the buffer
        DWORD written = 0;
        if (!WriteFile(m_impl->stdinWrite.Get(), frame.bgra.data(), static_cast<DWORD>(expected), &written, nullptr) ||
            written != expected) {
            // ffmpeg closed stdin (exited); drain its stderr and stop feeding.
            break;
        }
    }
    // Drain the stderr tail so the error is reportable.
    char buffer[4096];
    std::string tail;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(m_impl->stderrRead.Get(), nullptr, 0, nullptr, &available, nullptr)) break;
        if (!available) break;
        DWORD got = 0;
        if (!ReadFile(m_impl->stderrRead.Get(), buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || !got) break;
        tail.append(buffer, got);
        if (tail.size() > 8192) tail.erase(0, tail.size() - 8192);
    }
    if (!tail.empty()) {
        std::lock_guard<std::mutex> lock(m_impl->errorMutex);
        m_impl->errorTail = std::move(tail);
    }
}

void Encoder::Stop()
{
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;
    m_impl->queueCv.notify_all();
    if (m_feedThread.joinable()) m_feedThread.join();
    // Close stdin so ffmpeg finalizes the last HLS segment on its own terms.
    m_impl->stdinWrite.Reset();
    if (m_impl->process) {
        if (WaitForSingleObject(m_impl->process.Get(), kExitWaitMs) != WAIT_OBJECT_0) {
            TerminateJobObject(m_impl->job.Get(), 1);
            WaitForSingleObject(m_impl->process.Get(), kExitWaitMs);
        }
    }
    m_impl->process.Reset();
    m_impl->stderrRead.Reset();
    m_impl->job.Reset();
    {
        std::lock_guard<std::mutex> lock(m_impl->queueMutex);
        m_impl->queue.clear();
    }
    LOG("Cast encoder stopped (" << m_encoderUsed << ")");
}

std::string Encoder::LastError() const
{
    std::lock_guard<std::mutex> lock(m_impl->errorMutex);
    return m_impl->errorTail;
}

} // namespace cast_encoder
