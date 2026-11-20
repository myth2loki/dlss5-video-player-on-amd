#pragma once

// Pure SOAP/UPnP message construction and parsing for AVTransport control.
// Every function here is a side-effect-free string transform, so the exact
// bytes sent to a DMR and the interpretation of its answers are unit-testable
// without WinHTTP or a television. The transport (WinHTTP POST) lives in
// CastControl.cpp and only consumes what is built here.
//
// These are the message shapes verified against the real SMG / 极光TV DMRs in
// the POC (doc section 13/14): SOAPACTION must be quoted, the body carries the
// urn:schemas-upnp-org:service:AVTransport:1 namespace, and DIDL metadata uses
// an http-get protocolInfo string. A 501 fault is parsed, not treated as a
// transport error (see IsFault / FaultCode).

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cast_soap {

inline constexpr std::string_view kSoapEnvelope = "http://schemas.xmlsoap.org/soap/envelope/";
inline constexpr std::string_view kAvTransportService = "urn:schemas-upnp-org:service:AVTransport:1";
inline constexpr std::string_view kDidlNamespace = "urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/";

// XML-escape untrusted text before it enters a SOAP body. All five predefined
// entities are handled; without this a title or URL containing '&' produces a
// malformed request the DMR silently rejects.
inline std::string XmlEscape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

// Reverse XmlEscape for text parsed back out of a SOAP answer. Single left-to-
// right pass so "&amp;lt;" decodes to the literal "&lt;" rather than to "<":
// each entity is consumed exactly once.
inline std::string XmlUnescape(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '&') {
            const std::size_t semi = text.find(';', i + 1);
            if (semi != std::string_view::npos && semi - i <= 6) {
                const std::string_view entity = text.substr(i + 1, semi - i - 1);
                const char* decoded = nullptr;
                if (entity == "amp") decoded = "&";
                else if (entity == "lt") decoded = "<";
                else if (entity == "gt") decoded = ">";
                else if (entity == "quot") decoded = "\"";
                else if (entity == "apos") decoded = "'";
                if (decoded) {
                    out.push_back(*decoded);
                    i = semi + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i++]);
    }
    return out;
}

//_the SOAPACTION header value, including the required double quotes.
inline std::string SoapAction(std::string_view action)
{
    return "\"" + std::string(kAvTransportService) + "#" + std::string(action) + "\"";
}

// Build a SOAP request body. `params` are already escaped inner elements, e.g.
// "<InstanceID>0</InstanceID>". Kept as a raw string so each action composes its
// own parameters without this function knowing the AVTransport schema.
inline std::string Envelope(std::string_view action, std::string_view inner)
{
    std::string body;
    body.reserve(160 + inner.size());
    body += "<?xml version=\"1.0\" encoding=\"utf-8\"?>";
    body += "<s:Envelope xmlns:s=\"";
    body += kSoapEnvelope;
    body += "\"><s:Body><u:";
    body += action;
    body += " xmlns:u=\"";
    body += kAvTransportService;
    body += "\">";
    body += inner;
    body += "</u:";
    body += action;
    body += "></s:Body></s:Envelope>";
    return body;
}

inline std::string SetUriBody(std::string_view instanceId, std::string_view uri,
                              std::string_view didlMetadata)
{
    std::string inner;
    inner.reserve(96 + uri.size() + didlMetadata.size());
    inner += "<InstanceID>" + XmlEscape(instanceId) + "</InstanceID>";
    inner += "<CurrentURI>" + XmlEscape(uri) + "</CurrentURI>";
    inner += "<CurrentURIMetaData>" + XmlEscape(didlMetadata) + "</CurrentURIMetaData>";
    return Envelope("SetAVTransportURI", inner);
}

inline std::string PlayBody(std::string_view instanceId)
{
    return Envelope("Play", "<InstanceID>" + std::string(XmlEscape(instanceId)) +
                            "</InstanceID><Speed>1</Speed>");
}

inline std::string StopBody(std::string_view instanceId)
{
    return Envelope("Stop", "<InstanceID>" + std::string(XmlEscape(instanceId)) + "</InstanceID>");
}

inline std::string GetTransportInfoBody(std::string_view instanceId)
{
    return Envelope("GetTransportInfo",
                    "<InstanceID>" + std::string(XmlEscape(instanceId)) + "</InstanceID>");
}

inline std::string GetPositionInfoBody(std::string_view instanceId)
{
    return Envelope("GetPositionInfo",
                    "<InstanceID>" + std::string(XmlEscape(instanceId)) + "</InstanceID>");
}

// DIDL-Lite metadata for a pushed URL. `mime` is e.g.
// "application/x-mpegURL" for HLS, which the real DMRs accept. protocolInfo is
// derived from it; title is escaped so a film name with '&' cannot break XML.
inline std::string DidlMetadata(std::string_view url, std::string_view mime,
                                std::string_view title = "Live")
{
    const std::string protocol = "http-get:*:" + std::string(mime) + ":*";
    std::string out;
    out.reserve(320 + url.size() + title.size() + protocol.size());
    out += "<DIDL-Lite xmlns=\"";
    out += kDidlNamespace;
    out += "\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\"";
    out += " xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\">";
    out += "<item id=\"0\" parentID=\"-1\" restricted=\"1\">";
    out += "<dc:title>" + XmlEscape(title) + "</dc:title>";
    out += "<upnp:class>object.item.videoItem</upnp:class>";
    out += "<res protocolInfo=\"" + XmlEscape(protocol) + "\">" + XmlEscape(url) + "</res>";
    out += "</item></DIDL-Lite>";
    return out;
}

// Find the first occurrence of <tag>...</tag>, optionally namespace-prefixed
// (e.g. "dc:title"). Returns the trimmed inner text, or nullopt when absent.
inline std::optional<std::string> TagValue(std::string_view xml, std::string_view tag)
{
    // Match "<tag>" or "<prefix:tag>" with a single scan; the prefix, if any,
    // appears between '<' and the tag name and contains no '>' or whitespace.
    auto find_open = [&](std::string_view& rest) -> bool {
        std::size_t pos = 0;
        while ((pos = rest.find('<', pos)) != std::string_view::npos) {
            const std::size_t gt = rest.find('>', pos + 1);
            if (gt == std::string_view::npos) return false;
            std::string_view name = rest.substr(pos + 1, gt - pos - 1);
            // Strip an optional namespace prefix.
            const std::size_t colon = name.find(':');
            if (colon != std::string_view::npos) name.remove_prefix(colon + 1);
            if (name == tag) {
                rest.remove_prefix(gt + 1);
                return true;
            }
            pos = gt + 1;
        }
        return false;
    };

    std::string_view rest = xml;
    if (!find_open(rest)) return std::nullopt;
    const std::size_t close = rest.find("</");
    if (close == std::string_view::npos) return std::nullopt;
    const std::size_t gt = rest.find('>', close + 2);
    if (gt == std::string_view::npos) return std::nullopt;
    std::string_view value = rest.substr(0, close);
    // Trim surrounding whitespace.
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return std::string{};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return XmlUnescape(value.substr(first, last - first + 1));
}

inline bool IsFault(std::string_view xml)
{
    return xml.find("<s:Fault") != std::string_view::npos ||
           xml.find("<Fault") != std::string_view::npos;
}

inline int FaultCode(std::string_view xml)
{
    const auto value = TagValue(xml, "errorCode");
    if (!value) return 0;
    int code = 0;
    const char* begin = value->data();
    const char* end = begin + value->size();
    const auto [ptr, ec] = std::from_chars(begin, end, code);
    return ec == std::errc{} && ptr == end ? code : 0;
}

inline std::optional<double> ParseSecondsField(std::string_view field)
{
    // from_chars(double) is not available on every MSVC the project builds with,
    // so parse the whole part as an integer and the fraction digit by digit.
    const std::size_t dot = field.find('.');
    const std::size_t wholeEnd = dot == std::string_view::npos ? field.size() : dot;
    long long whole = 0;
    const auto [ptr, ec] = std::from_chars(field.data(), field.data() + wholeEnd, whole);
    if (ec != std::errc{} || ptr != field.data() + wholeEnd) return std::nullopt;
    double value = static_cast<double>(whole);
    if (dot != std::string_view::npos) {
        double frac = 0.0, scale = 0.1;
        for (std::size_t i = dot + 1; i < field.size(); ++i) {
            if (field[i] < '0' || field[i] > '9') return std::nullopt;
            frac += (field[i] - '0') * scale;
            scale *= 0.1;
        }
        value += frac;
    }
    return value;
}

inline std::optional<long long> ParseIntField(std::string_view field)
{
    long long value = 0;
    const auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (ec != std::errc{} || ptr != field.data() + field.size()) return std::nullopt;
    return value;
}

// UPnP clock "H:MM:SS(.frac)" -> seconds. Returns nullopt on malformed input.
// Accepts both "0:01:23.5" and the two-field "01:23.5" some DMRs emit.
inline std::optional<double> ParseClock(std::string_view value)
{
    if (value.empty()) return std::nullopt;
    const std::size_t c1 = value.find(':');
    if (c1 == std::string_view::npos) return std::nullopt;
    const std::size_t c2 = value.find(':', c1 + 1);
    if (c2 == std::string_view::npos) {
        // Two fields: MM:SS(.frac).
        const auto minutes = ParseIntField(value.substr(0, c1));
        const auto seconds = ParseSecondsField(value.substr(c1 + 1));
        if (!minutes || !seconds) return std::nullopt;
        return *minutes * 60.0 + *seconds;
    }
    // Three fields: H:MM:SS(.frac).
    const auto hours = ParseIntField(value.substr(0, c1));
    const auto minutes = ParseIntField(value.substr(c1 + 1, c2 - c1 - 1));
    const auto seconds = ParseSecondsField(value.substr(c2 + 1));
    if (!hours || !minutes || !seconds) return std::nullopt;
    return *hours * 3600.0 + *minutes * 60.0 + *seconds;
}

} // namespace cast_soap
