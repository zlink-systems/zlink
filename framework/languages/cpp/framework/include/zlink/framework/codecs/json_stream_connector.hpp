/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/json_profile.hpp>
#include <zlink/stream_connector/contracts/connector.hpp>
#include <zlink/stream_connector/contracts/stream_payload.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace zlink::stream_connector::codecs
{

template <typename T> struct codec_traits
{
    static constexpr codec_t codec = codec_t::json;
    static constexpr bool default_json_profile = true;

    static bool try_decode_message_pack (const std::vector<std::uint8_t> &payload,
                                         T &value,
                                         std::string *error = nullptr)
    {
        const auto json = nlohmann::json::from_msgpack (payload, true, false);
        if (json.is_discarded ())
            return zlink::detail::json_profile::fail (error, "Invalid MessagePack payload");
        return zlink::detail::json_profile::from_value (json, value, error);
    }

#if ZLINK_HAS_EXCEPTIONS

    static std::vector<std::uint8_t> encode (const T &value)
    {
        const auto text = zlink::detail::json_profile::encode (value);
        return {text.begin (), text.end ()};
    }

    static T decode (const std::vector<std::uint8_t> &payload)
    {
        return zlink::detail::json_profile::decode<T> (payload.begin (), payload.end ());
    }

    static T decode_message_pack (const std::vector<std::uint8_t> &payload)
    {
        T value{};
        std::string error;
        if (!try_decode_message_pack (payload, value, &error))
            throw std::invalid_argument (error);
        return value;
    }
#endif
};

template <typename T>
void decode_payload (codec_t codec, const std::vector<std::uint8_t> &payload, T &value)
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
            value = codec_traits<T>::decode_message_pack (payload);
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

/* Returns the registration handle (stream-connector §7): the caller keeps it
 * for as long as the handler must run. */
template <typename T>
[[nodiscard]] subscription_t
on (connector_t &connector, std::string packet_name, std::function<void (const T &)> callback)
{
    return connector.on<packet_t> (std::move (packet_name), [callback = std::move (callback)] (
                                                              const message_t<packet_t> &message) {
        T value{};
        decode_payload (message.payload.codec, message.payload.payload, value);
        callback (std::move (value));
    });
}

template <typename T>
[[nodiscard]] subscription_t on (connector_t &connector, std::function<void (const T &)> callback)
{
    return on<T> (connector, detail::message_packet_name<T> (), std::move (callback));
}

} // namespace zlink::stream_connector::codecs

namespace zlink::stream_connector
{

template <typename T>
    requires (!requires { codecs::codec_traits<T>::default_json_profile; })
std::vector<std::uint8_t> to_stream_payload (const T &value)
{
    return codecs::codec_traits<T>::encode (value);
}

template <typename T>
    requires requires { codecs::codec_traits<T>::default_json_profile; }
bool from_stream_payload (codec_t codec, const std::vector<std::uint8_t> &payload, T &value)
{
    if (codec == codec_t::raw)
        return true;
    if (codec == codec_t::message_pack)
        return codecs::codec_traits<T>::try_decode_message_pack (payload, value);
    auto decoded = zlink::detail::json_profile::try_decode<T> (payload.begin (), payload.end ());
    if (!decoded)
        return false;
    value = std::move (*decoded);
    return true;
}

template <typename T>
    requires (!requires { codecs::codec_traits<T>::default_json_profile; })
void from_stream_payload (codec_t codec, const std::vector<std::uint8_t> &payload, T &value)
{
    codecs::decode_payload (codec, payload, value);
}

template <typename T>
    requires (!requires { codecs::codec_traits<T>::default_json_profile; })
void from_stream_payload (const std::vector<std::uint8_t> &payload, T &value)
{
    value = codecs::codec_traits<T>::decode (payload);
}

} // namespace zlink::stream_connector
