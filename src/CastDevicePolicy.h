#pragma once

// DLNA control-plane device identity and liveness policy.
//
// This header is the C++ translation of the rules grilled and verified in the
// POC (doc sections 14.8 / 14.9). It is deliberately dependency-free and
// side-effect-free so its rules can be unit-tested without a network, a TV, or
// a Win32 socket: every decision here is a pure function of its inputs.
//
// The three rules it exists to protect (each was a real bug):
//   1. Identity is the UDN. IP changes with DHCP and friendlyName collides
//      (one Sony TV exposes two "SONY KD-55X9500H" DMRs on one IP), so neither
//      is a key. The registry is keyed by UDN.
//   2. Reachability is NOT castability. A DMR with no active playback instance
//      legitimately answers GetTransportInfo with a 501 SOAP fault. That proves
//      the endpoint is alive, not offline. Only SetAVTransportURI in the cast
//      step answers "can it play our URL" - probing it would hijack the TV.
//   3. A device is a DMR only if it declares AVTransport:1. Sony's sibling
//      :7084 service exposes only DIAL/ScalarWebAPI - that is what bilibili uses,
//      but it cannot accept our URL. Label it honestly, never as an offline DMR.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cast_device {

// A discovered UPnP device, as parsed from its description XML. Kept as plain
// data so discovery (network) and policy (pure) stay separable and testable.
struct Device {
    std::string udn;          // uuid:... — the identity key; empty only for malformed devices
    std::string friendlyName; // display only
    std::string modelName;    // display only
    std::string location;     // description URL
    std::string controlURL;   // AVTransport SOAP endpoint (absolute)
    std::string ip;           // connection address only
    bool hasAVTransport{false};
    // Filled by Scan()/Probe(), not part of description parsing:
    std::chrono::steady_clock::time_point lastSeen{};
    bool reachable{false};
    std::string probeDetail;  // e.g. "PLAYING", "STOPPED", "UPnP-error-501", "nonDmr"
};

inline constexpr std::chrono::seconds kDeviceTtl{90};

// Stable identity: UDN when present, else the LOCATION url as a last-resort key
// for malformed devices (never IP/friendlyName — both are unstable).
inline std::string IdentityKey(const Device& device)
{
    return device.udn.empty() ? device.location : device.udn;
}

// True when the device declares AVTransport:1 and is therefore a real DMR that
// could in principle accept SetAVTransportURI. A DIAL-only endpoint fails this
// and must be labelled "DIAL 非 DMR", not treated as a broken DMR.
inline bool IsDmr(const Device& device)
{
    return device.hasAVTransport;
}

// The correction at the heart of 14.9: any SOAP answer (a 200 state or a 500/501
// fault) proves reachability. Only a timeout / refused connection / missing HTTP
// response means offline. This takes the *raw* transport result and the parsed
// state so the liveness decision can be asserted in tests with either shape.
struct ProbeResult {
    bool httpAnswerReceived{false}; // did a SOAP endpoint answer at all?
    int httpStatus{0};
    std::string transportState;    // CurrentTransportState from a 200 answer
    int upnpErrorCode{0};          // <errorCode> from a fault, 0 if none
};

inline bool IsReachable(const ProbeResult& probe)
{
    // A fault is a valid UPnP answer. Anything below 600 that produced an HTTP
    // status is "alive"; no status at all is the only offline signal.
    return probe.httpAnswerReceived && probe.httpStatus > 0 && probe.httpStatus < 600;
}

// Human-readable liveness detail, matching the labels the POC console shows.
// A 501 is reported as an error code, not as "offline"; the user can see the
// endpoint is alive yet refusing, which is honest and actionable.
inline std::string ProbeDetail(const ProbeResult& probe)
{
    if (!IsReachable(probe)) return "no-response";
    if (!probe.transportState.empty()) return probe.transportState;
    if (probe.upnpErrorCode != 0) return "UPnP-error-" + std::to_string(probe.upnpErrorCode);
    return "HTTP-" + std::to_string(probe.httpStatus);
}

// Friendly name with a stable UDN suffix when names collide on the LAN.
// Suffix is the last 5 hex chars of the uuid (stripped of "uuid:"), matching
// the POC's display_name so the menu and the console agree.
inline std::string DisplayName(const Device& device, const std::vector<Device>& all)
{
    const std::string& name = device.friendlyName.empty() ? device.ip : device.friendlyName;
    const std::string shown = name.empty() ? "unknown" : name;
    const int collisions = static_cast<int>(std::count_if(
        all.begin(), all.end(), [&](const Device& d) {
            const std::string& other = d.friendlyName.empty() ? d.ip : d.friendlyName;
            return other == name;
        }));
    if (collisions > 1 && !device.udn.empty()) {
        std::string uuid = device.udn;
        constexpr std::string_view prefix = "uuid:";
        if (uuid.rfind(prefix, 0) == 0) uuid.erase(0, prefix.size());
        const std::string tail = uuid.size() > 5 ? uuid.substr(uuid.size() - 5) : uuid;
        return shown + " (" + tail + ")";
    }
    return shown;
}

// A registry of discovered devices keyed by UDN. Scan results are merged by
// identity: re-discovering a device refreshes lastSeen instead of duplicating.
// Entries past TTL are marked offline but RETAINED (so the menu can still name
// the last device and explain it is gone) — never silently dropped.
class DeviceRegistry {
public:
    // Merge one discovered/observed device. Returns the stored entry's key.
    std::string Upsert(Device device)
    {
        device.lastSeen = std::chrono::steady_clock::now();
        const std::string key = IdentityKey(device);
        auto it = m_devices.find(key);
        if (it == m_devices.end()) {
            m_devices.emplace(key, std::move(device));
        } else {
            // Keep the freshest connection fields; never let a stale scan
            // overwrite a newer controlURL after a DHCP/IP change.
            it->second.friendlyName = std::move(device.friendlyName);
            it->second.modelName = std::move(device.modelName);
            it->second.location = std::move(device.location);
            it->second.controlURL = std::move(device.controlURL);
            it->second.ip = std::move(device.ip);
            it->second.hasAVTransport = device.hasAVTransport;
            it->second.lastSeen = device.lastSeen;
        }
        return key;
    }

    Device* Find(const std::string& key)
    {
        auto it = m_devices.find(key);
        return it == m_devices.end() ? nullptr : &it->second;
    }
    const Device* Find(const std::string& key) const
    {
        auto it = m_devices.find(key);
        return it == m_devices.end() ? nullptr : &it->second;
    }

    bool IsOnline(const Device& device,
                  std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const
    {
        return now - device.lastSeen <= kDeviceTtl;
    }

    // Snapshot for the UI. Offline flag is computed, never stored, so it cannot
    // go stale when a device is re-seen.
    std::vector<Device> Snapshot(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const
    {
        std::vector<Device> out;
        out.reserve(m_devices.size());
        for (const auto& [key, device] : m_devices) {
            Device copy = device;
            if (!IsOnline(device, now)) copy.reachable = false;
            out.push_back(std::move(copy));
        }
        // Stable display order: online first, then friendlyName, so the menu
        // does not reshuffle between opens (UDP response order is random).
        std::stable_sort(out.begin(), out.end(), [](const Device& a, const Device& b) {
            return a.friendlyName < b.friendlyName;
        });
        return out;
    }

    // All devices, for DisplayName collision counting.
    std::vector<Device> All() const
    {
        std::vector<Device> out;
        out.reserve(m_devices.size());
        for (const auto& [key, device] : m_devices) out.push_back(device);
        return out;
    }

    void Clear() { m_devices.clear(); }
    std::size_t Size() const { return m_devices.size(); }

private:
    std::unordered_map<std::string, Device> m_devices;
};

} // namespace cast_device
