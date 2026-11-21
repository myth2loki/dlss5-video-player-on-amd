#include "CastSession.h"
#include "Log.h"
#include "Utf8Text.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

namespace cast_session {

namespace {

// Where the last-device UDN is remembered. Kept beside the log so a packaged
// build has a writable location; a failure to save is non-fatal.
std::filesystem::path LastDevicePath()
{
    return Log::Path().parent_path() / "dlna-last-device.txt";
}

} // namespace

struct Session::Impl {
    Config config;
    cast_net::Discovery discovery;
    std::unique_ptr<cast_net::CastControl> control;
    cast_encoder::Encoder encoder;
    std::thread httpThread;
    std::atomic<bool> httpStop{false};
    std::filesystem::path hlsDir;

    mutable std::mutex statusMutex;
    Status status;

    std::string chosenUdn;
    std::string chosenDeviceName;
    std::string mediaPath;
    double seekSeconds{0.0};

    uint64_t startRenderId{0};
    bool drained{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> frameValid{false};
    // lastFrame is written by the render thread (SubmitFrame) and read by the
    // pause-hold thread below; guard it so the hold frame is never a torn copy.
    mutable std::mutex frameMutex;
    std::vector<uint8_t> lastFrame;
    uint32_t lastW{0}, lastH{0};

    // While paused the render loop stops, so no new frames reach the encoder.
    // ffmpeg's rawvideo input only produces frames it is fed; without a hold
    // feed the HLS playlist freezes while the TV still reports PLAYING. A tiny
    // thread re-feeds the cached last frame at the encoder frame rate so the
    // live playlist keeps advancing until playback resumes (doc 14.4/14.6).
    std::thread holdThread;
    std::atomic<bool> holdStop{false};

    std::atomic<int> retryCount{0};
    std::chrono::steady_clock::time_point lastFeedTime{};
    mutable std::mutex discoveryMutex;
    // Set by CancelScan, polled by Scan between datagrams and probes. Lives here
    // rather than being passed in because the UI cancels from a different thread
    // than the one that started the scan.
    std::atomic<bool> scanCancel{false};

    std::string streamUrl() const
    {
        const std::string ip = config.httpBindIp == "0.0.0.0" ? cast_net::LocalLanIp() : config.httpBindIp;
        return "http://" + ip + ":" + std::to_string(config.httpPort) + "/live.m3u8";
    }

    void setStatus(State state, std::string detail = {})
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        status.state = state;
        if (!detail.empty()) status.detail = std::move(detail);
    }

    // Copy the cached frame under the lock; empty when no frame has landed yet.
    cast_encoder::Frame CachedFrame()
    {
        cast_encoder::Frame out;
        std::lock_guard<std::mutex> lock(frameMutex);
        if (!frameValid.load(std::memory_order_acquire) || lastFrame.empty()) return out;
        out.bgra = lastFrame;
        out.width = lastW;
        out.height = lastH;
        return out;
    }

    void HoldLoop()
    {
        // Rawvideo has no wall clock of its own: one frame in = one frame out.
        // Feed the same cached frame at the configured fps so ffmpeg keeps
        // writing HLS segments while the player's render loop is parked.
        const int fps = config.fps > 0 ? config.fps : 30;
        const auto period = std::chrono::milliseconds(1000 / fps);
        while (!holdStop.load(std::memory_order_acquire)) {
            cast_encoder::Frame hold = CachedFrame();
            if (!hold.bgra.empty()) encoder.PushFrame(std::move(hold));
            std::this_thread::sleep_for(period);
        }
    }

    void StartHold()
    {
        if (holdThread.joinable()) return; // already holding
        holdStop.store(false, std::memory_order_release);
        holdThread = std::thread([this] { HoldLoop(); });
    }

    void StopHold()
    {
        // Must join before Stop() takes the encoder down, else HoldLoop would
        // push into a stopped encoder (PushFrame returns false, but the join
        // also guarantees no thread outlives the session).
        holdStop.store(true, std::memory_order_release);
        if (holdThread.joinable()) holdThread.join();
    }
};

Session::Session() : m_impl(std::make_unique<Impl>()) {}

Config& Session::Config_() { return m_impl->config; }

Session::~Session()
{
    Stop();
}

int Session::Scan(std::chrono::seconds listen)
{
    std::lock_guard<std::mutex> lock(m_impl->discoveryMutex);
    m_impl->scanCancel.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> statusLock(m_impl->statusMutex);
        m_impl->status.state = State::Scanning;
    }
    const auto scanStart = std::chrono::steady_clock::now();
    const int found = m_impl->discovery.Scan(cast_net::LocalLanIp(), listen, &m_impl->scanCancel);

    // Probe what this scan actually saw. SSDP only proves a device answers
    // multicast; whether its AVTransport endpoint is alive is a separate
    // question. Without this step the menu's "reachable" flag is whatever the
    // last cast left behind, so a TV that was just switched on keeps reading
    // "offline" until someone tries to cast to it and fails.
    //
    // Only devices seen in THIS scan (lastSeen advanced past scanStart) and only
    // real DMRs; a bounded count and a short timeout keep the window small.
    static constexpr std::size_t kMaxProbesPerScan = 8;
    std::vector<std::string> probeKeys;
    for (const auto& device : m_impl->discovery.Registry().Snapshot()) {
        if (!cast_device::IsDmr(device)) continue;
        if (device.lastSeen < scanStart) continue;
        probeKeys.push_back(cast_device::IdentityKey(device));
        if (probeKeys.size() >= kMaxProbesPerScan) break;
    }
    for (const std::string& key : probeKeys) {
        if (m_impl->scanCancel.load(std::memory_order_acquire)) break;
        const cast_device::Device* device = m_impl->discovery.Registry().Find(key);
        if (!device) continue;
        const auto probe = m_impl->discovery.Probe(*device, std::chrono::milliseconds{1200});
        m_impl->discovery.Registry().SetProbe(key, cast_device::IsReachable(probe),
                                              cast_device::ProbeDetail(probe));
    }
    {
        std::lock_guard<std::mutex> statusLock(m_impl->statusMutex);
        m_impl->status.state = m_impl->encoder.Running() ? State::Streaming : State::Idle;
    }
    return found;
}

void Session::CancelScan()
{
    m_impl->scanCancel.store(true, std::memory_order_release);
}

std::vector<Session::MenuEntry> Session::DeviceMenu() const
{
    std::lock_guard<std::mutex> lock(m_impl->discoveryMutex);
    // Snapshot, not All(): it sorts by friendly name and recomputes offline from
    // the TTL, so the list does not reshuffle between two opens of the menu.
    const auto devices = m_impl->discovery.Registry().Snapshot();
    std::vector<MenuEntry> out;
    out.reserve(devices.size());
    for (const auto& device : devices) {
        MenuEntry entry;
        entry.key = cast_device::IdentityKey(device);
        entry.displayName = cast_device::DisplayName(device, devices);
        entry.online = m_impl->discovery.Registry().IsOnline(device);
        entry.dmr = cast_device::IsDmr(device);
        entry.reachable = device.reachable;
        // A code, not a sentence (14.9): DIAL-only is not a DMR; a 501 means
        // alive-but-idle, which is not the same thing as offline.
        if (!entry.dmr) entry.label = DeviceLabel::DialOnly;
        else if (!entry.reachable) entry.label = DeviceLabel::Offline;
        else if (device.probeDetail == "UPnP-error-501") entry.label = DeviceLabel::IdleReachable;
        else if (!device.probeDetail.empty()) {
            entry.label = DeviceLabel::Detail;
            entry.detail = device.probeDetail;
        } else entry.label = DeviceLabel::None;
        out.push_back(std::move(entry));
    }
    return out;
}

bool Session::Start(const std::string& deviceUdn, const std::string& mediaPath,
                    double seekSeconds, bool paused, uint64_t startRenderId, std::string* error)
{
    const auto fail = [&](std::string message) -> bool {
        {
            std::lock_guard<std::mutex> lock(m_impl->statusMutex);
            m_impl->status.state = State::Error;
            m_impl->status.detail = message;
        }
        if (error) *error = message;
        LOG("Cast start failed: " << message);
        return false;
    };

    // Resolve the device by identity (UDN), never by name or IP.
    std::lock_guard<std::mutex> lock(m_impl->discoveryMutex);
    const cast_device::Device* device = m_impl->discovery.Registry().Find(deviceUdn);
    if (!device) return fail("device not found: " + deviceUdn);
    if (!cast_device::IsDmr(*device)) return fail("device is DIAL, not a DMR");

    // Probe just before casting (double insurance, 14.8), but a probe cannot
    // decide castability — SetURI below is the real test.
    const auto probe = m_impl->discovery.Probe(*device);
    if (!cast_device::IsReachable(probe)) return fail("device unreachable: " + cast_device::ProbeDetail(probe));

    Stop(); // idempotent; clears a previous session if any
    m_impl->chosenUdn = deviceUdn;
    m_impl->chosenDeviceName = cast_device::DisplayName(*device, m_impl->discovery.Registry().All());
    m_impl->mediaPath = mediaPath;
    m_impl->seekSeconds = seekSeconds;
    m_impl->startRenderId = startRenderId;
    m_impl->drained = (startRenderId == 0);
    m_impl->paused.store(paused, std::memory_order_release);
    m_impl->frameValid.store(false, std::memory_order_release);

    // Fixed temp directory for HLS, keyed by nothing user-controlled.
    const auto tempRoot = std::filesystem::temp_directory_path();
    m_impl->hlsDir = tempRoot / "dlss5-cast-hls";
    std::error_code ec;
    std::filesystem::remove_all(m_impl->hlsDir, ec); // fresh session, no stale segments
    std::filesystem::create_directories(m_impl->hlsDir, ec);
    if (ec) return fail("cannot create HLS dir: " + ec.message());

    // HTTP server (14.1: child process in the product; the served directory is
    // a strict allowlist so there is no path-traversal surface either way).
    m_impl->httpStop.store(false, std::memory_order_release);
    cast_http_server::Settings httpSettings;
    httpSettings.directory = m_impl->hlsDir;
    httpSettings.bindIp = m_impl->config.httpBindIp;
    httpSettings.port = m_impl->config.httpPort;
    httpSettings.stop = [this] { return m_impl->httpStop.load(std::memory_order_acquire); };
    m_impl->httpThread = std::thread([httpSettings] { cast_http_server::RunServer(httpSettings); });

    // Encoder child (h264_amf preferred, libx264 fallback).
    cast_encoder::Settings encSettings;
    encSettings.hlsDirectory = m_impl->hlsDir;
    encSettings.ffmpegPath = m_impl->config.ffmpegPath;
    encSettings.width = m_impl->config.width;
    encSettings.height = m_impl->config.height;
    encSettings.fps = m_impl->config.fps;
    encSettings.preferAmf = m_impl->config.preferAmf;
    encSettings.bitrateKbps = m_impl->config.bitrateKbps;
    encSettings.audioSource = mediaPath;
    encSettings.audioSeekSeconds = seekSeconds;
    const auto enc = m_impl->encoder.Start(encSettings);
    if (!enc.started) {
        m_impl->httpStop.store(true, std::memory_order_release);
        if (m_impl->httpThread.joinable()) m_impl->httpThread.join();
        return fail("encoder: " + enc.error);
    }

    // Push the URL to the DMR and play (14.9: this is the real castability test).
    // Do NOT block here waiting for the first segment: Start() runs on the UI
    // thread and stalls the render loop that FEEDS the encoder, so the segment
    // would never appear (self-deadlock, verified 2026-11-21). Instead the HTTP
    // server holds the DMR's first playlist GET until the stream is actually
    // ready; see HlsReady in CastHttpServer.h and its wait in ServeOne.
    m_impl->control = std::make_unique<cast_net::CastControl>(device->controlURL);
    const std::string url = m_impl->streamUrl();
    const auto pushed = m_impl->control->SetUriAndPlay(url, "application/x-mpegURL", m_impl->chosenDeviceName);
    if (!pushed.httpAnswer || pushed.soapFault) {
        m_impl->encoder.Stop();
        m_impl->httpStop.store(true, std::memory_order_release);
        if (m_impl->httpThread.joinable()) m_impl->httpThread.join();
        return fail("DMR refused URL: " + pushed.detail + " (fault " + std::to_string(pushed.faultCode) + ")");
    }

    {
        std::lock_guard<std::mutex> statusLock(m_impl->statusMutex);
        m_impl->status.state = State::Streaming;
        m_impl->status.deviceName = m_impl->chosenDeviceName;
        m_impl->status.deviceUdn = m_impl->chosenUdn;
        m_impl->status.streamUrl = url;
        m_impl->status.encoder = enc.encoderUsed;
        m_impl->status.width = m_impl->config.width;
        m_impl->status.height = m_impl->config.height;
        m_impl->status.fps = m_impl->config.fps;
        m_impl->status.detail = "connected";
    }
    m_impl->retryCount.store(0, std::memory_order_relaxed);
    SaveLastDevice();
    LOG("Cast: streaming to " << m_impl->chosenDeviceName << " via " << url);
    return true;
}

void Session::SubmitFrame(const uint8_t* bgra, uint32_t width, uint32_t height, uint64_t renderId)
{
    if (!m_impl->encoder.Running()) return;
    if (renderId != 0 && renderId < m_impl->startRenderId) return; // drain stale backlog (14.2)
    m_impl->drained = true;
    if (!bgra || width == 0 || height == 0) return;
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;

    // Keep the last frame for the paused hold path (14.6). The hold thread
    // reads this under the same lock, so publish a complete frame only.
    {
        std::lock_guard<std::mutex> lock(m_impl->frameMutex);
        m_impl->lastFrame.assign(bgra, bgra + bytes);
        m_impl->lastW = width;
        m_impl->lastH = height;
        m_impl->frameValid.store(true, std::memory_order_release);
    }
    cast_encoder::Frame frame;
    frame.bgra.assign(bgra, bgra + bytes);
    frame.width = width;
    frame.height = height;
    frame.id = renderId;
    m_impl->encoder.PushFrame(std::move(frame));
    m_impl->lastFeedTime = std::chrono::steady_clock::now();
}

void Session::SetPaused(bool paused)
{
    m_impl->paused.store(paused, std::memory_order_release);
    if (!m_impl->encoder.Running()) return;
    if (paused) {
        // The render loop stops while paused. Feed the cached frame once now
        // for an immediate hold, then keep re-feeding it at the encoder fps so
        // ffmpeg (rawvideo, frame-driven) never starves and HLS stays live
        // (14.4/14.6). StartHold is idempotent across repeated pause ticks.
        cast_encoder::Frame hold = m_impl->CachedFrame();
        if (!hold.bgra.empty()) m_impl->encoder.PushFrame(std::move(hold));
        m_impl->StartHold();
    } else {
        m_impl->StopHold();
    }
}

void Session::OnSourceChanged()
{
    // Source change invalidates the audio decode and frame source; stop casting
    // but keep the device choice (14.6).
    const std::string keep = m_impl->chosenUdn;
    Stop();
    m_impl->chosenUdn = keep;
}

void Session::Stop()
{
    // Join the hold thread before the encoder goes away; otherwise it could
    // push cached frames into an encoder already tearing down.
    m_impl->StopHold();
    m_impl->paused.store(false, std::memory_order_release);
    m_impl->httpStop.store(true, std::memory_order_release);
    m_impl->encoder.Stop();
    if (m_impl->httpThread.joinable()) m_impl->httpThread.join();
    if (m_impl->control) {
        // Best-effort Stop on the DMR; ignore faults — we are tearing down.
        m_impl->control->Stop();
        m_impl->control.reset();
    }
    m_impl->frameValid.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(m_impl->statusMutex);
        m_impl->status.state = State::Idle;
        m_impl->status.streamUrl.clear();
        m_impl->status.encoder.clear();
        m_impl->status.detail.clear();
    }
    std::error_code ec;
    std::filesystem::remove_all(m_impl->hlsDir, ec);
}

Status Session::GetStatus() const
{
    std::lock_guard<std::mutex> lock(m_impl->statusMutex);
    return m_impl->status;
}

State Session::GetState() const
{
    std::lock_guard<std::mutex> lock(m_impl->statusMutex);
    return m_impl->status.state;
}

bool Session::Casting() const
{
    return m_impl->encoder.Running();
}

std::string Session::LastDeviceUdn() const
{
    return LoadLastDevice();
}

void Session::SaveLastDevice() const
{
    if (m_impl->chosenUdn.empty()) return;
    std::ofstream out(LastDevicePath(), std::ios::binary | std::ios::trunc);
    if (out) out << m_impl->chosenUdn;
}

std::string Session::LoadLastDevice() const
{
    std::ifstream in(LastDevicePath(), std::ios::binary);
    if (!in) return {};
    std::string udn((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return udn;
}

} // namespace cast_session
