/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "actor_gateway_fixture.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include <utility>

namespace zlink::framework::test::actor_gateway
{
zlink::framework::actor_ref_t test_actor_ref (std::string node,
                                              std::string actor_type,
                                              std::string actor_id,
                                              std::uint64_t generation)
{
    return zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string (std::move (node)), std::move (actor_type),
      std::move (actor_id), generation);
}

std::shared_ptr<zlink::framework::runtime::serial_application_record_t> accepted_actor_fifo_record (
  const zlink::framework::runtime::protocol::actor_route_fence_t &source_fence,
  std::uint64_t sequence,
  const zlink::framework::runtime::messaging::envelope_header_t &envelope,
  std::string payload)
{
    namespace runtime = zlink::framework::runtime;
    const bool request = envelope.kind == runtime::messaging::message_kind_t::request;
    auto parts =
      zlink::framework::detail::encode_actor_packet (envelope, zlink::message_t::from (payload));
    runtime::protocol::actor_message_header_t header;
    header.operation = {1, sequence};
    header.target = source_fence;
    if (request)
        header.correlation = sequence;
    auto record = std::make_shared<runtime::serial_application_record_t> ();
    record->payload ().emplace ();
    record->payload ()->sequence = sequence;
    record->payload ()->application_payload_bytes = payload.size ();
    record->payload ()->application_record = runtime::stateful::accepted_actor_record (
      header,
      {"fence-source-owner", 1, source_fence.target_node_routing_id,
       source_fence.target_node_generation},
      runtime::protocol::application_payload_t::from_parts (std::move (parts).take_items ()),
      request);
    return record;
}
} // namespace zlink::framework::test::actor_gateway
