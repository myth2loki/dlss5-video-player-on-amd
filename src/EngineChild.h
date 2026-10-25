#pragma once

#include "InheritedHandles.h"
#include "KillOnCloseJob.h"
#include "Log.h"
#include "Utf8Text.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// The machinery both subtitle engines need: a child process in a kill-on-close
// job, a pipe its output can be drained from without blocking a cancel, and the
// two Win32 handle wrappers that make every early return in a run also a correct
// one.
//
// Shared rather than copied: the translator (llama-server) and the recogniser
// (whisper-server) differ in their protocol, and in nothing else. Two copies of
// this would drift - one would learn to close a handle the other kept.
namespace engine_child {

// A Win32 handle that closes itself.
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

// The same, for WinHTTP.
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
// pipe the player drains; without, they go to NUL - an engine that logs for
// minutes would otherwise fill a pipe nobody reads and block.
struct Child {
    Handle process;
    Handle output;
};

// Starts `executable` with `arguments`, suspended, in `job`, and only then lets
// it run: there is no window in which it is unowned.
inline bool Spawn(const std::filesystem::path& executable, const std::wstring& arguments, HANDLE job,
                  bool capture, Child& child, const char* what)
{
    SECURITY_ATTRIBUTES inheritPipe{};
    inheritPipe.nLength = sizeof(inheritPipe);
    inheritPipe.bInheritHandle = TRUE;

    Handle pipeRead, pipeWrite;
    if (capture) {
        HANDLE readRaw = nullptr, writeRaw = nullptr;
        if (!CreatePipe(&readRaw, &writeRaw, &inheritPipe, 0)) {
            LOG("Engine: CreatePipe failed winerr=" << GetLastError());
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
        LOG("Engine: opening NUL failed winerr=" << GetLastError());
        return false;
    }
    if (!capture) {
        nulOut.Reset(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &inheritPipe, OPEN_EXISTING, 0, nullptr));
        if (!nulOut) {
            LOG("Engine: opening NUL for writing failed winerr=" << GetLastError());
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
        LOG("Engine: the inherited-handle list could not be built");
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
        LOG("Engine: CreateProcess(" << what << ") failed winerr=" << startError);
        return false;
    }
    Handle thread(info.hThread);
    if (!job || !AssignProcessToJobObject(job, info.hProcess) ||
        ResumeThread(thread.Get()) == DWORD(-1)) {
        LOG("Engine: " << what << " could not be put in its job winerr=" << GetLastError());
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
inline std::string Drain(HANDLE pipe, size_t cap, const std::function<bool()>& cancel,
                         DWORD timeoutMillis)
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

// Writes a whole file, closed before it reports success: a file another process
// is about to open must be complete on disk, not merely written.
inline bool WriteWholeFile(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), std::streamsize(text.size()));
    out.close();
    return out.good();
}

// Runs a lambda when it goes out of scope.
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

inline int HttpStatus(HINTERNET request)
{
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        return 0;
    return int(status);
}

struct HttpResponse {
    int status = 0;   // 0 when the request never produced one
    std::string body;
};

// One request to a local engine, or nothing when it could not be made at all.
// A non-200 answer is returned rather than treated as a failure: llama-server
// answers 503 while it loads, and that is information the caller needs.
inline std::optional<HttpResponse> HttpRequest(HINTERNET connection, const std::wstring& method,
                                               const std::string& path, const std::string& body,
                                               const std::string& contentType, int receiveTimeoutMillis)
{
    if (!connection) return std::nullopt;
    const std::wstring widePath = utf8_text::ToWide(path);
    InternetHandle request(WinHttpOpenRequest(connection, method.c_str(), widePath.c_str(), nullptr,
                                              WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!request) return std::nullopt;
    WinHttpSetTimeouts(request.Get(), 2000, 2000, 30000, receiveTimeoutMillis);
    std::wstring headers;
    if (!contentType.empty()) headers = L"Content-Type: " + utf8_text::ToWide(contentType) + L"\r\n";
    const DWORD length = DWORD(body.size());
    // WinHttpSendRequest takes a mutable buffer it only reads; const_cast keeps
    // the caller's bytes const rather than copying a chunk of audio per request.
    void* payload = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data());
    if (!WinHttpSendRequest(request.Get(),
                            headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : DWORD(headers.size()), payload, length, length, 0))
        return std::nullopt;
    if (!WinHttpReceiveResponse(request.Get(), nullptr)) return std::nullopt;
    HttpResponse answer;
    answer.status = HttpStatus(request.Get());
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &available) || available == 0) break;
        const size_t offset = answer.body.size();
        const size_t chunk = std::min<size_t>(available, 64u << 10);
        answer.body.resize(offset + chunk);
        DWORD read = 0;
        const BOOL ok = WinHttpReadData(request.Get(), answer.body.data() + offset, DWORD(chunk), &read);
        answer.body.resize(offset + read);
        if (!ok || read == 0 || answer.body.size() > (16u << 20)) break;
    }
    return answer;
}

} // namespace engine_child
