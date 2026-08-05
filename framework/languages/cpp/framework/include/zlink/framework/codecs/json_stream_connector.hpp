/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/codecs/json.hpp>
#include <zlink/stream_connector/contracts/connector.hpp>
#include <zlink/stream_connector/contracts/stream_payload.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <utility>

namespace zlink::stream_connector::codecs
{

template <typename T> struct codec_traits
{
    static constexpr codec_t codec = codec_t::json;

    static zlink::message_t encode (const T &value) { return zlink::message_t::from_json (value); }

    static T decode (const zlink::message_t &message) { return message.parse_json<T> (); }
};

template <typename T> void decode_payload (codec_t codec, const zlink::message_t &payload, T &value)
{
    switch (codec) {
        case codec_t::json:
            value = codec_traits<T>::decode (payload);
            return;
        case codec_t::protobuf:
            value = codec_traits<T>::decode (payload);
            return;
        case codec_t::raw:
            return;
        case codec_t::message_pack:
            value = nlohmann::json::from_msgpack (payload.to_bytes ()).template get<T> ();
            return;
    }
    value = codec_traits<T>::decode (payload);
}

template <typename T> packet_t encode_packet (const T &value)
{
    packet_t packet;
    packet.name = detail::message_packet_name<T> ();
    packet.codec = codec_traits<T>::codec;
    packet.payload = codec_traits<T>::encode (value);
    return packet;
}

template <typename T>
connector_t &
on (connector_t &connector, std::string packet_name, std::function<void (const T &)> callback)
{
    return connector.on<packet_t> (std::move (packet_name),
                                   [callback = std::move (callback)] (const packet_t &packet) {
                                       T value{};
                                       decode_payload (packet.codec, packet.payload, value);
                                       callback (std::move (value));
                                   });
}

template <typename T>
connector_t &on (connector_t &connector, std::function<void (const T &)> callback)
{
    return on<T> (connector, detail::message_packet_name<T> (), std::move (callback));
}

} // namespace zlink::stream_connector::codecs

namespace zlink::stream_connector
{

template <typename T> zlink::message_t to_stream_payload (const T &value)
{
    return codecs::codec_traits<T>::encode (value);
}

template <typename T>
void from_stream_payload (codec_t codec, const zlink::message_t &payload, T &value)
{
    codecs::decode_payload (codec, payload, value);
}

template <typename T> void from_stream_payload (const zlink::message_t &payload, T &value)
{
    value = codecs::codec_traits<T>::decode (payload);
}

} // namespace zlink::stream_connector
