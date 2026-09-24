#include "EngineLobbyProtocol.h"
#include "ZLinkStreamConnector.h"

#include <zlink/stream_connector.hpp>

#include <string>
#include <type_traits>

static_assert (std::is_default_constructible_v<FZLinkStreamPacket>);

void ValidateEngineLobbyConnectorSurface ()
{
    zlink::stream_connector::connector_options_t Options;
    Options.endpoint = "ws://127.0.0.1:22700";
    Options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;

    zlink::stream_connector::packet_t Packet;
    Packet.name = engine_lobby::packet::join_req;
    Packet.codec = zlink::stream_connector::codec_t::json;
    const std::string Payload = R"({"name":"unreal-player"})";
    Packet.payload.assign (Payload.begin (), Payload.end ());

    UZLinkStreamConnector *UnrealConnector = nullptr;
    (void) Options;
    (void) Packet;
    (void) UnrealConnector;
}
