/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/messaging/envelope_codec.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>

#include <cstdint>
#include <optional>

namespace zlink::framework::detail
{

struct route_received_packet_t
{
    zlink::routing_id_t source_node_rid;
    std::optional<std::uint64_t> request_seq;
    runtime::messaging::message_parts_t parts;
    std::optional<zlink::routing_id_t> source_session_rid;
};

struct route_dispatch_reply_t
{
    zlink::routing_id_t target_node_rid;
    std::optional<std::uint64_t> request_seq;
    runtime::messaging::message_parts_t parts;
};

} // namespace zlink::framework::detail
