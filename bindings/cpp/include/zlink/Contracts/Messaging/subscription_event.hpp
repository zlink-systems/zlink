/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "../Core/routing_id.hpp"

#include <optional>
#include <string>

namespace zlink
{

/// @brief A subscriber's subscribe or unsubscribe event as observed by an XPUB socket.
struct subscription_event_t
{
    subscription_event_t () : routing_id (std::nullopt), topic (), subscribed (false) {}

    std::optional<routing_id_t> routing_id;
    std::string topic;
    bool subscribed;
};

/// @brief A subscription filter entry (topic string and whether it is a pattern).
struct subscription_filter_t
{
    std::string filter;
    bool is_pattern = false;
};

} // namespace zlink
