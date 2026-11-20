// Standalone assertions for CastSoap.h — pure string transforms, no network.
#include "CastSoap.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace cast_soap;

namespace {
int g_pass = 0;
int g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { ++g_pass; } \
    else { ++g_fail; std::printf("FAIL: %s (line %d)\n", name, __LINE__); } \
} while (0)

bool Contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}
} // namespace

int main()
{
    // --- XML escaping ---
    CHECK(XmlEscape("a&b<c>d\"e'f") == "a&amp;b&lt;c&gt;d&quot;e&apos;f", "escape all five entities");

    // --- SOAPACTION is quoted with the AVTransport URN ---
    CHECK(SoapAction("Play") == "\"urn:schemas-upnp-org:service:AVTransport:1#Play\"",
          "SOAPACTION quoted with URN");

    // --- SetAVTransportURI body shape ---
    const std::string didl = DidlMetadata("http://192.168.1.31:8080/live.m3u8",
                                          "application/x-mpegURL", "Film & Cast");
    CHECK(Contains(didl, "http-get:*:application/x-mpegURL:*"), "DIDL protocolInfo from mime");
    CHECK(Contains(didl, "Film &amp; Cast"), "DIDL title escaped");
    CHECK(Contains(didl, "<upnp:class>object.item.videoItem</upnp:class>"), "DIDL video class");

    const std::string uri = "http://192.168.1.31:8080/live.m3u8";
    const std::string setUri = SetUriBody("0", uri, didl);
    CHECK(Contains(setUri, "<u:SetAVTransportURI xmlns:u=\""), "SetURI action element");
    CHECK(Contains(setUri, "<CurrentURI>http://192.168.1.31:8080/live.m3u8</CurrentURI>"), "SetURI current URI");
    CHECK(Contains(setUri, "&lt;DIDL-Lite"), "SetURI metadata double-escaped correctly");

    CHECK(Contains(PlayBody("0"), "<Speed>1</Speed>"), "Play body has speed 1");
    CHECK(Contains(StopBody("0"), "<u:Stop"), "Stop body");
    CHECK(Contains(GetTransportInfoBody("0"), "<u:GetTransportInfo"), "GetTransportInfo body");

    // --- parsing answers ---
    const std::string ok =
        "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
        "<u:GetTransportInfoResponse xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        "<CurrentTransportState>PLAYING</CurrentTransportState>"
        "</u:GetTransportInfoResponse></s:Body></s:Envelope>";
    const auto state = TagValue(ok, "CurrentTransportState");
    CHECK(state && *state == "PLAYING", "parse transport state");

    const std::string fault =
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><s:Fault>"
        "<faultcode>s:Client</faultcode><detail><UPnPError>"
        "<errorCode>501</errorCode><errorDescription>Action Failed</errorDescription>"
        "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    CHECK(IsFault(fault), "detect SOAP fault");
    CHECK(FaultCode(fault) == 501, "parse 501 fault code");
    CHECK(FaultCode(ok) == 0, "no fault code on success");

    // --- clock parsing ---
    const auto t1 = ParseClock("0:01:23.5");
    CHECK(t1 && std::abs(*t1 - 83.5) < 1e-6, "parse H:MM:SS.frac");
    const auto t2 = ParseClock("01:23.25");
    CHECK(t2 && std::abs(*t2 - 83.25) < 1e-6, "parse MM:SS.frac");
    const auto t3 = ParseClock("1:02:03");
    CHECK(t3 && std::abs(*t3 - 3723.0) < 1e-6, "parse H:MM:SS");
    CHECK(!ParseClock("garbage"), "reject malformed clock");
    CHECK(!ParseClock("0:aa:00"), "reject non-numeric clock");

    // --- namespaced tag lookup (dc:title) ---
    const auto title = TagValue(didl, "title");
    CHECK(title && *title == "Film & Cast", "namespaced tag lookup strips prefix");

    std::printf("CastSoap tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
