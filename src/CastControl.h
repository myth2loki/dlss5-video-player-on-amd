#pragma once

// SOAP AVTransport control channel. Each method targets one DMR controlURL and
// issues a single state-changing action. This class does NO device discovery or
// probing — those are CastDiscovery's job — so the "probe must not hijack
// playback" rule stays structural: the side-effecting actions live only here.

#include "CastDevicePolicy.h"
#include "CastSoap.h"

#include <chrono>
#include <optional>
#include <string>

namespace cast_net {

struct ControlResult {
    bool httpAnswer{false};   // did the endpoint answer at all?
    int status{0};
    bool soapFault{false};
    int faultCode{0};
    std::string detail;       // parsed state / error description / transport note
};

class CastControl {
public:
    explicit CastControl(std::string controlUrl,
                         std::chrono::milliseconds timeout = std::chrono::milliseconds{5000});

    // Push a URL to the DMR and start playing. `mime` selects the DIDL
    // protocolInfo (application/x-mpegURL for HLS, which the real SMG/极光TV
    // DMRs accept). SetURI and Play are separate actions, as required by UPnP.
    ControlResult SetUriAndPlay(const std::string& streamUrl, std::string_view mime,
                                std::string_view title = "Live");

    ControlResult Play();
    ControlResult Stop();

    // Current transport state ("PLAYING"/"STOPPED"/...) and playhead seconds.
    // A 501 fault is a valid answer (no active instance) and is reported with
    // soapFault=true rather than as a transport failure.
    struct Transport {
        ControlResult result;
        std::string state;
        std::optional<double> positionSeconds;
    };
    Transport GetTransport();
    Transport GetPosition();

    const std::string& ControlUrl() const { return m_controlUrl; }

private:
    ControlResult Post(std::string_view action, const std::string& body);
    std::string m_controlUrl;
    std::chrono::milliseconds m_timeout;
};

} // namespace cast_net
