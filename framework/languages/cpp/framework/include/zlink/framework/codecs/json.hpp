/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <nlohmann/json.hpp>

#include <span>

namespace zlink::framework::codecs::json
{

template <typename T> T parse_message (const message_t &message)
{
    const auto *begin = reinterpret_cast<const char *> (message.data ());
    const auto *end = begin ? begin + message.size () : begin;
    return nlohmann::json::parse (begin, end).template get<T> ();
}

template <typename T> message_t make_message (const T &value)
{
    const auto json = nlohmann::json (value);
    const auto text = json.dump ();
    return message_t::from (std::as_bytes (std::span<const char> (text.data (), text.size ())));
}

} // namespace zlink::framework::codecs::json

namespace zlink
{

template <typename T> message_t message_t::from_json (const T &value_)
{
    return framework::codecs::json::make_message (value_);
}

template <typename T> T message_t::parse_json () const
{
    return framework::codecs::json::parse_message<T> (*this);
}

} // namespace zlink
