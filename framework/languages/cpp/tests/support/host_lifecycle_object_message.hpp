/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include <zlink/json.hpp>

namespace zlink::framework::test::host_lifecycle
{
struct degraded_object_message_t
{
    static constexpr const char *packet_name = "degraded-object-message";
    int value = 0;
};
}

template <>
struct zlink::json::dto_traits<zlink::framework::test::host_lifecycle::degraded_object_message_t>
{
    static constexpr auto fields = std::tuple{zlink::json::field (
      "value", &zlink::framework::test::host_lifecycle::degraded_object_message_t::value)};
};
