/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/messaging/envelope_codec.hpp"
#include <zlink/json.hpp>
#include <tuple>
#include <utility>
#include <vector>

namespace framework_channel_messaging_fixture
{
struct missing_probe_request_t
{
    static constexpr const char *packet_name = "missing.request";
    int value{};
};
} // namespace framework_channel_messaging_fixture

template <>
struct zlink::json::dto_traits<framework_channel_messaging_fixture::missing_probe_request_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &framework_channel_messaging_fixture::missing_probe_request_t::value)};
};

namespace framework_channel_messaging_fixture
{
inline zlink::framework::runtime::messaging::message_parts_t
copy_message_parts (const std::vector<zlink::message_t> &parts)
{
    std::vector<zlink::message_t> copied;
    copied.reserve (parts.size ());
    for (const auto &part : parts) {
        copied.push_back (zlink::message_t::from (part.to_string ()));
    }
    return zlink::framework::runtime::messaging::message_parts_t (std::move (copied));
}

inline zlink::framework::task_t<std::vector<zlink::message_t>>
await_native_reply (zlink::async_result_t<std::vector<zlink::message_t>> pending)
{
    co_return co_await std::move (pending);
}
} // namespace framework_channel_messaging_fixture
