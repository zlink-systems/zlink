/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/stream_connector/contracts/zlink_stream_enums.hpp>

#include <concepts>
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

template <typename T> std::string message_packet_name ()
{
    if constexpr (static_packet_name<T>) {
        return T::packet_name;
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

template <typename TMessage> zlink::message_t to_packet_payload (const TMessage &message, ...)
{
    return zlink::message_t::from_json (message);
}

template <typename TMessage>
auto apply_packet_payload (TMessage &message,
                           const zlink::message_t &payload,
                           int) -> decltype (from_stream_payload (payload, message), void ());

template <typename TMessage> void apply_packet_payload (TMessage &, const zlink::message_t &, ...);

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
