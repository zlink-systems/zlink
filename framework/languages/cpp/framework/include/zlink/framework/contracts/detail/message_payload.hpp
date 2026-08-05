/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/Contracts/Messaging/message.hpp>

#include <string>
#include <type_traits>
#include <utility>

namespace zlink::framework::detail
{

template <typename TMessage>
auto to_message_payload (const TMessage &message, int) -> decltype (to_stream_payload (message),
                                                                    zlink::message_t{})
{
    auto payload = to_stream_payload (message);
    if constexpr (std::is_same_v<decltype (payload), zlink::message_t>) {
        return payload;
    } else {
        return zlink::message_t::from (std::move (payload));
    }
}

template <typename TMessage> zlink::message_t to_message_payload (const TMessage &, long)
{
    return zlink::message_t::from (std::string ("{}"));
}

} // namespace zlink::framework::detail
