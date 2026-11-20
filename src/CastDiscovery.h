#pragma once

// SSDP/UPnP device discovery for DLNA casting, on the CastDevicePolicy model.
// Owns the UDP multicast M-SEARCH and the WinHTTP GET of each description XML;
// turns raw responses into Device records that carry a UDN identity. Policy
// decisions (identity, TTL, display name, liveness) live in CastDevicePolicy.h
// and are not repeated here.
//
// Threading: Discovery is created and driven on the cast control thread. It is
// not internally synchronized; the session serializes access to Registry().

#include "CastDevicePolicy.h"

#include <chrono>
#include <string>
#include <vector>

namespace cast_net {

// Discover DMRs on the LAN. `localIp` is the address of the interface to join
// the SSDP multicast group on (the caller passes the player's LAN IP); an empty
// string lets the OS pick. Returns one Device per unique UDN with `lastSeen`
// set and reachable=false until probeDevice is called.
//
// `seconds` bounds the whole listen window. SSDP answers are connectionless and
// may arrive in any order; results are deduped by IdentityKey inside the
// registry, so callers should merge via Registry().Upsert rather than trusting
// the returned vector's order.
class Discovery {
public:
    Discovery() = default;
    Discovery(const Discovery&) = delete;
    Discovery& operator=(const Discovery&) = delete;
    ~Discovery() = default;

    // M-SEARCH + parse descriptions. Returns the number of devices newly seen.
    // Each discovered device is merged into the registry. `st` is the SSDP
    // search target; "upnp:rootdevice" finds all devices including non-DMRs,
    // which is intentional so a DIAL endpoint can be labelled honestly.
    int Scan(std::string localIp, std::chrono::seconds listen = std::chrono::seconds{6});

    // Fetch one description URL directly (no SSDP), e.g. a device the user
    // entered by hand or a remembered LOCATION. Returns false when the URL
    // cannot be read or parsed.
    bool FetchDescription(const std::string& location, cast_device::Device& out);

    // Probe endpoint reachability with GetTransportInfo. Per 14.9 a 501 fault
    // is reachable; only no HTTP response is offline. `out` receives the raw
    // fields for policy to judge via IsReachable/ProbeDetail. Never calls a
    // state-changing action, so it cannot hijack what is playing on the TV.
    cast_device::ProbeResult Probe(const cast_device::Device& device,
                                   std::chrono::milliseconds timeout = std::chrono::milliseconds{3000});

    cast_device::DeviceRegistry& Registry() { return m_registry; }
    const cast_device::DeviceRegistry& Registry() const { return m_registry; }

private:
    cast_device::DeviceRegistry m_registry;
};

// Best-effort detection of this machine's LAN IPv4 (the address on the default
// route / the one that can reach 239.255.255.250). Returns empty on failure.
// The session uses it both for SSDP membership and for the HLS stream URL the
// TV will fetch.
std::string LocalLanIp();

} // namespace cast_net
