/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <zlink/json.hpp>
#include <tuple>

namespace framework_channel_messaging_fixture
{
struct request_t
{
    static constexpr const char *packet_name = "profile.lookup";
    int value{};
};
} // namespace framework_channel_messaging_fixture

template <> struct zlink::json::dto_traits<framework_channel_messaging_fixture::request_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("value", &framework_channel_messaging_fixture::request_t::value)};
};

namespace framework_channel_messaging_fixture
{
struct reply_t
{
    int value{};
};
} // namespace framework_channel_messaging_fixture

template <> struct zlink::json::dto_traits<framework_channel_messaging_fixture::reply_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("value", &framework_channel_messaging_fixture::reply_t::value)};
};

namespace framework_channel_messaging_fixture
{
struct event_t
{
    static constexpr const char *packet_name = "profile.changed.event";
    int value{};
};
} // namespace framework_channel_messaging_fixture

template <> struct zlink::json::dto_traits<framework_channel_messaging_fixture::event_t>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("value", &framework_channel_messaging_fixture::event_t::value)};
};
