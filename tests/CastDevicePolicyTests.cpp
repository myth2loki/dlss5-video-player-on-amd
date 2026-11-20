// Standalone assertions for CastDevicePolicy.h — no network, no TV, no Win32.
// Compile with a C++20 compiler and run; a non-zero exit is a failed gate.
#include "CastDevicePolicy.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace cast_device;
using Clock = std::chrono::steady_clock;

namespace {
int g_pass = 0;
int g_fail = 0;

#define CHECK(cond, name) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

Device MakeDevice(std::string udn, std::string name, bool avt = true)
{
    Device d;
    d.udn = std::move(udn);
    d.friendlyName = std::move(name);
    d.hasAVTransport = avt;
    d.location = "http://192.168.1.24:63636/description.xml";
    d.controlURL = "http://192.168.1.24:63636/AVTransport/ctrl";
    d.ip = "192.168.1.24";
    return d;
}
} // namespace

int main()
{
    // --- identity: UDN is the key, IP/name are not ---
    Device a = MakeDevice("uuid:SMG-f57f", "SMG投屏24");
    Device b = MakeDevice("uuid:JIGUANG-aabb", "客厅极光TV");
    CHECK(IdentityKey(a) == "uuid:SMG-f57f", "identity uses UDN");
    Device malformed = MakeDevice("", "nameless");
    malformed.location = "http://10.0.0.9/desc.xml";
    CHECK(IdentityKey(malformed) == malformed.location, "identity falls back to LOCATION without UDN");

    DeviceRegistry reg;
    reg.Upsert(a);
    reg.Upsert(b);
    CHECK(reg.Size() == 2, "two distinct UDNs -> two entries");
    Device a2 = a;
    a2.ip = "192.168.1.99"; // DHCP changed
    a2.controlURL = "http://192.168.1.99:63636/AVTransport/ctrl";
    reg.Upsert(a2);
    CHECK(reg.Size() == 2, "re-discovery by UDN dedupes (no IP duplicate)");
    CHECK(reg.Find("uuid:SMG-f57f")->ip == "192.168.1.99", "re-discovery refreshes IP/controlURL");
    CHECK(!reg.Find("192.168.1.24"), "IP is never a registry key");

    // --- DMR truth: AVTransport declares a controllable renderer ---
    CHECK(IsDmr(a), "AVTransport device is a DMR");
    Device dial = MakeDevice("uuid:SONY-7084", "KD-55X9500H DIAL", false);
    CHECK(!IsDmr(dial), "DIAL-only endpoint is not a DMR");

    // --- the 14.9 fix: 501 is reachable, only no-response is offline ---
    ProbeResult playing;
    playing.httpAnswerReceived = true;
    playing.httpStatus = 200;
    playing.transportState = "PLAYING";
    CHECK(IsReachable(playing), "200 with state is reachable");
    CHECK(ProbeDetail(playing) == "PLAYING", "200 detail is transport state");

    ProbeResult fault501;
    fault501.httpAnswerReceived = true;
    fault501.httpStatus = 500;
    fault501.upnpErrorCode = 501;
    CHECK(IsReachable(fault501), "501 SOAP fault is reachable (not offline)");
    CHECK(ProbeDetail(fault501) == "UPnP-error-501", "501 detail names the error code");

    ProbeResult offline;
    offline.httpAnswerReceived = false;
    offline.httpStatus = 0;
    CHECK(!IsReachable(offline), "no HTTP answer is offline");
    CHECK(ProbeDetail(offline) == "no-response", "offline detail is no-response");

    // --- display-name disambiguation on collision ---
    Device sony1 = MakeDevice("uuid:SONY-68553", "SONY KD-55X9500H");
    Device sony2 = MakeDevice("uuid:SONY-a03a7", "SONY KD-55X9500H");
    std::vector<Device> all{sony1, sony2, a};
    CHECK(DisplayName(sony1, all) == "SONY KD-55X9500H (68553)", "collision appends UDN tail");
    CHECK(DisplayName(a, all) == "SMG投屏24", "unique name has no suffix");

    // --- TTL: offline is computed and retained, never dropped ---
    DeviceRegistry ttlReg;
    Device old = MakeDevice("uuid:OLD-11111", "Old TV");
    ttlReg.Upsert(old);
    // Force the entry stale by rewriting lastSeen beyond TTL.
    ttlReg.Find("uuid:OLD-11111")->lastSeen = Clock::now() - kDeviceTtl - std::chrono::seconds(1);
    const auto snap = ttlReg.Snapshot();
    CHECK(snap.size() == 1, "stale device is retained in snapshot");
    CHECK(!snap[0].reachable, "stale device is marked offline in snapshot");

    std::printf("CastDevicePolicy tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
