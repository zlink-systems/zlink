/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"

#include <utility>

namespace zlink::framework::runtime::stateful
{
// The consumer sees the accepted payload only for this call. Canonical bytes
// remain with the turn; reading a dispatch view does not rebuild its metadata.
template <typename Consumer>
auto with_accepted_application (const turn_record_t &turn,
                                Consumer &&consumer,
                                bool capture_flow = true)
{
    if (turn.application_record) {
        const auto &record = *turn.application_record;
        const auto &application = std::visit (
          [] (const auto &body) -> const protocol::application_payload_t & {
              return body.application;
          },
          record.body);
        return std::forward<Consumer> (consumer) (record.kind, application);
    }
    if (turn.frozen_record) {
        const auto &record = *turn.frozen_record;
        if (!record.application)
            throw protocol::service_wire_error_t ("accepted turn has no application payload");
        return std::forward<Consumer> (consumer) (record.kind, *record.application);
    }
    const auto record = protocol::decode_frozen_record (turn.payload, capture_flow);
    if (!record.application)
        throw protocol::service_wire_error_t ("accepted turn has no application payload");
    return std::forward<Consumer> (consumer) (record.kind, *record.application);
}

// Both native ingest and the host's local accepted-message adapter use this
// representation. Source and target authority are supplied by their existing
// admission owner; this conversion makes no routing or authority decision.
inline protocol::frozen_application_record_t
accepted_actor_record (const protocol::actor_message_header_t &header,
                       protocol::request_source_fence_t source,
                       protocol::application_payload_t application,
                       bool request)
{
    constexpr std::uint32_t actor_request_operation_kind = 4;
    protocol::frozen_application_record_t record;
    record.kind = request ? protocol::frozen_record_kind_t::actor_request
                          : protocol::frozen_record_kind_t::actor_send;
    record.source = std::move (source);
    if (header.bound_session_source) {
        record.source_kind = protocol::frozen_source_kind_t::bound_session;
        record.source_actor =
          std::make_pair (header.target.actor_id, header.target.object_generation);
        record.source_session_routing_id = header.bound_session_source->session_routing_id;
        record.source_binding_generation = header.bound_session_source->binding_generation;
        record.source_session_sequence = header.bound_session_source->session_sequence;
    } else {
        record.source_kind = header.source_actor ? protocol::frozen_source_kind_t::actor
                                                 : protocol::frozen_source_kind_t::node;
        record.source_actor = header.source_actor;
    }
    record.message_follow_hop_count = header.message_follow_hop_count;
    record.operation = header.operation;
    record.operation_kind = request ? actor_request_operation_kind : 0;
    record.reply_route_id = header.correlation;
    record.body = protocol::frozen_actor_application_body_t{header.target, std::move (application)};
    return record;
}
inline protocol::frozen_application_record_t
accepted_spot_record (const protocol::spot_message_header_t &header,
                      protocol::request_source_fence_t source,
                      protocol::application_payload_t application,
                      bool request)
{
    constexpr std::uint32_t spot_request_operation_kind = 3;
    protocol::frozen_application_record_t record;
    record.kind = request ? protocol::frozen_record_kind_t::spot_request
                          : protocol::frozen_record_kind_t::spot_send;
    record.source = std::move (source);
    record.source_kind = header.source_spot_id.empty () ? protocol::frozen_source_kind_t::node
                                                        : protocol::frozen_source_kind_t::spot;
    if (!header.source_spot_id.empty ())
        record.source_spot_id = header.source_spot_id;
    record.message_follow_hop_count = header.message_follow_hop_count;
    record.operation = header.operation;
    record.operation_kind = request ? spot_request_operation_kind : 0;
    record.reply_route_id = header.correlation;
    record.body = protocol::frozen_spot_application_body_t{
      header.target, header.target.owner_lease_generation, std::move (application)};
    return record;
}
} // namespace zlink::framework::runtime::stateful
