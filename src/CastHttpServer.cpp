#include "CastHttpServer.h"
#include "Log.h"

// WIN32_LEAN_AND_MEAN / NOMINMAX come from the project's global compile
// definitions (CMakeLists.txt); redefining them here only warns C4005.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace cast_http_server {

namespace {

std::once_flag g_wsaOnce;
bool g_wsaOk = false;
void EnsureWsa()
{
    std::call_once(g_wsaOnce, [] {
        WSADATA data{};
        g_wsaOk = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
}

class Socket {
public:
    Socket() = default;
    explicit Socket(SOCKET s) : m_s(s) {}
    ~Socket() { Reset(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept : m_s(o.Release()) {}
    Socket& operator=(Socket&& o) noexcept
    {
        if (this != &o) { Reset(); m_s = o.Release(); }
        return *this;
    }
    SOCKET Get() const { return m_s; }
    explicit operator bool() const { return m_s != INVALID_SOCKET; }
    SOCKET Release() { SOCKET s = m_s; m_s = INVALID_SOCKET; return s; }
    void Reset(SOCKET s = INVALID_SOCKET)
    {
        if (m_s != INVALID_SOCKET) closesocket(m_s);
        m_s = s;
    }
private:
    SOCKET m_s{INVALID_SOCKET};
};

// Send all bytes, handling partial sends and EINTR-like WSAEINTR.
bool SendAll(SOCKET s, const char* data, std::size_t size)
{
    std::size_t sent = 0;
    while (sent < size) {
        const int n = send(s, data + sent, static_cast<int>(std::min<std::size_t>(size - sent, 1u << 20)), 0);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string HttpResponseHeader(int status, const std::string& statusText, uint64_t contentLength,
                               bool hasRange, ByteRange range, uint64_t fileSize,
                               std::string_view contentType = "application/octet-stream")
{
    char buffer[512];
    std::string out;
    const std::string type(contentType);
    const int n = std::snprintf(buffer, sizeof(buffer),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Accept-Ranges: bytes\r\n"
        "Content-Length: %llu\r\n",
        status, statusText.c_str(), type.c_str(), static_cast<unsigned long long>(contentLength));
    if (n > 0) out.append(buffer, static_cast<std::size_t>(n));
    if (hasRange) {
        const int r = std::snprintf(buffer, sizeof(buffer), "Content-Range: bytes %llu-%llu/%llu\r\n",
            static_cast<unsigned long long>(range.first), static_cast<unsigned long long>(range.last),
            static_cast<unsigned long long>(fileSize));
        if (r > 0) out.append(buffer, static_cast<std::size_t>(r));
    }
    out += "Connection: close\r\n\r\n";
    return out;
}

void ServeOne(SOCKET client, const std::filesystem::path& directory, const std::function<bool()>& stop)
{
    // Read only the request line + headers, bounded. No body is expected for GET.
    std::array<char, 8192> buf{};
    std::string request;
    request.reserve(buf.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (request.size() < buf.size() && std::chrono::steady_clock::now() < deadline) {
        const int got = recv(client, buf.data(), static_cast<int>(buf.size() - request.size()), 0);
        if (got <= 0) break;
        request.append(buf.data(), static_cast<std::size_t>(got));
        if (request.find("\r\n\r\n") != std::string::npos) break;
    }
    // Parse first line: "GET /live.m3u8 HTTP/1.1".
    const std::size_t lineEnd = request.find("\r\n");
    const std::string_view firstLine(request.data(), lineEnd == std::string::npos ? request.size() : lineEnd);
    if (firstLine.rfind("GET ", 0) != 0) {
        const std::string header = HttpResponseHeader(405, "Method Not Allowed", 0, false, {}, 0);
        SendAll(client, header.data(), header.size());
        return;
    }
    const std::size_t pathStart = 4;
    const std::size_t pathEnd = firstLine.find(' ', pathStart);
    if (pathEnd == std::string_view::npos) return;
    std::string_view target = firstLine.substr(pathStart, pathEnd - pathStart);
    // Strip query; a target with a query is still checked on its path part.
    if (const std::size_t q = target.find('?'); q != std::string_view::npos) target = target.substr(0, q);
    if (!target.empty() && target.front() == '/') target.remove_prefix(1);

    if (!IsAllowedName(target)) {
        const std::string header = HttpResponseHeader(404, "Not Found", 0, false, {}, 0);
        SendAll(client, header.data(), header.size());
        return;
    }

    // Range header (case-insensitive scan of the bounded request).
    std::string_view rangeHeader;
    std::size_t pos = lineEnd == std::string::npos ? request.size() : lineEnd + 2;
    while (pos < request.size()) {
        const std::size_t eol = request.find("\r\n", pos);
        if (eol == std::string::npos) break;
        const std::string_view line(request.data() + pos, eol - pos);
        pos = eol + 2;
        if (line.size() >= 6) {
            std::string lower(line.substr(0, 6));
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            });
            if (lower == "range:") {
                std::size_t colon = line.find(':');
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
                rangeHeader = value;
            }
        }
    }

    // A DMR issues its first GET the instant SetAVTransportURI returns — before
    // the encoder has written the first segment. Serving a 404 there makes the
    // TV declare "content not playable" and never retry, even though the same
    // stream plays fine once written (verified 2026-11-21). Hold the playlist
    // GET on this server thread (never the UI thread) until the first real
    // segment exists. Bounded and stop-aware so Stop() tears down promptly.
    if (target == "live.m3u8" && !HlsReady(directory)) {
        // Only the DMR's first early GET needs to wait; log that one (with the
        // outcome) and stay silent for the steady-state playlist polls.
        LOG("Cast HTTP: early playlist GET, holding for first segment");
        const auto readyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        while (!HlsReady(directory) && std::chrono::steady_clock::now() < readyDeadline &&
               !(stop && stop())) {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }
        LOG("Cast HTTP: playlist hold done, HlsReady=" << HlsReady(directory));
    }

    const std::filesystem::path filePath = directory / std::string(target);
    std::error_code ec;
    const uint64_t fileSize = static_cast<uint64_t>(std::filesystem::file_size(filePath, ec));
    if (ec || fileSize == 0) {
        const std::string header = HttpResponseHeader(404, "Not Found", 0, false, {}, 0);
        SendAll(client, header.data(), header.size());
        return;
    }

    bool hasRange = false;
    ByteRange range{0, fileSize - 1};
    if (!rangeHeader.empty()) {
        if (auto parsed = ParseRange(rangeHeader, fileSize)) {
            range = *parsed;
            hasRange = true;
        } else {
            const std::string header = HttpResponseHeader(416, "Range Not Satisfiable", 0, false, {}, fileSize);
            SendAll(client, header.data(), header.size());
            return;
        }
    }
    const uint64_t length = hasRange ? (range.last - range.first + 1) : fileSize;
    const int status = hasRange ? 206 : 200;
    const std::string statusText = hasRange ? "Partial Content" : "OK";
    const std::string header = HttpResponseHeader(status, statusText, length, hasRange, range, fileSize,
                                                  ContentTypeFor(target));
    if (!SendAll(client, header.data(), header.size())) return;

    std::ifstream file(filePath, std::ios::binary);
    if (!file) return;
    if (hasRange) file.seekg(static_cast<std::streamoff>(range.first), std::ios::beg);
    std::array<char, 64 * 1024> chunk{};
    uint64_t remaining = length;
    while (remaining > 0 && file) {
        const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(remaining, chunk.size()));
        file.read(chunk.data(), static_cast<std::streamsize>(want));
        const std::streamsize got = file.gcount();
        if (got <= 0) break;
        if (!SendAll(client, chunk.data(), static_cast<std::size_t>(got))) break;
        remaining -= static_cast<uint64_t>(got);
    }
}

} // namespace

bool RunServer(const Settings& settings)
{
    EnsureWsa();
    if (!g_wsaOk) {
        LOG("Cast HTTP: WSAStartup failed winerr=" << WSAGetLastError());
        return false;
    }
    Socket listener(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (!listener) {
        LOG("Cast HTTP: socket failed winerr=" << WSAGetLastError());
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(listener.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(settings.port);
    inet_pton(AF_INET, settings.bindIp.c_str(), &addr.sin_addr);
    if (bind(listener.Get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG("Cast HTTP: bind " << settings.bindIp << ":" << settings.port
            << " failed winerr=" << WSAGetLastError());
        return false;
    }
    if (listen(listener.Get(), 8) != 0) {
        LOG("Cast HTTP: listen failed winerr=" << WSAGetLastError());
        return false;
    }
    // Non-blocking accept so the stop flag is observed promptly.
    const DWORD nonBlocking = 1;
    ioctlsocket(listener.Get(), FIONBIO, const_cast<DWORD*>(&nonBlocking));

    LOG("Cast HTTP: serving " << settings.directory.string() << " on "
        << settings.bindIp << ":" << settings.port);
    while (!(settings.stop && settings.stop())) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const SOCKET accepted = accept(listener.Get(), reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (accepted == INVALID_SOCKET) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        Socket client(accepted);
        const DWORD blocking = 0;
        ioctlsocket(client.Get(), FIONBIO, const_cast<DWORD*>(&blocking));
        ServeOne(client.Get(), settings.directory, settings.stop);
    }
    return true;
}

} // namespace cast_http_server
