#pragma once

// Minimal WinHTTP GET/POST shared by discovery (description XML) and control
// (SOAP actions). One implementation so timeouts, header construction and
// response draining cannot drift between the two. Header-only: WinHTTP is
// already a link dependency of the player.

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace cast_http {

// RAII for WinHTTP handles.
class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET h) : m_h(h) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : m_h(o.Release()) {}
    Handle& operator=(Handle&& o) noexcept
    {
        if (this != &o) { Reset(); m_h = o.Release(); }
        return *this;
    }
    HINTERNET Get() const { return m_h; }
    explicit operator bool() const { return m_h != nullptr; }
    HINTERNET Release() { HINTERNET h = m_h; m_h = nullptr; return h; }
    void Reset(HINTERNET h = nullptr)
    {
        if (m_h) WinHttpCloseHandle(m_h);
        m_h = h;
    }
private:
    HINTERNET m_h{nullptr};
};

struct UrlParts {
    std::wstring host;
    INTERNET_PORT port{INTERNET_DEFAULT_HTTP_PORT};
    std::wstring path;
    bool https{false};
};

inline std::optional<UrlParts> ParseUrl(std::string_view url)
{
    constexpr std::string_view http = "http://";
    constexpr std::string_view https = "https://";
    bool secure = false;
    std::string_view rest;
    if (url.rfind(http, 0) == 0) rest = url.substr(http.size());
    else if (url.rfind(https, 0) == 0) { rest = url.substr(https.size()); secure = true; }
    else return std::nullopt;
    const std::size_t slash = rest.find('/');
    const std::string_view authority = rest.substr(0, slash);
    const std::string_view path = slash == std::string_view::npos ? std::string_view("/") : rest.substr(slash);
    INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    std::string_view host = authority;
    if (const std::size_t colon = authority.rfind(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        const auto portText = authority.substr(colon + 1);
        int parsed = 0;
        const auto [ptr, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), parsed);
        if (ec != std::errc{} || ptr != portText.data() + portText.size() || parsed <= 0 || parsed > 65535)
            return std::nullopt;
        port = static_cast<INTERNET_PORT>(parsed);
    }
    if (host.empty()) return std::nullopt;
    std::wstring wideHost(host.begin(), host.end());
    std::wstring widePath(path.begin(), path.end());
    return UrlParts{std::move(wideHost), port, std::move(widePath), secure};
}

struct HttpResult { int status{0}; std::string body; };

// One request. `headers` is the full additional-header block in WCHAR form
// (WinHTTP's W API takes LPCWSTR); `body` is raw bytes sent as-is for POST.
// A non-2xx response is returned with its status and body — no exception, no
// early "offline" verdict: a SOAP 501 fault is a valid, informative answer.
inline HttpResult Request(const std::string& url, bool isGet, const std::wstring& headers,
                          const std::string& body, std::chrono::milliseconds timeout)
{
    HttpResult result;
    const auto parts = ParseUrl(url);
    if (!parts) return result;
    Handle session(WinHttpOpen(L"DLSSVideoPlayer-Cast/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return result;
    const int t = static_cast<int>(timeout.count());
    WinHttpSetTimeouts(session.Get(), t, t, t, t);
    Handle connect(WinHttpConnect(session.Get(), parts->host.c_str(), parts->port, 0));
    if (!connect) return result;
    Handle request(WinHttpOpenRequest(connect.Get(), isGet ? L"GET" : L"POST",
                                      parts->path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, parts->https ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return result;
    WinHttpSetTimeouts(request.Get(), t, t, t, t);
    const BOOL sent = WinHttpSendRequest(
        request.Get(), headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
        isGet ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        isGet ? 0 : static_cast<DWORD>(body.size()),
        isGet ? 0 : static_cast<DWORD>(body.size()), 0);
    if (!sent) return result;
    if (!WinHttpReceiveResponse(request.Get(), nullptr)) return result;
    DWORD status = 0, size = sizeof(status);
    if (WinHttpQueryHeaders(request.Get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
        result.status = static_cast<int>(status);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.Get(), &available) || available == 0) break;
        const std::size_t offset = result.body.size();
        const std::size_t chunk = std::min<std::size_t>(available, 64u * 1024u);
        result.body.resize(offset + chunk);
        DWORD read = 0;
        const BOOL ok = WinHttpReadData(request.Get(), result.body.data() + offset,
                                        static_cast<DWORD>(chunk), &read);
        result.body.resize(offset + read);
        if (!ok || read == 0 || result.body.size() > (4u << 20)) break;
    }
    return result;
}

inline HttpResult Get(const std::string& url, std::chrono::milliseconds timeout)
{
    return Request(url, true, {}, {}, timeout);
}

// SOAP POST: builds Content-Type + quoted SOAPACTION headers from the action.
inline HttpResult SoapPost(const std::string& url, std::string_view soapAction,
                           const std::string& body, std::chrono::milliseconds timeout)
{
    const std::wstring wideAction(soapAction.begin(), soapAction.end());
    const std::wstring headers =
        L"Content-Type: text/xml; charset=utf-8\r\nSOAPACTION: " + wideAction + L"\r\n";
    return Request(url, false, headers, body, timeout);
}

} // namespace cast_http
