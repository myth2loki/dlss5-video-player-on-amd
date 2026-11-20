#include "CastDiscovery.h"
#include "CastHttp.h"
#include "CastSoap.h"
#include "Log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace cast_net {

namespace {

// One-time WSAStartup for the process. Winsock requires a matching cleanup;
// the process is short-lived relative to the player and never unloads it, which
// is safe (WSACleanup at exit is optional when the process is terminating).
std::once_flag g_wsaOnce;
bool g_wsaOk = false;
void EnsureWsa()
{
    std::call_once(g_wsaOnce, [] {
        WSADATA data{};
        g_wsaOk = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        if (!g_wsaOk) LOG("Cast: WSAStartup failed winerr=" << WSAGetLastError());
    });
}

// RAII for a SOCKET.
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


// Resolve a possibly-relative controlURL against the description LOCATION.
std::string ResolveControlUrl(const std::string& location, std::string_view control)
{
    if (control.empty()) return {};
    if (control.rfind("http://", 0) == 0 || control.rfind("https://", 0) == 0)
        return std::string(control);
    const std::size_t slash = location.rfind('/');
    const std::string base = slash == std::string::npos ? std::string() : location.substr(0, slash + 1);
    std::string relative(control);
    if (relative.front() == '/') {
        // Absolute path: keep scheme+authority from LOCATION, drop its path.
        const std::size_t scheme = location.find("://");
        if (scheme == std::string::npos) return base + relative;
        const std::size_t authorityEnd = location.find('/', scheme + 3);
        return location.substr(0, authorityEnd) + relative;
    }
    return base + relative;
}

} // namespace

std::string LocalLanIp()
{
    EnsureWsa();
    if (!g_wsaOk) return {};
    // A connected UDP socket to a public address reveals the local interface
    // the OS would route through, without sending a packet. Falls back to the
    // first non-loopback IPv4 from getaddrinfo on the machine name.
    Socket sock(socket(AF_INET, SOCK_DGRAM, 0));
    if (sock) {
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(80);
        inet_pton(AF_INET, "223.5.5.5", &remote.sin_addr);
        if (connect(sock.Get(), reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
            sockaddr_in local{};
            int len = sizeof(local);
            if (getsockname(sock.Get(), reinterpret_cast<sockaddr*>(&local), &len) == 0) {
                char buf[INET_ADDRSTRLEN]{};
                if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf))) return buf;
            }
        }
    }
    char name[256]{};
    if (gethostname(name, sizeof(name)) != 0) return {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &result) != 0) return {};
    std::string found;
    for (addrinfo* it = result; it; it = it->ai_next) {
        auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
        char buf[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf)) &&
            std::string(buf) != "127.0.0.1") { found = buf; break; }
    }
    freeaddrinfo(result);
    return found;
}

bool Discovery::FetchDescription(const std::string& location, cast_device::Device& out)
{
    const cast_http::HttpResult http = cast_http::Get(location, std::chrono::milliseconds{4000});
    if (http.status < 200 || http.status >= 300) return false;
    const std::string& xml = http.body;

    const auto tag = [&](std::string_view name) -> std::string {
        const auto value = cast_soap::TagValue(xml, name);
        return value ? *value : std::string{};
    };
    out.udn = tag("UDN");
    out.friendlyName = tag("friendlyName");
    if (out.friendlyName.empty()) out.friendlyName = "unknown";
    out.modelName = tag("modelName");
    out.location = location;

    // A real DMR declares AVTransport:1 in one of its <service> blocks. The
    // Sony DIAL sibling (:7084) does not; label it honestly rather than as a
    // broken DMR (doc 14.9).
    const std::size_t avt = xml.find("AVTransport:1");
    out.hasAVTransport = avt != std::string::npos;
    std::string control;
    if (out.hasAVTransport) {
        // Find the <service> block containing AVTransport:1, then its controlURL.
        std::size_t searchFrom = 0;
        while (searchFrom < xml.size()) {
            const std::size_t svcStart = xml.find("<service>", searchFrom);
            if (svcStart == std::string::npos) break;
            const std::size_t svcEnd = xml.find("</service>", svcStart);
            if (svcEnd == std::string::npos) break;
            const std::string_view block(xml.data() + svcStart, svcEnd - svcStart);
            if (block.find("AVTransport:1") != std::string_view::npos) {
                const auto url = cast_soap::TagValue(block, "controlURL");
                if (url) control = *url;
                break;
            }
            searchFrom = svcEnd + 10;
        }
    }
    out.controlURL = ResolveControlUrl(location, control);

    // IP is connection-only, parsed from the LOCATION host.
    const auto parts = cast_http::ParseUrl(location);
    if (parts) {
        // The host from a description URL is an IPv4 address or ASCII hostname;
        // narrow explicitly (wchar_t -> char is lossy in general, but never here).
        out.ip.clear();
        out.ip.reserve(parts->host.size());
        for (wchar_t c : parts->host) out.ip.push_back(c < 128 ? static_cast<char>(c) : '?');
    }
    return true;
}

int Discovery::Scan(std::string localIp, std::chrono::seconds listen)
{
    EnsureWsa();
    if (!g_wsaOk) return 0;
    if (localIp.empty()) localIp = LocalLanIp();

    static constexpr std::string_view kMulticast = "239.255.255.250";
    static constexpr unsigned short kSsdpPort = 1900;
    const std::string msearch =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 3\r\n"
        "ST: upnp:rootdevice\r\n"
        "\r\n";

    Socket sock(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!sock) { LOG("Cast: SSDP socket failed winerr=" << WSAGetLastError()); return 0; }
    BOOL reuse = TRUE;
    setsockopt(sock.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    // Join the multicast group on the chosen local interface so replies reach us.
    if (!localIp.empty()) {
        struct ip_mreq group{};
        inet_pton(AF_INET, kMulticast.data(), &group.imr_multiaddr);
        inet_pton(AF_INET, localIp.c_str(), &group.imr_interface);
        setsockopt(sock.Get(), IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&group), sizeof(group));
    }
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = 0; // ephemeral; replies come back to the source port
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock.Get(), reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
        LOG("Cast: SSDP bind failed winerr=" << WSAGetLastError());
        return 0;
    }
    const DWORD nonBlocking = 1;
    ioctlsocket(sock.Get(), FIONBIO, const_cast<DWORD*>(&nonBlocking));

    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(kSsdpPort);
    inet_pton(AF_INET, kMulticast.data(), &target.sin_addr);
    for (int i = 0; i < 3; ++i)
        sendto(sock.Get(), msearch.data(), static_cast<int>(msearch.size()), 0,
               reinterpret_cast<sockaddr*>(&target), sizeof(target));

    std::vector<std::string> locations;
    const auto deadline = std::chrono::steady_clock::now() + listen;
    char buffer[8192];
    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int got = recvfrom(sock.Get(), buffer, sizeof(buffer), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); continue; }
        const std::string_view response(buffer, static_cast<std::size_t>(got));
        std::size_t pos = 0;
        while (pos < response.size()) {
            const std::size_t eol = response.find('\n', pos);
            const std::string_view line = response.substr(pos, eol == std::string_view::npos
                                                              ? std::string_view::npos : eol - pos);
            pos = eol == std::string_view::npos ? response.size() : eol + 1;
            std::size_t colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            std::string_view key = line.substr(0, colon);
            // Case-insensitive "LOCATION" without allocating a lowercase copy.
            if (key.size() != 8) continue;
            bool isLocation = true;
            constexpr std::string_view lower = "location";
            for (std::size_t i = 0; i < 8; ++i) {
                char c = key[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                if (c != lower[i]) { isLocation = false; break; }
            }
            if (!isLocation) continue;
            std::string_view value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                                      value.front() == '\r')) value.remove_prefix(1);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t' ||
                                      value.back() == '\r')) value.remove_suffix(1);
            if (value.empty()) continue;
            std::string location(value);
            if (std::find(locations.begin(), locations.end(), location) == locations.end())
                locations.push_back(std::move(location));
        }
    }

    int added = 0;
    for (const std::string& location : locations) {
        cast_device::Device device;
        if (!FetchDescription(location, device)) continue;
        const std::size_t before = m_registry.Size();
        m_registry.Upsert(std::move(device));
        if (m_registry.Size() > before) ++added;
    }
    LOG("Cast: SSDP found " << locations.size() << " locations, " << added << " new devices");
    return added;
}

cast_device::ProbeResult Discovery::Probe(const cast_device::Device& device,
                                          std::chrono::milliseconds timeout)
{
    cast_device::ProbeResult result;
    if (device.controlURL.empty()) return result;
    const std::string body = cast_soap::GetTransportInfoBody("0");
    const cast_http::HttpResult http = cast_http::SoapPost(device.controlURL, cast_soap::SoapAction("GetTransportInfo"),
                                     body, timeout);
    if (http.status <= 0) return result; // no answer at all -> offline
    result.httpAnswerReceived = true;
    result.httpStatus = http.status;
    const auto state = cast_soap::TagValue(http.body, "CurrentTransportState");
    if (state) result.transportState = *state;
    result.upnpErrorCode = cast_soap::FaultCode(http.body);
    return result;
}

} // namespace cast_net
