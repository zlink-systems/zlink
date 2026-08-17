/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/raw_stateful_dispatch.hpp"

#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/sha256.hpp"

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

constexpr std::uint32_t terminal_protocol_error = 104;
constexpr std::uint32_t terminal_internal_error = 105;
constexpr std::uint32_t terminal_rejected = 106;
constexpr std::uint32_t terminal_conflict = 107;

class mailbox_claim_release_guard_t final
{
  public:
    mailbox_claim_release_guard_t (
      mesh::raw_mesh_node_owner_t &transport,
      std::shared_ptr<mesh::service_mailbox_claim_t> claim) :
        _transport (&transport),
        _claim (std::move (claim))
    {
    }

    ~mailbox_claim_release_guard_t () noexcept
    {
        if (_transport && _claim) {
            (void) _transport->mailbox ().release (*_claim);
        }
    }

    mailbox_claim_release_guard_t (const mailbox_claim_release_guard_t &) = delete;
    mailbox_claim_release_guard_t &operator= (
      const mailbox_claim_release_guard_t &) = delete;

    void dismiss () noexcept
    {
        _transport = nullptr;
        _claim.reset ();
    }

  private:
    mesh::raw_mesh_node_owner_t *_transport;
    std::shared_ptr<mesh::service_mailbox_claim_t> _claim;
};

void reply_failure_then_release_claim (
  mesh::raw_mesh_node_owner_t &transport,
  mesh::service_mailbox_record_t request,
  std::uint32_t terminal_result,
  std::uint32_t failure_code,
  std::shared_ptr<mesh::service_mailbox_claim_t> claim)
{
    try {
        (void) transport.reply_failure (
          request, terminal_result, failure_code);
    }
    catch (...) {
    }
    (void) transport.mailbox ().release (*claim);
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

bool valid_coordinator (
  const protocol::relocation_coordinator_fence_t &value) noexcept
{
    return !value.owner_id.empty () && value.lease_generation != 0
           && !value.node_routing_id.empty () && value.node_generation != 0
           && !value.expected_authority_store_version.empty ();
}

bool valid_relocation_object (
  const protocol::relocation_object_t &value) noexcept
{
    if (value.object_id.empty () || value.object_generation == 0)
        return false;
    switch (value.kind) {
        case protocol::relocation_object_kind_t::actor:
        case protocol::relocation_object_kind_t::user_spot:
            return value.expected_authority_owner_generation != 0;
        case protocol::relocation_object_kind_t::instance_spot:
            return !value.stable_type.empty ()
                   && value.expected_authority_owner_generation == 0;
    }
    return false;
}

protocol::relocation_object_t canonical_relocation_object_identity (
  protocol::relocation_object_t value)
{
    if (value.kind != protocol::relocation_object_kind_t::instance_spot)
        value.stable_type.clear ();
    return value;
}

std::array<std::byte, 32> digest_bytes (
  const std::vector<std::uint8_t> &bytes)
{
    std::vector<std::byte> public_bytes;
    public_bytes.reserve (bytes.size ());
    for (const auto value : bytes)
        public_bytes.push_back (static_cast<std::byte> (value));
    return runtime::sha256 (public_bytes);
}

std::size_t retained_bytes (
  const raw_relocation_terminal_target_registration_t &registration)
{
    auto total = protocol::encode_reply_relay (registration.relay).size ();
    if (registration.application_reply)
        total += protocol::encode_application_payload (
                   *registration.application_reply).size ();
    total += registration.request_source.owner_id.size ()
             + registration.request_source.node_routing_id.size ()
             + registration.relay_destination_node_routing_id.size ();
    return total;
}

bool frozen_matches_registration (
  const protocol::frozen_record_t &frozen,
  const raw_relocation_target_registration_t &registration)
{
    if (!frozen.target)
        return false;
    const auto &target = *frozen.target;
    return target.kind == registration.object.kind
           && target.object_id == registration.object.object_id
           && target.object_generation
                == registration.object.object_generation
           && target.authority_owner_generation
                == registration.object.expected_authority_owner_generation;
}

}

accepted_record_authority_resolver_t
make_location_store_authority_resolver (
  zlink::framework::location_repository_t &store)
{
    return [&store] (const accepted_record_authority_query_t &query)
      -> std::optional<accepted_record_authority_t> {
        try {
            const auto kind = query.target.kind == object_kind_t::actor
              ? placement_object_kind_t::actor
              : query.target.kind == object_kind_t::user_spot
                  ? placement_object_kind_t::user_spot
                  : placement_object_kind_t::instance_spot;
            const auto authority = store.read_authority (
              query.target.kind == object_kind_t::actor
                ? actor_authority_key (query.target.key)
                : spot_authority_key (query.target.key)).result ().value ();
            const auto *snapshot =
              std::get_if<authority_snapshot_t> (&authority);
            const auto target_rid = node_rid_t::from_string (
              zlink::routing_id_t::from (
                query.target_node_routing_id).to_string ());
            if (!snapshot
                || snapshot->allocation.object_kind != kind
                || snapshot->object_generation
                     != query.target.object_generation
                || snapshot->authority_owner_generation
                     != query.target.authority_owner_generation
                || snapshot->allocation.target.mesh_name
                     != query.target.mesh_name
                || snapshot->allocation.target.node_rid.value ()
                     != target_rid.value ()
                || snapshot->allocation.target.node_lifecycle_generation
                     != query.target_node_generation) {
                return std::nullopt;
            }

            const auto nodes = store.list_mesh_nodes (
              query.target.mesh_name).result ().value ();
            const auto source_rid = zlink::routing_id_t::from (
              query.source_node_routing_id);
            const auto source = std::find_if (
              nodes.items.begin (), nodes.items.end (),
              [&] (const mesh_node_descriptor_t &node) {
                  return node.rid == source_rid
                         && node.lifecycle_generation
                              == query.source_node_generation;
              });
            if (source == nodes.items.end () || source->owner_id.empty ()
                || source->lease_generation <= 0) {
                return std::nullopt;
            }
            const auto lease = store.read_owner_lease (
              source->owner_id).result ().value ();
            const auto *live = std::get_if<owner_lease_found_t> (&lease);
            if (!live || live->token.lease_generation
                           != source->lease_generation
                || live->lease_expires_at <= live->store_now) {
                return std::nullopt;
            }
            return accepted_record_authority_t{
              {source->owner_id,
               static_cast<std::uint64_t> (source->lease_generation),
               query.source_node_routing_id,
               query.source_node_generation},
              static_cast<std::uint64_t> (
                snapshot->allocation.target.owner.lease_generation)};
        }
        catch (...) {
            return std::nullopt;
        }
    };
}

raw_stateful_dispatch_t::raw_stateful_dispatch_t (
  stateful_object_runtime_t &objects,
  mesh::raw_mesh_node_owner_t &transport,
  accepted_record_authority_resolver_t authority_resolver) :
    _objects (&objects), _transport (&transport),
    _authority_resolver (std::move (authority_resolver))
{
}

stateful_error_t raw_stateful_dispatch_t::commit_accepted_ingress (
  const object_ref_t &owner,
  turn_record_t turn,
  pending_delivery_t pending,
  bool allocate_sequence,
  stateful_error_t collision_error)
{
    const auto owner_key = delivery_key (owner, 0);
    const auto sequence_key = std::pair{owner.kind, owner.key};
    std::lock_guard lock (_mutex);

    const auto discarding = _discarding_owners.find (owner_key);
    if (discarding != _discarding_owners.end ()
        && discarding->second == owner) {
        return collision_error;
    }

    std::map<std::pair<object_kind_t, std::string>,
             std::uint64_t>::iterator next_sequence;
    bool inserted_sequence = false;
    if (allocate_sequence) {
        try {
            const auto inserted = _next_sequence.try_emplace (
              sequence_key, std::uint64_t{1});
            next_sequence = inserted.first;
            inserted_sequence = inserted.second;
        }
        catch (...) {
            return stateful_error_t::backpressured;
        }
        if (next_sequence->second == 0
            || next_sequence->second
                 == std::numeric_limits<std::uint64_t>::max ()) {
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

    const auto pending_key = delivery_key (owner, turn.sequence);
    if (_pending.contains (pending_key)) {
        if (inserted_sequence)
            _next_sequence.erase (next_sequence);
        return collision_error;
    }

    std::map<delivery_key_t, pending_delivery_t>::iterator pending_entry;
    try {
        auto inserted = _pending.emplace (
          pending_key, std::move (pending));
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
    try {
        enqueued = _objects->enqueue (
          owner, turn_domain_t::application, std::move (turn));
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
    return stateful_error_t::none;
}

stateful_error_t raw_stateful_dispatch_t::ingest (
  const object_ref_t &owner)
{
    auto claim = _transport->mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application,
      mailbox_owner (owner), 1, 16u * 1024u * 1024u);
    if (!claim)
        return stateful_error_t::not_found;

    auto claim_holder = std::make_shared<mesh::service_mailbox_claim_t> (
      std::move (*claim));
    mailbox_claim_release_guard_t claim_guard (*_transport, claim_holder);
    auto record = std::move (claim_holder->records.front ());
    stateful_error_t validation = stateful_error_t::none;
    protocol::frozen_application_record_t frozen;
    protocol::application_payload_t payload;
    std::size_t application_payload_bytes = 0;
    std::optional<protocol::command> application_command;
    std::optional<protocol::actor_message_header_t> actor_header;
    std::optional<protocol::spot_message_header_t> spot_header;
    std::optional<accepted_record_authority_t> accepted_authority;
    try {
        const auto header = protocol::decode_header (
          record.parts.front ());
        if (record.parts.size () != 2 || !_authority_resolver
            || record.source_routing_id.empty ()
            || !record.source_node_generation)
            validation = stateful_error_t::invalid;
        else {
            application_payload_bytes =
              protocol::application_payload_hwm_bytes (record.parts[1]);
        }

        accepted_record_authority_query_t query;
        query.target = owner;
        query.source_node_routing_id = record.source_routing_id;
        query.source_node_generation =
          record.source_node_generation;

        if (validation == stateful_error_t::none
            && owner.kind == object_kind_t::actor) {
            if (header.kind != protocol::command::actorSend
                && header.kind != protocol::command::actorRequest) {
                validation = stateful_error_t::invalid;
            }
            else {
                auto actor = protocol::decode_actor_message_header (
                  record.parts.front (), header.kind);
                if (!matches_application_route (owner, actor.target)) {
                    validation = stateful_error_t::conflict;
                }
                else if (const bool bound_session_routed =
                           actor.bound_session_source.has_value ()
                           || record.bound_session_source.has_value ();
                         bound_session_routed
                         && !(actor.bound_session_source
                              && !actor.bound_session_source
                                    ->session_routing_id.empty ()
                              && actor.bound_session_source
                                     ->binding_generation
                                   != 0
                              && actor.bound_session_source
                                     ->session_sequence
                                   != 0)) {
                    // Cross-language capture contract: a bound-session-routed
                    // frame that is missing its exact fence (session RID +
                    // binding generation + session sequence) is pre-Captured.
                    // Never freeze it under the node/actor fallback identity;
                    // reject it with the retryable moving terminal so the
                    // session owner keeps redelivery ownership.
                    validation = stateful_error_t::moving;
                }
                else {
                    query.target_node_routing_id =
                      actor.target.target_node_routing_id;
                    query.target_node_generation =
                      actor.target.target_node_generation;
                    query.source_kind = actor.bound_session_source
                      ? protocol::frozen_source_kind_t::bound_session
                      : actor.source_actor
                          ? protocol::frozen_source_kind_t::actor
                          : protocol::frozen_source_kind_t::node;
                    query.source_actor = actor.source_actor;
                    if (actor.bound_session_source) {
                        query.source_session_routing_id =
                          actor.bound_session_source->session_routing_id;
                        query.source_binding_generation =
                          actor.bound_session_source->binding_generation;
                        query.source_session_sequence =
                          actor.bound_session_source->session_sequence;
                    }
                    const auto authority = _authority_resolver (query);
                    if (!authority
                        || actor.target.owner_lease_generation
                             != authority->target_owner_lease_generation) {
                        validation = stateful_error_t::conflict;
                    }
                    else {
                        application_command = header.kind;
                        actor_header = std::move (actor);
                        accepted_authority = *authority;
                    }
                }
            }
        }
        else if (validation == stateful_error_t::none) {
            if (header.kind != protocol::command::spotSend
                && header.kind != protocol::command::spotRequest) {
                validation = stateful_error_t::invalid;
            }
            else {
                auto spot = protocol::decode_spot_message_header (
                  record.parts.front (), header.kind);
                if (!matches_application_route (owner, spot.target)) {
                    validation = stateful_error_t::conflict;
                }
                else {
                    query.target_node_routing_id =
                      spot.target.target_node_routing_id;
                    query.target_node_generation =
                      spot.target.target_node_generation;
                    query.source_kind = spot.source_spot_id.empty ()
                      ? protocol::frozen_source_kind_t::node
                      : protocol::frozen_source_kind_t::spot;
                    if (!spot.source_spot_id.empty ())
                        query.source_spot_id = spot.source_spot_id;
                    const auto authority = _authority_resolver (query);
                    if (!authority
                        || spot.target.owner_lease_generation
                             != authority->target_owner_lease_generation) {
                        validation = stateful_error_t::conflict;
                    }
                    else {
                        application_command = header.kind;
                        spot_header = std::move (spot);
                        accepted_authority = *authority;
                    }
                }
            }
        }
        if (validation == stateful_error_t::none) {
            if (actor_header) {
                const auto &actor = *actor_header;
                frozen.kind = *application_command
                  == protocol::command::actorRequest
                  ? protocol::frozen_record_kind_t::actor_request
                  : protocol::frozen_record_kind_t::actor_send;
                frozen.source = accepted_authority->source;
                if (actor.bound_session_source) {
                    // The bound-session frozen source identity is the exact
                    // owning Actor of the current binding record, which is the
                    // fenced ingest target of a bound-session frame.
                    frozen.source_kind =
                      protocol::frozen_source_kind_t::bound_session;
                    frozen.source_actor = std::make_pair (
                      owner.key, owner.object_generation);
                    frozen.source_session_routing_id =
                      actor.bound_session_source->session_routing_id;
                    frozen.source_binding_generation =
                      actor.bound_session_source->binding_generation;
                    frozen.source_session_sequence =
                      actor.bound_session_source->session_sequence;
                } else {
                    frozen.source_kind = actor.source_actor
                      ? protocol::frozen_source_kind_t::actor
                      : protocol::frozen_source_kind_t::node;
                    frozen.source_actor = actor.source_actor;
                }
                frozen.operation = actor.operation;
                frozen.operation_kind = *application_command
                  == protocol::command::actorRequest ? 4u : 0u;
                frozen.reply_route_id = actor.correlation;
            } else if (spot_header) {
                const auto &spot = *spot_header;
                frozen.kind = *application_command
                  == protocol::command::spotRequest
                  ? protocol::frozen_record_kind_t::spot_request
                  : protocol::frozen_record_kind_t::spot_send;
                frozen.source_kind = spot.source_spot_id.empty ()
                  ? protocol::frozen_source_kind_t::node
                  : protocol::frozen_source_kind_t::spot;
                frozen.source = accepted_authority->source;
                if (!spot.source_spot_id.empty ())
                    frozen.source_spot_id = spot.source_spot_id;
                frozen.operation = spot.operation;
                frozen.operation_kind = *application_command
                  == protocol::command::spotRequest ? 3u : 0u;
                frozen.reply_route_id = spot.correlation;
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
        if (record.request_sequence && record.correlation) {
            // moving replies with the retryable relocation terminal
            // (conflict + spotMoving maps to a retryable unavailable on the
            // requester), so the session owner keeps redelivery ownership.
            const auto terminal_result =
              validation == stateful_error_t::generation_stale
                  || validation == stateful_error_t::moving
                ? terminal_conflict
                : validation == stateful_error_t::conflict
                    ? terminal_rejected
                    : terminal_protocol_error;
            const auto failure_code =
              validation == stateful_error_t::generation_stale
                ? static_cast<std::uint32_t> (
                    protocol::framework_error_code::actorLocationStale)
                : validation == stateful_error_t::moving
                    ? static_cast<std::uint32_t> (
                        protocol::framework_error_code::spotMoving)
                : validation == stateful_error_t::conflict
                    ? static_cast<std::uint32_t> (
                        protocol::framework_error_code::requestRejected)
                    : static_cast<std::uint32_t> (
                        protocol::framework_error_code::requestProtocolError);
            claim_guard.dismiss ();
            reply_failure_then_release_claim (
              *_transport,
              std::move (record),
              terminal_result,
              failure_code,
              claim_holder);
        }
        return validation;
    }

    try {
        /* flow-correlation §4: at Off the wire flow pair is neither validated
         * nor materialized at this ingress (structural skip only). */
        payload = protocol::decode_application_payload (record.parts[1],
                                                        capture_flow ());
        if (actor_header) {
            auto accepted_target = actor_header->target;
            accepted_target.object_generation = owner.object_generation;
            accepted_target.owner_lease_generation =
              accepted_authority->target_owner_lease_generation;
            frozen.body = protocol::frozen_actor_application_body_t{
              std::move (accepted_target), payload};
        } else {
            auto accepted_target = spot_header->target;
            accepted_target.object_generation = owner.object_generation;
            frozen.body = protocol::frozen_spot_application_body_t{
              std::move (accepted_target),
              accepted_authority->target_owner_lease_generation, payload};
        }
    }
    catch (const protocol::service_wire_error_t &) {
        if (record.request_sequence && record.correlation) {
            claim_guard.dismiss ();
            reply_failure_then_release_claim (
              *_transport,
              std::move (record),
              terminal_protocol_error,
              static_cast<std::uint32_t> (
                protocol::framework_error_code::requestProtocolError),
              claim_holder);
        }
        return stateful_error_t::invalid;
    }

    protocol::frozen_record_t accepted_frozen;
    try {
        accepted_frozen =
          protocol::summarize_frozen_application_record (frozen);
    }
    catch (...) {
        return stateful_error_t::backpressured;
    }
    const auto request = frozen.reply_route_id.has_value ();
    stateful_error_t enqueued;
    try {
        enqueued = commit_accepted_ingress (
          owner,
          turn_record_t{0, {}, application_payload_bytes,
                        std::move (frozen)},
          pending_delivery_t{
            owner,
            std::move (accepted_frozen),
            record,
            request,
            {},
            claim_holder},
          true,
          stateful_error_t::conflict);
    }
    catch (...) {
        enqueued = stateful_error_t::backpressured;
    }
    if (enqueued != stateful_error_t::none) {
        if (record.request_sequence && record.correlation) {
            claim_guard.dismiss ();
            reply_failure_then_release_claim (
              *_transport,
              std::move (record),
              terminal_rejected,
              static_cast<std::uint32_t> (
                protocol::framework_error_code::requestRejected),
              claim_holder);
        }
        return enqueued;
    }
    claim_guard.dismiss ();
    return stateful_error_t::none;
}
std::pair<stateful_error_t, std::optional<stateful_delivery_t>>
raw_stateful_dispatch_t::try_claim (const object_ref_t &owner)
{
    std::lock_guard lock (_mutex);
    auto [error, turn] =
      _objects->try_claim (owner, turn_domain_t::application);
    if (error != stateful_error_t::none || !turn) {
        return {error, std::nullopt};
    }
    auto pending =
      _pending.find (delivery_key (owner, turn->sequence));
    if (pending == _pending.end ()) {
        (void) _objects->complete_claim (
          owner, turn_domain_t::application);
        return {stateful_error_t::conflict, std::nullopt};
    }
    if (!pending->second.frozen.application) {
        (void) _objects->complete_claim (
          owner, turn_domain_t::application);
        return {stateful_error_t::conflict, std::nullopt};
    }
    auto frozen = std::move (pending->second.frozen);
    auto payload = std::move (*frozen.application);
    return {
      stateful_error_t::none,
      stateful_delivery_t{
        owner, std::move (*turn), std::move (frozen), std::move (payload),
        pending->second.request}};
}

task_t<stateful_error_t> raw_stateful_dispatch_t::complete_async (
  const stateful_delivery_t &delivery,
  std::optional<protocol::application_payload_t> reply)
{
    pending_delivery_t pending;
    {
        std::lock_guard lock (_mutex);
        const auto found = _pending.find (
          delivery_key (delivery.owner, delivery.turn.sequence));
        if (found == _pending.end ())
            co_return stateful_error_t::conflict;
        pending = std::move (found->second);
        _pending.erase (found);
    }
    mailbox_claim_release_guard_t mailbox_guard (
      *_transport, pending.mailbox_claim);
    if (pending.relocated_terminal) {
        const auto completed = pending.relocated_terminal (reply);
        const auto claim_error = _objects->complete_claim (
          delivery.owner, turn_domain_t::application);
        co_return completed ? claim_error : stateful_error_t::conflict;
    }
    if (delivery.request) {
        const auto delivered = !reply
          ? _transport->reply_failure (
              pending.transport,
              terminal_internal_error,
              static_cast<std::uint32_t> (
                protocol::framework_error_code::requestFailed))
          : _transport->reply (pending.transport, *reply);
        if (!delivered) {
            const auto claim_error = _objects->complete_claim (
              delivery.owner, turn_domain_t::application);
            co_return claim_error == stateful_error_t::none
              ? stateful_error_t::conflict
              : claim_error;
        }
    }
    co_return _objects->complete_claim (
      delivery.owner, turn_domain_t::application);
}

stateful_error_t raw_stateful_dispatch_t::stage_relocated (
  const object_ref_t &owner,
  turn_record_t turn,
  std::function<bool (
    const std::optional<protocol::application_payload_t> &)> terminal)
{
    protocol::frozen_record_t frozen;
    try {
        /* flow-correlation §4: the frozen application flow pair is
         * observation-only — skip validation/materialization at Off. */
        frozen = protocol::decode_frozen_record (turn.payload, capture_flow ());
        if (!frozen.application)
            return stateful_error_t::invalid;
        turn.application_payload_bytes =
          protocol::application_payload_hwm_bytes (*frozen.application);
        frozen.canonical_bytes.clear ();
    }
    catch (...) {
        return stateful_error_t::invalid;
    }
    const auto request = frozen.reply_route_id.has_value ();
    try {
        return commit_accepted_ingress (
          owner,
          std::move (turn),
          pending_delivery_t{
            owner,
            std::move (frozen),
            {},
            request,
            std::move (terminal),
            {}},
          true,
          stateful_error_t::already_exists);
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
        auto found = _pending.find (
          pending_key);
        if (found == _pending.end ()
            || found->first.kind != owner.kind
            || found->first.key != owner.key
            || !found->second.request
            || !found->second.transport.request_sequence
            || !found->second.transport.correlation
            || !found->second.transport.operation
            || *found->second.transport.operation
                 != std::pair{relay.operation.high, relay.operation.low}) {
            found = std::find_if (
              _pending.begin (), _pending.end (),
              [&] (const auto &entry) {
                  return entry.first.kind == owner.kind
                         && entry.first.key == owner.key
                         && entry.second.request
                         && entry.second.transport.request_sequence
                         && entry.second.transport.correlation
                         && entry.second.transport.operation
                         && *entry.second.transport.operation
                              == std::pair{
                                relay.operation.high,
                                relay.operation.low};
              });
        }
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
        if (relay.terminal_result == 0 && reply)
            delivered = _transport->reply (
              pending.transport, *reply);
        else
            delivered = _transport->reply_failure (
              pending.transport,
              relay.terminal_result == 0 ? terminal_internal_error
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

    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    {
        std::lock_guard lock (_mutex);
        const auto found = _pending.find (pending_key);
        if (found == _pending.end ())
            co_return true;
        claim = std::move (found->second.mailbox_claim);
        _pending.erase (found);
    }
    mailbox_claim_release_guard_t mailbox_guard (*_transport, std::move (claim));
    co_return true;
}

stateful_error_t raw_stateful_dispatch_t::discard_pending (
  const object_ref_t &owner)
{
    const auto owner_key = delivery_key (owner, 0);
    struct cleanup_t
    {
        std::uint64_t sequence;
        std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    };
    std::vector<cleanup_t> cleanup;
    {
        std::lock_guard lock (_mutex);
        try {
            const auto [_, inserted] = _discarding_owners.emplace (
              owner_key, owner);
            if (!inserted)
                return stateful_error_t::conflict;
        }
        catch (...) {
            return stateful_error_t::backpressured;
        }
        try {
            const auto count = std::count_if (
              _pending.begin (), _pending.end (),
              [&] (const auto &entry) {
                  return entry.second.owner == owner;
              });
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
            cleanup.push_back (
              {entry->first.sequence,
               std::move (entry->second.mailbox_claim)});
            entry = _pending.erase (entry);
        }
    }
    for (auto &item : cleanup) {
        (void) _objects->discard_application (owner, item.sequence);
        if (item.claim)
            (void) _transport->mailbox ().release (*item.claim);
    }
    {
        std::lock_guard lock (_mutex);
        _discarding_owners.erase (owner_key);
    }
    return stateful_error_t::none;
}

stateful_error_t raw_stateful_dispatch_t::discard_pending (
  const object_ref_t &owner,
  std::uint64_t sequence)
{
    if (sequence == 0)
        return stateful_error_t::invalid;
    const auto pending_key = delivery_key (owner, sequence);
    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
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
    if (claim)
        (void) _transport->mailbox ().release (*claim);
    return object_error == stateful_error_t::not_found
             ? stateful_error_t::none
             : object_error;
}

bool raw_stateful_dispatch_t::delivery_key_t::operator< (
  const delivery_key_t &other) const noexcept
{
    return std::tie (kind, key, sequence)
           < std::tie (other.kind, other.key, other.sequence);
}

std::string raw_stateful_dispatch_t::mailbox_owner (
  const object_ref_t &owner)
{
    if (owner.kind == object_kind_t::actor) {
        return "actor:" + owner.key;
    }
    return "spot:" + owner.key;
}

bool raw_stateful_dispatch_t::matches_application_route (
  const object_ref_t &owner,
  const protocol::actor_route_fence_t &route)
{
    return owner.kind == object_kind_t::actor
           && owner.key == route.actor_id
           && owner.authority_owner_generation
                == route.authority_owner_generation;
}

bool raw_stateful_dispatch_t::matches_application_route (
  const object_ref_t &owner,
  const protocol::spot_route_fence_t &route)
{
    return owner.kind != object_kind_t::actor
           && owner.key == route.spot_id
           && owner.authority_owner_generation
                == route.authority_owner_generation;
}

raw_stateful_dispatch_t::delivery_key_t
raw_stateful_dispatch_t::delivery_key (
  const object_ref_t &owner,
  std::uint64_t sequence)
{
    return {owner.kind, owner.key, sequence};
}


raw_relocation_replay_coordinator_t::raw_relocation_replay_coordinator_t (
  mesh::raw_mesh_node_owner_t &transport,
  std::size_t terminal_record_limit,
  std::size_t terminal_byte_limit,
  std::chrono::milliseconds relay_retry_interval,
  std::chrono::milliseconds terminal_tombstone_retention) :
    _transport (&transport),
    _terminal_record_limit (terminal_record_limit),
    _terminal_byte_limit (terminal_byte_limit),
    _relay_retry_interval (relay_retry_interval),
    _terminal_tombstone_retention (terminal_tombstone_retention)
{
    if (_terminal_record_limit == 0 || _terminal_byte_limit == 0
        || _relay_retry_interval <= std::chrono::milliseconds::zero ()
        || _terminal_tombstone_retention
             <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument (
          "relocation replay limits must be positive");
}

bool raw_relocation_replay_coordinator_t::key_t::operator< (
  const key_t &other) const noexcept
{
    return std::tie (relocation.high, relocation.low,
                     target_attempt_generation, object.kind,
                     object.stable_type, object.object_id,
                     object.object_generation,
                     object.expected_authority_owner_generation)
           < std::tie (other.relocation.high, other.relocation.low,
                       other.target_attempt_generation,
                       other.object.kind, other.object.stable_type,
                       other.object.object_id,
                       other.object.object_generation,
                       other.object.expected_authority_owner_generation);
}

bool raw_relocation_replay_coordinator_t::terminal_key_t::operator< (
  const terminal_key_t &other) const noexcept
{
    return std::tie (relocation.high, relocation.low,
                     operation.high, operation.low)
           < std::tie (other.relocation.high, other.relocation.low,
                       other.operation.high, other.operation.low);
}

raw_relocation_replay_coordinator_t::key_t
raw_relocation_replay_coordinator_t::key (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const protocol::relocation_object_t &object)
{
    return {relocation, target_attempt_generation,
            canonical_relocation_object_identity (object)};
}

raw_relocation_replay_coordinator_t::target_activity_guard_t::
target_activity_guard_t (
  raw_relocation_replay_coordinator_t *owner,
  key_t target) noexcept :
    owner (owner),
    target (target)
{
}

raw_relocation_replay_coordinator_t::target_activity_guard_t::
~target_activity_guard_t () noexcept
{
    if (active && owner)
        owner->release_target_activity (target);
}

void raw_relocation_replay_coordinator_t::release_target_activity (
  const key_t &target) noexcept
{
    std::lock_guard lock (_gate);
    const auto found = _targets.find (target);
    if (found != _targets.end () && found->second.active_stages != 0)
        --found->second.active_stages;
    _gate_condition.notify_all ();
}

raw_relocation_replay_coordinator_t::terminal_key_t
raw_relocation_replay_coordinator_t::terminal_key (
  const protocol::relocation_id_t &relocation,
  const protocol::wire_operation_id_t &operation)
{
    return {relocation, operation};
}

std::string raw_relocation_replay_coordinator_t::mailbox_owner (
  const std::vector<std::uint8_t> &rid)
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
    if (!nonzero (registration.relocation)
        || registration.target_attempt_generation == 0
        || !valid_coordinator (registration.coordinator)
        || registration.relocation_source_node_routing_id.empty ()
        || registration.relocation_source_node_generation == 0
        || !valid_relocation_object (registration.object)
        || !registration.stage)
        return false;
    std::lock_guard lock (_gate);
    try {
        const auto target_key = key (
          registration.relocation,
          registration.target_attempt_generation,
          registration.object);
        return _targets.emplace (
          target_key, target_state_t{std::move (registration)}).second;
    }
    catch (...) {
        return false;
    }
}

bool raw_relocation_replay_coordinator_t::seal_target (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const protocol::relocation_object_t &object)
{
    std::lock_guard lock (_gate);
    const auto found = _targets.find (
      key (relocation, target_attempt_generation, object));
    if (found == _targets.end () || found->second.closing
        || found->second.removing)
        return false;
    found->second.closing = true;
    return true;
}

bool raw_relocation_replay_coordinator_t::drain_target (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const protocol::relocation_object_t &object)
{
    std::unique_lock lock (_gate);
    const auto target_key = key (
      relocation, target_attempt_generation, object);
    const auto found = _targets.find (target_key);
    if (found == _targets.end () || !found->second.closing)
        return false;
    _gate_condition.wait (lock, [this, &target_key] {
        const auto current = _targets.find (target_key);
        return current == _targets.end ()
               || current->second.active_stages == 0;
    });
    const auto current = _targets.find (target_key);
    return current != _targets.end ()
           && !current->second.removing
           && current->second.active_stages == 0;
}

bool raw_relocation_replay_coordinator_t::unregister_target (
  const protocol::relocation_id_t &relocation,
  std::uint64_t target_attempt_generation,
  const protocol::relocation_object_t &object)
{
    std::unique_lock lock (_gate);
    const auto target_key = key (
      relocation, target_attempt_generation, object);
    auto found = _targets.find (target_key);
    if (found == _targets.end ())
        return false;
    if (found->second.removing)
        return false;
    found->second.removing = true;
    found->second.closing = true;
    _gate_condition.wait (lock, [this, &target_key] {
        const auto current = _targets.find (target_key);
        return current == _targets.end ()
               || current->second.active_stages == 0;
    });
    found = _targets.find (target_key);
    if (found == _targets.end ())
        return false;
    _targets.erase (found);
    return true;
}

task_t<raw_relocation_replay_result_t>
raw_relocation_replay_coordinator_t::pump_one ()
{
    const auto local = _transport->topology ().local_descriptor ();
    auto claim = _transport->mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::infrastructure,
      mailbox_owner (local.node_routing_id), 1, 16u * 1024u * 1024u);
    if (!claim)
        co_return raw_relocation_replay_result_t::no_data;
    auto record = std::move (claim->records.front ());
    const auto result = co_await process (record);
    (void) _transport->mailbox ().release (*claim);
    co_return result;
}

task_t<raw_relocation_replay_result_t>
raw_relocation_replay_coordinator_t::process (
  const mesh::service_mailbox_record_t &record)
{
    if (record.parts.empty ())
        co_return raw_relocation_replay_result_t::invalid;
    try {
        const auto header = protocol::decode_header (
          record.parts.front ());
        if (header.kind == protocol::command::relocationData) {
            const auto control = protocol::decode_relocation_control (
              record.parts.front ());
            const auto *data =
              std::get_if<protocol::relocation_data_t> (&control);
            co_return data ? process_data (record, *data)
                           : raw_relocation_replay_result_t::invalid;
        }
        if (header.kind == protocol::command::replyRelay)
            co_return co_await process_reply_relay (
              record, protocol::decode_reply_relay (
                        record.parts.front ()));
        if (header.kind == protocol::command::replyRelayAck)
            co_return co_await process_reply_relay_ack (
              record, protocol::decode_reply_relay_ack (
                        record.parts.front ()));
    }
    catch (const protocol::service_wire_error_t &) {
        co_return raw_relocation_replay_result_t::invalid;
    }
    co_return raw_relocation_replay_result_t::invalid;
}

raw_relocation_replay_result_t
raw_relocation_replay_coordinator_t::process_data (
  const mesh::service_mailbox_record_t &record,
  const protocol::relocation_data_t &data)
{
    if (record.parts.size () != 1
        || !nonzero (data.relocation)
        || data.target_attempt_generation == 0
        || !valid_coordinator (data.coordinator)
        || data.sender_role != protocol::relocation_role_t::source
        || !valid_relocation_object (data.object)
        || !nonzero (data.record.operation))
        return raw_relocation_replay_result_t::invalid;

    const auto state_key = key (
      data.relocation, data.target_attempt_generation,
      data.object);
    target_activity_guard_t activity_guard (this, state_key);
    std::function<bool (const protocol::relocation_data_t &)> stage;
    std::function<void (const protocol::relocation_data_t &)> rollback;
    {
        std::unique_lock lock (_gate);
        const auto found = _targets.find (state_key);
        if (found == _targets.end ())
            return raw_relocation_replay_result_t::not_registered;
        auto &state = found->second;
        const auto &registration = state.registration;
        if (registration.relocation != data.relocation
            || registration.target_attempt_generation
                 != data.target_attempt_generation
            || registration.coordinator != data.coordinator
            || registration.object != data.object
            || record.source_routing_id
                 != registration.relocation_source_node_routing_id
            || record.source_node_generation
                 != registration.relocation_source_node_generation
            || !frozen_matches_registration (
                data.record, registration))
            return raw_relocation_replay_result_t::stale_fence;
        if (state.closing || state.removing)
            return raw_relocation_replay_result_t::stale_fence;
        try {
            stage = registration.stage;
            rollback = registration.rollback;
            ++state.active_stages;
            activity_guard.activate ();
        }
        catch (...) {
            return raw_relocation_replay_result_t::restore_failed;
        }
    }

    bool staged = false;
    try {
        staged = stage (data);
    }
    catch (...) {
        staged = false;
    }

    if (!staged) {
        if (rollback) {
            try {
                rollback (data);
            }
            catch (...) {
            }
        }
        return raw_relocation_replay_result_t::restore_failed;
    }
    return raw_relocation_replay_result_t::applied;
}

bool raw_relocation_replay_coordinator_t::register_terminal_source (
  raw_relocation_terminal_source_registration_t registration)
{
    if (!nonzero (registration.relocation)
        || !valid_coordinator (registration.coordinator)
        || !nonzero (registration.operation)
        || !valid_source (registration.request_source)
        || registration.target_node_routing_id.empty ()
        || registration.target_node_generation == 0
        || registration.target_attempt_generation == 0
        || registration.participant_id == 0
        || registration.sequence == 0
        || registration.reply_route_id == 0
        || !registration.complete)
        return false;
    std::lock_guard lock (_gate);
    if (_terminal_sources.size () >= _terminal_record_limit)
        return false;
    return _terminal_sources.emplace (
      terminal_key (registration.relocation,
                    registration.operation),
      terminal_source_state_t{std::move (registration)}).second;
}

bool raw_relocation_replay_coordinator_t::unregister_terminal_source (
  const protocol::relocation_id_t &relocation,
  const protocol::wire_operation_id_t &operation)
{
    std::lock_guard lock (_gate);
    return _terminal_sources.erase (
             terminal_key (relocation, operation))
           != 0;
}

bool raw_relocation_replay_coordinator_t::register_terminal_target (
  raw_relocation_terminal_target_registration_t registration)
{
    const auto &relay = registration.relay;
    if (!nonzero (relay.relocation) || !nonzero (relay.operation)
        || !valid_coordinator (relay.coordinator)
        || !valid_source (registration.request_source)
        || relay.target_attempt_generation == 0
        || relay.participant_id == 0 || relay.sequence == 0
        || relay.reply_route_id == 0 || !registration.persist_ack
        || !registration.persist_source_lease_expiry
        || (relay.terminal_result != 0
            && registration.application_reply)
        || (relay.terminal_result != 0
            && relay.failure_code
                 == protocol::framework_error_code::none))
        return false;
    const auto bytes = retained_bytes (registration);
    std::lock_guard lock (_gate);
    const auto item_key = terminal_key (
      relay.relocation, relay.operation);
    const auto existing = _terminal_targets.find (item_key);
    if (existing != _terminal_targets.end ())
        return existing->second.registration.relay == relay
               && existing->second.registration.request_source
                    == registration.request_source
               && existing->second.registration.application_reply
                    == registration.application_reply;
    if (_terminal_targets.size () >= _terminal_record_limit
        || bytes > _terminal_byte_limit - std::min (
             _terminal_byte_limit, _terminal_retained_bytes))
        return false;
    terminal_target_state_t state;
    state.registration = std::move (registration);
    state.retained_bytes = bytes;
    state.next_retry = clock_t::time_point::min ();
    _terminal_targets.emplace (item_key, std::move (state));
    _terminal_retained_bytes += bytes;
    return true;
}

task_t<std::size_t> raw_relocation_replay_coordinator_t::retry_terminal_relays (
  clock_t::time_point now)
{
    struct pending_send_t
    {
        terminal_key_t key;
        std::vector<std::uint8_t> target;
        protocol::reply_relay_t relay;
        std::optional<protocol::application_payload_t> reply;
    };
    std::vector<pending_send_t> pending;
    {
        std::lock_guard lock (_gate);
        for (auto &[item_key, state] : _terminal_targets) {
            if (state.acknowledging || state.next_retry > now)
                continue;
            pending.push_back ({
              item_key,
              state.registration.relay_destination_node_routing_id.empty ()
                ? state.registration.request_source.node_routing_id
                : state.registration.relay_destination_node_routing_id,
              state.registration.relay,
              state.registration.application_reply});
            state.next_retry = now + _relay_retry_interval;
        }
    }
    std::size_t sent = 0;
    for (auto &item : pending) {
        try {
            if (co_await _transport->send_reply_relay (
                  item.target, item.relay, std::move (item.reply)))
                ++sent;
        }
        catch (...) {
            // The durable target record remains registered for the next retry.
        }
    }
    co_return sent;
}

task_t<raw_relocation_replay_result_t>
raw_relocation_replay_coordinator_t::process_reply_relay (
  const mesh::service_mailbox_record_t &record,
  const protocol::reply_relay_t &relay)
{
    const auto item_key = terminal_key (
      relay.relocation, relay.operation);
    std::optional<protocol::application_payload_t> reply;
    if (record.parts.size () == 2) {
        try {
            /* flow-correlation §4: reply flow pair is observation-only —
             * skip validation/materialization at Off. */
            reply = protocol::decode_application_payload (
              record.parts[1], capture_flow ());
        }
        catch (const protocol::service_wire_error_t &) {
            co_return raw_relocation_replay_result_t::invalid;
        }
    }
    else if (record.parts.size () != 1) {
        co_return raw_relocation_replay_result_t::invalid;
    }
    if ((relay.terminal_result != 0 && reply)
        || (relay.terminal_result != 0
            && relay.failure_code
                 == protocol::framework_error_code::none))
        co_return raw_relocation_replay_result_t::invalid;

    std::vector<std::uint8_t> fingerprint =
      protocol::encode_reply_relay (relay);
    if (reply) {
        const auto encoded = protocol::encode_application_payload (*reply);
        fingerprint.insert (
          fingerprint.end (), encoded.begin (), encoded.end ());
    }
    const auto digest = digest_bytes (fingerprint);
    std::function<bool (
      const protocol::reply_relay_t &,
      const std::optional<protocol::application_payload_t> &)> complete;
    protocol::request_source_fence_t request_source;
    bool duplicate = false;
    {
        std::lock_guard lock (_gate);
        const auto found = _terminal_sources.find (item_key);
        if (found == _terminal_sources.end ())
            co_return raw_relocation_replay_result_t::not_registered;
        auto &state = found->second;
        const auto &registration = state.registration;
        if (relay.relocation != registration.relocation
            || relay.coordinator != registration.coordinator
            || relay.operation != registration.operation
            || relay.target_attempt_generation
                 != registration.target_attempt_generation
            || relay.participant_id != registration.participant_id
            || relay.sequence != registration.sequence
            || relay.reply_route_id != registration.reply_route_id
            || record.source_routing_id
                 != registration.target_node_routing_id
            || record.source_node_generation
                 != registration.target_node_generation)
            co_return raw_relocation_replay_result_t::stale_fence;
        if (state.completed) {
            if (state.digest != digest)
                co_return raw_relocation_replay_result_t::conflicting_duplicate;
            duplicate = true;
        }
        else if (state.completing) {
            co_return raw_relocation_replay_result_t::sequence_gap;
        }
        else {
            state.completing = true;
            complete = registration.complete;
        }
        request_source = registration.request_source;
    }

    if (!duplicate) {
        bool persisted = false;
        try {
            persisted = complete (relay, reply);
        }
        catch (...) {
            persisted = false;
        }
        std::lock_guard lock (_gate);
        const auto found = _terminal_sources.find (item_key);
        if (found == _terminal_sources.end ())
            co_return raw_relocation_replay_result_t::not_registered;
        found->second.completing = false;
        if (!persisted)
            co_return raw_relocation_replay_result_t::persistence_failed;
        found->second.completed = true;
        found->second.digest = digest;
        found->second.completed_at = clock_t::now ();
    }

    const protocol::reply_relay_ack_t ack{
      relay.relocation,
      relay.coordinator,
      relay.operation,
      relay.reply_route_id,
      request_source,
      duplicate
        ? protocol::reply_relay_ack_status_t::already_terminal
        : protocol::reply_relay_ack_status_t::terminal_received};
    try {
        if (!(co_await _transport->send_reply_relay_ack (
              record.source_routing_id, ack)))
            co_return raw_relocation_replay_result_t::transport_failed;
    }
    catch (...) {
        co_return raw_relocation_replay_result_t::transport_failed;
    }
    co_return duplicate
      ? raw_relocation_replay_result_t::terminal_duplicate
      : raw_relocation_replay_result_t::terminal_received;
}

task_t<raw_relocation_replay_result_t>
raw_relocation_replay_coordinator_t::process_reply_relay_ack (
  const mesh::service_mailbox_record_t &record,
  const protocol::reply_relay_ack_t &ack)
{
    if (record.parts.size () != 1)
        co_return raw_relocation_replay_result_t::invalid;
    const auto item_key = terminal_key (
      ack.relocation, ack.operation);
    std::function<bool (protocol::reply_relay_ack_status_t)> persist;
    {
        std::lock_guard lock (_gate);
        const auto found = _terminal_targets.find (item_key);
        if (found == _terminal_targets.end ())
            co_return raw_relocation_replay_result_t::not_registered;
        auto &state = found->second;
        const auto &registration = state.registration;
        if (ack.relocation != registration.relay.relocation
            || ack.coordinator != registration.relay.coordinator
            || ack.operation != registration.relay.operation
            || ack.reply_route_id != registration.relay.reply_route_id
            || ack.request_source != registration.request_source
            || record.source_routing_id
                 != registration.request_source.node_routing_id
            || record.source_node_generation
                 != registration.request_source.node_generation)
            co_return raw_relocation_replay_result_t::stale_fence;
        if (state.acknowledging)
            co_return raw_relocation_replay_result_t::ack_ignored;
        state.acknowledging = true;
        persist = registration.persist_ack;
    }

    bool persisted = false;
    try {
        persisted = persist (ack.status);
    }
    catch (...) {
        persisted = false;
    }
    std::lock_guard lock (_gate);
    const auto found = _terminal_targets.find (item_key);
    if (found == _terminal_targets.end ())
        co_return raw_relocation_replay_result_t::not_registered;
    found->second.acknowledging = false;
    if (!persisted)
        co_return raw_relocation_replay_result_t::persistence_failed;
    _terminal_retained_bytes -= found->second.retained_bytes;
    _terminal_targets.erase (found);
    co_return raw_relocation_replay_result_t::relay_acknowledged;
}

bool raw_relocation_replay_coordinator_t::confirm_terminal_source_lease_expired (
  const protocol::relocation_id_t &relocation,
  const protocol::wire_operation_id_t &operation,
  const protocol::request_source_fence_t &exact_source)
{
    const auto item_key = terminal_key (relocation, operation);
    std::function<bool ()> persist;
    {
        std::lock_guard lock (_gate);
        const auto found = _terminal_targets.find (item_key);
        if (found == _terminal_targets.end ()
            || found->second.registration.request_source
                 != exact_source
            || found->second.acknowledging)
            return false;
        found->second.acknowledging = true;
        persist =
          found->second.registration.persist_source_lease_expiry;
    }
    bool persisted = false;
    try {
        persisted = persist ();
    }
    catch (...) {
        persisted = false;
    }
    std::lock_guard lock (_gate);
    const auto found = _terminal_targets.find (item_key);
    if (found == _terminal_targets.end ())
        return false;
    found->second.acknowledging = false;
    if (!persisted)
        return false;
    _terminal_retained_bytes -= found->second.retained_bytes;
    _terminal_targets.erase (found);
    return true;
}

std::size_t raw_relocation_replay_coordinator_t::reap_terminal_tombstones (
  clock_t::time_point now)
{
    std::lock_guard lock (_gate);
    std::size_t removed = 0;
    for (auto iterator = _terminal_sources.begin ();
         iterator != _terminal_sources.end ();) {
        if (iterator->second.completed
            && iterator->second.completed_at
                 + _terminal_tombstone_retention <= now) {
            iterator = _terminal_sources.erase (iterator);
            ++removed;
        }
        else {
            ++iterator;
        }
    }
    return removed;
}

std::size_t
raw_relocation_replay_coordinator_t::pending_terminal_relays () const
{
    std::lock_guard lock (_gate);
    return _terminal_targets.size ();
}

std::size_t
raw_relocation_replay_coordinator_t::terminal_retained_bytes () const
{
    std::lock_guard lock (_gate);
    return _terminal_retained_bytes;
}


} // namespace zlink::framework::runtime::stateful
