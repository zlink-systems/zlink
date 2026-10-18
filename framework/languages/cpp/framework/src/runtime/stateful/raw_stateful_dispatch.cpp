/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <tuple>
#include <utility>

namespace zlink::framework::runtime::stateful
{

namespace
{

using authority_result_t = std::pair<stateful_error_t, std::optional<accepted_record_authority_t>>;

constexpr std::size_t mailbox_claim_byte_limit = 16u * 1024u * 1024u;


class mailbox_claim_release_guard_t final
{
  public:
    mailbox_claim_release_guard_t (mesh::raw_mesh_node_owner_t &transport,
                                   accepted_mailbox_reservation_t claim) :
        _transport (&transport), _claim (std::move (claim))
    {
    }

    ~mailbox_claim_release_guard_t () noexcept
    {
        if (!_transport)
            return;
        std::visit (
          [this] (auto &claim) {
              using claim_t = std::decay_t<decltype (claim)>;
              if constexpr (std::is_same_v<claim_t,
                                           std::shared_ptr<mesh::service_mailbox_claim_t>>) {
                  if (claim)
                      (void) _transport->mailbox ().release (*claim);
              } else if (claim) {
                  claim ();
              }
          },
          _claim);
    }

    mailbox_claim_release_guard_t (const mailbox_claim_release_guard_t &) = delete;
    mailbox_claim_release_guard_t &operator= (const mailbox_claim_release_guard_t &) = delete;

    void dismiss () noexcept
    {
        _transport = nullptr;
        _claim = {};
    }

  private:
    mesh::raw_mesh_node_owner_t *_transport;
    accepted_mailbox_reservation_t _claim;
};

class application_claim_release_guard_t final
{
  public:
    explicit application_claim_release_guard_t (
      const std::function<stateful_error_t ()> &release) noexcept :
        _release (&release)
    {
    }
    ~application_claim_release_guard_t () noexcept
    {
        if (_release)
            (void) release ();
    }
    application_claim_release_guard_t (const application_claim_release_guard_t &) = delete;
    application_claim_release_guard_t &
    operator= (const application_claim_release_guard_t &) = delete;

    stateful_error_t release ()
    {
        const auto callback = std::exchange (_release, nullptr);
        return callback ? (*callback) () : stateful_error_t::none;
    }

  private:
    const std::function<stateful_error_t ()> *_release;
};

void reply_failure_then_release_claim (mesh::raw_mesh_node_owner_t &transport,
                                       mesh::service_mailbox_record_t request,
                                       std::uint32_t terminal_result,
                                       std::uint32_t failure_code,
                                       accepted_mailbox_reservation_t claim)
{
    mailbox_claim_release_guard_t release (transport, std::move (claim));
    try {
        (void) transport.reply_failure (request, terminal_result, failure_code);
    }
    catch (...) {
    }
}

bool nonzero (const protocol::wire_operation_id_t &value) noexcept
{
    return value.high != 0 || value.low != 0;
}

bool nonzero (const protocol::relocation_id_t &value) noexcept
{
    return value.high != 0 || value.low != 0;
}

bool valid_source (const protocol::request_source_fence_t &value) noexcept
{
    return !value.owner_id.empty () && value.lease_generation != 0
           && !value.node_routing_id.empty () && value.node_generation != 0;
}

bool valid_coordinator (const protocol::relocation_coordinator_fence_t &value) noexcept
{
    return !value.owner_id.empty () && value.lease_generation != 0
           && !value.node_routing_id.empty () && value.node_generation != 0
           && !value.expected_authority_store_version.empty ();
}

bool valid_relocation_object (const protocol::relocation_object_t &value) noexcept
{
    if (value.object_id.empty () || value.object_generation == 0)
        return false;
    switch (value.kind) {
        case protocol::relocation_object_kind_t::actor:
        case protocol::relocation_object_kind_t::user_spot:
            return value.expected_authority_owner_generation != 0;
        case protocol::relocation_object_kind_t::instance_spot:
            return !value.stable_type.empty () && value.expected_authority_owner_generation == 0;
    }
    return false;
}

protocol::relocation_object_t
canonical_relocation_object_identity (protocol::relocation_object_t value)
{
    if (value.kind != protocol::relocation_object_kind_t::instance_spot)
        value.stable_type.clear ();
    return value;
}

std::array<std::byte, 32> digest_bytes (const std::vector<std::uint8_t> &bytes)
{
    std::vector<std::byte> public_bytes;
    public_bytes.reserve (bytes.size ());
    for (const auto value : bytes)
        public_bytes.push_back (static_cast<std::byte> (value));
    return runtime::sha256 (public_bytes);
}

std::size_t retained_bytes (const raw_relocation_terminal_target_registration_t &registration)
{
    auto total = protocol::encode_reply_relay (registration.relay).size ();
    if (registration.application_reply)
        total += protocol::encode_application_payload (*registration.application_reply).size ();
    total += registration.request_source.owner_id.size ()
             + registration.request_source.node_routing_id.size ()
             + registration.relay_destination_node_routing_id.size ();
    return total;
}

bool frozen_matches_registration (const protocol::frozen_record_t &frozen,
                                  const raw_relocation_target_registration_t &registration)
{
    if (!frozen.target)
        return false;
    const auto &target = *frozen.target;
    return target.kind == registration.object.kind
           && target.object_id == registration.object.object_id
           && target.object_generation == registration.object.object_generation
           && target.authority_owner_generation
                == registration.object.expected_authority_owner_generation;
}

}

accepted_record_authority_resolver_t
make_location_store_authority_resolver (zlink::framework::location_repository_t &store)
{
    return [&store] (accepted_record_authority_query_t query)
             -> task_t<std::optional<accepted_record_authority_t>> {
        try {
            const auto authority = co_await store.read_authority (
              query.target.kind == object_kind_t::actor ? actor_authority_key (query.target.key)
                                                        : spot_authority_key (query.target.key));
            const auto *snapshot = std::get_if<authority_snapshot_t> (&authority);
            const auto target_rid = node_rid_t::from_string (
              zlink::routing_id_t::from (query.target_node_routing_id).to_string ());
            // A Spot wire address covers both User and Instance Spots. The
            // native facade is a mailbox owner; Store owns the placement kind.
            const bool matches_kind =
              snapshot
              && (query.target.kind == object_kind_t::actor
                    ? snapshot->allocation.object_kind == placement_object_kind_t::actor
                    : snapshot->allocation.object_kind == placement_object_kind_t::user_spot
                        || snapshot->allocation.object_kind
                             == placement_object_kind_t::instance_spot);
            if (!matches_kind || snapshot->object_generation != query.target.object_generation
                || snapshot->authority_owner_generation != query.target.authority_owner_generation
                || snapshot->allocation.target.mesh_name != query.target.mesh_name
                || snapshot->allocation.target.node_rid.value () != target_rid.value ()
                || snapshot->allocation.target.node_lifecycle_generation
                     != query.target_node_generation) {
                co_return std::nullopt;
            }

            const auto nodes = co_await store.list_mesh_nodes (query.target.mesh_name);
            const auto source_rid = zlink::routing_id_t::from (query.source_node_routing_id);
            const auto source = std::find_if (
              nodes.items.begin (), nodes.items.end (), [&] (const mesh_node_descriptor_t &node) {
                  return node.rid == source_rid
                         && node.lifecycle_generation == query.source_node_generation;
              });
            if (source == nodes.items.end () || source->owner_id.empty ()
                || source->lease_generation <= 0) {
                co_return std::nullopt;
            }
            const auto lease = co_await store.read_owner_lease (source->owner_id);
            const auto *live = std::get_if<owner_lease_found_t> (&lease);
            if (!live || live->token.lease_generation != source->lease_generation
                || live->lease_expires_at <= live->store_now) {
                co_return std::nullopt;
            }
            co_return accepted_record_authority_t{
              {source->owner_id, static_cast<std::uint64_t> (source->lease_generation),
               query.source_node_routing_id, query.source_node_generation},
              static_cast<std::uint64_t> (snapshot->allocation.target.owner.lease_generation)};
        }
        catch (...) {
            co_return std::nullopt;
        }
    };
}

raw_stateful_dispatch_t::raw_stateful_dispatch_t (
  stateful_object_runtime_t &objects,
  mesh::raw_mesh_node_owner_t &transport,
  accepted_record_authority_resolver_t authority_resolver) :
    _objects (&objects),
    _transport (&transport),
    _authority_resolver (std::move (authority_resolver))
{
}

stateful_error_t raw_stateful_dispatch_t::commit_accepted_ingress (const object_ref_t &owner,
                                                                   turn_record_t turn,
                                                                   pending_delivery_t pending,
                                                                   bool allocate_sequence,
                                                                   stateful_error_t collision_error,
                                                                   bool restored)
{
    const auto owner_key = delivery_key (owner, 0);
    const auto sequence_key = std::pair{owner.kind, owner.key};
    std::unique_lock lock (_mutex);

    const auto discarding = _discarding_owners.find (owner_key);
    if (discarding != _discarding_owners.end () && discarding->second == owner) {
        return collision_error;
    }

    std::map<std::pair<object_kind_t, std::string>, std::uint64_t>::iterator next_sequence;
    bool inserted_sequence = false;
    if (allocate_sequence) {
        try {
            const auto inserted = _next_sequence.try_emplace (sequence_key, std::uint64_t{1});
            next_sequence = inserted.first;
            inserted_sequence = inserted.second;
        }
        catch (...) {
            return stateful_error_t::backpressured;
        }
        if (next_sequence->second == 0
            || next_sequence->second == std::numeric_limits<std::uint64_t>::max ()) {
            if (inserted_sequence)
                _next_sequence.erase (next_sequence);
            return stateful_error_t::conflict;
        }
        turn.sequence = next_sequence->second;
    }
    if (turn.sequence == 0) {
        if (inserted_sequence)
            _next_sequence.erase (next_sequence);
        return stateful_error_t::invalid;
    }

    detail::message_flow_tracer_t tracer (_transport->dispatch_options ());
    auto admitted_application = tracer.enabled_for (message_flow_outcome_t::admitted)
                                  ? std::move (pending.frozen.application)
                                  : std::optional<protocol::application_payload_t>{};
    const auto admitted_kind = pending.frozen.kind;
    const auto admitted_operation = pending.frozen.operation;
    // Pending owns terminal identity and reply routing; the native turn owns body bytes.
    pending.frozen.application.reset ();
    pending.frozen.canonical_bytes.clear ();
    pending.transport.parts.clear ();
    const auto pending_key = delivery_key (owner, turn.sequence);
    if (_pending.contains (pending_key)) {
        if (inserted_sequence)
            _next_sequence.erase (next_sequence);
        return collision_error;
    }

    std::map<delivery_key_t, pending_delivery_t>::iterator pending_entry;
    try {
        auto inserted = _pending.emplace (pending_key, std::move (pending));
        if (!inserted.second) {
            if (inserted_sequence)
                _next_sequence.erase (next_sequence);
            return collision_error;
        }
        pending_entry = inserted.first;
    }
    catch (...) {
        if (inserted_sequence)
            _next_sequence.erase (next_sequence);
        return stateful_error_t::backpressured;
    }

    stateful_error_t enqueued = stateful_error_t::none;
    std::function<void ()> deliver;
    bool relocation_handoff = false;
    try {
        enqueued = _objects->enqueue (owner, turn_domain_t::application, std::move (turn), &deliver,
                                      restored, &relocation_handoff);
    }
    catch (...) {
        enqueued = stateful_error_t::backpressured;
    }
    if (enqueued != stateful_error_t::none) {
        _pending.erase (pending_entry);
        if (inserted_sequence)
            _next_sequence.erase (next_sequence);
        return enqueued;
    }

    if (allocate_sequence)
        ++next_sequence->second;
    auto claim = relocation_handoff ? std::move (pending_entry->second.mailbox_claim)
                                    : accepted_mailbox_reservation_t{};
    lock.unlock ();
    {
        // Relocation owns the accepted record; _pending keeps the original
        // reply token while the ordinary mailbox permit is returned.
        mailbox_claim_release_guard_t release (*_transport, std::move (claim));
    }
    if (admitted_application) {
        tracer.trace (message_flow_outcome_t::admitted, [&] {
            message_flow_event_t event{};
            event.outcome = message_flow_outcome_t::admitted;
            const bool actor = owner.kind == object_kind_t::actor;
            const bool request = admitted_kind == protocol::frozen_record_kind_t::actor_request
                                 || admitted_kind == protocol::frozen_record_kind_t::spot_request;
            event.surface =
              actor ? dispatch_error_surface_t::spot_actor : dispatch_error_surface_t::spot_route;
            event.message_kind =
              actor ? (request ? dispatch_message_kind_t::actor_request
                               : dispatch_message_kind_t::actor_send)
                    : (request ? dispatch_message_kind_t::request : dispatch_message_kind_t::send);
            if (actor)
                event.actor_id = owner.key;
            else
                event.spot_id = owner.key;
            event.packet_name = admitted_application->packet_name;
            event.flow_id = admitted_application->flow_id;
            event.flow_origin = admitted_application->flow_origin;
            if (admitted_application->packet_name == protocol::framework_multipart_packet_name
                && admitted_application->content_type
                     == protocol::framework_multipart_content_type) {
                auto parts = protocol::decode_application_parts (*admitted_application);
                if (!parts.empty ()) {
                    const auto header = messaging::envelope_codec_t{}.decode_header (
                      parts.front (), tracer.capture_enabled ());
                    if (header) {
                        event.packet_name = header.value ().message_name;
                        event.channel_name = header.value ().channel_name;
                        if (!header.value ().correlation_id.empty ())
                            event.correlation_id = header.value ().correlation_id;
                        event.flow_id = header.value ().flow_id;
                        event.flow_origin = header.value ().flow_origin;
                    }
                }
            }
            if (tracer.enabled (message_flow_log_mode_t::detailed)) {
                event.detail_stage = "canonical_application.commit_accepted_ingress";
                event.detail_result =
                  "sequence=" + std::to_string (pending_key.sequence)
                  + " operation_high=" + std::to_string (admitted_operation.high)
                  + " operation_low=" + std::to_string (admitted_operation.low);
            }
            return event;
        });
    }
    if (deliver)
        deliver ();
    return stateful_error_t::none;
}

task_t<bool>
raw_stateful_dispatch_t::forward_accepted (object_ref_t owner,
                                           turn_record_t turn,
                                           std::vector<std::uint8_t> target_routing_id,
                                           std::uint64_t target_generation,
                                           std::uint64_t target_lease_generation,
                                           std::chrono::milliseconds window,
                                           std::optional<std::uint64_t> target_authority_generation)
{
    const auto application =
      turn.application_record
        ? protocol::summarize_frozen_application_record (*turn.application_record)
      : turn.frozen_record ? *turn.frozen_record
                           : protocol::decode_frozen_record (turn.payload, capture_flow ());
    if (!application.application)
        co_return false;
    const auto sequence = turn.sequence;
    if (!target_authority_generation) {
        if (!application.reply_route_id) {
            co_return discard_pending (owner, sequence) == stateful_error_t::none;
        }
        const auto failure = messaging::request_failure_mapper_t{}
                               .target_failure_reply (framework_error_kind_t::unavailable)
                               .value ();
        const protocol::reply_relay_t relay{
          application.operation,
          *application.reply_route_id,
          {},
          0,
          {},
          0,
          sequence,
          failure.terminal_result,
          static_cast<protocol::framework_error_code> (failure.failure_code)};
        co_return co_await complete_relocated_source_async (owner, sequence, relay, std::nullopt);
    }

    auto complete = [this, owner, sequence, operation = application.operation,
                     reply_route = application.reply_route_id] (
                      foundation::operation_terminal_t terminal, zlink::message_t bytes) {
        // A hop with no observed terminal retains the original delivery for
        // authority settlement. An explicit application failure is a reply.
        if (terminal != foundation::operation_terminal_t::completed && bytes.is_empty ())
            return;
        std::optional<protocol::application_payload_t> reply;
        protocol::reply_relay_t relay{
          operation, *reply_route, {}, 0, {}, 0, sequence, 0, protocol::framework_error_code::none};
        if (terminal == foundation::operation_terminal_t::completed)
            reply = protocol::decode_application_payload (protocol::message_bytes (bytes),
                                                          capture_flow ());
        else {
            const auto failed = protocol::decode_reply_header (protocol::message_bytes (bytes));
            relay.terminal_result = failed.terminal_result;
            relay.failure_code = static_cast<protocol::framework_error_code> (failed.failure_code);
        }
        auto completion = std::make_shared<task_t<bool>> (
          complete_relocated_source_async (owner, sequence, relay, reply));
        detail::observe_task_completion (*completion, [completion] (const result_t<bool> &) {});
    };
    bool sent = false;
    if (owner.kind == object_kind_t::actor) {
        protocol::actor_route_fence_t target{owner.key, owner.object_generation,      {},
                                             0,         *target_authority_generation, 0};
        target.target_node_routing_id = target_routing_id;
        target.target_node_generation = target_generation;
        target.owner_lease_generation = target_lease_generation;
        std::optional<protocol::actor_message_header_t::bound_session_source_t> session;
        if (application.source_session_routing_id)
            session = protocol::actor_message_header_t::bound_session_source_t{
              *application.source_session_routing_id, application.source_binding_generation,
              application.source_session_sequence};
        if (application.reply_route_id.has_value ())
            sent = co_await _transport->request_to_actor (
              target_routing_id, application.source_actor, target, *application.application, window,
              std::move (complete), application.operation, session);
        else
            sent = co_await _transport->send_to_actor (target_routing_id, application.source_actor,
                                                       target, *application.application, session,
                                                       application.operation);
    } else {
        protocol::spot_route_fence_t target{owner.key, owner.object_generation,      {},
                                            0,         *target_authority_generation, 0};
        target.target_node_routing_id = target_routing_id;
        target.target_node_generation = target_generation;
        target.owner_lease_generation = target_lease_generation;
        if (application.reply_route_id.has_value ())
            sent = co_await _transport->request_to_spot (
              target_routing_id, application.source_spot_id.value_or (""), target,
              *application.application, window, std::move (complete), application.operation);
        else
            sent = co_await _transport->send_to_spot (
              target_routing_id, application.source_spot_id.value_or (""), target,
              *application.application, application.operation);
    }
    co_return sent;
}

task_t<stateful_error_t> raw_stateful_dispatch_t::ingest (object_ref_t owner)
{
    auto claim =
      _transport->mailbox ().try_claim_owner (mesh::service_mailbox_domain_t::application,
                                              mailbox_owner (owner), 1, mailbox_claim_byte_limit);
    if (!claim)
        co_return stateful_error_t::not_found;

    auto claim_holder = std::make_shared<mesh::service_mailbox_claim_t> (std::move (*claim));
    co_return co_await ingest_claimed (owner, std::move (claim_holder->records.front ()),
                                       claim_holder);
}

task_t<stateful_error_t>
raw_stateful_dispatch_t::ingest_claimed (object_ref_t owner,
                                         mesh::service_mailbox_record_t record,
                                         accepted_mailbox_reservation_t reservation)
{
    mailbox_claim_release_guard_t claim_guard (*_transport, reservation);
    stateful_error_t validation = stateful_error_t::none;
    protocol::frozen_application_record_t frozen;
    protocol::application_payload_t payload;
    std::size_t application_payload_bytes = 0;
    std::optional<protocol::command> application_command;
    std::optional<protocol::actor_message_header_t> actor_header;
    std::optional<protocol::spot_message_header_t> spot_header;
    std::optional<accepted_record_authority_t> accepted_authority;
    try {
        if (record.parts.size () != 2 || !_authority_resolver || record.source_routing_id.empty ()
            || !record.source_node_generation)
            validation = stateful_error_t::invalid;
        else {
            application_payload_bytes =
              protocol::application_payload_hwm_bytes (protocol::message_bytes (record.parts[1]));
        }

        if (validation != stateful_error_t::none)
            throw protocol::service_wire_error_t ("invalid application wire record");
        const auto header =
          protocol::decode_header (protocol::message_bytes (record.parts.front ()));

        if (validation == stateful_error_t::none && owner.kind == object_kind_t::actor) {
            if (header.kind != protocol::command::actorSend
                && header.kind != protocol::command::actorRequest) {
                validation = stateful_error_t::invalid;
            } else {
                auto actor = protocol::decode_actor_message_header (
                  protocol::message_bytes (record.parts.front ()), header.kind);
                auto [error, authority] = co_await accepted_actor_authority (
                  owner, actor, record.source_routing_id, record.source_node_generation,
                  record.bound_session_source.has_value ());
                validation = error;
                if (validation == stateful_error_t::none) {
                    application_command = header.kind;
                    actor_header = std::move (actor);
                    accepted_authority = *authority;
                }
            }
        } else if (validation == stateful_error_t::none) {
            if (header.kind != protocol::command::spotSend
                && header.kind != protocol::command::spotRequest) {
                validation = stateful_error_t::invalid;
            } else {
                auto spot = protocol::decode_spot_message_header (
                  protocol::message_bytes (record.parts.front ()), header.kind);
                auto [error, authority] = co_await accepted_spot_authority (
                  owner, spot, record.source_routing_id, record.source_node_generation);
                validation = error;
                if (validation == stateful_error_t::none) {
                    application_command = header.kind;
                    spot_header = std::move (spot);
                    accepted_authority = *authority;
                }
            }
        }
        if (validation == stateful_error_t::none) {
            if (actor_header) {
                // Actor metadata and accepted target are normalized together below.
                frozen.operation = actor_header->operation;
            } else if (spot_header) {
                frozen.operation = spot_header->operation;
            }
            if (!nonzero (frozen.operation))
                validation = stateful_error_t::invalid;
        }
    }
    catch (const protocol::service_wire_error_t &) {
        validation = stateful_error_t::invalid;
    }
    catch (...) {
        validation = stateful_error_t::conflict;
    }

    if (validation != stateful_error_t::none) {
        if (record.reply_token && record.correlation) {
            // moving replies with the retryable relocation terminal
            // (conflict + spotMoving maps to a retryable unavailable on the
            // requester), so the session owner keeps redelivery ownership.
            const auto terminal_result =
              validation == stateful_error_t::generation_stale
                  || validation == stateful_error_t::moving
                ? static_cast<std::uint32_t> (protocol::request_terminal_result::conflict)
              : validation == stateful_error_t::conflict
                ? static_cast<std::uint32_t> (protocol::request_terminal_result::rejected)
                : static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError);
            const auto failure_code =
              validation == stateful_error_t::generation_stale
                ? static_cast<std::uint32_t> (protocol::framework_error_code::actorLocationStale)
              : validation == stateful_error_t::moving
                ? static_cast<std::uint32_t> (protocol::framework_error_code::spotMoving)
              : validation == stateful_error_t::conflict
                ? static_cast<std::uint32_t> (protocol::framework_error_code::requestRejected)
                : static_cast<std::uint32_t> (protocol::framework_error_code::requestProtocolError);
            claim_guard.dismiss ();
            reply_failure_then_release_claim (*_transport, std::move (record), terminal_result,
                                              failure_code, reservation);
        }
        co_return validation;
    }

    try {
        /* flow-correlation §4: at Off the wire flow pair is neither validated
         * nor materialized at this ingress (structural skip only). */
        payload = protocol::decode_application_payload (protocol::message_bytes (record.parts[1]),
                                                        capture_flow ());
        if (actor_header) {
            auto accepted_target = actor_header->target;
            accepted_target.object_generation = owner.object_generation;
            accepted_target.owner_lease_generation =
              accepted_authority->target_owner_lease_generation;
            actor_header->target = std::move (accepted_target);
            frozen =
              accepted_actor_record (*actor_header, accepted_authority->source, payload,
                                     *application_command == protocol::command::actorRequest);
        } else {
            auto accepted_target = spot_header->target;
            accepted_target.owner_lease_generation =
              accepted_authority->target_owner_lease_generation;
            spot_header->target = std::move (accepted_target);
            frozen = accepted_spot_record (*spot_header, accepted_authority->source, payload,
                                           *application_command == protocol::command::spotRequest);
        }
    }
    catch (const protocol::service_wire_error_t &) {
        if (record.reply_token && record.correlation) {
            claim_guard.dismiss ();
            reply_failure_then_release_claim (
              *_transport, std::move (record),
              static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError),
              static_cast<std::uint32_t> (protocol::framework_error_code::requestProtocolError),
              reservation);
        }
        co_return stateful_error_t::invalid;
    }

    protocol::frozen_record_t accepted_frozen;
    try {
        accepted_frozen = protocol::summarize_frozen_application_record (frozen);
    }
    catch (...) {
        co_return stateful_error_t::backpressured;
    }
    const auto request = frozen.reply_route_id.has_value ();
    stateful_error_t enqueued;
    try {
        enqueued = commit_accepted_ingress (
          owner, turn_record_t{0, {}, application_payload_bytes, std::move (frozen)},
          pending_delivery_t{owner, std::move (accepted_frozen), record, request, {}, reservation},
          true, stateful_error_t::conflict);
    }
    catch (...) {
        enqueued = stateful_error_t::backpressured;
    }
    if (enqueued != stateful_error_t::none) {
        if (record.reply_token && record.correlation) {
            claim_guard.dismiss ();
            reply_failure_then_release_claim (
              *_transport, std::move (record),
              static_cast<std::uint32_t> (protocol::request_terminal_result::rejected),
              static_cast<std::uint32_t> (protocol::framework_error_code::requestRejected),
              reservation);
        }
        co_return enqueued;
    }
    claim_guard.dismiss ();
    co_return stateful_error_t::none;
}
std::pair<stateful_error_t, std::optional<stateful_delivery_t>>
raw_stateful_dispatch_t::try_claim (const object_ref_t &owner,
                                    std::function<void ()> *before_application_handler,
                                    std::string *stable_type)
{
    std::lock_guard lock (_mutex);
    auto [error, turn] = _objects->try_claim (owner, turn_domain_t::application, stable_type);
    if (error != stateful_error_t::none || !turn) {
        return {error, std::nullopt};
    }
    auto pending = _pending.find (delivery_key (owner, turn->sequence));
    if (pending == _pending.end ()) {
        (void) release_application_claim (owner);
        return {stateful_error_t::conflict, std::nullopt};
    }
    // Terminal completion still reads the retained operation/source identity.
    // Claim copies that summary; commit already removed its payload bytes.
    auto frozen = pending->second.frozen;
    if (before_application_handler)
        *before_application_handler =
          std::move (pending->second.transport.before_application_handler);
    auto accepted_owner = std::exchange (turn->accepted_owner, {});
    return {stateful_error_t::none,
            stateful_delivery_t{owner, std::move (*turn), std::move (frozen),
                                pending->second.request, std::move (accepted_owner)}};
}

stateful_error_t raw_stateful_dispatch_t::accept_local (
  const object_ref_t &owner,
  std::shared_ptr<runtime::serial_application_record_t> application_record,
  std::function<void ()> before_application_handler,
  std::function<bool (const std::optional<protocol::application_payload_t> &)> terminal)
{
    if (!application_record || !application_record->payload
        || !application_record->payload->application_record)
        return stateful_error_t::invalid;
    auto frozen = protocol::summarize_frozen_application_record (
      *application_record->payload->application_record);
    const auto request = frozen.reply_route_id.has_value ();
    auto turn = std::move (*application_record->payload);
    turn.application_payload_bytes = protocol::application_payload_hwm_bytes (*frozen.application);
    application_record->payload.reset ();
    mesh::service_mailbox_record_t transport;
    transport.before_application_handler = std::move (before_application_handler);
    pending_delivery_t pending{owner,   std::move (frozen),   std::move (transport),
                               request, std::move (terminal), {}};
    if (owner.kind == object_kind_t::actor)
        turn.accepted_owner = std::move (application_record);
    return commit_accepted_ingress (owner, std::move (turn), std::move (pending), true,
                                    stateful_error_t::conflict);
}

task_t<std::pair<stateful_error_t, std::optional<accepted_record_authority_t>>>
raw_stateful_dispatch_t::accepted_actor_authority (object_ref_t owner,
                                                   protocol::actor_message_header_t actor,
                                                   std::vector<std::uint8_t> source_node_routing_id,
                                                   std::uint64_t source_node_generation,
                                                   bool bound_session_routed,
                                                   local_source_fence_resolver_t local_source) const
{
    if (!matches_application_route (owner, actor.target))
        return task_t<authority_result_t> (
          result_t<authority_result_t>::success ({stateful_error_t::conflict, std::nullopt}));
    if ((bound_session_routed || actor.bound_session_source)
        && !(actor.bound_session_source && !actor.bound_session_source->session_routing_id.empty ()
             && actor.bound_session_source->binding_generation != 0
             && actor.bound_session_source->session_sequence != 0))
        return task_t<authority_result_t> (
          result_t<authority_result_t>::success ({stateful_error_t::moving, std::nullopt}));
    accepted_record_authority_query_t query;
    query.target = std::move (owner);
    query.source_node_routing_id = std::move (source_node_routing_id);
    query.source_node_generation = source_node_generation;
    query.target_node_routing_id = std::move (actor.target.target_node_routing_id);
    query.target_node_generation = actor.target.target_node_generation;
    query.source_kind = actor.bound_session_source ? protocol::frozen_source_kind_t::bound_session
                        : actor.source_actor       ? protocol::frozen_source_kind_t::actor
                                                   : protocol::frozen_source_kind_t::node;
    query.source_actor = std::move (actor.source_actor);
    if (actor.bound_session_source) {
        query.source_session_routing_id =
          std::move (actor.bound_session_source->session_routing_id);
        query.source_binding_generation = actor.bound_session_source->binding_generation;
        query.source_session_sequence = actor.bound_session_source->session_sequence;
    }
    return resolve_accepted_authority (std::move (query), actor.target.owner_lease_generation,
                                       std::move (local_source));
}

task_t<std::pair<stateful_error_t, std::optional<accepted_record_authority_t>>>
raw_stateful_dispatch_t::accepted_spot_authority (object_ref_t owner,
                                                  protocol::spot_message_header_t spot,
                                                  std::vector<std::uint8_t> source_node_routing_id,
                                                  std::uint64_t source_node_generation,
                                                  local_source_fence_resolver_t local_source) const
{
    if (!matches_application_route (owner, spot.target))
        return task_t<authority_result_t> (
          result_t<authority_result_t>::success ({stateful_error_t::conflict, std::nullopt}));
    accepted_record_authority_query_t query;
    query.target = std::move (owner);
    query.source_node_routing_id = std::move (source_node_routing_id);
    query.source_node_generation = source_node_generation;
    query.target_node_routing_id = std::move (spot.target.target_node_routing_id);
    query.target.object_generation = spot.target.object_generation;
    query.target.authority_owner_generation = spot.target.authority_owner_generation;
    query.target_node_generation = spot.target.target_node_generation;
    query.source_kind = spot.source_spot_id.empty () ? protocol::frozen_source_kind_t::node
                                                     : protocol::frozen_source_kind_t::spot;
    if (!spot.source_spot_id.empty ())
        query.source_spot_id = std::move (spot.source_spot_id);
    return resolve_accepted_authority (std::move (query), spot.target.owner_lease_generation,
                                       std::move (local_source));
}

task_t<std::pair<stateful_error_t, std::optional<accepted_record_authority_t>>>
raw_stateful_dispatch_t::resolve_accepted_authority (
  accepted_record_authority_query_t query,
  std::uint64_t target_owner_lease_generation,
  local_source_fence_resolver_t local_source) const
{
    if (!_authority_resolver)
        co_return std::pair{stateful_error_t::invalid, std::nullopt};
    std::optional<accepted_record_authority_t> authority;
    std::optional<protocol::request_source_fence_t> source;
    if (local_source) {
        const auto retained_source = co_await _objects->retained_relocation_source (query.target);
        if (retained_source)
            source = local_source ();
    }
    if (source) {
        // The local runtime owns this fence; the native gate chooses dispatch, hold or Follow.
        authority = accepted_record_authority_t{*source, source->lease_generation};
    } else {
        authority = co_await _authority_resolver (std::move (query));
    }
    // Encoded headers require a nonzero lease. Local headers may leave it to this owner.
    if (!authority
        || (target_owner_lease_generation != 0
            && target_owner_lease_generation != authority->target_owner_lease_generation))
        co_return std::pair{stateful_error_t::conflict, std::nullopt};
    co_return std::pair{stateful_error_t::none, std::move (authority)};
}

stateful_error_t raw_stateful_dispatch_t::release_application_claim (const object_ref_t &owner)
{
    return _objects->complete_claim (owner, turn_domain_t::application);
}

stateful_error_t raw_stateful_dispatch_t::transfer_application_claim (const object_ref_t &owner,
                                                                      std::uint64_t sequence)
{
    accepted_mailbox_reservation_t reservation;
    {
        std::lock_guard lock (_mutex);
        const auto pending = _pending.find (delivery_key (owner, sequence));
        if (pending != _pending.end ())
            reservation = std::exchange (pending->second.mailbox_claim, {});
    }
    // A terminal may already have consumed the pending entry. The actual
    // claim carrier still owns this Kernel claim until this transfer runs.
    mailbox_claim_release_guard_t mailbox (*_transport, std::move (reservation));
    return release_application_claim (owner);
}

task_t<stateful_error_t>
raw_stateful_dispatch_t::complete_async (const stateful_delivery_t &delivery,
                                         std::optional<protocol::application_payload_t> reply)
{
    co_return complete (delivery, std::move (reply));
}

stateful_error_t
raw_stateful_dispatch_t::complete (const stateful_delivery_t &delivery,
                                   std::optional<protocol::application_payload_t> reply)
{
    const auto release = [this, &delivery] { return release_application_claim (delivery.owner); };
    return complete_with_claim (delivery, std::move (reply), std::cref (release));
}

stateful_error_t raw_stateful_dispatch_t::complete (
  const stateful_delivery_t &delivery,
  std::optional<protocol::application_payload_t> reply,
  const std::shared_ptr<runtime::serial_application_record_t> &accepted_record)
{
    const auto release = [&accepted_record] {
        accepted_record->release_native_claim ();
        return stateful_error_t::none;
    };
    return complete_with_claim (delivery, std::move (reply), std::cref (release));
}

stateful_error_t raw_stateful_dispatch_t::complete_with_claim (
  const stateful_delivery_t &delivery,
  std::optional<protocol::application_payload_t> reply,
  const std::function<stateful_error_t ()> &release_claim)
{
    pending_delivery_t pending;
    {
        std::lock_guard lock (_mutex);
        const auto found = _pending.find (delivery_key (delivery.owner, delivery.turn.sequence));
        if (found == _pending.end ())
            return stateful_error_t::conflict;
        pending = std::move (found->second);
        _pending.erase (found);
    }
    application_claim_release_guard_t application_guard (release_claim);
    mailbox_claim_release_guard_t mailbox_guard (*_transport, std::move (pending.mailbox_claim));
    bool completed = true;
    if (pending.relocated_terminal) {
        completed = pending.relocated_terminal (reply);
    } else if (delivery.request) {
        completed =
          !reply ? _transport->reply_failure (
                     pending.transport,
                     static_cast<std::uint32_t> (protocol::request_terminal_result::internalError),
                     static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed))
                 : _transport->reply (pending.transport, *reply);
    }
    const auto claim_error = application_guard.release ();
    return claim_error != stateful_error_t::none ? claim_error
           : completed                           ? stateful_error_t::none
                                                 : stateful_error_t::conflict;
}

stateful_error_t raw_stateful_dispatch_t::stage_relocated (
  const object_ref_t &owner,
  turn_record_t turn,
  std::function<bool (const std::optional<protocol::application_payload_t> &)> terminal)
{
    turn.accepted_owner.reset ();
    protocol::frozen_record_t frozen;
    try {
        /* flow-correlation §4: the frozen application flow pair is
         * observation-only — skip validation/materialization at Off. */
        frozen = protocol::decode_frozen_record (turn.payload, capture_flow ());
        if (!frozen.application)
            return stateful_error_t::invalid;
        turn.application_payload_bytes =
          protocol::application_payload_hwm_bytes (*frozen.application);
        turn.frozen_record = frozen;
        turn.payload.clear ();
        frozen.canonical_bytes.clear ();
    }
    catch (...) {
        return stateful_error_t::invalid;
    }
    const auto request = frozen.reply_route_id.has_value ();
    try {
        return commit_accepted_ingress (
          owner, std::move (turn),
          pending_delivery_t{owner, std::move (frozen), {}, request, std::move (terminal), {}},
          true, stateful_error_t::already_exists, true);
    }
    catch (...) {
        return stateful_error_t::backpressured;
    }
}

task_t<bool> raw_stateful_dispatch_t::complete_relocated_source_async (
  const object_ref_t &owner,
  std::uint64_t sequence,
  const protocol::reply_relay_t &relay,
  const std::optional<protocol::application_payload_t> &reply)
{
    pending_delivery_t pending;
    delivery_key_t pending_key{owner.kind, owner.key, sequence};
    {
        std::lock_guard lock (_mutex);
        const auto matches = [&] (const auto &entry) {
            return entry.second.owner == owner && entry.second.request
                   && entry.second.frozen.operation == relay.operation
                   && entry.second.frozen.reply_route_id == relay.reply_route_id
                   && (entry.second.relocated_terminal
                       || (entry.second.transport.reply_token
                           && entry.second.transport.correlation));
        };
        auto found = _pending.find (pending_key);
        if (found == _pending.end () || !matches (*found))
            found = std::find_if (_pending.begin (), _pending.end (), matches);
        if (found == _pending.end ())
            co_return false;
        if (found->second.relocated_completing)
            co_return false;
        pending_key = found->first;
        found->second.relocated_completing = true;
        pending = found->second;
    }
    bool delivered = false;
    try {
        if (pending.relocated_terminal)
            delivered =
              pending.relocated_terminal (relay.terminal_result == 0 ? reply : std::nullopt);
        else if (relay.terminal_result == 0 && reply)
            delivered = _transport->reply (pending.transport, *reply);
        else
            delivered = _transport->reply_failure (
              pending.transport,
              relay.terminal_result == 0
                ? static_cast<std::uint32_t> (protocol::request_terminal_result::internalError)
                : relay.terminal_result,
              static_cast<std::uint32_t> (relay.failure_code));
    }
    catch (...) {
        delivered = false;
    }
    if (!delivered) {
        std::lock_guard lock (_mutex);
        const auto found = _pending.find (pending_key);
        if (found != _pending.end ())
            found->second.relocated_completing = false;
        co_return false;
    }

    accepted_mailbox_reservation_t claim;
    {
        std::lock_guard lock (_mutex);
        const auto found = _pending.find (pending_key);
        if (found == _pending.end ())
            co_return true;
        claim = std::move (found->second.mailbox_claim);
        _pending.erase (found);
    }
    (void) _objects->discard_application (owner, pending_key.sequence);
    mailbox_claim_release_guard_t mailbox_guard (*_transport, std::move (claim));
    co_return true;
}

void raw_stateful_dispatch_t::release_relocated_payloads (const object_ref_t &owner)
{
    std::vector<std::pair<accepted_mailbox_reservation_t, std::function<void ()>>> reservations;
    {
        std::lock_guard lock (_mutex);
        reservations.reserve (
          std::count_if (_pending.begin (), _pending.end (),
                         [&] (const auto &entry) { return entry.second.owner == owner; }));
        for (auto entry = _pending.begin (); entry != _pending.end ();) {
            auto &pending = entry->second;
            if (pending.owner != owner) {
                ++entry;
                continue;
            }
            reservations.emplace_back (
              std::move (pending.mailbox_claim),
              std::exchange (pending.transport.before_application_handler, {}));
            if (!pending.request) {
                entry = _pending.erase (entry);
                continue;
            }
            // Completion needs the original reply token and operation, not
            // the source's already committed application payload.
            pending.frozen.application.reset ();
            pending.frozen.canonical_bytes.clear ();
            pending.transport.parts.clear ();
            ++entry;
        }
    }
    for (auto &[claim, initial_reservation] : reservations) {
        {
            mailbox_claim_release_guard_t release (*_transport, std::move (claim));
        }
        if (initial_reservation)
            initial_reservation ();
    }
}

stateful_error_t raw_stateful_dispatch_t::discard_pending (const object_ref_t &owner)
{
    return discard_owner_pending (owner, false);
}

stateful_error_t raw_stateful_dispatch_t::fail_pending_unavailable (const object_ref_t &owner)
{
    return discard_owner_pending (owner, true);
}

stateful_error_t raw_stateful_dispatch_t::discard_owner_pending (const object_ref_t &owner,
                                                                 bool reply_unavailable)
{
    const auto owner_key = delivery_key (owner, 0);
    struct cleanup_t
    {
        std::uint64_t sequence;
        accepted_mailbox_reservation_t claim;
        std::optional<mesh::service_mailbox_record_t> request;
    };
    std::vector<cleanup_t> cleanup;
    {
        std::lock_guard lock (_mutex);
        try {
            const auto [_, inserted] = _discarding_owners.emplace (owner_key, owner);
            if (!inserted)
                return stateful_error_t::conflict;
        }
        catch (...) {
            return stateful_error_t::backpressured;
        }
        try {
            const auto count =
              std::count_if (_pending.begin (), _pending.end (),
                             [&] (const auto &entry) { return entry.second.owner == owner; });
            cleanup.reserve (count);
        }
        catch (...) {
            _discarding_owners.erase (owner_key);
            return stateful_error_t::backpressured;
        }
        for (auto entry = _pending.begin (); entry != _pending.end ();) {
            if (entry->second.owner != owner) {
                ++entry;
                continue;
            }
            cleanup.push_back ({entry->first.sequence, std::move (entry->second.mailbox_claim),
                                reply_unavailable && entry->second.request
                                  ? std::make_optional (std::move (entry->second.transport))
                                  : std::nullopt});
            entry = _pending.erase (entry);
        }
    }
    for (auto &item : cleanup) {
        (void) _objects->discard_application (owner, item.sequence);
        if (item.request) {
            const auto failure = messaging::request_failure_mapper_t{}
                                   .target_failure_reply (framework_error_kind_t::unavailable)
                                   .value ();
            reply_failure_then_release_claim (*_transport, std::move (*item.request),
                                              failure.terminal_result, failure.failure_code,
                                              std::move (item.claim));
        } else {
            mailbox_claim_release_guard_t release (*_transport, std::move (item.claim));
        }
    }
    {
        std::lock_guard lock (_mutex);
        _discarding_owners.erase (owner_key);
    }
    return stateful_error_t::none;
}

stateful_error_t raw_stateful_dispatch_t::discard_pending (const object_ref_t &owner,
                                                           std::uint64_t sequence)
{
    if (sequence == 0)
        return stateful_error_t::invalid;
    const auto pending_key = delivery_key (owner, sequence);
    accepted_mailbox_reservation_t claim;
    stateful_error_t object_error = stateful_error_t::none;
    {
        std::lock_guard lock (_mutex);
        const auto found = _pending.find (pending_key);
        if (found == _pending.end ())
            return stateful_error_t::not_found;
        if (found->second.owner != owner)
            return stateful_error_t::conflict;
        claim = std::move (found->second.mailbox_claim);
        _pending.erase (found);
        object_error = _objects->discard_application (owner, sequence);
    }
    mailbox_claim_release_guard_t release (*_transport, std::move (claim));
    return object_error == stateful_error_t::not_found ? stateful_error_t::none : object_error;
}

bool raw_stateful_dispatch_t::delivery_key_t::operator< (const delivery_key_t &other) const noexcept
{
    return std::tie (kind, key, sequence) < std::tie (other.kind, other.key, other.sequence);
}

std::string raw_stateful_dispatch_t::mailbox_owner (const object_ref_t &owner)
{
    return mesh::service_mailbox_t::application_owner (
      owner.kind == object_kind_t::actor ? host::owner_kind_t::actor : host::owner_kind_t::spot,
      owner.key);
}

bool raw_stateful_dispatch_t::matches_application_route (const object_ref_t &owner,
                                                         const protocol::actor_route_fence_t &route)
{
    return owner.kind == object_kind_t::actor && owner.key == route.actor_id
           && owner.authority_owner_generation == route.authority_owner_generation;
}

bool raw_stateful_dispatch_t::matches_application_route (const object_ref_t &owner,
                                                         const protocol::spot_route_fence_t &route)
{
    // Instance activation can reuse its mailbox facade across authority generations.
    // The accepted authority resolver validates the public address's full fence.
    return owner.kind != object_kind_t::actor && owner.key == route.spot_id;
}

raw_stateful_dispatch_t::delivery_key_t
raw_stateful_dispatch_t::delivery_key (const object_ref_t &owner, std::uint64_t sequence)
{
    return {owner.kind, owner.key, sequence};
}


raw_relocation_replay_coordinator_t::raw_relocation_replay_coordinator_t (
  mesh::raw_mesh_node_owner_t &transport,
  std::chrono::milliseconds relay_retry_interval,
  std::chrono::milliseconds terminal_tombstone_retention) :
    _transport (&transport),
    _lane_executor (),
    _lane (_lane_executor),
    _relay_retry_interval (relay_retry_interval),
    _terminal_tombstone_retention (terminal_tombstone_retention)
{
    if (_relay_retry_interval <= std::chrono::milliseconds::zero ()
        || _terminal_tombstone_retention <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument ("relocation replay limits must be positive");
}

bool raw_relocation_replay_coordinator_t::key_t::operator< (const key_t &other) const noexcept
{
    return std::tie (relocation.high, relocation.low, target_attempt_generation, object.kind,
                     object.stable_type, object.object_id, object.object_generation,
                     object.expected_authority_owner_generation)
           < std::tie (other.relocation.high, other.relocation.low, other.target_attempt_generation,
                       other.object.kind, other.object.stable_type, other.object.object_id,
                       other.object.object_generation,
                       other.object.expected_authority_owner_generation);
}

bool raw_relocation_replay_coordinator_t::terminal_key_t::operator< (
  const terminal_key_t &other) const noexcept
{
    return std::tie (relocation.high, relocation.low, operation.high, operation.low)
           < std::tie (other.relocation.high, other.relocation.low, other.operation.high,
                       other.operation.low);
}

raw_relocation_replay_coordinator_t::key_t
raw_relocation_replay_coordinator_t::key (const protocol::relocation_id_t &relocation,
                                          std::uint64_t target_attempt_generation,
                                          const protocol::relocation_object_t &object)
{
    return {relocation, target_attempt_generation, canonical_relocation_object_identity (object)};
}

raw_relocation_replay_coordinator_t::target_activity_guard_t::target_activity_guard_t (
  raw_relocation_replay_coordinator_t *owner, key_t target) noexcept :
    owner (owner), target (target)
{
}

raw_relocation_replay_coordinator_t::target_activity_guard_t::~target_activity_guard_t () noexcept
{
    if (active && owner)
        owner->release_target_activity (target);
}

void raw_relocation_replay_coordinator_t::release_target_activity (const key_t &target) noexcept
{
    try {
        _lane
          .run ([this, &target] {
              const auto found = _targets.find (target);
              if (found != _targets.end () && found->second.active_stages != 0)
                  --found->second.active_stages;
          })
          .get ();
        std::lock_guard lock (_activity_wait_mutex);
        _activity_changed.notify_all ();
    }
    catch (...) {
    }
}

raw_relocation_replay_coordinator_t::terminal_key_t
raw_relocation_replay_coordinator_t::terminal_key (const protocol::relocation_id_t &relocation,
                                                   const protocol::wire_operation_id_t &operation)
{
    return {relocation, operation};
}

std::string
raw_relocation_replay_coordinator_t::mailbox_owner (const std::vector<std::uint8_t> &rid)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill ('0');
    for (const auto byte : rid)
        stream << std::setw (2) << static_cast<unsigned int> (byte);
    return stream.str ();
}

bool raw_relocation_replay_coordinator_t::register_target (
  raw_relocation_target_registration_t registration)
{
    if (!nonzero (registration.relocation) || registration.target_attempt_generation == 0
        || !valid_coordinator (registration.coordinator)
        || registration.relocation_source_node_routing_id.empty ()
        || registration.relocation_source_node_generation == 0
        || !valid_relocation_object (registration.object) || !registration.stage)
        return false;
    try {
        return _lane
          .run ([this, registration = std::move (registration)] () mutable {
              const auto target_key =
                key (registration.relocation, registration.target_attempt_generation,
                     registration.object);
              return _targets.emplace (target_key, target_state_t{std::move (registration)}).second;
          })
          .get ();
    }
    catch (...) {
        return false;
    }
}

bool raw_relocation_replay_coordinator_t::unregister_target (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const protocol::relocation_object_t &object)
{
    const auto target_key = key (relocation, target_attempt_generation, object);
    if (!_lane
           .run ([this, &target_key] {
               const auto found = _targets.find (target_key);
               if (found == _targets.end () || found->second.removing)
                   return false;
               found->second.removing = true;
               found->second.closing = true;
               return true;
           })
           .get ())
        return false;
    std::unique_lock wait_lock (_activity_wait_mutex);
    _activity_changed.wait (wait_lock, [this, &target_key] {
        return _lane
          .run ([this, &target_key] {
              const auto current = _targets.find (target_key);
              return current == _targets.end () || current->second.active_stages == 0;
          })
          .get ();
    });
    return _lane
      .run ([this, &target_key] {
          const auto found = _targets.find (target_key);
          if (found == _targets.end () || found->second.active_stages != 0)
              return false;
          _targets.erase (found);
          return true;
      })
      .get ();
}

task_t<raw_relocation_replay_result_t> raw_relocation_replay_coordinator_t::pump_one ()
{
    const auto local = _transport->topology ().local_descriptor ();
    auto claim = _transport->mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::infrastructure, mailbox_owner (local.node_routing_id), 1,
      mailbox_claim_byte_limit);
    if (!claim)
        co_return raw_relocation_replay_result_t::no_data;
    auto record = std::move (claim->records.front ());
    const auto result = co_await process (record);
    (void) _transport->mailbox ().release (*claim);
    co_return result;
}

task_t<raw_relocation_replay_result_t>
raw_relocation_replay_coordinator_t::process (const mesh::service_mailbox_record_t &record)
{
    if (record.parts.empty ())
        co_return raw_relocation_replay_result_t::invalid;
    try {
        const auto header =
          protocol::decode_header (protocol::message_bytes (record.parts.front ()));
        if (header.kind == protocol::command::relocationData) {
            const auto control =
              protocol::decode_relocation_control (protocol::message_bytes (record.parts.front ()));
            const auto *data = std::get_if<protocol::relocation_data_t> (&control);
            co_return data ? process_data (record, *data) : raw_relocation_replay_result_t::invalid;
        }
        if (header.kind == protocol::command::replyRelay)
            co_return co_await process_reply_relay (
              record,
              protocol::decode_reply_relay (protocol::message_bytes (record.parts.front ())));
        if (header.kind == protocol::command::replyRelayAck)
            co_return co_await process_reply_relay_ack (
              record,
              protocol::decode_reply_relay_ack (protocol::message_bytes (record.parts.front ())));
    }
    catch (const protocol::service_wire_error_t &) {
        co_return raw_relocation_replay_result_t::invalid;
    }
    co_return raw_relocation_replay_result_t::invalid;
}

raw_relocation_replay_result_t
raw_relocation_replay_coordinator_t::process_data (const mesh::service_mailbox_record_t &record,
                                                   const protocol::relocation_data_t &data)
{
    if (record.parts.size () != 1 || !nonzero (data.relocation)
        || data.target_attempt_generation == 0 || !valid_coordinator (data.coordinator)
        || data.sender_role != protocol::relocation_role_t::source
        || !valid_relocation_object (data.object) || !nonzero (data.record.operation))
        return raw_relocation_replay_result_t::invalid;

    const auto state_key = key (data.relocation, data.target_attempt_generation, data.object);
    target_activity_guard_t activity_guard (this, state_key);
    struct stage_work_t
    {
        raw_relocation_replay_result_t result = raw_relocation_replay_result_t::restore_failed;
        std::function<bool (const protocol::relocation_data_t &)> stage;
        std::function<void (const protocol::relocation_data_t &)> rollback;
    };
    auto work =
      _lane
        .run ([this, &state_key, &record, &data] {
            stage_work_t work;
            const auto found = _targets.find (state_key);
            if (found == _targets.end ()) {
                work.result = raw_relocation_replay_result_t::not_registered;
                return work;
            }
            auto &state = found->second;
            const auto &registration = state.registration;
            if (registration.relocation != data.relocation
                || registration.target_attempt_generation != data.target_attempt_generation
                || registration.coordinator != data.coordinator
                || registration.object != data.object
                || record.source_routing_id != registration.relocation_source_node_routing_id
                || record.source_node_generation != registration.relocation_source_node_generation
                || !frozen_matches_registration (data.record, registration)) {
                work.result = raw_relocation_replay_result_t::stale_fence;
                return work;
            }
            if (state.closing || state.removing) {
                work.result = raw_relocation_replay_result_t::stale_fence;
                return work;
            }
            try {
                work.stage = registration.stage;
                work.rollback = registration.rollback;
                ++state.active_stages;
                work.result = raw_relocation_replay_result_t::applied;
            }
            catch (...) {
                work.result = raw_relocation_replay_result_t::restore_failed;
            }
            return work;
        })
        .get ();
    if (work.result != raw_relocation_replay_result_t::applied)
        return work.result;
    activity_guard.activate ();

    bool staged = false;
    try {
        staged = work.stage (data);
    }
    catch (...) {
        staged = false;
    }

    if (!staged) {
        if (work.rollback) {
            try {
                work.rollback (data);
            }
            catch (...) {
            }
        }
        return raw_relocation_replay_result_t::restore_failed;
    }
    return raw_relocation_replay_result_t::applied;
}

task_t<void> raw_relocation_replay_coordinator_t::unregister_terminal_sources (
  const protocol::relocation_id_t &relocation)
{
    co_await _lane.run_task ([this, &relocation] {
        std::erase_if (_terminal_sources,
                       [&] (const auto &entry) { return entry.first.relocation == relocation; });
        publish_next_due_on_lane ();
    });
}

bool raw_relocation_replay_coordinator_t::register_terminal_source (
  raw_relocation_terminal_source_registration_t registration)
{
    if (!nonzero (registration.relocation) || !valid_coordinator (registration.coordinator)
        || !nonzero (registration.operation) || !valid_source (registration.request_source)
        || registration.target_node_routing_id.empty () || registration.target_node_generation == 0
        || registration.target_attempt_generation == 0 || registration.participant_id == 0
        || registration.sequence == 0 || registration.reply_route_id == 0 || !registration.complete)
        return false;
    return _lane
      .run ([this, registration = std::move (registration)] () mutable {
          return _terminal_sources
            .emplace (terminal_key (registration.relocation, registration.operation),
                      terminal_source_state_t{std::move (registration)})
            .second;
      })
      .get ();
}

bool raw_relocation_replay_coordinator_t::unregister_terminal_source (
  const protocol::relocation_id_t &relocation, const protocol::wire_operation_id_t &operation)
{
    return _lane
      .run ([this, &relocation, &operation] {
          return _terminal_sources.erase (terminal_key (relocation, operation)) != 0;
      })
      .get ();
}

bool raw_relocation_replay_coordinator_t::register_terminal_target (
  raw_relocation_terminal_target_registration_t registration)
{
    const auto &relay = registration.relay;
    if (!nonzero (relay.relocation) || !nonzero (relay.operation)
        || !valid_coordinator (relay.coordinator) || !valid_source (registration.request_source)
        || relay.target_attempt_generation == 0 || relay.participant_id == 0 || relay.sequence == 0
        || relay.reply_route_id == 0 || !registration.persist_ack
        || !registration.persist_source_lease_expiry
        || (relay.terminal_result != 0 && registration.application_reply)
        || (relay.terminal_result != 0
            && relay.failure_code == protocol::framework_error_code::none))
        return false;
    const auto bytes = retained_bytes (registration);
    const auto registered =
      _lane
        .run ([this, registration = std::move (registration), bytes] () mutable {
            const auto item_key =
              terminal_key (registration.relay.relocation, registration.relay.operation);
            const auto existing = _terminal_targets.find (item_key);
            if (existing != _terminal_targets.end ())
                return existing->second.registration.relay == registration.relay
                       && existing->second.registration.request_source
                            == registration.request_source
                       && existing->second.registration.application_reply
                            == registration.application_reply;
            terminal_target_state_t state;
            state.registration = std::move (registration);
            state.retained_bytes = bytes;
            state.next_retry = clock_t::time_point::min ();
            _terminal_targets.emplace (item_key, std::move (state));
            _next_due.store (due_now, std::memory_order_release);
            _terminal_retained_bytes += bytes;
            return true;
        })
        .get ();
    if (registered)
        _transport->signal_activity ();
    return registered;
}

task_t<std::size_t>
raw_relocation_replay_coordinator_t::retry_terminal_relays (clock_t::time_point now)
{
    if (!due (now))
        return task_t<std::size_t> (result_t<std::size_t>::success (0));
    return retry_due_terminal_relays (now);
}

task_t<std::size_t>
raw_relocation_replay_coordinator_t::retry_due_terminal_relays (clock_t::time_point now)
{
    struct pending_send_t
    {
        terminal_key_t key;
        std::vector<std::uint8_t> target;
        protocol::reply_relay_t relay;
        std::optional<protocol::application_payload_t> reply;
    };
    auto pending = _lane
                     .run ([this, now] {
                         std::vector<pending_send_t> pending;
                         for (auto &[item_key, state] : _terminal_targets) {
                             if (state.acknowledging || state.next_retry > now)
                                 continue;
                             pending.push_back (
                               {item_key,
                                state.registration.relay_destination_node_routing_id.empty ()
                                  ? state.registration.request_source.node_routing_id
                                  : state.registration.relay_destination_node_routing_id,
                                state.registration.relay, state.registration.application_reply});
                             state.next_retry = now + _relay_retry_interval;
                         }
                         publish_next_due_on_lane ();
                         return pending;
                     })
                     .get ();
    std::size_t sent = 0;
    for (auto &item : pending) {
        try {
            if (co_await _transport->send_reply_relay (item.target, item.relay,
                                                       std::move (item.reply)))
                ++sent;
        }
        catch (...) {
            // The durable target record remains registered for the next retry.
        }
    }
    co_return sent;
}

task_t<raw_relocation_replay_result_t> raw_relocation_replay_coordinator_t::process_reply_relay (
  const mesh::service_mailbox_record_t &record, const protocol::reply_relay_t &relay)
{
    const auto item_key = terminal_key (relay.relocation, relay.operation);
    std::optional<protocol::application_payload_t> reply;
    if (record.parts.size () == 2) {
        try {
            /* flow-correlation §4: reply flow pair is observation-only —
             * skip validation/materialization at Off. */
            reply = protocol::decode_application_payload (protocol::message_bytes (record.parts[1]),
                                                          capture_flow ());
        }
        catch (const protocol::service_wire_error_t &) {
            co_return raw_relocation_replay_result_t::invalid;
        }
    } else if (record.parts.size () != 1) {
        co_return raw_relocation_replay_result_t::invalid;
    }
    if ((relay.terminal_result != 0 && reply)
        || (relay.terminal_result != 0
            && relay.failure_code == protocol::framework_error_code::none))
        co_return raw_relocation_replay_result_t::invalid;

    std::vector<std::uint8_t> fingerprint = protocol::encode_reply_relay (relay);
    if (reply) {
        const auto encoded = protocol::encode_application_payload (*reply);
        fingerprint.insert (fingerprint.end (), encoded.begin (), encoded.end ());
    }
    const auto digest = digest_bytes (fingerprint);
    struct completion_work_t
    {
        raw_relocation_replay_result_t failure = raw_relocation_replay_result_t::not_registered;
        std::function<bool (const protocol::reply_relay_t &,
                            const std::optional<protocol::application_payload_t> &)>
          complete;
        protocol::request_source_fence_t request_source;
        bool duplicate = false;
        bool proceed = false;
    };
    auto work =
      _lane
        .run ([this, &item_key, &relay, &record, &digest] {
            completion_work_t work;
            const auto found = _terminal_sources.find (item_key);
            if (found == _terminal_sources.end ())
                return work;
            auto &state = found->second;
            const auto &registration = state.registration;
            if (relay.relocation != registration.relocation
                || relay.coordinator != registration.coordinator
                || relay.operation != registration.operation
                || relay.target_attempt_generation != registration.target_attempt_generation
                || relay.participant_id != registration.participant_id
                || relay.sequence != registration.sequence
                || relay.reply_route_id != registration.reply_route_id
                || record.source_routing_id != registration.target_node_routing_id
                || record.source_node_generation != registration.target_node_generation) {
                work.failure = raw_relocation_replay_result_t::stale_fence;
                return work;
            }
            if (state.completed) {
                if (state.digest != digest) {
                    work.failure = raw_relocation_replay_result_t::conflicting_duplicate;
                    return work;
                }
                work.duplicate = true;
            } else if (state.completing) {
                work.failure = raw_relocation_replay_result_t::sequence_gap;
                return work;
            } else {
                state.completing = true;
                work.complete = std::move (state.registration.complete);
            }
            work.request_source = registration.request_source;
            work.proceed = true;
            return work;
        })
        .get ();
    if (!work.proceed)
        co_return work.failure;

    if (!work.duplicate) {
        bool persisted = false;
        try {
            persisted = work.complete (relay, reply);
        }
        catch (...) {
            persisted = false;
        }
        const auto completed = _lane
                                 .run ([this, &item_key, persisted, &digest, &work] {
                                     const auto found = _terminal_sources.find (item_key);
                                     if (found == _terminal_sources.end ())
                                         return raw_relocation_replay_result_t::not_registered;
                                     found->second.completing = false;
                                     if (!persisted) {
                                         found->second.registration.complete =
                                           std::move (work.complete);
                                         return raw_relocation_replay_result_t::persistence_failed;
                                     }
                                     found->second.completed = true;
                                     found->second.digest = digest;
                                     found->second.completed_at = clock_t::now ();
                                     _next_due.store (due_now, std::memory_order_release);
                                     return raw_relocation_replay_result_t::terminal_received;
                                 })
                                 .get ();
        if (completed != raw_relocation_replay_result_t::terminal_received)
            co_return completed;
        _transport->signal_activity ();
    }

    const protocol::reply_relay_ack_t ack{
      relay.relocation,
      relay.coordinator,
      relay.operation,
      relay.reply_route_id,
      work.request_source,
      work.duplicate ? protocol::reply_relay_ack_status_t::already_terminal
                     : protocol::reply_relay_ack_status_t::terminal_received};
    try {
        if (!(co_await _transport->send_reply_relay_ack (record.source_routing_id, ack)))
            co_return raw_relocation_replay_result_t::transport_failed;
    }
    catch (...) {
        co_return raw_relocation_replay_result_t::transport_failed;
    }
    co_return work.duplicate ? raw_relocation_replay_result_t::terminal_duplicate
                             : raw_relocation_replay_result_t::terminal_received;
}

task_t<raw_relocation_replay_result_t>
raw_relocation_replay_coordinator_t::process_reply_relay_ack (
  const mesh::service_mailbox_record_t &record, const protocol::reply_relay_ack_t &ack)
{
    if (record.parts.size () != 1)
        co_return raw_relocation_replay_result_t::invalid;
    const auto item_key = terminal_key (ack.relocation, ack.operation);
    struct ack_work_t
    {
        raw_relocation_replay_result_t failure = raw_relocation_replay_result_t::not_registered;
        std::function<bool (protocol::reply_relay_ack_status_t)> persist;
        bool proceed = false;
    };
    auto work =
      _lane
        .run ([this, &item_key, &ack, &record] {
            ack_work_t work;
            const auto found = _terminal_targets.find (item_key);
            if (found == _terminal_targets.end ())
                return work;
            auto &state = found->second;
            const auto &registration = state.registration;
            if (ack.relocation != registration.relay.relocation
                || ack.coordinator != registration.relay.coordinator
                || ack.operation != registration.relay.operation
                || ack.reply_route_id != registration.relay.reply_route_id
                || ack.request_source != registration.request_source
                || record.source_routing_id != registration.request_source.node_routing_id
                || record.source_node_generation != registration.request_source.node_generation) {
                work.failure = raw_relocation_replay_result_t::stale_fence;
                return work;
            }
            if (state.acknowledging) {
                work.failure = raw_relocation_replay_result_t::ack_ignored;
                return work;
            }
            state.acknowledging = true;
            work.persist = std::move (state.registration.persist_ack);
            work.proceed = true;
            return work;
        })
        .get ();
    if (!work.proceed)
        co_return work.failure;

    bool persisted = false;
    try {
        persisted = work.persist (ack.status);
    }
    catch (...) {
        persisted = false;
    }
    const auto result = _lane
                          .run ([this, &item_key, persisted, &work] {
                              const auto found = _terminal_targets.find (item_key);
                              if (found == _terminal_targets.end ())
                                  return raw_relocation_replay_result_t::not_registered;
                              found->second.acknowledging = false;
                              _next_due.store (due_now, std::memory_order_release);
                              if (!persisted) {
                                  found->second.registration.persist_ack = std::move (work.persist);
                                  return raw_relocation_replay_result_t::persistence_failed;
                              }
                              _terminal_retained_bytes -= found->second.retained_bytes;
                              _terminal_targets.erase (found);
                              return raw_relocation_replay_result_t::relay_acknowledged;
                          })
                          .get ();
    if (result == raw_relocation_replay_result_t::persistence_failed)
        _transport->signal_activity ();
    co_return result;
}

bool raw_relocation_replay_coordinator_t::confirm_terminal_source_lease_expired (
  const protocol::relocation_id_t &relocation,
  const protocol::wire_operation_id_t &operation,
  const protocol::request_source_fence_t &exact_source)
{
    const auto item_key = terminal_key (relocation, operation);
    struct expiry_work_t
    {
        std::function<bool ()> persist;
        bool proceed = false;
    };
    auto work = _lane
                  .run ([this, &item_key, &exact_source] {
                      expiry_work_t work;
                      const auto found = _terminal_targets.find (item_key);
                      if (found == _terminal_targets.end ()
                          || found->second.registration.request_source != exact_source
                          || found->second.acknowledging)
                          return work;
                      found->second.acknowledging = true;
                      work.persist =
                        std::move (found->second.registration.persist_source_lease_expiry);
                      work.proceed = true;
                      return work;
                  })
                  .get ();
    if (!work.proceed)
        return false;
    bool persisted = false;
    try {
        persisted = work.persist ();
    }
    catch (...) {
        persisted = false;
    }
    bool retry_reactivated = false;
    const auto confirmed = _lane
                             .run ([this, &item_key, persisted, &work, &retry_reactivated] {
                                 const auto found = _terminal_targets.find (item_key);
                                 if (found == _terminal_targets.end ())
                                     return false;
                                 found->second.acknowledging = false;
                                 _next_due.store (due_now, std::memory_order_release);
                                 if (!persisted) {
                                     found->second.registration.persist_source_lease_expiry =
                                       std::move (work.persist);
                                     retry_reactivated = true;
                                     return false;
                                 }
                                 _terminal_retained_bytes -= found->second.retained_bytes;
                                 _terminal_targets.erase (found);
                                 return true;
                             })
                             .get ();
    if (retry_reactivated)
        _transport->signal_activity ();
    return confirmed;
}

bool raw_relocation_replay_coordinator_t::due (clock_t::time_point now) const noexcept
{
    return now.time_since_epoch ().count () >= _next_due.load (std::memory_order_acquire);
}

void raw_relocation_replay_coordinator_t::publish_next_due_on_lane () const
{
    auto next = never_due;
    for (const auto &[key, state] : _terminal_targets) {
        if (!state.acknowledging)
            next = std::min (next, state.next_retry.time_since_epoch ().count ());
    }
    for (const auto &[key, state] : _terminal_sources) {
        if (state.completed)
            next = std::min (
              next,
              (state.completed_at + _terminal_tombstone_retention).time_since_epoch ().count ());
    }
    _next_due.store (next, std::memory_order_release);
}

std::size_t raw_relocation_replay_coordinator_t::reap_terminal_tombstones (clock_t::time_point now)
{
    if (!due (now))
        return 0;
    return _lane
      .run ([this, now] {
          std::size_t removed = 0;
          for (auto iterator = _terminal_sources.begin (); iterator != _terminal_sources.end ();) {
              if (iterator->second.completed
                  && iterator->second.completed_at + _terminal_tombstone_retention <= now) {
                  iterator = _terminal_sources.erase (iterator);
                  ++removed;
              } else {
                  ++iterator;
              }
          }
          publish_next_due_on_lane ();
          return removed;
      })
      .get ();
}

std::optional<raw_relocation_replay_coordinator_t::clock_t::time_point>
raw_relocation_replay_coordinator_t::next_activity () const
{
    auto next = _next_due.load (std::memory_order_acquire);
    if (next == due_now) {
        _lane.run ([this] { publish_next_due_on_lane (); }).get ();
        next = _next_due.load (std::memory_order_acquire);
    }
    if (next == never_due)
        return std::nullopt;
    return clock_t::time_point (clock_t::duration (next));
}

std::size_t raw_relocation_replay_coordinator_t::pending_terminal_relays () const
{
    return _lane.run ([this] { return _terminal_targets.size (); }).get ();
}

std::size_t raw_relocation_replay_coordinator_t::terminal_retained_bytes () const
{
    return _lane.run ([this] { return _terminal_retained_bytes; }).get ();
}


} // namespace zlink::framework::runtime::stateful
