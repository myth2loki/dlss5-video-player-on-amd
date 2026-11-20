#include "CastControl.h"
#include "CastHttp.h"
#include "Log.h"

#include <string>
#include <string_view>

namespace cast_net {

CastControl::CastControl(std::string controlUrl, std::chrono::milliseconds timeout)
    : m_controlUrl(std::move(controlUrl)), m_timeout(timeout)
{
}

ControlResult CastControl::Post(std::string_view action, const std::string& body)
{
    ControlResult result;
    const cast_http::HttpResult http =
        cast_http::SoapPost(m_controlUrl, cast_soap::SoapAction(action), body, m_timeout);
    if (http.status <= 0) return result; // no HTTP answer -> endpoint offline
    result.httpAnswer = true;
    result.status = http.status;
    result.soapFault = cast_soap::IsFault(http.body);
    result.faultCode = cast_soap::FaultCode(http.body);
    if (result.soapFault) {
        const auto desc = cast_soap::TagValue(http.body, "errorDescription");
        result.detail = desc ? *desc : "UPnP fault";
    } else {
        const auto state = cast_soap::TagValue(http.body, "CurrentTransportState");
        if (state) result.detail = *state;
    }
    return result;
}

ControlResult CastControl::SetUriAndPlay(const std::string& streamUrl, std::string_view mime,
                                         std::string_view title)
{
    if (m_controlUrl.empty()) return {};
    const std::string metadata = cast_soap::DidlMetadata(streamUrl, mime, title);
    const ControlResult setUri = Post("SetAVTransportURI", cast_soap::SetUriBody("0", streamUrl, metadata));
    if (!setUri.httpAnswer || setUri.soapFault) {
        LOG("Cast: SetAVTransportURI failed status=" << setUri.status
            << " fault=" << setUri.faultCode << " detail=" << setUri.detail);
        return setUri;
    }
    const ControlResult play = Post("Play", cast_soap::PlayBody("0"));
    if (!play.httpAnswer || play.soapFault) {
        LOG("Cast: Play failed status=" << play.status
            << " fault=" << play.faultCode << " detail=" << play.detail);
    }
    return play;
}

ControlResult CastControl::Play()
{
    return Post("Play", cast_soap::PlayBody("0"));
}

ControlResult CastControl::Stop()
{
    return Post("Stop", cast_soap::StopBody("0"));
}

CastControl::Transport CastControl::GetTransport()
{
    Transport out;
    out.result = Post("GetTransportInfo", cast_soap::GetTransportInfoBody("0"));
    // Post stored CurrentTransportState in detail on success; a fault has none.
    out.state = out.result.soapFault ? std::string{} : out.result.detail;
    return out;
}

CastControl::Transport CastControl::GetPosition()
{
    Transport out;
    const cast_http::HttpResult http =
        cast_http::SoapPost(m_controlUrl, cast_soap::SoapAction("GetPositionInfo"),
                            cast_soap::GetPositionInfoBody("0"), m_timeout);
    if (http.status <= 0) return out;
    out.result.httpAnswer = true;
    out.result.status = http.status;
    out.result.soapFault = cast_soap::IsFault(http.body);
    out.result.faultCode = cast_soap::FaultCode(http.body);
    const auto state = cast_soap::TagValue(http.body, "TrackDuration");
    if (state) out.state = *state;
    const auto clock = cast_soap::TagValue(http.body, "RelTime");
    if (clock) out.positionSeconds = cast_soap::ParseClock(*clock);
    return out;
}

} // namespace cast_net
