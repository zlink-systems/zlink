/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>

#include <concepts>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>

namespace zlink::stream_connector::detail
{

template <typename T> concept static_packet_name = requires
{
    {
        T::packet_name
    } -> std::convertible_to<const char *>;
};

template <typename T> concept protobuf_descriptor_name = requires
{
    {
        std::string (T::descriptor ()->name ())
    } -> std::same_as<std::string>;
};

template <typename T> std::string message_packet_name ()
{
    if constexpr (static_packet_name<T>) {
        return T::packet_name;
    } else if constexpr (protobuf_descriptor_name<T>) {
        return std::string (T::descriptor ()->name ());
    } else {
        return typeid (T).name ();
    }
}

template <typename TMessage>
auto to_packet_payload (const TMessage &message, int) -> decltype (to_stream_payload (message),
                                                                   zlink::message_t{})
{
    auto payload = to_stream_payload (message);
    if constexpr (std::is_same_v<decltype (payload), zlink::message_t>) {
        return payload;
    } else {
        return zlink::message_t::from (std::move (payload));
    }
}

template <typename TMessage> requires requires (const TMessage &message, std::string *bytes)
{
    {
        message.SerializeToString (bytes)
    } -> std::same_as<bool>;
}
zlink::message_t to_packet_payload (const TMessage &message, long)
{
    std::string bytes;
    if (!message.SerializeToString (&bytes)) {
        throw std::runtime_error ("typed protobuf payload serialization failed");
    }
    return zlink::message_t::from (bytes);
}

template <typename TMessage> zlink::message_t to_packet_payload (const TMessage &message, ...)
{
    return zlink::message_t::from_json (message);
}

template <typename TMessage>
auto apply_packet_payload (TMessage &message,
                           const zlink::message_t &payload,
                           int) -> decltype (from_stream_payload (payload, message), void ());

template <typename TMessage> void apply_packet_payload (TMessage &, const zlink::message_t &, ...);

template <typename TMessage> requires requires (TMessage &message, const std::string &bytes)
{
    {
        message.ParseFromString (bytes)
    } -> std::same_as<bool>;
}
void apply_packet_payload (TMessage &message, const zlink::message_t &payload, long)
{
    if (!message.ParseFromString (payload.to_string ())) {
        throw std::runtime_error ("typed protobuf payload parse failed");
    }
}

template <typename TMessage>
auto apply_packet_payload (TMessage &message,
                           zlink::stream_connector::codec_t codec,
                           const zlink::message_t &payload,
                           int) -> decltype (from_stream_payload (codec, payload, message), void ())
{
    from_stream_payload (codec, payload, message);
}

template <typename TMessage>
void apply_packet_payload (TMessage &message,
                           zlink::stream_connector::codec_t,
                           const zlink::message_t &payload,
                           ...)
{
    apply_packet_payload (message, payload, 0);
}

template <typename TMessage>
auto apply_packet_payload (TMessage &message,
                           const zlink::message_t &payload,
                           int) -> decltype (from_stream_payload (payload, message), void ())
{
    from_stream_payload (payload, message);
}

template <typename TMessage> void apply_packet_payload (TMessage &, const zlink::message_t &, ...)
{
}

} // namespace zlink::stream_connector::detail
