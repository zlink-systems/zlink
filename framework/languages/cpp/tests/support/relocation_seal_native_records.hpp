/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace framework_relocation_seal_fixture
{
inline zlink::framework::runtime::mesh::service_mailbox_record_t
spot_send (zlink::framework::runtime::host::public_host_runtime_t &host,
           const zlink::framework::runtime::stateful::object_ref_t &spot,
           std::uint64_t operation,
           std::uint8_t source,
           std::optional<std::uint64_t> correlation = std::nullopt)
{
    auto parts = zlink::framework::detail::backend::materialize_binding_parts (
      {zlink::framework::runtime::protocol::encode_spot_message_header (
         correlation ? zlink::framework::runtime::protocol::command::spotRequest
                     : zlink::framework::runtime::protocol::command::spotSend,
         "source-spot",
         zlink::framework::runtime::protocol::spot_route_fence_t{
           spot.key, spot.object_generation, {1}, 1, 1, 3},
         {1, operation}, correlation),
       zlink::framework::runtime::protocol::encode_application_payload (
         {"Probe", "application/json", {'{', '}'}})});
    auto source_routing_id = std::vector<std::uint8_t>{source};
    auto record = zlink::framework::runtime::mesh::service_mailbox_record_t::from_parts (
      zlink::framework::runtime::mesh::service_mailbox_t::application_owner (
        zlink::framework::runtime::host::owner_kind_t::spot, spot.key),
      zlink::framework::runtime::mesh::service_mailbox_domain_t::application, std::move (parts),
      std::move (source_routing_id), correlation, 1);
    record.source_original =
      zlink::framework::runtime::mesh::raw_mesh_node_owner_t::local_accepted_source (
        record.source_routing_id, record.source_node_generation, {});
    return record;
}

inline zlink::framework::runtime::mesh::service_mailbox_record_t
native_spot_record (zlink::framework::runtime::host::public_host_runtime_t &host,
                    const zlink::framework::runtime::stateful::object_ref_t &spot,
                    std::uint64_t operation,
                    std::optional<std::uint64_t> correlation)
{
    auto wire = spot_send (host, spot, operation, 2, correlation);
    zlink::framework::runtime::messaging::envelope_header_t envelope;
    envelope.kind = correlation ? zlink::framework::runtime::messaging::message_kind_t::request
                                : zlink::framework::runtime::messaging::message_kind_t::command;
    envelope.channel_name = "mesh";
    envelope.message_name = "NativeProbe";
    std::vector<zlink::message_t> parts;
    parts.push_back (
      zlink::framework::runtime::messaging::envelope_codec_t{}.encode_header (envelope));
    parts.push_back (zlink::message_t::from ("{}"));
    wire.body_parts ().back () = zlink::framework::detail::backend::materialize_binding_part (
      zlink::framework::runtime::protocol::encode_application_payload (
        zlink::framework::runtime::protocol::application_payload_t::from_parts (parts)));
    return wire;
}

inline std::uint64_t operation_of (const zlink::framework::runtime::stateful::turn_record_t &turn)
{
    if (turn.frozen_record)
        return turn.frozen_record->operation.low;
    return turn.application_record ? turn.application_record->operation.low : 0;
}

inline std::vector<std::uint64_t>
operations_of (const std::vector<zlink::framework::runtime::stateful::turn_record_t> &turns)
{
    std::vector<std::uint64_t> operations;
    for (const auto &turn : turns)
        operations.push_back (operation_of (turn));
    return operations;
}
} // namespace framework_relocation_seal_fixture
