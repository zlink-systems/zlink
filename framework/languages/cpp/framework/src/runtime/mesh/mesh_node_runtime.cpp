/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include <zlink/framework/contracts/configuration/detail/framework_options_state.hpp>
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/sha256.hpp"
#include "runtime/locations/authority_key_codec.hpp"

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/failure_origin_wire.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/transport/endpoint_notation.hpp"
#include "runtime/utils/uuid.hpp"
#include "runtime/utils/relocation_id_generator.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/errors/error.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <random>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

namespace mesh_endpoint_notation = zlink::framework::runtime::transport;

namespace zlink::framework::detail
{
struct mesh_node_runtime_t::message_follow_subscription_state_t
{
    struct dispatch_frame_t
    {
        message_follow_subscription_state_t *state = nullptr;
        dispatch_frame_t *previous = nullptr;
    };

    explicit message_follow_subscription_state_t (
      std::function<void (const runtime::protocol::message_follow_notice_t &)> callback) :
        handler (std::move (callback))
    {
    }

    bool begin_dispatch () noexcept
    {
        std::lock_guard lock (mutex);
        if (!active)
            return false;
        ++in_flight;
        return true;
    }

    void finish_dispatch () noexcept
    {
        std::function<void (const runtime::protocol::message_follow_notice_t &)>
          retired_handler;
        {
            std::lock_guard lock (mutex);
            assert (in_flight != 0);
            --in_flight;
            if (!active && in_flight == 0)
                retired_handler = std::move (handler);
            terminal.notify_all ();
        }
    }

    void deactivate_and_wait () noexcept
    {
        std::size_t current_thread_dispatches = 0;
        for (auto *frame = current_dispatch; frame != nullptr;
             frame = frame->previous) {
            if (frame->state == this)
                ++current_thread_dispatches;
        }
        std::function<void (const runtime::protocol::message_follow_notice_t &)>
          retired_handler;
        {
            std::unique_lock lock (mutex);
            active = false;
            terminal.wait (lock, [this, current_thread_dispatches] {
                return in_flight <= current_thread_dispatches;
            });
            if (in_flight == 0)
                retired_handler = std::move (handler);
        }
    }

    std::mutex mutex;
    std::condition_variable terminal;
    bool active = true;
    std::size_t in_flight = 0;
    std::function<void (const runtime::protocol::message_follow_notice_t &)> handler;
    static thread_local dispatch_frame_t *current_dispatch;
};

thread_local mesh_node_runtime_t::message_follow_subscription_state_t::dispatch_frame_t *
  mesh_node_runtime_t::message_follow_subscription_state_t::current_dispatch = nullptr;

namespace
{

std::chrono::milliseconds one_way_send_timeout (const mesh_node_builder_state_t &state)
{
    return state.socket.send_timeout.value_or (std::chrono::seconds (1));
}

std::uint64_t make_lifecycle_generation ()
{
    static std::atomic_uint64_t counter{1};
    const auto random = (static_cast<std::uint64_t> (std::random_device{}()) << 32u)
                        ^ static_cast<std::uint64_t> (std::random_device{}());
    const auto time =
      static_cast<std::uint64_t> (std::chrono::steady_clock::now ().time_since_epoch ().count ());
    auto value = (random ^ time ^ counter.fetch_add (1, std::memory_order_relaxed))
                 & static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ());
    return value == 0 ? 1 : value;
}

runtime::relocation_id_generator_t &relocation_ids ()
{
    static runtime::relocation_id_generator_t value;
    return value;
}

bool same_bound_session_relocation_identity (
  const bound_session_relocation_route_t &left,
  const bound_session_relocation_route_t &right) noexcept
{
    return left.session_owner_node == right.session_owner_node
           && left.session_owner_node_generation
                == right.session_owner_node_generation
           && left.session_owner.owner_id == right.session_owner.owner_id
           && left.session_owner.lease_generation
                == right.session_owner.lease_generation
           && left.session == right.session
           && left.binding_generation == right.binding_generation;
}

bool valid_routing_id_prefix (std::string_view value) noexcept
{
    if (value.empty () || value.size () > 64)
        return false;
    for (const auto character : value) {
        const auto is_alphanumeric = (character >= 'A' && character <= 'Z')
                                     || (character >= 'a' && character <= 'z')
                                     || (character >= '0' && character <= '9');
        if (!is_alphanumeric && character != '.' && character != '_' && character != '-')
            return false;
    }
    return true;
}

bool framework_owned_node_message (const std::vector<zlink::message_t> &parts)
{
    try {
        runtime::messaging::message_parts_t encoded (parts);
        const auto header =
          runtime::messaging::envelope_codec_t{}.decode_header (encoded, false);
        return header && header.value ().message_name.starts_with ("__zlink.");
    }
    catch (...) {
        return false;
    }
}

std::size_t message_follow_payload_bytes (const runtime::messaging::envelope_header_t &header,
                                          const zlink::message_t &payload)
{
    auto total =
      payload.bytes ().size () + header.message_name.size () + header.content_type.size ();
    for (const auto &[key, value] : header.metadata)
        total += key.size () + value.size ();
    return total;
}

inline constexpr std::string_view message_follow_path_key = "__zlink.messageFollowVisitedNodes";
inline constexpr std::size_t message_follow_authority_path_bytes = 21;

struct message_follow_path_t
{
    std::string encoded;
    std::set<std::string> visited;
};

result_t<message_follow_path_t>
advance_message_follow_path (const std::map<std::string, std::string> &metadata,
                             std::string local_node,
                             std::uint64_t local_authority_owner_generation)
{
    if (local_node.empty () || local_authority_owner_generation == 0) {
        return result_t<message_follow_path_t>::failure (
          framework_error_kind_t::protocol_error,
          "Actor Message Follow source fence is incomplete");
    }
    local_node.push_back ('@');
    local_node += std::to_string (local_authority_owner_generation);
    message_follow_path_t path;
    if (const auto found = metadata.find (std::string (message_follow_path_key));
        found != metadata.end ()) {
        path.encoded = found->second;
        if (path.encoded.empty ()) {
            return result_t<message_follow_path_t>::failure (
              framework_error_kind_t::protocol_error,
              "Actor Message Follow visited-fence path is empty");
        }
        std::size_t offset = 0;
        while (offset < path.encoded.size ()) {
            const auto separator = path.encoded.find (',', offset);
            const auto token = path.encoded.substr (
              offset, separator == std::string::npos ? std::string::npos : separator - offset);
            if (token.empty () || token.size () > 128) {
                return result_t<message_follow_path_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "Actor Message Follow visited-fence path is malformed");
            }
            if (!path.visited.insert (token).second) {
                return result_t<message_follow_path_t>::failure (
                  framework_error_kind_t::unavailable,
                  "Actor Message Follow route contains a repeated authority fence");
            }
            if (separator == std::string::npos)
                break;
            offset = separator + 1;
            if (offset == path.encoded.size ()) {
                return result_t<message_follow_path_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "Actor Message Follow visited-fence path has an empty entry");
            }
        }
    }
    if (path.visited.size () >= runtime::protocol::messageFollowHopCount
        || path.visited.contains (local_node)) {
        return result_t<message_follow_path_t>::failure (
          framework_error_kind_t::unavailable,
          "Actor Message Follow returned to a visited authority fence");
    }
    path.visited.insert (local_node);
    if (!path.encoded.empty ())
        path.encoded.push_back (',');
    path.encoded += local_node;
    return result_t<message_follow_path_t>::success (std::move (path));
}

class actor_message_follow_lease_t
{
  public:
    actor_message_follow_lease_t (spot_node_runtime_t runtime,
                                  actor_ref_t actor,
                                  runtime::protocol::actor_route_fence_t source_fence,
                                  std::size_t payload_bytes) :
        _runtime (std::move (runtime)), _actor (std::move (actor)),
        _source_fence (std::move (source_fence)), _payload_bytes (payload_bytes)
    {
    }

    ~actor_message_follow_lease_t ()
    {
        _runtime.release_actor_message_follow (_actor, _source_fence, _payload_bytes);
    }

  private:
    spot_node_runtime_t _runtime;
    actor_ref_t _actor;
    runtime::protocol::actor_route_fence_t _source_fence;
    std::size_t _payload_bytes;
};

std::string node_submit_target (const zlink::routing_id_t &node)
{
    return "mesh:node:" + node.to_hex ();
}

std::string channel_submit_target (const std::string &channel)
{
    return "mesh:channel:" + channel;
}

std::string spot_submit_target (const zlink::routing_id_t &node, const std::string &spot)
{
    return "mesh:spot:" + node.to_hex () + ":" + spot;
}

std::string actor_submit_target (const actor_ref_t &actor)
{
    return "mesh:actor:" + std::string (actor.node_rid ().value ()) + ":"
           + std::string (actor.actor_id ().value ()) + ":"
           + std::to_string (actor.object_generation ());
}

std::string send_ready_target (const host::send_ready_data_t &ready)
{
    using kind_t = host::send_ready_data_t::destination_kind_t;
    switch (ready.destination_kind) {
        case kind_t::node:
            return node_submit_target (ready.target_node_rid);
        case kind_t::channel:
            return channel_submit_target (ready.channel_name);
        case kind_t::spot:
            return spot_submit_target (ready.target_node_rid, ready.target_spot_id);
        case kind_t::actor:
        case kind_t::bound_session:
            return actor_submit_target (ready.target_actor);
    }
    return {};
}

} // namespace

namespace
{

framework_exception_t configuration_error (std::string message)
{
    return framework_exception_t (framework_error_kind_t::protocol_error, std::move (message));
}

std::uint64_t next_connection_intent_id ()
{
    static std::atomic_uint64_t next{1};
    return next.fetch_add (1, std::memory_order_relaxed);
}

} // namespace

mesh_node_builder_state_t::mesh_node_builder_state_t (std::string name) :
    mesh_name (name),
    spot_state (std::make_shared<spot_node_builder_state_t> (name)),
    spot_builder (spot_state)
{
    listen_port = 0;
    listen_endpoint = mesh_endpoint_notation::normalize_endpoint (
      "tcp://" + mesh_endpoint_notation::bracket_ipv6_host (bind_host) + ":0");
}

void bind_mesh_handler_services (std::shared_ptr<mesh_node_builder_state_t> state,
                                 service_collection_t &services)
{
    if (!state) {
        throw configuration_error ("MeshNode registration is required");
    }
    std::vector<std::function<void (service_collection_t &)>> registrars;
    {
        std::lock_guard lock (state->mutex);
        state->services = &services;
        registrars.swap (state->pending_handler_service_registrars);
    }
    for (auto &registrar : registrars) {
        registrar (services);
    }
}

void register_mesh_handler_service (std::shared_ptr<mesh_node_builder_state_t> state,
                                    std::function<void (service_collection_t &)> registrar)
{
    if (!state || !registrar) {
        throw configuration_error ("MeshNode handler service registration is invalid");
    }
    service_collection_t *services = nullptr;
    {
        std::lock_guard lock (state->mutex);
        services = state->services;
        if (services == nullptr) {
            state->pending_handler_service_registrars.push_back (std::move (registrar));
            return;
        }
    }
    registrar (*services);
}

mesh_node_runtime_t::mesh_node_runtime_t (std::shared_ptr<mesh_node_builder_state_t> state) :
    _state (std::move (state))
{
    if (!_state) {
        throw configuration_error ("MeshNode registration is required");
    }
}

mesh_node_runtime_t::~mesh_node_runtime_t ()
{
    stop ();
}

void mesh_node_runtime_t::bind_serializers (serializer_registry_t &serializers) noexcept
{
    _serializers = &serializers;
}

void mesh_node_runtime_t::bind_descriptor_publisher (
  std::function<void (const std::map<std::string, int> &, int, std::uint64_t)> publisher)
{
    std::lock_guard lock (_state->mutex);
    _descriptor_publisher = std::move (publisher);
}

host::actor_join_operation_result_t
admit_wire_actor_join (const std::shared_ptr<spot_node_builder_state_t> &spot_state,
                       const zlink::routing_id_t &local_node_rid,
                       const runtime::protocol::actor_join_request_t &request,
                       const std::optional<runtime::protocol::application_payload_t> &payload)
{
    host::actor_join_operation_result_t rejected;
    try {
        spot_node_runtime_t spot (spot_state);
        // The wire body carries no stable type (unlike actorCreate's
        // stableType) — spec 15's admission semantics key on actor
        // identity, and this node must already know that identity (via a
        // prior actorCreate or join) to admit it here.
        const auto actor_type = spot.resolve_actor_type (request.actor.actor_id);
        if (!actor_type)
            return rejected;
        // The actor fence's node coordinates name the CURRENT owner — the
        // source node originating this proposal.
        const auto actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (
            zlink::routing_id_t::from (request.actor.target_node_routing_id).to_string ()),
          *actor_type, request.actor.actor_id, request.actor.object_generation);
        const auto payload_message =
          payload ? zlink::message_t::from (payload->payload) : zlink::message_t{};
        spot_id_t target_spot_id;
        if (request.entry) {
            const auto entry_spot_id = spot.resolve_entry_spot_id ();
            if (!entry_spot_id)
                return rejected;
            target_spot_id = *entry_spot_id;
        } else {
            target_spot_id = spot_id_t (request.target_spot.spot_id);
        }
        const auto spot_generation =
          spot.resolve_spot_generation (local_node_rid, target_spot_id);
        if (!spot_generation || *spot_generation == 0)
            return rejected;
        if (!request.entry) {
            // Approval-only admission (spec 15 §478-527): run the
            // application admission callback and register the relocation
            // temporary queue (identity-keyed pending admission) with the
            // prepared factory — nothing else. Spec 51 §9: a transfer id
            // never travels on this wire body; the runtime derives one
            // locally from the exact attempt identity (source node
            // RID/generation + correlation), so a duplicate resend of the
            // same attempt parks against the existing preparation while a
            // NEWER attempt (fresh correlation) gets a distinct id and
            // evicts the parked older attempt (later-attempt-wins,
            // try_add_admission).
            std::string transfer_id = "wire-actor-join:";
            static constexpr char hex_digits[] = "0123456789abcdef";
            for (const auto byte : request.actor.target_node_routing_id) {
                transfer_id += hex_digits[(byte >> 4) & 0x0f];
                transfer_id += hex_digits[byte & 0x0f];
            }
            transfer_id += ':';
            transfer_id += std::to_string (request.actor.target_node_generation);
            transfer_id += ':';
            transfer_id += std::to_string (request.correlation);
            const auto admitted = spot.admit_remote_actor_to_spot (
              std::move (transfer_id), actor_ref, spot_id_t{}, target_spot_id,
              payload_message, request.actor.target_node_generation,
              request.correlation, request.actor.authority_owner_generation);
            if (!admitted || !admitted.value ().accepted)
                return rejected;
        }
        // JoinEntrySpot has no approval round trip in the store path and
        // registers no preparation at admission (spec 15 §4.2) — the entry
        // branch above therefore approves without side effects; its
        // temporary queue is registered on the later Restore request.
        host::actor_join_operation_result_t result;
        result.join_result = runtime::protocol::actor_join_result_t::accepted;
        result.spot =
          runtime::protocol::actor_join_reply_spot_ref_t{target_spot_id, *spot_generation};
        // Approval-only: membership has not moved yet, so the accepted
        // reply carries the PROPOSED membership epoch (current + 1 — the
        // analog of java's admission-reply coreMembershipEpoch + 1). The
        // target CAS later in the transfer sequence is what actually
        // advances membership.
        result.membership_epoch =
          spot.resolve_actor_membership_epoch (request.actor.actor_id).value_or (0) + 1;
        result.receive_chunk_limit_bytes = static_cast<std::uint32_t> (
          detail::spot_actor_join_advertised_receive_chunk_limit_bytes);
        return result;
    }
    catch (...) {
        return rejected;
    }
}

void mesh_node_runtime_t::start ()
{
    if (_node) {
        return;
    }
    _stopping.store (false, std::memory_order_release);

    std::shared_ptr<handler_group_options_state_t> handler_groups;
    std::vector<std::pair<std::string, std::string>> mesh_handler_groups;
    {
        std::lock_guard state_lock (_state->mutex);
        handler_groups = _state->handler_groups;
        if (handler_groups) {
            for (const auto &[channel_name, channel] : _state->channels) {
                if (channel.server && !channel.handler_group.empty ()) {
                    mesh_handler_groups.emplace_back (channel_name, channel.handler_group);
                }
            }
        }
    }
    if (handler_groups) {
        for (const auto &[channel_name, group_name] : mesh_handler_groups) {
            mesh_channel_server_builder_t channel (_state, channel_name);
            handler_groups->install_mesh_handlers (group_name, channel);
        }
    }

    std::lock_guard lock (_state->mutex);
    if (const auto options = _state->framework_options.lock ()) {
        if (!_state->bind_host_override) {
            _state->bind_host = options->bind_host;
        }
        if (!_state->advertise_host_override) {
            _state->advertise_host = options->advertise_host;
        }
    }
    if (_state->listen_port) {
        _state->listen_endpoint = mesh_endpoint_notation::normalize_endpoint (
          "tcp://" + mesh_endpoint_notation::bracket_ipv6_host (_state->bind_host) + ":"
            + std::to_string (*_state->listen_port));
    }
    _state->spot_state->one_way_send_timeout = one_way_send_timeout (*_state);
    _state->spot_state->instance_spot_idle_timeout = _state->instance_spot_idle_timeout;
    if (_state->mesh_name.empty ()) {
        throw configuration_error ("MeshName is required");
    }
    if (_state->listen_endpoint.empty ()) {
        throw configuration_error ("MeshNode listen endpoint is required");
    }
    if (!_state->routing_id) {
        throw configuration_error ("MeshNode routing id is required");
    }
    if (!_state->core_context) {
        throw configuration_error ("MeshNode shared Core Context is required");
    }
    for (const auto &[channel_name, channel] : _state->channels) {
        if (!channel.role_selected)
            throw configuration_error ("RouteMesh channel requires a Client or Server role: "
                                       + channel_name);
    }
    if (_state->object_role == object_role_t::client && _state->has_node_direct_handler) {
        throw configuration_error (
          "Object Client cannot register application Node direct handlers");
    }
    if (_state->socket.send_timeout
        && (_state->socket.send_timeout->count () <= 0
            || _state->socket.send_timeout->count () > std::numeric_limits<int>::max ())) {
        throw configuration_error (
          "MeshNode send timeout must be between 1 and INT_MAX milliseconds");
    }

    std::vector<runtime::mesh::service_channel_descriptor_t> channels;
    channels.reserve (_state->channels.size ());
    for (const auto &[channel_name, channel] : _state->channels) {
        if (channel.server)
            channels.push_back (
              runtime::mesh::service_channel_descriptor_t{channel_name, channel.weight});
    }
    std::sort (channels.begin (), channels.end (),
               [] (const auto &left, const auto &right) { return left.name < right.name; });
    std::set<std::string> object_stable_types (_state->spot_state->snapshot.actor_types.begin (),
                                               _state->spot_state->snapshot.actor_types.end ());
    object_stable_types.insert ("framework.spot");
    auto node = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      runtime::mesh::raw_mesh_node_options_t{
        runtime::mesh::service_node_descriptor_t{
          .mesh_name = _state->mesh_name,
          .node_routing_id = _state->routing_id->to_bytes (),
          .lifecycle_generation = make_lifecycle_generation (),
          .descriptor_revision = 1,
          .advertised_endpoint = _state->listen_endpoint,
          .channels = std::move (channels),
          .state = runtime::mesh::service_node_state_t::preparing,
          .object_role = _state->object_role == object_role_t::client
                           ? runtime::mesh::service_object_role_t::client
                         : _state->object_role == object_role_t::server
                           ? runtime::mesh::service_object_role_t::server
                           : runtime::mesh::service_object_role_t::none,
          .placement_weight = _state->placement_weight},
        runtime::dispatch_limits::application_mailbox_messages,
        runtime::dispatch_limits::application_mailbox_bytes,
        runtime::dispatch_limits::control_mailbox_messages,
        runtime::dispatch_limits::control_mailbox_bytes,
        _state->advertise_host,
        _state->auto_hwm_profile},
      _state->spot_state->snapshot.entry_spot_name.value_or ("entry"),
      std::move (object_stable_types), _route_cache_max_age,
      _owner_lease_fencing_margin, _state->core_context,
      _session_relocation_seal_timeout});
    /* flow-correlation §4: thread the flow-capture provider so the host's
     * cold decode paths skip flow validation/materialization at level Off. */
    node->set_flow_capture_provider (
      [spot_state = _state->spot_state] {
          return message_flow_tracer_t (spot_state->dispatch).capture_enabled ();
      });
    if (_spot_route_fence_resolver)
        node->configure_spot_route_fence_resolver (_spot_route_fence_resolver);
    if (_user_spot_store && _user_spot_materializer) {
        node->configure_user_spot_operations (_user_spot_store, _user_spot_materializer);
    }
    if (_actor_create_target)
        node->configure_actor_create_operations (_actor_create_target);
    // actorJoin(28) receiver admission: self-contained (unlike actorCreate,
    // it needs no external Location Store / materializer), so it is always
    // wired here rather than gated on external configuration. The admission
    // is APPROVAL-ONLY (spec 15 §478-527): admit_wire_actor_join registers
    // the relocation temporary queue and prepared factory and replies
    // approval; actor construction/installation, target location claim,
    // membership CAS, and application dispatch belong to the later
    // transfer/commit stages (prepare/finalize on the actor transfer
    // coordinator). Known deferred items: route binding of wire-admitted
    // actors (an admitted actor is not yet routable by a subsequent
    // actorSend/actorRequest until the transfer commit installs the route)
    // and threading the negotiated chunk limit into the direct-transfer
    // capture. Both are acceptable only because nothing yet selects this
    // wire path in production (the originate fence-gate is a separate,
    // later increment).
    node->configure_actor_join_operations (
      [state = _state] (const runtime::protocol::actor_join_request_t &request,
                        const std::optional<runtime::protocol::application_payload_t> &payload,
                        host::actor_join_operation_target_completion_t completion) {
          completion (admit_wire_actor_join (state->spot_state, *state->routing_id,
                                             request, payload));
      });
    if (_instance_spot_materializer) {
        node->configure_instance_spot_operations (_user_spot_store, _instance_spot_relocations,
                                                  _instance_spot_owner,
                                                  _instance_spot_materializer);
    }
    if (_session_route_owner_resolver)
        node->configure_session_route_owner (_session_route_owner_resolver);
    if (_session_route_target_owner_resolver) {
        node->configure_session_route_target_owner (
          _session_route_target_owner_resolver);
    }
    if (_stateful_dispatch_resolver)
        node->configure_stateful_dispatch (_stateful_dispatch_resolver);
    if (_bound_session_operations)
        node->configure_bound_session_operations (*_bound_session_operations);
    node->configure_message_follow_handler (
      [this] (const auto &notice) { dispatch_message_follow (notice); });
    if (_relocation_authority && _relocation_store)
        node->configure_relocation (_relocation_authority, _relocation_store,
                                    _aggregate_relocation_authority,
                                    _relocation_limits);
    node->start ();
    if (_instance_spot_materializer)
        (void) node->recover_instance_spot_activations ();
    const auto resolved_endpoint = node->status ().local_endpoint ();
    if (!resolved_endpoint.empty ()) {
        _state->listen_endpoint = resolved_endpoint;
    }
    {
        std::lock_guard peer_lock (_peer_mutex);
        for (const auto &peer : _state->peer_connections) {
            const auto intent = peer.expected_routing_id
                                  ? node->connect_peer (peer.endpoint, *peer.expected_routing_id)
                                  : node->connect_peer (peer.endpoint);
            if (intent)
                _peer_connection_intents.emplace (peer.endpoint, next_connection_intent_id ());
        }
    }
    _node = std::move (node);
    const auto callback_gate = _peer_callback_gate;
    _state->runtime_peer_connect = [this,
                                    callback_gate] (const mesh_peer_connection_t &connection) {
        {
            std::lock_guard lock (callback_gate->mutex);
            if (callback_gate->stopping)
                return;
            ++callback_gate->active;
        }
        try {
            if (connection.expected_routing_id)
                connect_peer (*connection.expected_routing_id, connection.endpoint);
            else
                connect_peer (connection.endpoint);
        }
        catch (...) {
            std::lock_guard lock (callback_gate->mutex);
            if (--callback_gate->active == 0)
                callback_gate->changed.notify_all ();
            throw;
        }
        std::lock_guard lock (callback_gate->mutex);
        if (--callback_gate->active == 0)
            callback_gate->changed.notify_all ();
    };
    _state->runtime_peer_disconnect = [this,
                                       callback_gate] (const mesh_peer_connection_t &connection) {
        {
            std::lock_guard lock (callback_gate->mutex);
            if (callback_gate->stopping)
                return;
            ++callback_gate->active;
        }
        try {
            if (connection.expected_routing_id)
                disconnect_peer (*connection.expected_routing_id, connection.endpoint);
            else
                disconnect_peer (connection.endpoint);
        }
        catch (...) {
            std::lock_guard lock (callback_gate->mutex);
            if (--callback_gate->active == 0)
                callback_gate->changed.notify_all ();
            throw;
        }
        std::lock_guard lock (callback_gate->mutex);
        if (--callback_gate->active == 0)
            callback_gate->changed.notify_all ();
    };
    spot_node_runtime_t spot_runtime (_state->spot_state);
    spot_runtime.attach_native_node (_node);
    if (_state->spot_state->snapshot.entry_spot_name) {
        auto native_entry = _node->entry_spot ();
        (void) spot_runtime.get_or_create_spot (
          *_state->spot_state->snapshot.entry_spot_name, spot_id_t (native_entry.spot_id ()),
          zlink::message_t{}, native_entry.status ().lifecycle_generation (), _state->mesh_name);
    }
}

void mesh_node_runtime_t::configure_user_spot_operations (
  std::shared_ptr<location_repository_t> store, host::user_spot_materializer_t materializer)
{
    if (_node)
        throw configuration_error ("User Spot operations must be configured before MeshNode start");
    _user_spot_store = std::move (store);
    _user_spot_materializer = std::move (materializer);
}

void mesh_node_runtime_t::configure_spot_route_fence_resolver (
  host::spot_route_fence_resolver_t resolver,
  std::chrono::milliseconds route_cache_max_age,
  std::chrono::milliseconds owner_lease_fencing_margin,
  std::chrono::milliseconds session_relocation_seal_timeout)
{
    if (_node)
        throw configuration_error (
          "Spot route fence resolver must be configured before MeshNode start");
    if (route_cache_max_age < std::chrono::milliseconds::zero ())
        throw configuration_error ("Spot route cache age must not be negative");
    if (owner_lease_fencing_margin < std::chrono::milliseconds::zero ())
        throw configuration_error ("Owner lease fencing margin must not be negative");
    if (session_relocation_seal_timeout <= std::chrono::milliseconds::zero ())
        throw configuration_error (
          "Session relocation seal timeout must be greater than zero");
    _spot_route_fence_resolver = std::move (resolver);
    _route_cache_max_age = route_cache_max_age;
    _owner_lease_fencing_margin = owner_lease_fencing_margin;
    _session_relocation_seal_timeout = session_relocation_seal_timeout;
}

void mesh_node_runtime_t::configure_actor_route_resolver (
  std::function<std::optional<runtime::spot_address_t> (const actor_ref_t &)> resolver,
  std::function<void (const runtime::protocol::actor_route_fence_t &)> invalidator)
{
    if (_node)
        throw configuration_error ("Actor route resolver must be configured before MeshNode start");
    _actor_route_resolver = std::move (resolver);
    _actor_route_invalidator = std::move (invalidator);
}

void mesh_node_runtime_t::configure_actor_create_operations (
  host::actor_create_operation_target_t target)
{
    if (_node)
        throw configuration_error (
          "Actor create operations must be configured before MeshNode start");
    _actor_create_target = std::move (target);
}

void mesh_node_runtime_t::configure_instance_spot_operations (
  std::shared_ptr<location_repository_t> store,
  std::shared_ptr<runtime::stateful::relocation_store_port_t> relocations,
  location_owner_token_t owner,
  host::instance_spot_activation_materializer_t materializer)
{
    if (_node)
        throw configuration_error (
          "Instance Spot operations must be configured before MeshNode start");
    if (!store || !relocations || owner.owner_id.empty () || owner.lease_generation <= 0
        || !materializer)
        throw configuration_error ("Instance Spot operations require Location and Relocation "
                                   "Stores, an owner lease, and a materializer");
    _user_spot_store = std::move (store);
    _instance_spot_relocations = std::move (relocations);
    _instance_spot_owner = std::move (owner);
    _instance_spot_materializer = std::move (materializer);
}

void mesh_node_runtime_t::configure_relocation_runtime (
  std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority,
  std::shared_ptr<runtime::stateful::relocation_store_port_t> relocations,
  std::shared_ptr<runtime::stateful::aggregate_authority_port_t> aggregate_authority,
  runtime::stateful::relocation_limits_t relocation_limits)
{
    if (_node)
        throw configuration_error ("Relocation runtime must be configured before MeshNode start");
    if (!authority || !relocations)
        throw configuration_error ("Relocation runtime requires Location and Relocation Stores");
    if (relocation_limits.payload_chunk_limit_bytes == 0
        || relocation_limits.payload_chunk_limit_bytes
             > runtime::protocol::relocationChunkBytes
        || relocation_limits.cutover_wait_timeout
             <= std::chrono::milliseconds::zero ())
        throw configuration_error (
          "Relocation transfer options are out of range");
    _relocation_authority = std::move (authority);
    _relocation_store = std::move (relocations);
    _aggregate_relocation_authority = std::move (aggregate_authority);
    _relocation_limits = relocation_limits;
}

void mesh_node_runtime_t::configure_bound_session_relocation_resolver (
  std::function<std::optional<bound_session_relocation_route_t> (
    const runtime::stateful::object_ref_t &)> resolver)
{
    if (!resolver)
        throw configuration_error (
          "Bound Session relocation resolver is required");
    if (_node)
        throw configuration_error (
          "Bound Session relocation resolver must be configured before MeshNode start");
    _bound_session_relocation_resolver = std::move (resolver);
}

task_t<mesh_node_runtime_t::session_relocation_seal_outcome_t>
mesh_node_runtime_t::seal_bound_sessions (
  std::vector<std::pair<runtime::stateful::object_ref_t,
                        authority_snapshot_t>> participants,
  runtime::protocol::relocation_id_t relocation,
  runtime::protocol::relocation_coordinator_fence_t coordinator,
  std::chrono::milliseconds timeout)
{
    using runtime::foundation::operation_terminal_t;
    session_relocation_seal_outcome_t outcome;
    if (!_bound_session_relocation_resolver) {
        outcome.completed = true;
        co_return outcome;
    }
    if (!_node || timeout <= std::chrono::milliseconds::zero ())
        co_return outcome;

    const auto abort_prepared = [&] () -> task_t<bool> {
        if (outcome.checkpoints.empty ())
            co_return true;
        co_return co_await route_bound_sessions (
          outcome.checkpoints, {},
          runtime::protocol::session_relocation_route_action_t::abort);
    };

    for (const auto &[source, authority] : participants) {
        if (source.kind != runtime::stateful::object_kind_t::actor)
            continue;

        std::optional<bound_session_relocation_route_t> session;
        bool resolver_failed = false;
        try {
            session = _bound_session_relocation_resolver (source);
        }
        catch (const std::exception &) {
            resolver_failed = true;
        }
        catch (...) {
            resolver_failed = true;
        }
        if (resolver_failed) {
            outcome.recovery_required = !co_await abort_prepared ();
            co_return outcome;
        }
        if (!session)
            continue;
        const auto seal_boundary_sequence = session->observed_sequence;
        if (session->session_owner_node.to_bytes ().empty ()
            || session->session_owner_node_generation == 0
            || session->session_owner.owner_id.empty ()
            || session->session_owner.lease_generation <= 0
            || session->session.to_bytes ().empty ()
            || session->binding_generation == 0
            || authority.object_generation != source.object_generation
            || authority.authority_owner_generation
                 != source.authority_owner_generation
            || authority.owner.owner_id.empty ()
            || authority.owner.lease_generation <= 0) {
            outcome.recovery_required = !co_await abort_prepared ();
            co_return outcome;
        }

        const runtime::protocol::session_relocation_seal_t seal{
          relocation,
          coordinator,
          runtime::protocol::relocation_role_t::source,
          {source.key,
           source.object_generation,
           coordinator.node_routing_id,
           coordinator.node_generation,
           source.authority_owner_generation,
           static_cast<std::uint64_t> (
             authority.owner.lease_generation)},
          session->session_owner_node.to_bytes (),
          session->session_owner_node_generation,
          session->session_owner.owner_id,
          static_cast<std::uint64_t> (
            session->session_owner.lease_generation),
          session->session.to_bytes (),
          session->binding_generation};

        struct completion_t
        {
            operation_terminal_t terminal =
              operation_terminal_t::transport_failed;
            std::optional<host::session_relocation_seal_result_t> result;
        };
        auto completion = std::make_shared<detail::task_completion_source_t<
          completion_t>> ();
        auto completion_task = completion->task ();
        bool submitted = false;
        try {
            submitted = co_await _node->seal_session_remote (
              session->session_owner_node, seal, timeout,
              [seal] {
                  // The durable record keeps the exact seal request so
                  // recovery can reject a different binding or coordinator.
                  return runtime::protocol::encode_session_relocation_seal (
                    seal);
              },
              [completion] (
                operation_terminal_t terminal,
                std::optional<host::session_relocation_seal_result_t>
                  result) {
                  completion->complete (result_t<completion_t>::success (
                    {terminal, std::move (result)}));
              });
        }
        catch (...) {
            submitted = false;
        }
        if (!submitted) {
            outcome.recovery_required = !co_await abort_prepared ();
            co_return outcome;
        }
        const auto completed = co_await completion_task;
        if (completed.terminal != operation_terminal_t::completed
            || !completed.result) {
            // The request may have reached the owner even when its reply was
            // lost. The source cannot safely infer the seal state.
            outcome.recovery_required = true;
            co_return outcome;
        }

        auto sealed = *completed.result;
        bool converged = false;
        std::optional<bound_session_relocation_route_t> current;
        try {
            current = _bound_session_relocation_resolver (source);
        }
        catch (...) {
            current.reset ();
        }
        converged = current
                    && same_bound_session_relocation_identity (*session, *current)
                    && current->observed_sequence == seal_boundary_sequence;
        outcome.checkpoints.push_back (
          {source, authority, *session, std::move (sealed), seal_boundary_sequence});
        if (!converged) {
            outcome.recovery_required = !co_await abort_prepared ();
            co_return outcome;
        }
    }
    outcome.completed = true;
    co_return outcome;
}

task_t<std::optional<std::vector<runtime::protocol::session_relocation_route_t>>>
mesh_node_runtime_t::capture_session_routes (
  std::vector<std::pair<runtime::stateful::object_ref_t, authority_snapshot_t>> participants,
  runtime::protocol::relocation_id_t relocation,
  runtime::protocol::relocation_coordinator_fence_t coordinator,
  mesh_node_descriptor_t target,
  std::shared_ptr<session_relocation_seal_outcome_t> outcome,
  std::shared_ptr<bool> attempted)
{
    *attempted = true;
    *outcome = co_await seal_bound_sessions (
      participants, relocation, coordinator, std::chrono::seconds (5));
    if (!outcome->completed)
        co_return std::nullopt;
    std::vector<runtime::protocol::session_relocation_route_t> routes;
    routes.reserve (outcome->checkpoints.size ());
    for (const auto &checkpoint : outcome->checkpoints) {
        routes.push_back (make_session_relocation_route (
          checkpoint, target.rid, target.lifecycle_generation,
          runtime::protocol::session_relocation_route_action_t::commit));
    }
    co_return routes;
}

task_t<bool> mesh_node_runtime_t::route_bound_sessions (
  const std::vector<session_relocation_checkpoint_t> &checkpoints,
  const mesh_node_descriptor_t &target,
  runtime::protocol::session_relocation_route_action_t action)
{
    if (!_node)
        co_return checkpoints.empty ();
    for (const auto &checkpoint : checkpoints) {
        const auto route = make_session_relocation_route (
          checkpoint, target.rid, target.lifecycle_generation, action);
        try {
            if (!co_await _node->route_session_remote (
                  checkpoint.session.session_owner_node, route))
                co_return false;
        }
        catch (...) {
            co_return false;
        }
    }
    co_return true;
}

runtime::protocol::session_relocation_route_t
mesh_node_runtime_t::make_session_relocation_route (
  const session_relocation_checkpoint_t &checkpoint,
  const zlink::routing_id_t &target_node,
  std::uint64_t target_node_generation,
  runtime::protocol::session_relocation_route_action_t action) const
{
    const auto commit =
      action
      == runtime::protocol::session_relocation_route_action_t::commit;
    return runtime::protocol::session_relocation_route_t{
      checkpoint.seal.sealed.relocation,
      checkpoint.seal.sealed.coordinator,
      commit ? runtime::protocol::relocation_role_t::target
             : runtime::protocol::relocation_role_t::source,
      {checkpoint.source.key, checkpoint.source.object_generation},
      checkpoint.session.session_owner_node.to_bytes (),
      checkpoint.session.session_owner_node_generation,
      checkpoint.session.session_owner.owner_id,
      static_cast<std::uint64_t> (
        checkpoint.session.session_owner.lease_generation),
      checkpoint.session.session.to_bytes (),
      checkpoint.session.binding_generation,
      {action,
       commit ? checkpoint.source.authority_owner_generation : 0,
       commit ? checkpoint.source.authority_owner_generation + 1 : 0,
       commit ? target_node.to_bytes ()
              : std::vector<std::uint8_t>{},
       commit ? target_node_generation : 0,
       commit ? 0 : checkpoint.source.authority_owner_generation}};
}

task_t<runtime::stateful::relocation_result_t>
mesh_node_runtime_t::relocate_application_actor (const actor_ref_t &actor,
                                                 const mesh_node_descriptor_t &target,
                                                 const authority_snapshot_t &authority)
{
    const auto blocked = [] {
        return runtime::stateful::relocation_result_t{
          runtime::stateful::relocation_terminal_t::blocked,
          runtime::stateful::relocation_reason_t::restore_failed, std::nullopt};
    };
    if (!_node || target.lifecycle_generation == 0 || target.owner_id.empty ()
        || target.lease_generation <= 0)
        co_return blocked ();
    auto *maintenance = _node->maintenance ();
    const auto source = _node->resolve_actor (actor);
    const auto status = _node->status ();
    if (!maintenance || !source || authority.object_generation != source->object_generation
        || authority.authority_owner_generation != source->authority_owner_generation
        || authority.owner.owner_id.empty () || authority.owner.lease_generation <= 0
        || authority.store_version.empty ()
        || authority.allocation.target.node_rid.value () != status.routing_id ().to_string ())
        co_return blocked ();

    runtime::protocol::relocation_id_t relocation;
    try {
        relocation = relocation_ids ().issue ();
    }
    catch (...) {
        co_return blocked ();
    }
    const runtime::protocol::relocation_coordinator_fence_t coordinator{
      authority.owner.owner_id, static_cast<std::uint64_t> (authority.owner.lease_generation),
      status.routing_id ().to_bytes (), status.lifecycle_generation (), authority.store_version};
    auto session_seal = std::make_shared<session_relocation_seal_outcome_t> ();
    auto session_checkpoint_attempted = std::make_shared<bool> (false);

    runtime::stateful::eligible_relocation_unit_t::canonical_wire_context_t wire{
      .relocation = relocation,
      .target_attempt_generation = target.lifecycle_generation,
      .coordinator = coordinator,
      .target_node_routing_id = target.rid.to_bytes (),
      .target_node_generation = target.lifecycle_generation,
      .capture_session_routes =
        [this, source = *source, authority, relocation, coordinator,
         target, session_seal, session_checkpoint_attempted] () {
            return capture_session_routes (
              {{source, authority}}, relocation, coordinator, target,
              session_seal, session_checkpoint_attempted);
        },
      .prepare_target =
        [this, target, source_status = status, source = *source,
         stable_type =
           std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
         relocation, coordinator] (
          const std::vector<runtime::stateful::frozen_object_state_t> &,
          const runtime::stateful::relocation_payload_manifest_t &manifest)
          -> task_t<bool> {
            runtime::protocol::relocation_object_kind_t kind;
            switch (source.kind) {
                case runtime::stateful::object_kind_t::actor:
                    kind = runtime::protocol::relocation_object_kind_t::actor;
                    break;
                case runtime::stateful::object_kind_t::user_spot:
                    kind = runtime::protocol::relocation_object_kind_t::user_spot;
                    break;
                case runtime::stateful::object_kind_t::instance_spot:
                    kind = runtime::protocol::relocation_object_kind_t::instance_spot;
                    break;
                default:
                    co_return false;
            }
            co_return co_await _node->prepare_relocation_remote (
              target.rid,
              runtime::protocol::relocation_prepare_t{
                relocation,
                target.lifecycle_generation,
                coordinator,
                {target.rid.to_bytes (), target.lifecycle_generation, target.owner_id,
                 static_cast<std::uint64_t> (target.lease_generation)},
                runtime::protocol::relocation_role_t::source,
                {kind, stable_type, source.key, source.object_generation,
                 source.authority_owner_generation},
                source_status.routing_id ().to_bytes (),
                source_status.lifecycle_generation (),
                manifest.total_length,
                manifest.chunk_count,
                manifest.checksum_crc32c,
                static_cast<std::uint64_t> (
                  std::max<std::int64_t> (0, target.application_version))},
              std::chrono::seconds (5));
        },
      .send_state_chunk =
        [this, target] (const runtime::protocol::relocation_state_t &chunk)
          -> task_t<bool> {
            co_return co_await _node->transport ().send_relocation_control (
              target.rid.to_bytes (), chunk);
        },
      .send_relocation_data =
        [this, target] (
          const std::vector<runtime::protocol::relocation_data_t> &records,
          const runtime::stateful::relocation_ingress_batch_t &) -> task_t<bool> {
            for (const auto &record : records) {
                if (!co_await _node->transport ().send_relocation_control (
                      target.rid.to_bytes (), record))
                    co_return false;
            }
            co_return true;
        },
      .send_cutover =
        [this, target] (const runtime::protocol::relocation_cutover_t &cutover)
          -> task_t<runtime::stateful::eligible_relocation_unit_t::canonical_wire_context_t::cutover_enqueue_t> {
            using context_t = runtime::stateful::eligible_relocation_unit_t::
              canonical_wire_context_t;
            co_return co_await _node->cutover_relocation_remote (target.rid, cutover)
                     ? context_t::cutover_enqueue_t::enqueued
                     : context_t::cutover_enqueue_t::not_enqueued;
        },
      .abort_target_before_cutover = [] { return true; }};

    std::vector<std::byte> inventory_bytes;
    inventory_bytes.reserve (source->key.size () + sizeof (source->object_generation)
                             + sizeof (source->authority_owner_generation));
    for (const auto value : source->key)
        inventory_bytes.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
    for (int shift = 56; shift >= 0; shift -= 8) {
        inventory_bytes.push_back (
          static_cast<std::byte> ((source->object_generation >> shift) & 0xffu));
        inventory_bytes.push_back (
          static_cast<std::byte> ((source->authority_owner_generation >> shift) & 0xffu));
    }
    const auto public_digest = runtime::sha256 (inventory_bytes);
    runtime::stateful::inventory_digest_t inventory_digest{};
    for (std::size_t index = 0; index != inventory_digest.size (); ++index)
        inventory_digest[index] = std::to_integer<std::uint8_t> (public_digest[index]);

    auto result = co_await maintenance->relocate (
      *source, target.rid.to_string (), {target.owner_id, target.lease_generation},
      256u * 1024u * 1024u, inventory_digest, wire);
    if (*session_checkpoint_attempted && !session_seal->completed) {
        result.terminal = session_seal->recovery_required
                            ? runtime::stateful::relocation_terminal_t::
                                recovery_required
                            : runtime::stateful::relocation_terminal_t::
                                blocked;
        result.reason = runtime::stateful::relocation_reason_t::
          bound_session_fence_incomplete;
        co_return result;
    }
    if (result.terminal
          == runtime::stateful::relocation_terminal_t::blocked
               && !co_await route_bound_sessions (
                 session_seal->checkpoints, {},
               runtime::protocol::session_relocation_route_action_t::abort)) {
        result.terminal =
          runtime::stateful::relocation_terminal_t::recovery_required;
        result.reason =
          runtime::stateful::relocation_reason_t::
            bound_session_fence_incomplete;
    }
    co_return result;
}

bool mesh_node_runtime_t::application_actor_transfer_in_progress (const actor_ref_t &actor) const
{
    return spot_node_runtime_t (_state->spot_state).actor_transfer_in_progress (actor);
}

result_t<bool> mesh_node_runtime_t::destroy_application_actor (const actor_ref_t &actor)
{
    const auto destroyed = spot_node_runtime_t (_state->spot_state).destroy_actor (actor);
    if (!destroyed)
        return destroyed;
    const auto stateful = cleanup_application_actor_stateful (actor);
    if (!stateful)
        return result_t<bool>::failure (stateful.error_kind (),
                                        stateful.error () ? stateful.error ()->what ()
                                                          : "stateful Actor cleanup failed");
    return result_t<bool>::success (true);
}

result_t<void> mesh_node_runtime_t::cleanup_application_actor_stateful (const actor_ref_t &actor)
{
    if (_node) {
        const auto stateful =
          _node->destroy_application_actor (actor.actor_id ().value (), actor.object_generation ());
        if (stateful != runtime::stateful::stateful_error_t::none
            && stateful != runtime::stateful::stateful_error_t::not_found)
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "stateful Actor cleanup failed");
        _actors.erase (std::string (actor.actor_id ().value ()));
    }
    return result_t<void>::success ();
}

task_t<runtime::stateful::aggregate_relocation_result_t>
mesh_node_runtime_t::relocate_application_unit (
  std::vector<runtime::stateful::object_ref_t> sources,
  std::vector<std::string> stable_types,
  const mesh_node_descriptor_t &target,
  const std::vector<authority_snapshot_t> &authorities)
{
    using namespace runtime::stateful;
    const auto blocked = [] {
        return runtime::stateful::aggregate_relocation_result_t{
          runtime::stateful::relocation_terminal_t::blocked,
          runtime::stateful::relocation_reason_t::restore_failed,
          {}};
    };
    if (!_node || sources.empty () || sources.size () != stable_types.size ()
        || sources.size () != authorities.size ()
        || target.lifecycle_generation == 0 || target.owner_id.empty ()
        || target.lease_generation <= 0)
        co_return blocked ();
    auto *maintenance = _node->maintenance ();
    const auto status = _node->status ();
    if (!maintenance)
        co_return blocked ();
    for (std::size_t index = 0; index != sources.size (); ++index) {
        if (stable_types[index].empty ()
            || authorities[index].object_generation != sources[index].object_generation
            || authorities[index].authority_owner_generation
                 != sources[index].authority_owner_generation
            || authorities[index].owner.owner_id.empty ()
            || authorities[index].owner.lease_generation <= 0
            || authorities[index].store_version.empty ()
            || authorities[index].allocation.target.node_rid.value ()
                 != status.routing_id ().to_string ())
            co_return blocked ();
    }

    struct participant_input_t
    {
        object_ref_t source;
        std::string stable_type;
        authority_snapshot_t authority;
    };
    std::vector<participant_input_t> input;
    input.reserve (sources.size ());
    for (std::size_t index = 0; index != sources.size (); ++index) {
        input.push_back ({std::move (sources[index]), std::move (stable_types[index]),
                          authorities[index]});
    }
    std::sort (input.begin (), input.end (), [] (const auto &left, const auto &right) {
        if (left.source.kind != right.source.kind)
            return left.source.kind < right.source.kind;
        return left.source.key < right.source.key;
    });
    sources.clear ();
    stable_types.clear ();
    for (std::size_t index = 0; index != input.size (); ++index) {
        sources.push_back (input[index].source);
        stable_types.push_back (input[index].stable_type);
    }
    const auto principal =
      std::find_if (input.begin (), input.end (), [] (const auto &participant) {
          return participant.source.kind == object_kind_t::user_spot;
      });
    const auto principal_index =
      principal != input.end ()
        ? static_cast<std::size_t> (std::distance (input.begin (), principal))
        : 0u;

    runtime::protocol::relocation_id_t relocation;
    try {
        relocation = relocation_ids ().issue ();
    }
    catch (...) {
        co_return blocked ();
    }
    const auto &coordinator_authority = input[principal_index].authority;
    const runtime::protocol::relocation_coordinator_fence_t coordinator{
      coordinator_authority.owner.owner_id,
      static_cast<std::uint64_t> (coordinator_authority.owner.lease_generation),
      status.routing_id ().to_bytes (), status.lifecycle_generation (),
      coordinator_authority.store_version};
    std::vector<std::pair<object_ref_t, authority_snapshot_t>>
      session_participants;
    session_participants.reserve (input.size ());
    for (const auto &participant : input)
        session_participants.emplace_back (
          participant.source, participant.authority);
    auto session_seal = std::make_shared<session_relocation_seal_outcome_t> ();
    auto session_checkpoint_attempted = std::make_shared<bool> (false);
    eligible_relocation_unit_t::canonical_wire_context_t wire{
      .relocation = relocation,
      .target_attempt_generation = target.lifecycle_generation,
      .coordinator = coordinator,
      .target_node_routing_id = target.rid.to_bytes (),
      .target_node_generation = target.lifecycle_generation,
      .capture_session_routes =
        [this, session_participants, relocation, coordinator, target,
         session_seal, session_checkpoint_attempted] () {
            return capture_session_routes (
              session_participants, relocation, coordinator, target,
              session_seal, session_checkpoint_attempted);
        },
      .prepare_target =
        [this, target, status, sources, stable_types, principal_index,
         relocation, coordinator] (
          const std::vector<frozen_object_state_t> &,
          const runtime::stateful::relocation_payload_manifest_t &manifest)
          -> task_t<bool> {
            runtime::protocol::relocation_object_kind_t kind;
            switch (sources[principal_index].kind) {
                case object_kind_t::actor:
                    kind = runtime::protocol::relocation_object_kind_t::actor;
                    break;
                case object_kind_t::user_spot:
                    kind = runtime::protocol::relocation_object_kind_t::user_spot;
                    break;
                case object_kind_t::instance_spot:
                    kind = runtime::protocol::relocation_object_kind_t::instance_spot;
                    break;
                default:
                    co_return false;
            }
            co_return co_await _node->prepare_relocation_remote (
              target.rid,
              runtime::protocol::relocation_prepare_t{
                relocation,
                target.lifecycle_generation,
                coordinator,
                {target.rid.to_bytes (), target.lifecycle_generation, target.owner_id,
                 static_cast<std::uint64_t> (target.lease_generation)},
                runtime::protocol::relocation_role_t::source,
                {kind, stable_types[principal_index], sources[principal_index].key,
                 sources[principal_index].object_generation,
                 sources[principal_index].authority_owner_generation},
                status.routing_id ().to_bytes (),
                status.lifecycle_generation (),
                manifest.total_length,
                manifest.chunk_count,
                manifest.checksum_crc32c,
                static_cast<std::uint64_t> (
                  std::max<std::int64_t> (0, target.application_version))},
              std::chrono::seconds (5));
        },
      .send_state_chunk =
        [this, target] (const runtime::protocol::relocation_state_t &chunk)
          -> task_t<bool> {
            co_return co_await _node->transport ().send_relocation_control (
              target.rid.to_bytes (), chunk);
        },
      .send_relocation_data =
        [this, target] (
          const std::vector<runtime::protocol::relocation_data_t> &records,
          const relocation_ingress_batch_t &) -> task_t<bool> {
            for (const auto &record : records) {
                if (!co_await _node->transport ().send_relocation_control (
                      target.rid.to_bytes (), record))
                    co_return false;
            }
            co_return true;
        },
      .send_cutover =
        [this, target] (const runtime::protocol::relocation_cutover_t &cutover)
          -> task_t<eligible_relocation_unit_t::canonical_wire_context_t::cutover_enqueue_t> {
            using context_t = eligible_relocation_unit_t::canonical_wire_context_t;
            co_return co_await _node->cutover_relocation_remote (target.rid, cutover)
                     ? context_t::cutover_enqueue_t::enqueued
                     : context_t::cutover_enqueue_t::not_enqueued;
        },
      .abort_target_before_cutover = [] { return true; }};

    std::vector<std::byte> inventory;
    for (const auto &source : sources) {
        for (const auto value : source.key)
            inventory.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
        for (int shift = 56; shift >= 0; shift -= 8) {
            inventory.push_back (static_cast<std::byte> (source.object_generation >> shift));
            inventory.push_back (
              static_cast<std::byte> (source.authority_owner_generation >> shift));
        }
    }
    const auto public_digest = runtime::sha256 (inventory);
    runtime::stateful::inventory_digest_t digest{};
    for (std::size_t index = 0; index != digest.size (); ++index)
        digest[index] = std::to_integer<std::uint8_t> (public_digest[index]);
    if (sources.size () == 1) {
        auto result = co_await maintenance->relocate (
          sources.front (), target.rid.to_string (), {target.owner_id, target.lease_generation},
          256u * 1024u * 1024u, digest, wire);
        if (*session_checkpoint_attempted && !session_seal->completed) {
            result.terminal = session_seal->recovery_required
                                ? relocation_terminal_t::recovery_required
                                : relocation_terminal_t::blocked;
            result.reason = runtime::stateful::relocation_reason_t::
              bound_session_fence_incomplete;
        }
        if (result.terminal == relocation_terminal_t::blocked
                 && !co_await route_bound_sessions (
                   session_seal->checkpoints, {},
                   runtime::protocol::
                     session_relocation_route_action_t::abort)) {
            result.terminal = relocation_terminal_t::recovery_required;
            result.reason =
              runtime::stateful::relocation_reason_t::
                bound_session_fence_incomplete;
        }
        co_return runtime::stateful::aggregate_relocation_result_t{
          result.terminal, result.reason, {}, result.replay_records, result.target_handoff};
    }
    auto result = co_await maintenance->relocate_aggregate (
      sources, target.rid.to_string (), {target.owner_id, target.lease_generation},
      256u * 1024u * 1024u, digest, wire);
    if (*session_checkpoint_attempted && !session_seal->completed) {
        result.terminal = session_seal->recovery_required
                            ? relocation_terminal_t::recovery_required
                            : relocation_terminal_t::blocked;
        result.reason = runtime::stateful::relocation_reason_t::
          bound_session_fence_incomplete;
    }
    if (result.terminal == relocation_terminal_t::blocked
             && !co_await route_bound_sessions (
               session_seal->checkpoints, {},
               runtime::protocol::session_relocation_route_action_t::abort)) {
        result.terminal = relocation_terminal_t::recovery_required;
        result.reason =
          runtime::stateful::relocation_reason_t::
            bound_session_fence_incomplete;
    }
    co_return result;
}

void mesh_node_runtime_t::configure_session_route_owner (
  std::function<std::optional<location_owner_token_t> ()> owner_resolver)
{
    if (!owner_resolver)
        throw configuration_error ("Session route owner resolver is required");
    _session_route_owner_resolver = std::move (owner_resolver);
    if (_node)
        _node->configure_session_route_owner (_session_route_owner_resolver);
}

void mesh_node_runtime_t::configure_session_route_target_owner (
  host::public_host_runtime_t::session_route_target_owner_resolver_t
    owner_resolver)
{
    if (!owner_resolver)
        throw std::invalid_argument (
          "Session route target owner resolver is required");
    _session_route_target_owner_resolver =
      std::move (owner_resolver);
    if (_node) {
        _node->configure_session_route_target_owner (
          _session_route_target_owner_resolver);
    }
}

void mesh_node_runtime_t::configure_stateful_dispatch (
  runtime::stateful::accepted_record_authority_resolver_t resolver)
{
    if (!resolver)
        throw configuration_error ("Stateful dispatch authority resolver is required");
    if (_node)
        throw configuration_error ("Stateful dispatch must be configured before MeshNode start");
    _stateful_dispatch_resolver = std::move (resolver);
}

void mesh_node_runtime_t::configure_bound_session_operations (
  host::bound_session_operations_t operations)
{
    if (!operations.bind || !operations.send || !operations.replaced)
        throw configuration_error ("Bound Session operations must all be configured");
    if (_node)
        throw configuration_error (
          "Bound Session operations must be configured before MeshNode start");
    _bound_session_operations = std::move (operations);
}

mesh_node_runtime_t::message_follow_subscription_id_t
mesh_node_runtime_t::subscribe_message_follow_invalidation (
  std::function<void (const runtime::protocol::message_follow_notice_t &)> handler)
{
    if (!handler)
        throw std::invalid_argument ("Message Follow invalidation handler is required");
    auto state =
      std::make_shared<message_follow_subscription_state_t> (std::move (handler));
    std::lock_guard lock (_message_follow_mutex);
    auto subscription_id = _next_message_follow_subscription_id++;
    while (subscription_id == 0
           || _message_follow_subscriptions.contains (subscription_id)) {
        subscription_id = _next_message_follow_subscription_id++;
    }
    _message_follow_subscriptions.emplace (subscription_id, std::move (state));
    return subscription_id;
}

void mesh_node_runtime_t::unsubscribe_message_follow_invalidation (
  message_follow_subscription_id_t subscription_id) noexcept
{
    std::shared_ptr<message_follow_subscription_state_t> state;
    {
        std::lock_guard lock (_message_follow_mutex);
        const auto found = _message_follow_subscriptions.find (subscription_id);
        if (found == _message_follow_subscriptions.end ())
            return;
        state = std::move (found->second);
        _message_follow_subscriptions.erase (found);
    }
    state->deactivate_and_wait ();
}

void mesh_node_runtime_t::dispatch_message_follow (
  const runtime::protocol::message_follow_notice_t &notice)
{
    spot_node_runtime_t spot (_state->spot_state);
    spot.invalidate_message_follow_route (notice);
    std::vector<std::shared_ptr<message_follow_subscription_state_t>> subscriptions;
    std::function<void (const runtime::protocol::actor_route_fence_t &)> actor_invalidator;
    {
        std::lock_guard lock (_message_follow_mutex);
        subscriptions.reserve (_message_follow_subscriptions.size ());
        for (const auto &[_, subscription] : _message_follow_subscriptions)
            subscriptions.push_back (subscription);
        actor_invalidator = _actor_route_invalidator;
    }
    if (actor_invalidator) {
        if (const auto *source =
              std::get_if<runtime::protocol::actor_route_fence_t> (&notice.source)) {
            try {
                actor_invalidator (*source);
            }
            catch (...) {
            }
        }
    }
    for (const auto &subscription : subscriptions) {
        if (!subscription->begin_dispatch ())
            continue;
        struct dispatch_guard_t
        {
            message_follow_subscription_state_t *state;
            message_follow_subscription_state_t::dispatch_frame_t frame;

            explicit dispatch_guard_t (
              message_follow_subscription_state_t *value) :
                state (value),
                frame{value,
                      message_follow_subscription_state_t::current_dispatch}
            {
                message_follow_subscription_state_t::current_dispatch = &frame;
            }

            ~dispatch_guard_t ()
            {
                message_follow_subscription_state_t::current_dispatch = frame.previous;
                state->finish_dispatch ();
            }
        } dispatch_guard (subscription.get ());
        try {
            subscription->handler (notice);
        }
        catch (...) {
        }
    }
}

task_t<bool> mesh_node_runtime_t::activate_instance_spot_remote (
  const zlink::routing_id_t &target_node,
  runtime::protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  runtime::protocol::application_payload_t application_payload,
  std::chrono::milliseconds timeout,
  host::instance_spot_activation_completion_t completion)
{
    if (!_node)
        co_return false;
    co_return co_await _node->activate_instance_spot_remote (
      target_node, std::move (request), std::move (metadata), std::move (application_payload),
      timeout, std::move (completion));
}

task_t<bool> mesh_node_runtime_t::send_instance_spot_activation_remote (
  const zlink::routing_id_t &target_node,
  runtime::protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  runtime::protocol::application_payload_t application_payload)
{
    if (!_node)
        co_return false;
    co_return co_await _node->send_instance_spot_activation_remote (
      target_node, std::move (request), std::move (metadata), std::move (application_payload));
}

void mesh_node_runtime_t::stop () noexcept
{
    /* Seal new submissions first, then let every synchronous operation that
     * is already waiting on a completion settle at its original deadline.
     * The hosted-service drain also waits for callback-owned reservations.
     * Publishing the stopping terminal before this barrier would turn an
     * admitted Message Follow reply into shutdown and clear its slot. */
    {
        std::unique_lock completion_lock (_completion_mutex);
        _completion_ready.wait (completion_lock, [this] {
            return _active_completion_waiters.load (
                     std::memory_order_acquire)
                   == 0;
        });
    }
    _stopping.store (true, std::memory_order_release);
    _completion_ready.notify_all ();
    {
        std::lock_guard state_lock (_state->mutex);
        _state->runtime_peer_connect = {};
        _state->runtime_peer_disconnect = {};
    }
    {
        std::unique_lock callback_lock (_peer_callback_gate->mutex);
        _peer_callback_gate->stopping = true;
        _peer_callback_gate->changed.wait (callback_lock,
                                           [this] { return _peer_callback_gate->active == 0; });
    }
    std::vector<std::shared_ptr<detail::task_completion_source_t<operation_completion_t>>>
      completion_awaiters;
    {
        std::lock_guard lock (_completion_mutex);
        _actor_join_continuations.clear ();
        _completed_operations.clear ();
        _timed_out_operations.clear ();
        _timed_out_operation_order.clear ();
        _completion_overflow_operations.clear ();
        _completion_overflow_order.clear ();
        for (auto &[_, awaiter] : _completion_awaiters)
            completion_awaiters.push_back (std::move (awaiter));
        _completion_awaiters.clear ();
    }
    for (auto &awaiter : completion_awaiters) {
        awaiter->complete (detail::boundary_failure<operation_completion_t> (
          detail::boundary_error_t::shutdown,
          "MeshNode operation stopped because the runtime is shutting down"));
    }
    if (!_node) {
        return;
    }
    try {
        {
            std::lock_guard lock (_peer_mutex);
            _peer_connection_intents.clear ();
        }
        _actors.clear ();
        for (auto &[_, spot] : _spots)
            (void) spot.close ();
        _spots.clear ();
        spot_node_runtime_t (_state->spot_state).detach_native_node ();
        _node->close ();
    }
    catch (...) {
    }
    _node.reset ();
}

void mesh_node_runtime_t::connect_peer (const zlink::routing_id_t &expected_routing_id,
                                        const std::string &endpoint,
                                        std::uint64_t expected_lifecycle_generation,
                                        std::string security_identity)
{
    if (!_node || endpoint.empty ())
        return;
    std::lock_guard lock (_peer_mutex);
    if (const auto existing = _peer_connection_intents.find (endpoint);
        existing != _peer_connection_intents.end ())
        return;
    const auto submitted = _node->connect_peer (
      endpoint, expected_routing_id, expected_lifecycle_generation, std::move (security_identity));
    if (submitted)
        _peer_connection_intents.emplace (endpoint, next_connection_intent_id ());
}

void mesh_node_runtime_t::connect_peer (const std::string &endpoint, std::string security_identity)
{
    if (!_node || endpoint.empty ())
        return;
    std::lock_guard lock (_peer_mutex);
    if (_peer_connection_intents.contains (endpoint))
        return;
    if (_node->connect_peer (endpoint))
        _peer_connection_intents.emplace (endpoint, next_connection_intent_id ());
    static_cast<void> (security_identity);
}

void mesh_node_runtime_t::expect_peer (const zlink::routing_id_t &expected_routing_id,
                                       const std::string &endpoint,
                                       std::uint64_t expected_lifecycle_generation,
                                       std::string security_identity)
{
    if (!_node || endpoint.empty ())
        return;
    _node->expect_peer (endpoint, expected_routing_id, expected_lifecycle_generation,
                        std::move (security_identity));
}

void mesh_node_runtime_t::forget_peer (const zlink::routing_id_t &expected_routing_id,
                                       const std::string &endpoint)
{
    if (!_node || endpoint.empty ())
        return;
    _node->forget_peer (endpoint, expected_routing_id);
}

void mesh_node_runtime_t::disconnect_peer (const std::string &endpoint) noexcept
{
    if (!_node || endpoint.empty ())
        return;
    try {
        std::lock_guard lock (_peer_mutex);
        const auto found = _peer_connection_intents.find (endpoint);
        _node->disconnect_peer (endpoint);
        if (found != _peer_connection_intents.end ())
            _peer_connection_intents.erase (found);
    }
    catch (...) {
    }
}

void mesh_node_runtime_t::disconnect_peer (const zlink::routing_id_t &expected_routing_id,
                                           const std::string &endpoint) noexcept
{
    if (!_node || endpoint.empty ())
        return;
    try {
        std::lock_guard lock (_peer_mutex);
        _node->disconnect_peer (expected_routing_id.to_bytes (), endpoint);
        _peer_connection_intents.erase (endpoint);
    }
    catch (...) {
    }
}

bool mesh_node_runtime_t::wait_for_peer_ready (const zlink::routing_id_t &target,
                                               std::chrono::milliseconds timeout) const
{
    if (!_node || timeout <= std::chrono::milliseconds::zero ())
        return false;
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    const auto routing_id = target.to_bytes ();
    do {
        if (_node->transport ().topology ().peer (routing_id)) {
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    } while (std::chrono::steady_clock::now () < deadline);
    return _node->transport ().topology ().peer (routing_id).has_value ();
}

host::spot_handle_t mesh_node_runtime_t::get_or_create_spot (std::string spot_id)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    const auto &key = spot_id;
    if (const auto found = _spots.find (key); found != _spots.end ())
        return found->second;
    auto spot = _node->get_or_create_spot (spot_id);
    _spots.emplace (key, spot);
    return spot;
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::send_to_spot (const std::string &source_spot_id,
                                   const zlink::routing_id_t &target_node_rid,
                                   const std::string &target_spot_id,
                                   std::uint64_t target_spot_generation,
                                   const std::vector<zlink::message_t> &parts,
                                   std::vector<std::uint8_t> metadata)
{
    co_return co_await get_or_create_spot (source_spot_id)
      .send_to_spot (target_node_rid, target_spot_id, target_spot_generation, parts,
                     zlink::send_flags_t::dontwait, metadata);
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::request_to_spot (const std::string &source_spot_id,
                                      const zlink::routing_id_t &target_node_rid,
                                      const std::string &target_spot_id,
                                      std::uint64_t target_spot_generation,
                                      const std::vector<zlink::message_t> &parts,
                                      host::call_id_t &operation_id,
                                      std::chrono::milliseconds timeout,
                                      std::vector<std::uint8_t> metadata)
{
    co_return co_await get_or_create_spot (source_spot_id)
      .request_to_spot (target_node_rid, target_spot_id, target_spot_generation, parts,
                        operation_id, zlink::send_flags_t::none, timeout, metadata);
}

host::actor_handle_t
mesh_node_runtime_t::create_actor (std::string actor_type,
                                   std::string actor_id,
                                   const std::vector<zlink::message_t> &creation_parts,
                                   std::chrono::milliseconds timeout)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    if (actor_id.empty ()) {
        throw configuration_error ("actor id is required");
    }
    if (const auto found = _actors.find (actor_id); found != _actors.end ())
        return found->second;
    (void) creation_parts;
    (void) timeout;
    auto actor = _node->create_actor (std::move (actor_type), actor_id);
    _actors.emplace (std::move (actor_id), actor);
    return actor;
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::send_to_actor (const actor_ref_t &target,
                                    const std::vector<zlink::message_t> &parts,
                                    std::vector<std::uint8_t> metadata,
                                    std::uint64_t authority_owner_generation,
                                    std::uint64_t owner_lease_generation,
                                    std::optional<runtime::protocol::actor_message_header_t::bound_session_source_t>
                                      bound_session_source)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    co_return co_await _node->send_to_actor (
      target, parts, metadata, authority_owner_generation,
      owner_lease_generation, std::move (bound_session_source));
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::request_to_actor (const actor_ref_t &target,
                                       const std::vector<zlink::message_t> &parts,
                                       host::call_id_t &operation_id,
                                       std::chrono::milliseconds timeout,
                                       std::vector<std::uint8_t> metadata,
                                       std::uint64_t authority_owner_generation,
                                       std::uint64_t owner_lease_generation,
                                       std::optional<runtime::protocol::actor_message_header_t::bound_session_source_t>
                                         bound_session_source)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    co_return co_await _node->request_to_actor (
      target, parts, operation_id, timeout, metadata,
      authority_owner_generation, owner_lease_generation,
      std::move (bound_session_source));
}

zlink::context_t &mesh_node_runtime_t::native_context ()
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    return _node->transport ().context ();
}

std::size_t mesh_node_runtime_t::admitted_peer_count () const
{
    if (!_node) {
        return 0;
    }
    return _node->transport ().topology ().peers ().size ();
}

bool mesh_node_runtime_t::has_admitted_peer (const zlink::routing_id_t &peer_rid,
                                             std::uint64_t lifecycle_generation) const
{
    if (!_node || lifecycle_generation == 0)
        return false;
    const auto peer = _node->transport ().topology ().peer (peer_rid.to_bytes ());
    return peer && peer->descriptor.lifecycle_generation == lifecycle_generation
           && peer->descriptor.state == runtime::mesh::service_node_state_t::serving;
}

bool mesh_node_runtime_t::has_admitted_peer (const zlink::routing_id_t &peer_rid) const
{
    if (!_node)
        return false;
    const auto peer = _node->transport ().topology ().peer (peer_rid.to_bytes ());
    return peer && peer->descriptor.state == runtime::mesh::service_node_state_t::serving;
}

host::public_host_runtime_t &mesh_node_runtime_t::native_node ()
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    return *_node;
}

bool mesh_node_runtime_t::prepare_actor_transfer (const host::actor_transfer_prepare_t &prepare,
                                                  std::chrono::milliseconds timeout,
                                                  host::actor_transfer_token_t &token,
                                                  host::actor_transfer_prepare_result_t &result)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    (void) timeout;
    return _node->prepare_actor_transfer (prepare, token, result);
}

result_t<actor_ref_t> mesh_node_runtime_t::create_application_actor (
  std::string actor_type,
  std::string actor_id,
  const std::optional<zlink::message_t> &creation_payload,
  std::chrono::milliseconds timeout)
{
    {
        std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
        _state->spot_state->actor_types_by_id[actor_id] = actor_type;
        _state->spot_state->mesh_runtime_owned_native_actor_ids.insert (actor_id);
    }
    try {
        std::vector<zlink::message_t> parts;
        if (creation_payload)
            parts.push_back (*creation_payload);
        auto native = create_actor (actor_type, actor_id, parts, timeout);
        {
            std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
            _state->spot_state->core_actor_membership_epochs.try_emplace (actor_id, 1);
        }
        return result_t<actor_ref_t>::success (native.ref ());
    }
    catch (const std::exception &error) {
        std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
        _state->spot_state->actor_types_by_id.erase (actor_id);
        _state->spot_state->mesh_runtime_owned_native_actor_ids.erase (actor_id);
        return result_t<actor_ref_t>::failure (framework_error_kind_t::internal_failure,
                                               error.what ());
    }
}

result_t<actor_ref_t> mesh_node_runtime_t::create_application_actor (
  std::string actor_type,
  std::string actor_id,
  const std::optional<zlink::message_t> &creation_payload,
  std::uint64_t object_generation,
  std::uint64_t authority_owner_generation,
  std::chrono::milliseconds timeout)
{
    {
        std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
        _state->spot_state->actor_types_by_id[actor_id] = actor_type;
        _state->spot_state->mesh_runtime_owned_native_actor_ids.insert (actor_id);
    }
    try {
        std::vector<zlink::message_t> parts;
        if (creation_payload)
            parts.push_back (*creation_payload);
        if (!_node)
            throw configuration_error ("MeshNode has not started");
        auto native = _node->create_reserved_actor (
          actor_type,
          runtime::stateful::object_ref_t{runtime::stateful::object_kind_t::actor, actor_id,
                                          object_generation, authority_owner_generation,
                                          _state->mesh_name, _state->routing_id->to_string ()});
        _actors.insert_or_assign (actor_id, native);
        {
            std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
            _state->spot_state->core_actor_membership_epochs.try_emplace (actor_id, 1);
        }
        return result_t<actor_ref_t>::success (native.ref ());
    }
    catch (const std::exception &error) {
        std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
        _state->spot_state->actor_types_by_id.erase (actor_id);
        _state->spot_state->mesh_runtime_owned_native_actor_ids.erase (actor_id);
        return result_t<actor_ref_t>::failure (framework_error_kind_t::internal_failure,
                                               error.what ());
    }
}

result_t<actor_join_reply_t>
mesh_node_runtime_t::join_application_actor_to_entry_spot (const actor_ref_t &actor,
                                                           const node_rid_t &target_node,
                                                           const zlink::message_t &request,
                                                           std::chrono::milliseconds timeout)
{
    const auto found = _actors.find (std::string (actor.actor_id ().value ()));
    if (found == _actors.end ()) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "local Actor handle was not found");
    }
    host::call_id_t operation;
    const std::vector<zlink::message_t> parts{request};
    const auto submitted = found->second.join_entry_spot (
      zlink::routing_id_t::from (std::string (target_node.value ())), parts, operation, timeout);
    if (submitted != zlink::submit_result_t::ok) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      "Actor entry Spot join was not submitted");
    }
    auto joined = wait_for_join_completion (operation, actor, timeout);
    return joined;
}

result_t<void>
mesh_node_runtime_t::submit_application_actor_entry_spot_join (const actor_ref_t &actor,
                                                               const node_rid_t &target_node,
                                                               const zlink::message_t &request,
                                                               std::chrono::milliseconds timeout,
                                                               actor_join_completion_t completion)
{
    if (!completion)
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "Actor entry Spot join completion is required");
    const auto found = _actors.find (std::string (actor.actor_id ().value ()));
    if (found == _actors.end ())
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "local Actor handle was not found");

    host::call_id_t operation;
    const std::vector<zlink::message_t> parts{request};
    std::unique_lock lock (_completion_mutex);
    const auto submitted = found->second.join_entry_spot (
      zlink::routing_id_t::from (std::string (target_node.value ())), parts, operation, timeout);
    if (submitted != zlink::submit_result_t::ok)
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "Actor entry Spot join was not submitted");
    const auto [_, inserted] = _actor_join_continuations.emplace (
      operation, actor_join_continuation_t{actor, std::move (completion)});
    if (!inserted)
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "Actor entry Spot join operation was duplicated");
    return result_t<void>::success ();
}

bool mesh_node_runtime_t::complete_application_actor_entry_spot_join (
  const host::receive_record_t &record, const std::vector<zlink::message_t> &parts)
{
    actor_join_completion_t completion;
    std::optional<actor_ref_t> actor;
    {
        std::lock_guard lock (_completion_mutex);
        const auto found = _actor_join_continuations.find (record.operation_id);
        if (found == _actor_join_continuations.end ())
            return false;
        actor = found->second.actor;
        completion = std::move (found->second.completion);
        _actor_join_continuations.erase (found);
        (void) _completed_operations.erase (record.operation_id);
    }
    completion (actor_join_reply_from_completion (record, parts, *actor));
    return true;
}

task_t<runtime::messaging::message_parts_t>
mesh_node_runtime_t::request_actor_join_spot_route (
  const runtime::spot_address_t &target,
  runtime::messaging::message_parts_t encoded,
  std::chrono::milliseconds timeout)
{
    auto origin = get_or_create_spot ("__zlink-route-origin-" + routing_id ()->to_hex ());
    host::call_id_t operation;
    const auto submitted = co_await origin.request_to_spot (
      target.node_rid, spot_id_t (target.spot_id), target.object_generation, encoded.items (),
      operation, zlink::send_flags_t::none, timeout);
    if (submitted != zlink::submit_result_t::ok) {
        co_return result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::internal_failure,
          "Actor transfer route request was not submitted");
    }
    auto completed = co_await await_completion (operation);
    if (completed.record.terminal_result
        != static_cast<int> (zlink::request_result_t::ok)) {
        //  Classify the remote reply terminal + fine failure code rather than
        //  collapsing every non-OK completion to InternalFailure (spec 32 §5).
        const runtime::messaging::request_failure_mapper_t failure_mapper;
        const auto failure = failure_mapper.reply_header_exception (
          completed.record.terminal_result,
          completed.record.failure_errno,
          "Actor transfer route request");
        co_return result_t<runtime::messaging::message_parts_t>::failure (
          failure.kind (), failure.what ());
    }
    co_return runtime::messaging::message_parts_t (std::move (completed.parts));
}

struct mesh_node_runtime_t::remote_actor_join_state_t
{
    actor_ref_t actor;
    runtime::spot_address_t target;
    zlink::message_t request;
    std::chrono::milliseconds timeout;
    std::optional<zlink::routing_id_t> bound_session_node_rid;
    std::optional<zlink::routing_id_t> bound_session_rid;
    std::optional<spot_id_t> source_spot;
    std::uint64_t source_spot_generation = 0;
    runtime::stateful::object_ref_t source_actor;
    std::optional<authority_snapshot_t> source_authority;
    std::vector<std::uint8_t> source_node_rid;
    std::uint64_t source_node_generation = 0;
    std::string source_mesh_name;
    std::uint64_t source_owner_lease_generation = 0;
    std::string transfer_id;
    std::uint64_t completion_operation_id_high = 0;
    std::uint64_t completion_operation_id_low = 0;
    std::uint64_t actor_authority_owner_generation = 0;
    std::vector<std::uint8_t> admission_payload;
    std::string completion_root_reference;
    std::uint32_t completion_root_checksum = 0;
    session_relocation_seal_outcome_t session_seal;
    std::vector<std::uint8_t> encoded_session_relocation_route;
    std::vector<std::uint8_t> transfer_state;
    std::uint64_t membership_epoch = 1;
    host::actor_transfer_token_t core_token;
    std::chrono::steady_clock::time_point deadline;
};

task_t<actor_join_reply_t> mesh_node_runtime_t::join_application_actor_to_spot (
  actor_ref_t actor,
  const runtime::spot_address_t &target,
  const zlink::message_t &request,
  std::chrono::milliseconds timeout,
  std::optional<zlink::routing_id_t> bound_session_node_rid,
  std::optional<zlink::routing_id_t> bound_session_rid)
{
    spot_node_runtime_t spot_runtime (_state->spot_state);
    const auto completion_source_spot = spot_runtime.actor_spot (actor);
    auto deliver_completion = [&] (std::uint64_t operation_high, std::uint64_t operation_low,
                                   const result_t<actor_join_reply_t> &joined) -> result_t<void> {
        if (!joined) {
            return spot_runtime.deliver_actor_join_completion (
              actor, actor_join_failed_t{operation_high, operation_low, joined.error_kind ()},
              completion_source_spot);
        }
        const auto reply =
          joined.value ().reply.is_empty ()
            ? std::optional<message_t>{}
            : std::make_optional (message_t::from_raw (joined.value ().reply, _serializers));
        if (joined.value ().result_code == 0) {
            return spot_runtime.deliver_actor_join_completion (
              actor,
              actor_join_accepted_t{operation_high, operation_low, joined.value ().actor, reply},
              completion_source_spot);
        }
        return spot_runtime.deliver_actor_join_completion (
          actor, actor_join_rejected_t{operation_high, operation_low, reply},
          completion_source_spot);
    };
    const auto local_routing_id = routing_id ();
    const bool remote =
      local_routing_id && local_routing_id->to_hex () != target.node_rid.to_hex ();
    if (remote == false) {
        const auto found = _actors.find (std::string (actor.actor_id ().value ()));
        if (found == _actors.end ()) {
            co_return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "local Actor handle was not found");
        }
        host::call_id_t operation;
        const std::vector<zlink::message_t> parts{request};
        const auto submitted = found->second.join_spot (
          target.node_rid, spot_id_t (target.spot_id), target.object_generation, parts, operation,
          timeout);
        if (submitted != zlink::submit_result_t::ok) {
            co_return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::internal_failure, "Actor Spot join was not submitted");
        }
        auto completed = co_await await_completion (operation);
        auto joined = actor_join_reply_from_completion (completed.record, completed.parts, actor);
        const auto delivered = deliver_completion (operation.high, operation.low, joined);
        if (!delivered)
            co_return detail::propagate_failure<actor_join_reply_t> (
              delivered, "local Actor Join completion callback failed");
        co_return joined;
    }
    auto state = std::make_shared<remote_actor_join_state_t> (
      remote_actor_join_state_t{std::move (actor), target, request, timeout,
                                std::move (bound_session_node_rid),
                                std::move (bound_session_rid)});
    state->source_spot = completion_source_spot;
    // actorJoin(28) fence-gate: wire only when this node has explicitly
    // observed the target Spot's authority fence AND the target peer is
    // admitted at exactly that observed lifecycle generation. Defaults (and
    // today, always resolves) to the existing JSON admission path below --
    // see observe_spot_authority's doc comment for why.
    if (const auto observed = observed_spot_authority (
          target.node_rid, target.spot_id, target.object_generation);
        observed && has_admitted_peer (target.node_rid, observed->target_node_generation)) {
        co_return co_await admit_remote_application_actor_join_via_wire (
          std::move (state), *observed);
    }
    co_return co_await join_remote_application_actor_to_spot (std::move (state));
}

result_t<void> mesh_node_runtime_t::deliver_remote_actor_join (
  const remote_actor_join_state_t &s, const result_t<actor_join_reply_t> &r)
{
    spot_node_runtime_t spot (_state->spot_state);
    if (!r)
        return spot.deliver_actor_join_completion (
          s.actor, actor_join_failed_t{s.completion_operation_id_high,
                                      s.completion_operation_id_low, r.error_kind ()},
          s.source_spot);
    const auto reply = r.value ().reply.is_empty () ? std::optional<message_t>{}
      : std::make_optional (message_t::from_raw (r.value ().reply, _serializers));
    return r.value ().result_code == 0
      ? spot.deliver_actor_join_completion (
          s.actor, actor_join_accepted_t{s.completion_operation_id_high,
                                         s.completion_operation_id_low,
                                         r.value ().actor, reply}, s.source_spot)
      : spot.deliver_actor_join_completion (
          s.actor, actor_join_rejected_t{s.completion_operation_id_high,
                                         s.completion_operation_id_low, reply}, s.source_spot);
}

result_t<actor_join_reply_t> mesh_node_runtime_t::fail_remote_actor_join (
  const remote_actor_join_state_t &s, const result_t<actor_join_reply_t> &r, std::string m)
{
    const auto failed = detail::propagate_failure<actor_join_reply_t> (r, std::move (m));
    const auto delivered = deliver_remote_actor_join (s, failed);
    return delivered ? failed : detail::propagate_failure<actor_join_reply_t> (
      delivered, "remote Actor Join failure completion callback failed");
}

task_t<actor_join_reply_t> mesh_node_runtime_t::join_remote_application_actor_to_spot (
  std::shared_ptr<remote_actor_join_state_t> s)
{
    try {
        co_return co_await admit_remote_application_actor_join (s);
    }
    catch (const framework_exception_t &error) {
        co_return fail_remote_actor_join (
          *s, detail::result_access_t::failure<actor_join_reply_t> (error),
          "remote Actor Join failed");
    }
    catch (const std::exception &error) {
        co_return fail_remote_actor_join (
          *s, result_t<actor_join_reply_t>::failure (
                framework_error_kind_t::internal_failure, error.what ()),
          "remote Actor Join failed");
    }
}

task_t<actor_join_reply_t> mesh_node_runtime_t::admit_remote_application_actor_join (
  std::shared_ptr<remote_actor_join_state_t> s)
{
    if (!_serializers || !s->source_spot) {
        co_return result_t<actor_join_reply_t>::failure (
          !_serializers ? framework_error_kind_t::protocol_error : framework_error_kind_t::not_found,
          !_serializers ? "MeshNode serializers are not configured"
                        : "source Actor is not joined to a local Spot");
    }
    spot_node_runtime_t spot (_state->spot_state);
    const auto source_spot_generation = spot.resolve_spot_generation (
      _node->status ().routing_id (), *s->source_spot);
    if (!source_spot_generation) {
        const auto failed = result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found,
          "source Spot generation is unavailable");
        co_return fail_remote_actor_join (
          *s, failed, "source Spot generation is unavailable");
    }
    s->source_spot_generation = *source_spot_generation;
    auto reserved = spot.reserved_actor_transfer_id (s->actor);
    s->transfer_id = reserved ? std::move (*reserved) : spot.next_actor_transfer_id ();
    s->completion_operation_id_low =
      _state->next_join_completion_operation.fetch_add (1, std::memory_order_relaxed);
    s->completion_operation_id_high =
      static_cast<std::uint64_t> (std::hash<std::string>{}(_state->mesh_name)) | 1ULL;
    s->deadline = std::chrono::steady_clock::now () + s->timeout;
    const auto source = _node->resolve_actor (s->actor);
    if (!source || source->authority_owner_generation == 0) {
        const auto failed = result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "source Framework Actor authority is unavailable");
        co_return fail_remote_actor_join (*s, failed, "source Framework Actor authority is unavailable");
    }
    s->source_actor = *source;
    s->actor_authority_owner_generation = source->authority_owner_generation;
    runtime::messaging::client_call_codec_t codec;
    const auto request = spot_actor_admission_route_request_t{
      s->transfer_id, std::string (s->actor.node_rid ().value ()),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (s->actor)),
      std::string (s->actor.actor_id ().value ()), s->actor.object_generation (),
      s->actor_authority_owner_generation, s->completion_operation_id_high,
      s->completion_operation_id_low, *s->source_spot, s->target.spot_id,
      s->request.to_bytes ()};
    const auto header = codec.create_envelope (
      runtime::messaging::message_kind_t::request, "spot",
      spot_actor_admission_route_request_t::packet_name, s->timeout);
    auto encoded = codec.encode_envelope_parts (header, request, *_serializers);
    auto parts = co_await request_actor_join_spot_route (s->target, std::move (encoded), s->timeout);
    const auto reply = codec.decode_envelope_reply<spot_actor_admission_route_reply_t> (
      parts, *_serializers, "remote Actor admission reply is empty",
      "remote Actor admission reply decode failed", "ActorTransferAdmission");
    if (!reply)
        co_return fail_remote_actor_join (
          *s, detail::propagate_failure<actor_join_reply_t> (reply, "remote Actor admission failed"),
          "remote Actor admission failed");
    if (!reply.value ().accepted) {
        const auto rejected = result_t<actor_join_reply_t>::success (
          actor_join_reply_t{1, s->actor, zlink::message_t::from (reply.value ().payload)});
        const auto delivered = deliver_remote_actor_join (*s, rejected);
        co_return delivered ? rejected : detail::propagate_failure<actor_join_reply_t> (
          delivered, "remote Actor Join rejected completion callback failed");
    }
    s->completion_root_reference = reply.value ().completion_root_reference;
    s->completion_root_checksum = reply.value ().completion_root_checksum;
    s->admission_payload = reply.value ().payload;
    co_return co_await seal_remote_application_actor_join (std::move (s));
}

task_t<bool> mesh_node_runtime_t::abort_remote_actor_join_seal (
  std::shared_ptr<remote_actor_join_state_t> s)
{
    if (s->session_seal.checkpoints.empty ())
        co_return true;
    try {
        mesh_node_descriptor_t none;
        co_return co_await route_bound_sessions (
          s->session_seal.checkpoints, none,
          runtime::protocol::session_relocation_route_action_t::abort);
    } catch (...) { co_return false; }
}

namespace
{
std::string observed_spot_authority_key (const zlink::routing_id_t &target_node_rid,
                                         const std::string &target_spot_id,
                                         std::uint64_t object_generation)
{
    return target_node_rid.to_hex () + "|" + target_spot_id + "|"
           + std::to_string (object_generation);
}
} // namespace

void mesh_node_runtime_t::observe_spot_authority (
  const zlink::routing_id_t &target_node_rid, const std::string &target_spot_id,
  std::uint64_t object_generation, std::uint64_t target_node_generation,
  std::uint64_t authority_owner_generation, std::uint64_t owner_lease_generation)
{
    if (target_spot_id.empty () || object_generation == 0 || target_node_generation == 0
        || authority_owner_generation == 0 || owner_lease_generation == 0)
        return;
    std::lock_guard lock (_observed_spot_authority_mutex);
    _observed_spot_authorities[observed_spot_authority_key (
      target_node_rid, target_spot_id, object_generation)] = observed_spot_authority_t{
      target_node_generation, authority_owner_generation, owner_lease_generation};
}

std::optional<mesh_node_runtime_t::observed_spot_authority_t>
mesh_node_runtime_t::observed_spot_authority (const zlink::routing_id_t &target_node_rid,
                                              const std::string &target_spot_id,
                                              std::uint64_t object_generation) const
{
    std::lock_guard lock (_observed_spot_authority_mutex);
    const auto found = _observed_spot_authorities.find (
      observed_spot_authority_key (target_node_rid, target_spot_id, object_generation));
    if (found == _observed_spot_authorities.end ())
        return std::nullopt;
    return found->second;
}

namespace
{
std::string negotiated_receive_chunk_limit_key (const actor_ref_t &actor)
{
    return std::string (actor.actor_id ().value ()) + ":"
           + std::to_string (actor.object_generation ());
}
} // namespace

void mesh_node_runtime_t::record_negotiated_receive_chunk_limit (const actor_ref_t &actor,
                                                                 std::uint32_t limit_bytes)
{
    std::lock_guard lock (_negotiated_receive_chunk_limit_mutex);
    _negotiated_receive_chunk_limits[negotiated_receive_chunk_limit_key (actor)] = limit_bytes;
}

std::optional<std::uint32_t>
mesh_node_runtime_t::negotiated_receive_chunk_limit_bytes (const actor_ref_t &actor) const
{
    std::lock_guard lock (_negotiated_receive_chunk_limit_mutex);
    const auto found =
      _negotiated_receive_chunk_limits.find (negotiated_receive_chunk_limit_key (actor));
    if (found == _negotiated_receive_chunk_limits.end ())
        return std::nullopt;
    return found->second;
}

// actorJoin(28) originate fence-gate: only taken when (a) this node has
// explicitly observed the target Spot's authority fence via
// observe_spot_authority -- nothing calls that yet, so this is closed by
// construction on every current production path -- and (b) the target peer
// is admitted at exactly that observed lifecycle generation. request.
// correlation is minted from the same monotonic counter the JSON path's
// completion_operation_id_low already uses, forced odd/nonzero the same
// way completion_operation_id_high is (encode_actor_join_request throws on
// correlation == 0).
task_t<actor_join_reply_t> mesh_node_runtime_t::admit_remote_application_actor_join_via_wire (
  std::shared_ptr<remote_actor_join_state_t> s, observed_spot_authority_t observed)
{
    const auto source = _node->resolve_actor (s->actor);
    if (!source || source->authority_owner_generation == 0) {
        co_return fail_remote_actor_join (
          *s,
          result_t<actor_join_reply_t>::failure (
            framework_error_kind_t::not_found,
            "source Framework Actor authority is unavailable"),
          "source Framework Actor authority is unavailable");
    }
    // The wire actor-route-fence's ownerLeaseGeneration fences the *local
    // node's* current lease (cpp analog of dotnet's _localOwnerLeaseGeneration
    // read at ActorJoinRequest send time), not a per-actor lease -- the JSON
    // admission path (spot_actor_admission_route_request_t) has no per-actor
    // lease field at this phase either.
    const auto local_owner =
      _session_route_owner_resolver ? _session_route_owner_resolver () : std::nullopt;
    if (!local_owner || local_owner->lease_generation <= 0) {
        co_return fail_remote_actor_join (
          *s,
          result_t<actor_join_reply_t>::failure (
            framework_error_kind_t::unavailable,
            "local owner lease is unavailable for wire Actor join"),
          "local owner lease is unavailable for wire Actor join");
    }
    const auto local = _node->status ();
    // deliver_remote_actor_join / spot's completion delivery requires a
    // non-zero completion operation id, exactly like the JSON path's
    // admit_remote_application_actor_join sets before it can send its
    // request.
    s->completion_operation_id_low =
      _state->next_join_completion_operation.fetch_add (1, std::memory_order_relaxed);
    s->completion_operation_id_high =
      static_cast<std::uint64_t> (std::hash<std::string>{} (_state->mesh_name)) | 1ULL;
    const auto correlation = (s->completion_operation_id_low << 1) | 1ULL;
    const runtime::protocol::actor_join_request_t wire_request{
      correlation,
      runtime::protocol::actor_route_fence_t{
        std::string (s->actor.actor_id ().value ()), s->actor.object_generation (),
        local.routing_id ().to_bytes (), local.lifecycle_generation (),
        source->authority_owner_generation,
        static_cast<std::uint64_t> (local_owner->lease_generation)},
      false,
      runtime::protocol::spot_route_fence_t{
        s->target.spot_id, s->target.object_generation, s->target.node_rid.to_bytes (),
        observed.target_node_generation, observed.authority_owner_generation,
        observed.owner_lease_generation}};
    std::optional<runtime::protocol::application_payload_t> payload;
    if (!s->request.is_empty ())
        payload = runtime::protocol::application_payload_t{"", "", s->request.to_bytes ()};
    auto outcome = co_await _node->transport ().request_actor_join (
      s->target.node_rid.to_bytes (), wire_request, payload, s->timeout);
    if (!outcome.reply) {
        //  Spec 32 §5: classify instead of collapsing every failure to
        //  Unavailable — a malformed/identity-mismatched reply is
        //  ProtocolError, deadline expiry is DeadlineExceeded, and only a
        //  routing/transport loss is Unavailable.
        switch (outcome.failure) {
            case runtime::mesh::actor_join_wire_failure_t::protocol_error:
                co_return fail_remote_actor_join (
                  *s,
                  result_t<actor_join_reply_t>::failure (
                    framework_error_kind_t::protocol_error,
                    "wire Actor join reply was malformed or identity-fenced"),
                  "wire Actor join reply was malformed or identity-fenced");
            case runtime::mesh::actor_join_wire_failure_t::deadline_exceeded:
                co_return fail_remote_actor_join (
                  *s,
                  result_t<actor_join_reply_t>::failure (
                    framework_error_kind_t::deadline_exceeded,
                    "wire Actor join deadline elapsed before a reply"),
                  "wire Actor join deadline elapsed before a reply");
            case runtime::mesh::actor_join_wire_failure_t::unavailable:
            default:
                co_return fail_remote_actor_join (
                  *s,
                  result_t<actor_join_reply_t>::failure (
                    framework_error_kind_t::unavailable,
                    "wire Actor join route or transport was unavailable"),
                  "wire Actor join route or transport was unavailable");
        }
    }
    const auto &tail = *outcome.reply;
    if (tail.join_result == runtime::protocol::actor_join_result_t::accepted
        && tail.receive_chunk_limit_bytes != 0) {
        //  The negotiated receive limit is recorded per actor identity so
        //  the relocation direct-transfer capture (maintenance_runtime's
        //  advertised_receive_chunk_limit_bytes consumer,
        //  runtime/stateful/maintenance_runtime.cpp) can read it when it
        //  begins the transfer this admission approved. Threading the value
        //  into that consumer's call site stays deferred; recording it here
        //  keeps the negotiated value from being dropped at decode.
        record_negotiated_receive_chunk_limit (s->actor,
                                               tail.receive_chunk_limit_bytes);
    }
    //  The decoded application reply frame (if any) is the target's
    //  admission reply payload — surface it instead of discarding it.
    auto application_reply =
      outcome.application_reply
        ? zlink::message_t::from (outcome.application_reply->payload)
        : zlink::message_t{};
    const auto mapped =
      tail.join_result == runtime::protocol::actor_join_result_t::accepted
        ? result_t<actor_join_reply_t>::success (
            actor_join_reply_t{0, s->actor, application_reply})
        : result_t<actor_join_reply_t>::success (
            actor_join_reply_t{1, s->actor, application_reply});
    const auto delivered = deliver_remote_actor_join (*s, mapped);
    co_return delivered ? mapped
                        : detail::propagate_failure<actor_join_reply_t> (
                            delivered, "remote Actor Join completion callback failed");
}

task_t<actor_join_reply_t> mesh_node_runtime_t::seal_remote_application_actor_join (
  std::shared_ptr<remote_actor_join_state_t> s)
{
    if (!_user_spot_store || s->target.mesh_name.empty () || s->target.node_generation == 0
        || s->target.owner.owner_id.empty () || s->target.owner.lease_generation <= 0) {
        co_return fail_remote_actor_join (*s, result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::unavailable, "target Actor relocation authority is unavailable"),
          "target Actor relocation authority is unavailable");
    }
    try {
        const auto read = co_await _user_spot_store->read_authority (
          runtime::actor_authority_key (s->actor.actor_id ().value ()));
        const auto *authority = std::get_if<authority_snapshot_t> (&read);
        const auto status = _node->status ();
        if (!authority || authority->allocation.state != placement_allocation_state_t::active
            || authority->allocation.object_kind != placement_object_kind_t::actor
            || authority->object_generation != s->actor.object_generation ()
            || authority->authority_owner_generation != s->actor_authority_owner_generation
            || authority->allocation.target.node_rid.value () != s->source_actor.node_id
            || authority->allocation.target.node_lifecycle_generation != status.lifecycle_generation ()
            || authority->owner.lease_generation <= 0 || authority->store_version.empty ()) {
            co_return fail_remote_actor_join (*s, result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::unavailable, "source Actor authority changed during Join admission"),
              "source Actor authority changed during Join admission");
        }
        s->source_authority = *authority;
        s->source_mesh_name = authority->allocation.target.mesh_name;
        s->source_owner_lease_generation = static_cast<std::uint64_t> (authority->owner.lease_generation);
        s->source_node_rid = status.routing_id ().to_bytes ();
        s->source_node_generation = status.lifecycle_generation ();
    } catch (const std::exception &e) {
        co_return fail_remote_actor_join (
          *s, result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure, e.what ()),
          "Actor relocation authority lookup failed");
    }
    co_return co_await seal_remote_application_actor_join_call (std::move (s));
}

task_t<actor_join_reply_t> mesh_node_runtime_t::seal_remote_application_actor_join_call (
  std::shared_ptr<remote_actor_join_state_t> s)
{
    const auto now = std::chrono::steady_clock::now ();
    if (now >= s->deadline) {
        co_return fail_remote_actor_join (*s, result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::deadline_exceeded, "bound Session relocation seal deadline elapsed"),
          "bound Session relocation seal deadline elapsed");
    }
    try {
        const auto relocation = relocation_ids ().issue ();
        const auto &authority = *s->source_authority;
        const runtime::protocol::relocation_coordinator_fence_t coordinator{
          authority.owner.owner_id, static_cast<std::uint64_t> (authority.owner.lease_generation),
          s->source_node_rid, s->source_node_generation, authority.store_version};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
          s->deadline - now);
        auto completion =
          std::make_shared<detail::task_completion_source_t<
            session_relocation_seal_outcome_t>> ();
        auto output = completion->task ();
        auto attempt = std::make_shared<task_t<session_relocation_seal_outcome_t>> (
          seal_bound_sessions (
            {{s->source_actor, authority}}, relocation, coordinator, remaining));
        detail::observe_task_completion (
          *attempt, [completion, attempt] (
                      const result_t<session_relocation_seal_outcome_t> &settled) {
              completion->complete (settled);
          });
        s->session_seal = co_await output;
    } catch (const std::exception &e) {
        co_return fail_remote_actor_join (
          *s, result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure, e.what ()),
          "bound Session relocation seal failed");
    }
    if (!s->session_seal.completed || s->session_seal.checkpoints.size () > 1) {
        co_return fail_remote_actor_join (*s, result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::unavailable, "bound Session relocation seal did not complete"),
          "bound Session relocation seal did not complete");
    }
    if (!s->session_seal.checkpoints.empty ())
        s->encoded_session_relocation_route = runtime::protocol::encode_session_relocation_route (
          make_session_relocation_route (s->session_seal.checkpoints.front (), s->target.node_rid,
            s->target.node_generation, runtime::protocol::session_relocation_route_action_t::commit));
    co_return co_await prepare_remote_application_actor_join (std::move (s));
}

task_t<actor_join_reply_t> mesh_node_runtime_t::prepare_remote_application_actor_join (
  std::shared_ptr<remote_actor_join_state_t> s)
{
    spot_node_runtime_t spot (_state->spot_state);
    const auto prepared = spot.transfer_actor_out (s->actor, s->transfer_id);
    if (!prepared) {
        const auto failed = detail::propagate_failure<actor_join_reply_t> (prepared, "Actor transfer-out failed");
        (void) co_await abort_remote_actor_join_seal (s);
        co_return fail_remote_actor_join (*s, failed, "Actor transfer-out failed");
    }
    s->transfer_state = prepared.value ().state.to_bytes ();
    runtime::messaging::client_call_codec_t codec;
    const auto request = spot_actor_commit_route_request_t{
      .transfer_id=s->transfer_id, .actor_node_rid=std::string(s->actor.node_rid().value()),
      .actor_type=std::string(::zlink::framework::detail::actor_ref_access_t::actor_type(s->actor)),
      .actor_id=std::string(s->actor.actor_id().value()), .actor_generation=s->actor.object_generation(),
      .actor_authority_owner_generation=s->actor_authority_owner_generation,
      .completion_root_reference=s->completion_root_reference, .completion_root_checksum=s->completion_root_checksum,
      .target_spot_id=s->target.spot_id, .target_spot_generation=s->target.object_generation,
      .source_mesh_name=s->source_mesh_name, .target_mesh_name=s->target.mesh_name,
      .target_node_lifecycle_generation=s->target.node_generation, .target_owner_id=s->target.owner.owner_id,
      .target_owner_lease_generation=static_cast<std::uint64_t>(s->target.owner.lease_generation),
      .source_spot_id=*s->source_spot,
      .source_spot_generation=s->source_spot_generation,
      .session_relocation_route=s->encoded_session_relocation_route,
      .transfer_state=s->transfer_state, .core_transfer=true, .prepare=true};
    const auto header=codec.create_envelope(runtime::messaging::message_kind_t::request,"spot",
      spot_actor_commit_route_request_t::packet_name,s->timeout);
    auto encoded=codec.encode_envelope_parts(header,request,*_serializers);
    auto parts=co_await request_actor_join_spot_route(s->target,std::move(encoded),s->timeout);
    const auto reply=codec.decode_envelope_reply<spot_actor_join_route_reply_t>(
      parts,*_serializers,"remote Actor prepare reply is empty","remote Actor prepare reply decode failed","ActorTransferPrepare");
    if (!reply) {
      // Spec 28 explicit failure: PREPARE has not transferred authority yet
      // (that only happens at FINALIZE/cutover below), so a rejected or
      // failed PREPARE reply -- including the target's restore() throwing --
      // means the target definitely never took ownership. reconcile=false
      // closes the move and replays any queued backlog locally instead of
      // parking the Actor in the reconcile phase, which nothing ever
      // resolves; the Actor must stay servable on the source.
      spot.fail_remote_actor_transfer(s->actor,false);
      const auto failed=detail::propagate_failure<actor_join_reply_t>(reply,"remote Actor prepare failed");
      (void)co_await abort_remote_actor_join_seal(s);
      co_return fail_remote_actor_join(*s,failed,"remote Actor prepare failed");
    }
    { std::lock_guard<std::recursive_mutex> lock(_state->spot_state->mutex);
      if (const auto it=_state->spot_state->core_actor_membership_epochs.find(std::string(s->actor.actor_id().value()));
          it!=_state->spot_state->core_actor_membership_epochs.end()) s->membership_epoch=it->second; }
    const host::actor_transfer_prepare_t core{.role=host::actor_transfer_role_t::source,.transfer_id=s->transfer_id,
      .actor=s->actor,.source_spot_id=*s->source_spot,.target_spot_id=spot_id_t(s->target.spot_id),
      .target_spot_generation=s->target.object_generation,.target_node_rid=s->target.node_rid};
    host::actor_transfer_prepare_result_t result{s->actor,s->membership_epoch};
    if (!prepare_actor_transfer(core,s->timeout,s->core_token,result)) {
      // Same rationale as the reply-failure branch above: this is a local,
      // pre-commit PREPARE failure on the source's own Core state, still
      // before FINALIZE/cutover -- explicit, not ambiguous.
      spot.fail_remote_actor_transfer(s->actor,false);
      const auto failed=result_t<actor_join_reply_t>::failure(framework_error_kind_t::internal_failure,"source Framework Actor relocation prepare failed");
      (void)co_await abort_remote_actor_join_seal(s);
      co_return fail_remote_actor_join(*s,failed,"source Framework Actor relocation prepare failed");
    }
    co_return co_await finalize_remote_application_actor_join(std::move(s));
}

task_t<actor_join_reply_t> mesh_node_runtime_t::finalize_remote_application_actor_join (
  std::shared_ptr<remote_actor_join_state_t> s)
{
    spot_node_runtime_t spot(_state->spot_state);
    const auto now=std::chrono::steady_clock::now();
    if(now>=s->deadline) co_return fail_remote_actor_join(*s,result_t<actor_join_reply_t>::failure(
      framework_error_kind_t::deadline_exceeded,"remote Actor finalize deadline elapsed"),"remote Actor finalize failed");
    const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(s->deadline-now);
    std::vector<spot_actor_handoff_packet_t> backlog;
    for(auto &p:spot.take_actor_handoff_backlog(s->actor)) backlog.push_back(
      {std::move(p.packet_name),std::move(p.payload),std::move(p.content_type),std::move(p.metadata),p.is_request});
    const auto request=spot_actor_commit_route_request_t{
      .transfer_id=s->transfer_id,.actor_node_rid=std::string(s->actor.node_rid().value()),
      .actor_type=std::string(::zlink::framework::detail::actor_ref_access_t::actor_type(s->actor)),
      .actor_id=std::string(s->actor.actor_id().value()),.actor_generation=s->actor.object_generation(),
      .actor_authority_owner_generation=s->actor_authority_owner_generation,
      .completion_root_reference=s->completion_root_reference,.completion_root_checksum=s->completion_root_checksum,
      .target_spot_id=s->target.spot_id,.target_spot_generation=s->target.object_generation,
      .source_mesh_name=s->source_mesh_name,.target_mesh_name=s->target.mesh_name,
      .target_node_lifecycle_generation=s->target.node_generation,.target_owner_id=s->target.owner.owner_id,
      .target_owner_lease_generation=static_cast<std::uint64_t>(s->target.owner.lease_generation),
      .source_spot_id=*s->source_spot,
      .source_spot_generation=s->source_spot_generation,
      .bound_session_node_rid=s->bound_session_node_rid?s->bound_session_node_rid->to_string():std::string{},
      .bound_session_rid=s->bound_session_rid?s->bound_session_rid->to_string():std::string{},
      .session_relocation_route=s->encoded_session_relocation_route,.transfer_state=s->transfer_state,
      .handoff_backlog=std::move(backlog),.core_transfer=true,.core_membership_epoch=s->membership_epoch,
      .finalize_timeout_ms=static_cast<std::uint64_t>(remaining.count()),.finalize=true};
    runtime::messaging::client_call_codec_t codec;
    const auto header=codec.create_envelope(runtime::messaging::message_kind_t::command,"spot",
      spot_actor_commit_route_request_t::packet_name,remaining);
    auto encoded=codec.encode_envelope_parts(header,request,*_serializers);
    const auto submitted=co_await send_to_spot(
      *s->source_spot,s->target.node_rid,s->target.spot_id,
      s->target.object_generation,encoded.items());
    if(submitted!=zlink::submit_result_t::ok) {
      // Ambiguous outcome: the transport send itself failed, so whether the
      // target ever received FINALIZE is unknown. Capture the target
      // identity now -- the deadline handler reconciles against the
      // Location Store instead of blindly replaying locally (spec 28).
      const auto reconcile_source_fence=runtime::protocol::actor_route_fence_t{
        std::string(s->actor.actor_id().value()),s->actor.object_generation(),
        s->source_node_rid,s->source_node_generation,
        s->actor_authority_owner_generation,s->source_owner_lease_generation};
      const auto reconcile_target_fence=runtime::protocol::actor_route_fence_t{
        std::string(s->actor.actor_id().value()),s->actor.object_generation(),
        s->target.node_rid.to_bytes(),s->target.node_generation,
        s->actor_authority_owner_generation+1,
        static_cast<std::uint64_t>(s->target.owner.lease_generation)};
      const auto reconcile_target_actor=::zlink::framework::detail::actor_ref_access_t::make(
        node_rid_t::from_string(s->target.node_rid.to_string()),
        std::string(::zlink::framework::detail::actor_ref_access_t::actor_type(s->actor)),
        std::string(s->actor.actor_id().value()),s->actor.object_generation());
      spot.fail_remote_actor_transfer(s->actor,true,
        reconcile_target_context_t{
          spot_route_t{node_rid_t::from_string(s->target.node_rid.to_string()),
                       spot_id_t(s->target.spot_id),{}},
          reconcile_target_actor,reconcile_source_fence,reconcile_target_fence,
          s->transfer_id});
      (void)co_await abort_remote_actor_join_seal(s);
      co_return fail_remote_actor_join(
        *s,result_t<actor_join_reply_t>::failure(
          runtime::messaging::map_submit_result_error_kind(submitted),
          "remote Actor cutover was not submitted"),
        "remote Actor cutover was not submitted");
    }
    if(!s->core_token.commit(s->membership_epoch+1)) co_return fail_remote_actor_join(*s,result_t<actor_join_reply_t>::failure(
      framework_error_kind_t::internal_failure,"source Framework Actor relocation commit failed"),"source Framework Actor relocation commit failed");
    const auto joined=actor_join_reply_t{
      0,
      ::zlink::framework::detail::actor_ref_access_t::make(
        node_rid_t::from_string(s->target.node_rid.to_string()),
        std::string(::zlink::framework::detail::actor_ref_access_t::actor_type(s->actor)),
        std::string(s->actor.actor_id().value()),s->actor.object_generation()),
      zlink::message_t{}};
    const auto source=runtime::protocol::actor_route_fence_t{std::string(s->actor.actor_id().value()),s->actor.object_generation(),s->source_node_rid,s->source_node_generation,s->actor_authority_owner_generation,s->source_owner_lease_generation};
    const auto target=runtime::protocol::actor_route_fence_t{std::string(s->actor.actor_id().value()),s->actor.object_generation(),s->target.node_rid.to_bytes(),s->target.node_generation,s->actor_authority_owner_generation+1,static_cast<std::uint64_t>(s->target.owner.lease_generation)};
  try {
    co_await spot.complete_remote_actor_transfer(
      s->actor, joined.actor,
      {node_rid_t::from_string(s->target.node_rid.to_string()),
       spot_id_t(s->target.spot_id), {}},
      source, target, s->transfer_id);
  }
  catch (const framework_exception_t &error) {
    co_return fail_remote_actor_join(
      *s, detail::result_access_t::failure<actor_join_reply_t>(error),
      "committed target Actor route publication failed");
  }
  catch (const std::exception &error) {
    co_return fail_remote_actor_join(
      *s, result_t<actor_join_reply_t>::failure(
            framework_error_kind_t::internal_failure, error.what()),
      "committed target Actor route publication failed");
  }
    co_return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{joined.result_code, joined.actor,
                         zlink::message_t::from (s->admission_payload)});
}

result_t<std::shared_ptr<deferred_barrier_t>>
mesh_node_runtime_t::reserve_application_actor_join_barrier (const actor_ref_t &actor)
{
    spot_node_runtime_t spot_runtime (_state->spot_state);
    if (!spot_runtime.actor_spot (actor)) {
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::not_found, "Deferred Actor join source runtime was not found");
    }
    return spot_runtime.reserve_actor_join_barrier (actor);
}

result_t<actor_join_reply_t>
mesh_node_runtime_t::actor_join_reply_from_completion (const host::receive_record_t &record,
                                                       const std::vector<zlink::message_t> &parts,
                                                       const actor_ref_t &actor)
{
    if (record.terminal_result != 0) {
        //  Spec 32-framework-error-model:119-136 — a Framework failure carried
        //  on the completion (prerequisite or post-accept commit failure) is
        //  not an application rejection; classify it via the shared wire
        //  mapper instead of returning typed Rejected.
        const runtime::messaging::request_failure_mapper_t failure_mapper;
        const auto failure = failure_mapper.reply_header_exception (
          static_cast<std::uint32_t> (record.terminal_result),
          static_cast<std::uint32_t> (record.failure_errno),
          "Local Actor join");
        return result_t<actor_join_reply_t>::failure (
          failure.kind (), failure.what ());
    }
    if (!record.join_completion) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "Actor Spot completion did not carry a join result (terminal result "
            + std::to_string (record.terminal_result) + ", errno "
            + std::to_string (record.failure_errno) + ")");
    }
    const auto &joined = *record.join_completion;
    const auto reply = parts.empty () ? zlink::message_t{} : parts.front ();
    if (joined.join_result == host::join_admission_t::rejected) {
        return result_t<actor_join_reply_t>::success (actor_join_reply_t{1, actor, reply});
    }
    const auto &native = joined.current_actor;
    {
        std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
        ++_state->spot_state
            ->core_actor_membership_epochs[std::string (actor.actor_id ().value ())];
    }
    return result_t<actor_join_reply_t>::success (actor_join_reply_t{
      0,
      ::zlink::framework::detail::actor_ref_access_t::make (
        native.node_rid (),
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
        std::string (native.actor_id ().value ()), native.object_generation ()),
      reply});
}

result_t<actor_join_reply_t> mesh_node_runtime_t::wait_for_join_completion (
  const host::call_id_t &operation, const actor_ref_t &actor, std::chrono::milliseconds timeout)
{
    auto completed = wait_for_completion (operation, timeout);
    if (!completed) {
        return result_t<actor_join_reply_t>::failure (
          completed.error_kind (),
          completed.error () ? completed.error ()->what () : "Actor Spot join failed");
    }
    auto completion = std::move (completed.value ());
    return actor_join_reply_from_completion (completion.record, completion.parts, actor);
}

task_t<std::optional<zlink::message_t>>
mesh_node_runtime_t::relay_application_actor (const actor_ref_t &actor,
                                              const stream_header_t &header,
                                              const zlink::message_t &payload,
                                              std::chrono::milliseconds timeout,
                                              bool await_remote_admission,
                                              std::optional<bound_session_relay_source_t>
                                                bound_session_source)
{
    runtime::messaging::client_call_codec_t codec;
    const auto kind = header.kind () == stream_message_kind_t::send
                        ? runtime::messaging::message_kind_t::command
                        : runtime::messaging::message_kind_t::request;
    auto envelope =
      codec.create_envelope (kind, "actor", std::string (header.packet_name ()), timeout);
    envelope.content_type = std::string (stream_content_type (header.codec ()));
    envelope.metadata = header.metadata ().values ();
    co_return co_await relay_application_actor (
      actor, envelope, payload, timeout, zlink::routing_id_t::from (std::uint32_t{0}),
      runtime::protocol::actor_route_fence_t{}, 0, runtime::protocol::wire_operation_id_t{}, 0,
      await_remote_admission, std::move (bound_session_source));
}

task_t<std::optional<zlink::message_t>>
mesh_node_runtime_t::relay_application_actor (const actor_ref_t &actor,
                                              const runtime::messaging::envelope_header_t &header,
                                              const zlink::message_t &payload,
                                              std::chrono::milliseconds timeout)
{
    co_return co_await relay_application_actor (
      actor, header, payload, timeout, zlink::routing_id_t::from (std::uint32_t{0}),
      runtime::protocol::actor_route_fence_t{}, 0, runtime::protocol::wire_operation_id_t{}, 0);
}

task_t<std::optional<zlink::message_t>> mesh_node_runtime_t::relay_application_actor (
  actor_ref_t actor,
  runtime::messaging::envelope_header_t header,
  zlink::message_t payload,
  std::chrono::milliseconds timeout,
  zlink::routing_id_t source_node,
  runtime::protocol::actor_route_fence_t stale_route,
  std::uint8_t incoming_hop_count,
  runtime::protocol::wire_operation_id_t original_operation,
  std::uint64_t original_reply_route_id,
  bool await_remote_admission,
  std::optional<bound_session_relay_source_t> bound_session_source)
{
    try {
        spot_node_runtime_t spot_runtime (_state->spot_state);
        const auto local_routing_id = routing_id ();
        if (!local_routing_id) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::internal_failure,
              "Actor Message Follow requires a local routing identity");
        }
        const auto local_node = local_routing_id->to_hex ();
        auto payload_bytes = message_follow_payload_bytes (header, payload);
        payload_bytes += local_node.size () + message_follow_authority_path_bytes
                         + (header.metadata.contains (std::string (message_follow_path_key))
                              ? 1
                              : message_follow_path_key.size ());
        const bool source_transfer_in_progress = spot_runtime.actor_transfer_in_progress (actor);
        const bool replays_handoff_packet =
          header.metadata.contains (std::string (detail::actor_handoff_source_node_key))
          || header.metadata.contains ("__zlink.actorHandoffLateReplay");
        const bool has_exact_stale_route =
          stale_route.owner_lease_generation != 0;
        const bool exact_route_targets_local_source =
          has_exact_stale_route
          && stale_route.target_node_routing_id
               == local_routing_id->to_bytes ();
        auto acquired_follow =
          (source_transfer_in_progress && !replays_handoff_packet)
              || !exact_route_targets_local_source
            ? result_t<std::optional<detail::actor_message_follow_target_t>>::success (std::nullopt)
            : spot_runtime.try_acquire_actor_message_follow (actor, payload_bytes,
                                                             incoming_hop_count,
                                                             stale_route);
        if (!acquired_follow) {
            co_return detail::propagate_failure<std::optional<zlink::message_t>> (
              acquired_follow, "Actor Message Follow admission failed");
        }
        if (acquired_follow.value ()) {
            const auto follow_target = std::move (*acquired_follow.value ());
            actor_message_follow_lease_t lease (
              spot_runtime, actor, follow_target.source_fence, payload_bytes);
            auto follow_path = advance_message_follow_path (
              header.metadata, local_node,
              follow_target.source_fence.authority_owner_generation);
            if (!follow_path) {
                co_return detail::propagate_failure<std::optional<zlink::message_t>> (
                  follow_path, "Actor Message Follow loop detection failed");
            }
            const auto target_node =
              zlink::routing_id_t::from (std::string (follow_target.route.node_rid.value ()));
            auto target_identity = target_node.to_hex ();
            target_identity.push_back ('@');
            target_identity += std::to_string (
              follow_target.target_fence.authority_owner_generation);
            if (follow_path.value ().visited.contains (target_identity)) {
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::unavailable,
                  "Actor Message Follow target authority fence was already visited");
            }
            if (spot_runtime.actor_transfer_marker_enabled ()) {
                spot_runtime.emit_actor_transfer_marker (
                  "message_follow_relay", actor,
                  header.correlation_id.empty ()
                    ? std::string (actor.actor_id ().value ())
                    : header.correlation_id,
                  follow_target.route.spot_id, follow_target.route.node_rid);
            }
            spot_inbound_message_t metadata;
            metadata.content_type = header.content_type;
            metadata.values = header.metadata;
            if (header.kind == runtime::messaging::message_kind_t::request
                && !header.correlation_id.empty ()) {
                metadata.values.insert_or_assign ("__zlink.actorRequestId", header.correlation_id);
            }
            metadata.values[std::string (message_follow_path_key)] =
              std::move (follow_path.value ().encoded);
            metadata.values["__zlink.messageFollowHopCount"] =
              std::to_string (static_cast<unsigned int> (incoming_hop_count + 1));
            if (header.kind == runtime::messaging::message_kind_t::command)
                metadata.values["__zlink.actorRelayKind"] = "send";
            runtime::messaging::client_call_codec_t codec;
            auto request_header =
              codec.create_envelope (runtime::messaging::message_kind_t::request, "spot",
                                     spot_actor_packet_route_request_t::packet_name, timeout);
            auto request = make_spot_actor_packet_route_request (
              follow_target.actor, follow_target.route.spot_id, header.message_name, payload,
              metadata, follow_target.target_fence);
            auto request_parts =
              codec.encode_envelope_parts (request_header, request, *_serializers);
            const auto target_generation =
              spot_runtime.resolve_spot_generation (target_node, follow_target.route.spot_id);
            if (!target_generation) {
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::not_found,
                  "Actor message follow target Spot generation is unavailable");
            }
            auto origin = get_or_create_spot ("__zlink-route-origin-" + routing_id ()->to_hex ());
            host::call_id_t operation;
            const auto submitted = co_await origin.request_to_spot (
              target_node, follow_target.route.spot_id, *target_generation, request_parts.items (),
              operation, zlink::send_flags_t::none, timeout);
            if (submitted != zlink::submit_result_t::ok) {
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::internal_failure,
                  "Actor message follow route request was not submitted");
            }
            auto completed = co_await await_completion (operation);
            if (completed.record.terminal_result
                != static_cast<int> (zlink::request_result_t::ok)) {
                const runtime::messaging::request_failure_mapper_t failure_mapper;
                const auto failure = failure_mapper.reply_header_exception (
                  completed.record.terminal_result,
                  completed.record.failure_errno,
                  "Actor message follow route request");
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  failure.kind (), failure.what ());
            }
            runtime::messaging::message_parts_t reply_parts (std::move (completed.parts));
            auto decoded = codec.decode_envelope_reply<spot_actor_packet_route_reply_t> (
              reply_parts, *_serializers, "Actor message follow reply is empty",
              "Actor message follow reply decode failed", header.message_name);
            if (!decoded)
                co_return detail::propagate_failure<std::optional<zlink::message_t>> (
                  decoded, "Actor message follow relay failed");
            if ((original_operation.high != 0 || original_operation.low != 0)
                && !source_node.to_bytes ().empty () && stale_route.owner_lease_generation != 0
                && spot_runtime.try_begin_actor_message_follow_notification (
                  actor, follow_target.source_fence,
                  follow_target.target_fence)) {
                const auto accepted = co_await _node->send_message_follow (
                  source_node.to_bytes (),
                  runtime::protocol::message_follow_notice_t{
                    follow_target.source_fence, follow_target.target_fence,
                    static_cast<std::uint8_t> (incoming_hop_count + 1), 1,
                    static_cast<std::uint32_t> (
                      std::min<std::size_t> (
                        payload_bytes,
                        std::numeric_limits<std::uint32_t>::max ())),
                    original_operation, original_reply_route_id});
                (void) spot_runtime.complete_actor_message_follow_notification (
                  actor, follow_target.source_fence,
                  follow_target.target_fence, accepted);
            }
            co_return result_t<std::optional<zlink::message_t>>::success (
              decoded.value ().has_reply
                ? std::make_optional (zlink::message_t::from (decoded.value ().payload))
                : std::nullopt);
        }
        const auto &target_actor = actor;
        auto target_node_rid =
          zlink::routing_id_t::from (std::string (target_actor.node_rid ().value ()));
        std::uint64_t authority_owner_generation = 0;
        std::uint64_t owner_lease_generation = 0;
        const auto local_descriptor = _node->transport ().topology ().local_descriptor ();
        const bool targets_local_node =
          !target_actor.node_rid ().empty ()
          && target_actor.node_rid ().value ()
               == zlink::routing_id_t::from (local_descriptor.node_routing_id).to_string ();
        const bool targets_moving_local_source = targets_local_node && source_transfer_in_progress;
        if (targets_local_node && !source_transfer_in_progress
            && !spot_runtime.actor_route (target_actor)) {
            if (spot_runtime.actor_transfer_marker_enabled ()) {
                spot_runtime.emit_actor_transfer_marker ("message_follow_expired", target_actor,
                                                         {}, std::nullopt, std::nullopt);
            }
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::unavailable, "Actor Message Follow route has expired");
        }
        const bool has_exact_remote_route =
          has_exact_stale_route && !exact_route_targets_local_source;
        if (has_exact_remote_route) {
            if (stale_route.actor_id != target_actor.actor_id ().value ()
                || stale_route.object_generation
                     != target_actor.object_generation ()
                || stale_route.target_node_routing_id
                     != target_node_rid.to_bytes ()
                || stale_route.authority_owner_generation == 0) {
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::invalid_operation,
                  "bound Session Actor route fence is inconsistent");
            }
            /* A committed Session binding is already an exact route snapshot.
             * Use its authority and owner fence together instead of combining
             * its target node with a Location cache entry from the previous
             * owner. Before command 44, the same rule sends the old exact
             * route to its source, where Message Follow can relay it. */
            authority_owner_generation =
              stale_route.authority_owner_generation;
            owner_lease_generation =
              stale_route.owner_lease_generation;
        }
        else if (_actor_route_resolver) {
            const auto resolved = _actor_route_resolver (target_actor);
            if (!resolved || resolved->object_generation != target_actor.object_generation ()
                || resolved->authority_owner_generation == 0
                || resolved->owner.lease_generation <= 0) {
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::not_found, "Actor route fence is unavailable");
            }
            // A bound Session keeps the exact ActorRef established by bind.
            // The old owner either preserves the packet in its handoff backlog
            // or relays it through Message Follow. Replacing that route from a
            // location lookup here can split one Session's serial stream across
            // the old and new owners and let a later packet overtake backlog.
            if (!targets_moving_local_source && !await_remote_admission)
                target_node_rid = resolved->node_rid;
            authority_owner_generation = resolved->authority_owner_generation;
            owner_lease_generation = static_cast<std::uint64_t> (resolved->owner.lease_generation);
        } else if (_user_spot_store) {
            const auto authority = co_await _user_spot_store->read_authority (
              runtime::actor_authority_key (target_actor.actor_id ().value ()));
            if (const auto *snapshot = std::get_if<authority_snapshot_t> (&authority))
                authority_owner_generation = snapshot->authority_owner_generation;
        }
        const auto kind = header.kind;
        const bool ordered_bound_session_relay =
          header.metadata.contains (std::string (detail::bound_session_relay_sequence_key));
        auto encoded =
          runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (header, payload);
        const auto native_actor = host::mesh_node_t::remote_actor_ref (
          target_node_rid, std::string (target_actor.actor_id ().value ()),
          target_actor.object_generation ());
        const auto wire_bound_session_source = [&] {
            if (!bound_session_source)
                return std::optional<runtime::protocol::actor_message_header_t::bound_session_source_t>{};
            return std::make_optional (
              runtime::protocol::actor_message_header_t::bound_session_source_t{
                bound_session_source->session_rid.to_bytes (),
                bound_session_source->binding_generation,
                bound_session_source->session_sequence});
        } ();
        if (kind == runtime::messaging::message_kind_t::command && !await_remote_admission
            && !ordered_bound_session_relay) {
            const auto submitted = co_await send_to_actor (
              native_actor, encoded.items (), {}, authority_owner_generation,
              owner_lease_generation, wire_bound_session_source);
            if (submitted != zlink::submit_result_t::ok) {
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  runtime::messaging::map_submit_result_error_kind (submitted),
                  "Actor relay send was not accepted");
            }
            co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
        }

        host::call_id_t operation;
        const auto submitted = co_await request_to_actor (
          native_actor, encoded.items (), operation, timeout, {},
          authority_owner_generation, owner_lease_generation,
          wire_bound_session_source);
        if (submitted != zlink::submit_result_t::ok) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              runtime::messaging::map_submit_result_error_kind (submitted),
              "Actor relay request was not accepted");
        }
        auto completed = co_await await_completion (operation);
        //  Spec 20 §3/§4 — an ordered bound-session relay of a one-way send
        //  awaits only the remote admission terminal. Classify a non-OK
        //  terminal (spec 32 §5), and for a command return success before
        //  touching the parts: a successful one-way completion carries no
        //  application reply, so decoding an envelope from the empty parts
        //  fabricated an invalid_frame failure and dropped the message.
        if (completed.record.terminal_result
            != static_cast<int> (zlink::request_result_t::ok)) {
            const runtime::messaging::request_failure_mapper_t failure_mapper;
            const auto failure = failure_mapper.reply_header_exception (
              completed.record.terminal_result,
              completed.record.failure_errno,
              "Actor relay request");
            co_return result_t<std::optional<zlink::message_t>>::failure (
              failure.kind (), failure.what ());
        }
        if (kind == runtime::messaging::message_kind_t::command)
            co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
        runtime::messaging::message_parts_t reply (std::move (completed.parts));
        auto reply_header =
          runtime::messaging::envelope_codec_t{}.decode_header (reply, false);
        if (!reply_header) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              reply_header.error_kind (), reply_header.error ()
                                            ? reply_header.error ()->what ()
                                            : "Actor relay reply header decode failed");
        }
        if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
            const auto message =
              reply_header.value ().error_message.value_or ("Actor relay request failed");
            runtime::messaging::request_failure_mapper_t failure_mapper;
            auto mapped = failure_mapper.error_header_exception (
              reply_header.value ().error_code.value_or ("request_failed"), message,
              "Actor relay request");
            mapped = runtime::messaging::restore_failure_origin (reply_header.value (),
                                                                 std::move (mapped));
            co_return detail::result_access_t::failure<std::optional<zlink::message_t>> (mapped);
        }
        auto body = runtime::messaging::envelope_codec_t{}.decode_body (reply);
        if (!body)
            co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
        co_return result_t<std::optional<zlink::message_t>>::success (
          std::make_optional (std::move (body.value ())));
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<std::optional<zlink::message_t>> (error);
    }
    catch (const std::exception &error) {
        co_return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
}

std::optional<runtime::spot_address_t>
mesh_node_runtime_t::resolve_application_actor_route (const actor_ref_t &actor) const
{
    if (!_actor_route_resolver)
        return std::nullopt;
    const auto resolved = _actor_route_resolver (actor);
    if (!resolved || resolved->object_generation != actor.object_generation ()
        || resolved->authority_owner_generation == 0 || resolved->node_generation == 0
        || resolved->owner.lease_generation <= 0)
        return std::nullopt;
    return resolved;
}

std::optional<runtime::spot_address_t>
mesh_node_runtime_t::refresh_application_actor_route (
  const actor_ref_t &actor,
  const runtime::spot_address_t &stale_route) const
{
    if (_actor_route_invalidator) {
        _actor_route_invalidator (
          runtime::protocol::actor_route_fence_t{
            std::string (actor.actor_id ().value ()),
            stale_route.object_generation,
            stale_route.node_rid.to_bytes (),
            stale_route.node_generation,
            stale_route.authority_owner_generation,
            static_cast<std::uint64_t> (
              stale_route.owner.lease_generation)});
    }
    return resolve_application_actor_route (actor);
}

std::optional<runtime::spot_address_t>
mesh_node_runtime_t::wait_for_application_actor_route_change (
  const actor_ref_t &actor,
  const runtime::spot_address_t &stale_route,
  std::chrono::milliseconds timeout) const
{
    if (timeout <= std::chrono::milliseconds::zero ())
        return std::nullopt;
    const auto changed = [&stale_route] (
      const runtime::spot_address_t &candidate) {
        return candidate.node_rid != stale_route.node_rid
               || candidate.node_generation
                    != stale_route.node_generation
               || candidate.object_generation
                    != stale_route.object_generation
               || candidate.authority_owner_generation
                    != stale_route.authority_owner_generation
               || candidate.owner.lease_generation
                    != stale_route.owner.lease_generation;
    };
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    do {
        const auto candidate =
          resolve_application_actor_route (actor);
        if (candidate && changed (*candidate))
            return candidate;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    } while (std::chrono::steady_clock::now () < deadline);
    const auto candidate = resolve_application_actor_route (actor);
    return candidate && changed (*candidate)
      ? candidate
      : std::optional<runtime::spot_address_t>{};
}

task_t<application_actor_session_bind_outcome_t>
mesh_node_runtime_t::bind_application_actor_session (const actor_ref_t &actor,
                                                     const zlink::routing_id_t &session_rid,
                                                     std::uint64_t binding_generation,
                                                     const runtime::spot_address_t &actor_route,
                                                     std::chrono::milliseconds timeout)
{
    try {
        if (binding_generation == 0 || actor_route.node_generation == 0
            || actor_route.authority_owner_generation == 0
            || actor_route.owner.lease_generation <= 0
            || actor_route.object_generation != actor.object_generation ()) {
            co_return result_t<application_actor_session_bind_outcome_t>::failure (
              framework_error_kind_t::invalid_operation,
              "Remote Actor session binding fence is invalid");
        }
        auto completion = std::make_shared<
          detail::task_completion_source_t<runtime::protocol::reply_header_t>> ();
        auto output = completion->task ();
        const auto actor_fence = runtime::protocol::actor_route_fence_t{
          std::string (actor.actor_id ().value ()), actor.object_generation (),
          actor_route.node_rid.to_bytes (), actor_route.node_generation,
          actor_route.authority_owner_generation,
          static_cast<std::uint64_t> (actor_route.owner.lease_generation)};
        const auto submitted = co_await _node->transport ().request_bound_session_bind (
          actor_route.node_rid.to_bytes (),
          runtime::protocol::bound_session_bind_t{
            0,
            actor_fence,
            session_rid.to_bytes (),
            {runtime::protocol::bound_session_binding_state_t::active, binding_generation}},
          timeout,
          [completion] (runtime::foundation::operation_terminal_t terminal,
                        std::vector<std::uint8_t> payload) {
              if (terminal != runtime::foundation::operation_terminal_t::completed) {
                  const auto kind =
                    terminal
                        == runtime::foundation::operation_terminal_t::timed_out
                      ? framework_error_kind_t::deadline_exceeded
                      : framework_error_kind_t::unavailable;
                  completion->complete (result_t<runtime::protocol::reply_header_t>::failure (
                    kind,
                    "Remote Actor session binding did not complete successfully"));
                  return;
              }
              try {
                  completion->complete (result_t<runtime::protocol::reply_header_t>::success (
                    runtime::protocol::decode_reply_header (payload)));
              }
              catch (const runtime::protocol::service_wire_error_t &) {
                  completion->complete (result_t<runtime::protocol::reply_header_t>::failure (
                    framework_error_kind_t::protocol_error,
                    "Remote Actor session binding reply decode failed"));
              }
          });
        if (!submitted) {
            co_return result_t<application_actor_session_bind_outcome_t>::failure (
              framework_error_kind_t::not_configured,
              "Remote Actor session binding was not accepted");
        }
        const auto reply = co_await output;
        if (reply.terminal_result == 0) {
            co_return result_t<application_actor_session_bind_outcome_t>::success (
              application_actor_session_bind_outcome_t::bound);
        }
        const auto stale_route =
          reply.terminal_result
              == static_cast<std::uint32_t> (
                runtime::protocol::request_terminal_result::conflict)
          && reply.failure_code
              == static_cast<std::uint32_t> (
                runtime::protocol::framework_error_code::actorLocationStale);
        if (stale_route) {
            if (_actor_route_invalidator)
                _actor_route_invalidator (actor_fence);
            co_return result_t<application_actor_session_bind_outcome_t>::success (
              application_actor_session_bind_outcome_t::stale_route);
        }
        const auto actor_not_ready =
          reply.terminal_result
              == static_cast<std::uint32_t> (
                runtime::protocol::request_terminal_result::busy)
          && reply.failure_code == 0;
        if (actor_not_ready) {
            co_return result_t<application_actor_session_bind_outcome_t>::success (
              application_actor_session_bind_outcome_t::actor_not_ready);
        }
        co_return result_t<application_actor_session_bind_outcome_t>::failure (
          framework_error_kind_t::unavailable,
          "Remote Actor session binding was rejected");
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<
          application_actor_session_bind_outcome_t> (error);
    }
    catch (const std::exception &error) {
        co_return result_t<application_actor_session_bind_outcome_t>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
}

task_t<void> mesh_node_runtime_t::retire_application_actor_session (
  runtime::stateful::stream_binding_t binding,
  zlink::routing_id_t session_rid,
  std::chrono::milliseconds timeout)
{
    if (binding.binding_generation == 0
        || binding.actor.kind != runtime::stateful::object_kind_t::actor
        || binding.actor.key.empty () || binding.actor.object_generation == 0
        || binding.actor.authority_owner_generation == 0
        || binding.actor.node_id.empty ()
        || binding.target_node_generation == 0
        || binding.owner_lease_generation == 0
        || session_rid.to_bytes ().empty ()) {
        throw framework_exception_t (
          framework_error_kind_t::invalid_operation,
          "retired Actor session binding fence is invalid");
    }
    auto completion = std::make_shared<
      detail::task_completion_source_t<runtime::protocol::reply_header_t>> ();
    auto output = completion->task ();
    const auto actor_owner = zlink::routing_id_t::from (
      binding.actor.node_id);
    const auto submitted = co_await _node->transport ().request_bound_session_bind (
      actor_owner.to_bytes (),
      runtime::protocol::bound_session_bind_t{
        0,
        runtime::protocol::actor_route_fence_t{
          binding.actor.key, binding.actor.object_generation,
          actor_owner.to_bytes (), binding.target_node_generation,
          binding.actor.authority_owner_generation,
          binding.owner_lease_generation},
        session_rid.to_bytes (),
        {runtime::protocol::bound_session_binding_state_t::tombstone,
         binding.binding_generation}},
      timeout,
      [completion] (runtime::foundation::operation_terminal_t terminal,
                    std::vector<std::uint8_t> payload) {
          if (terminal != runtime::foundation::operation_terminal_t::completed) {
              completion->complete (
                result_t<runtime::protocol::reply_header_t>::failure (
                  framework_error_kind_t::unavailable,
                  "retired Actor session binding did not complete"));
              return;
          }
          try {
              completion->complete (
                result_t<runtime::protocol::reply_header_t>::success (
                  runtime::protocol::decode_reply_header (payload)));
          }
          catch (const runtime::protocol::service_wire_error_t &) {
              completion->complete (
                result_t<runtime::protocol::reply_header_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "retired Actor session binding reply decode failed"));
          }
      });
    if (!submitted) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "retired Actor session binding was not accepted");
    }
    const auto reply = co_await output;
    if (reply.terminal_result != 0) {
        throw framework_exception_t (
          framework_error_kind_t::invalid_operation,
          "retired Actor session binding was rejected");
    }
    co_return;
}

task_t<void> mesh_node_runtime_t::notify_application_actor_disconnected (
  const actor_ref_t &actor, const node_rid_t &target_node, std::chrono::milliseconds timeout)
{
    if (!_serializers) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "MeshNode serializers are not configured");
    }
    try {
        runtime::messaging::client_call_codec_t codec;
        auto envelope =
          codec.create_envelope (runtime::messaging::message_kind_t::request, "spot",
                                 spot_actor_disconnect_route_request_t::packet_name, timeout);
        auto encoded = codec.encode_envelope_parts (
          envelope, make_spot_actor_disconnect_route_request (actor), *_serializers);
        host::call_id_t operation;
        const auto submitted = co_await request_to_node (
          zlink::routing_id_t::from (std::string (target_node.value ())),
          encoded.items (), operation, timeout);
        if (submitted != zlink::submit_result_t::ok) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Actor disconnect notification was not submitted");
        }
        auto completed = co_await await_completion (operation);
        if (completed.record.terminal_result
            != static_cast<int> (zlink::request_result_t::ok)) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Actor disconnect notification returned an error");
        }
        runtime::messaging::message_parts_t reply (std::move (completed.parts));
        auto decoded = codec.decode_envelope_reply<spot_actor_disconnect_route_reply_t> (
          reply, *_serializers, "Actor disconnect reply is empty",
          "Actor disconnect reply decode failed", "DisconnectActor");
        if (!decoded) {
            throw decoded.error ()
                    ? *decoded.error ()
                    : framework_exception_t (framework_error_kind_t::internal_failure,
                                             "Actor disconnect notification failed");
        }
        co_return;
    }
    catch (const framework_exception_t &error) {
        throw error;
    }
    catch (const std::exception &error) {
        throw framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
    }
}

result_t<mesh_node_runtime_t::operation_completion_t>
mesh_node_runtime_t::wait_for_completion (const host::call_id_t &operation,
                                          std::chrono::milliseconds timeout,
                                          std::optional<zlink::routing_id_t> target)
{
    struct completion_wait_scope_t final
    {
        completion_wait_scope_t (std::atomic_uint64_t &count,
                                 std::condition_variable &settled,
                                 bool active) :
            count (count), settled (settled), active (active)
        {
            if (active)
                count.fetch_add (1, std::memory_order_acq_rel);
        }
        ~completion_wait_scope_t ()
        {
            if (active) {
                count.fetch_sub (1, std::memory_order_acq_rel);
                settled.notify_all ();
            }
        }
        std::atomic_uint64_t &count;
        std::condition_variable &settled;
        bool active;
    } completion_wait (
      _active_completion_waiters, _completion_ready,
      target && (!routing_id ()
                 || target->to_bytes () != routing_id ()->to_bytes ()));
    std::unique_lock lock (_completion_mutex);
    if (!_completion_ready.wait_for (lock, timeout, [&] {
            return _completed_operations.contains (operation)
                   || _completion_overflow_operations.contains (operation)
                   || _stopping.load (std::memory_order_acquire);
        })) {
        _completed_operations.erase (operation);
        if (_timed_out_operations.insert (operation).second)
            _timed_out_operation_order.push_back (operation);
        while (!_timed_out_operation_order.empty ()
               && !_timed_out_operations.contains (_timed_out_operation_order.front ()))
            _timed_out_operation_order.pop_front ();
        while (_timed_out_operations.size () > timed_out_operation_capacity) {
            if (_timed_out_operation_order.empty ())
                break;
            _timed_out_operations.erase (_timed_out_operation_order.front ());
            _timed_out_operation_order.pop_front ();
        }
        const auto local = routing_id ();
        const bool targets_local_node =
          target && local && target->to_bytes () == local->to_bytes ();
        if (target && !targets_local_node
            && !_node->transport ().topology ().peer (target->to_bytes ())) {
            return result_t<operation_completion_t>::failure (
              framework_error_kind_t::unavailable,
              "MeshNode target RouteMesh peer became unavailable");
        }
        return detail::boundary_failure<operation_completion_t> (
          detail::boundary_error_t::timed_out, "MeshNode operation timed out");
    }
    if (_completion_overflow_operations.erase (operation) != 0) {
        const auto found = std::find (
          _completion_overflow_order.begin (), _completion_overflow_order.end (), operation);
        if (found != _completion_overflow_order.end ())
            _completion_overflow_order.erase (found);
        return result_t<operation_completion_t>::failure (
          framework_error_kind_t::capacity_exceeded,
          "MeshNode completion holding table is full");
    }
    if (!_completed_operations.contains (operation)) {
        return detail::boundary_failure<operation_completion_t> (
          detail::boundary_error_t::shutdown,
          "MeshNode operation stopped because the runtime is shutting down");
    }
    operation_completion_t completion;
    if (!_completed_operations.take (operation, completion)) {
        return result_t<operation_completion_t>::failure (
          framework_error_kind_t::internal_failure,
          "MeshNode operation completion was consumed concurrently");
    }
    return result_t<operation_completion_t>::success (std::move (completion));
}

task_t<mesh_node_runtime_t::operation_completion_t>
mesh_node_runtime_t::await_completion (const host::call_id_t &operation)
{
    std::shared_ptr<detail::task_completion_source_t<operation_completion_t>> source;
    {
        std::lock_guard lock (_completion_mutex);
        if (_completion_overflow_operations.erase (operation) != 0) {
            co_return result_t<operation_completion_t>::failure (
              framework_error_kind_t::capacity_exceeded,
              "MeshNode completion holding table is full");
        }
        operation_completion_t completed;
        if (_completed_operations.take (operation, completed))
            co_return result_t<operation_completion_t>::success (std::move (completed));
        if (_stopping.load (std::memory_order_acquire)) {
            co_return detail::boundary_failure<operation_completion_t> (
              detail::boundary_error_t::shutdown,
              "MeshNode operation stopped because the runtime is shutting down");
        }
        source = std::make_shared<detail::task_completion_source_t<operation_completion_t>> ();
        if (!_completion_awaiters.emplace (operation, source).second) {
            co_return result_t<operation_completion_t>::failure (
              framework_error_kind_t::invalid_operation,
              "MeshNode operation completion is already awaited");
        }
    }
    co_return co_await source->task ();
}

std::optional<zlink::submit_result_t>
mesh_node_runtime_t::classify_node_direct_target (const zlink::routing_id_t &target) const
{
    if (!_user_spot_store)
        return std::nullopt;
    try {
        location_page_request_t page;
        for (;;) {
            const auto listed =
              _user_spot_store->list_mesh_nodes (_state->mesh_name, page).result ().value ();
            const auto found = std::find_if (listed.items.begin (), listed.items.end (),
                                             [&target] (const mesh_node_descriptor_t &descriptor) {
                                                 return descriptor.rid == target;
                                             });
            if (found != listed.items.end ()) {
                return found->object_role == object_role_t::client
                         ? std::optional<zlink::submit_result_t> (zlink::submit_result_t::not_found)
                         : std::nullopt;
            }
            if (!listed.continuation_token) {
                /* The target being absent from this page of the Location
                 * Store is not proof it does not exist as a live direct-send
                 * peer: route-only mesh members (object_role=none, e.g. a
                 * RouteMesh client with no spot hosting) are never published
                 * to the Location Store, so an admitted transport-level peer
                 * legitimately never appears here. Returning not_found
                 * unconditionally in this branch used to short-circuit
                 * every direct requestToNode/sendToNode before the real
                 * admission check (raw_mesh_node_owner_t's
                 * _topology.peer() gate) ever ran, permanently classifying
                 * such peers as not found regardless of admission state.
                 * Falling through with nullopt lets the caller consult the
                 * actual transport-level topology instead. */
                return std::nullopt;
            }
            page.continuation_token = listed.continuation_token;
        }
    }
    catch (...) {
        return std::nullopt;
    }
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                   const std::vector<zlink::message_t> &parts,
                                   std::vector<std::uint8_t> metadata)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    if (!framework_owned_node_message (parts)) {
        if (const auto classified = classify_node_direct_target (target))
            co_return *classified;
    }
    (void) metadata;
    co_return co_await _node->send_to_node (target, parts);
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                   const std::vector<zlink::message_t> &parts,
                                   const std::map<std::string, std::string> &metadata)
{
    const auto encoded = mesh_metadata_codec_t::encode (metadata);
    co_return co_await send_to_node (
      target, parts, std::vector<std::uint8_t> (encoded));
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::request_to_node (const zlink::routing_id_t &target,
                                      const std::vector<zlink::message_t> &parts,
                                      host::call_id_t &operation_id,
                                      std::chrono::milliseconds timeout,
                                      std::vector<std::uint8_t> metadata)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    if (!framework_owned_node_message (parts)) {
        if (const auto classified = classify_node_direct_target (target))
            co_return *classified;
    }
    (void) metadata;
    co_return co_await _node->request_to_node (
      target, parts, operation_id, timeout);
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::request_to_node (const zlink::routing_id_t &target,
                                      const std::vector<zlink::message_t> &parts,
                                      host::call_id_t &operation_id,
                                      std::chrono::milliseconds timeout,
                                      const std::map<std::string, std::string> &metadata)
{
    const auto encoded = mesh_metadata_codec_t::encode (metadata);
    co_return co_await request_to_node (
      target, parts, operation_id, timeout,
      std::vector<std::uint8_t> (encoded));
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::send_to_channel (const std::string &channel_name,
                                      const std::vector<zlink::message_t> &parts,
                                      std::vector<std::uint8_t> metadata)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    (void) metadata;
    co_return co_await _node->send_to_channel (channel_name, parts);
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::send_to_channel (const std::string &channel_name,
                                      const std::vector<zlink::message_t> &parts,
                                      const std::map<std::string, std::string> &metadata)
{
    const auto encoded = mesh_metadata_codec_t::encode (metadata);
    co_return co_await send_to_channel (
      channel_name, parts, std::vector<std::uint8_t> (encoded));
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::request_to_channel (const std::string &channel_name,
                                         const std::vector<zlink::message_t> &parts,
                                         host::call_id_t &operation_id,
                                         std::chrono::milliseconds timeout,
                                         std::vector<std::uint8_t> metadata)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    (void) metadata;
    co_return co_await _node->request_to_channel (
      channel_name, parts, operation_id, timeout);
}

task_t<zlink::submit_result_t>
mesh_node_runtime_t::request_to_channel (const std::string &channel_name,
                                         const std::vector<zlink::message_t> &parts,
                                         host::call_id_t &operation_id,
                                         std::chrono::milliseconds timeout,
                                         const std::map<std::string, std::string> &metadata)
{
    const auto encoded = mesh_metadata_codec_t::encode (metadata);
    co_return co_await request_to_channel (
      channel_name, parts, operation_id, timeout,
      std::vector<std::uint8_t> (encoded));
}

task_t<std::size_t> mesh_node_runtime_t::dispatch_ready (
  const std::function<void (const host::ready_record_t &,
                            const host::receive_record_t &,
                            std::vector<zlink::message_t>)> &dispatch,
  bool accept_application_receive)
{
    if (!dispatch)
        throw configuration_error ("MeshNode dispatch callback is required");

    co_return co_await _node->dispatch_ready (
      [&] (const host::ready_record_t &ready_record, const host::receive_record_t &record,
           std::vector<zlink::message_t> parts) {
          if (record.kind == host::record_kind_t::completion) {
              std::shared_ptr<detail::task_completion_source_t<operation_completion_t>> awaiter;
              {
                  std::lock_guard lock (_completion_mutex);
                  if (_timed_out_operations.erase (record.operation_id) != 0) {
                      const auto timed_out = std::find (
                        _timed_out_operation_order.begin (),
                        _timed_out_operation_order.end (), record.operation_id);
                      if (timed_out != _timed_out_operation_order.end ())
                          _timed_out_operation_order.erase (timed_out);
                  } else if (const auto found = _completion_awaiters.find (record.operation_id);
                             found != _completion_awaiters.end ()) {
                      awaiter = std::move (found->second);
                      _completion_awaiters.erase (found);
                  } else if (!_completed_operations.contains (record.operation_id)
                             && !_completed_operations.complete (
                               record.operation_id,
                               operation_completion_t{record, parts})) {
                      if (_completion_overflow_operations.insert (record.operation_id).second) {
                          _completion_overflow_order.push_back (record.operation_id);
                          while (_completion_overflow_operations.size ()
                                 > completion_capacity
                                 && !_completion_overflow_order.empty ()) {
                              _completion_overflow_operations.erase (
                                _completion_overflow_order.front ());
                              _completion_overflow_order.pop_front ();
                          }
                      }
                  }
              }
              if (awaiter) {
                  awaiter->complete (result_t<operation_completion_t>::success (
                    operation_completion_t{record, parts}));
              }
              _completion_ready.notify_all ();
          }
          dispatch (ready_record, record, std::move (parts));
      },
      accept_application_receive);
}

host::node_status_t mesh_node_runtime_t::status () const
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    return _node->status ();
}

std::string mesh_node_runtime_t::mesh_name () const
{
    std::lock_guard lock (_state->mutex);
    return _state->mesh_name;
}

std::optional<zlink::routing_id_t> mesh_node_runtime_t::routing_id () const
{
    std::lock_guard lock (_state->mutex);
    return _state->routing_id;
}

std::string mesh_node_runtime_t::listen_endpoint () const
{
    std::lock_guard lock (_state->mutex);
    if (_state->listen_port && !_state->bind_host_override) {
        if (const auto options = _state->framework_options.lock ()) {
            return mesh_endpoint_notation::normalize_endpoint (
              "tcp://" + mesh_endpoint_notation::bracket_ipv6_host (options->bind_host) + ":"
                + std::to_string (*_state->listen_port));
        }
    }
    return _state->listen_endpoint;
}

object_role_t mesh_node_runtime_t::object_role () const
{
    std::lock_guard lock (_state->mutex);
    return _state->object_role;
}

std::vector<std::string> mesh_node_runtime_t::channel_names () const
{
    std::lock_guard lock (_state->mutex);
    std::vector<std::string> result;
    result.reserve (_state->channels.size ());
    for (const auto &[name, _] : _state->channels)
        result.push_back (name);
    return result;
}

std::map<std::string, int> mesh_node_runtime_t::channel_weights () const
{
    std::lock_guard lock (_state->mutex);
    std::map<std::string, int> result;
    for (const auto &[name, registration] : _state->channels)
        if (registration.server)
            result.emplace (name, registration.weight);
    return result;
}

int mesh_node_runtime_t::placement_weight () const
{
    std::lock_guard lock (_state->mutex);
    return _state->placement_weight;
}

std::int32_t mesh_node_runtime_t::actor_limit () const
{
    std::lock_guard lock (_state->mutex);
    return _state->actor_limit;
}

std::int32_t mesh_node_runtime_t::spot_limit () const
{
    std::lock_guard lock (_state->mutex);
    return _state->spot_limit;
}

std::int32_t mesh_node_runtime_t::activation_concurrency_limit () const
{
    std::lock_guard lock (_state->mutex);
    return _state->activation_concurrency_limit;
}

void mesh_node_runtime_t::set_placement_weight (int weight)
{
    if (!_node)
        throw configuration_error ("MeshNode has not started");
    if (weight < 0 || weight > 10000)
        throw configuration_error ("placement weight must be in range 0..10000");
    auto descriptor = native_node ().transport ().topology ().local_descriptor ();
    if (descriptor.descriptor_revision == std::numeric_limits<std::uint64_t>::max ())
        throw configuration_error ("MeshNode descriptor revision is exhausted");
    descriptor.placement_weight = weight;
    ++descriptor.descriptor_revision;
    std::function<void (const std::map<std::string, int> &, int, std::uint64_t)> publisher;
    std::map<std::string, int> channel_weights;
    {
        std::lock_guard lock (_state->mutex);
        publisher = _descriptor_publisher;
        for (const auto &[name, registration] : _state->channels)
            if (registration.server)
                channel_weights.emplace (name, registration.weight);
    }
    if (publisher)
        publisher (channel_weights, weight, descriptor.descriptor_revision);
    native_node ().transport ().topology ().publish_local (std::move (descriptor));
    std::lock_guard lock (_state->mutex);
    _state->placement_weight = weight;
}

std::size_t mesh_node_runtime_t::max_pending () const noexcept
{
    return _state->max_pending;
}

void mesh_node_runtime_t::set_channel_weight (const std::string &channel_name, int weight)
{
    if (!_node)
        throw configuration_error ("MeshNode has not started");
    if (weight < 0 || weight > 10000)
        throw configuration_error ("channel weight must be in range 0..10000");
    auto descriptor = native_node ().transport ().topology ().local_descriptor ();
    const auto descriptor_channel =
      std::find_if (descriptor.channels.begin (), descriptor.channels.end (),
                    [&] (const auto &candidate) { return candidate.name == channel_name; });
    if (descriptor_channel == descriptor.channels.end ())
        throw configuration_error ("RouteMesh channel is not configured: " + mesh_name () + "/"
                                   + channel_name);
    descriptor_channel->weight = weight;
    if (descriptor.descriptor_revision == std::numeric_limits<std::uint64_t>::max ())
        throw configuration_error ("MeshNode descriptor revision is exhausted");
    ++descriptor.descriptor_revision;
    std::function<void (const std::map<std::string, int> &, int, std::uint64_t)> publisher;
    std::map<std::string, int> channel_weights;
    int placement_weight = 100;
    {
        std::lock_guard lock (_state->mutex);
        const auto found = _state->channels.find (channel_name);
        if (found == _state->channels.end () || !found->second.server)
            throw configuration_error ("RouteMesh channel is not configured: " + _state->mesh_name
                                       + "/" + channel_name);
        for (const auto &[name, registration] : _state->channels)
            if (registration.server)
                channel_weights.emplace (name, name == channel_name ? weight : registration.weight);
        placement_weight = _state->placement_weight;
        publisher = _descriptor_publisher;
    }
    if (publisher)
        publisher (channel_weights, placement_weight, descriptor.descriptor_revision);
    native_node ().transport ().topology ().publish_local (std::move (descriptor));
    std::lock_guard lock (_state->mutex);
    _state->channels.at (channel_name).weight = weight;
}

void mesh_node_runtime_t::application_work_enqueued () noexcept
{
    _pending_application_callbacks.fetch_add (1, std::memory_order_relaxed);
}

void mesh_node_runtime_t::application_work_started () noexcept
{
    _pending_application_callbacks.fetch_sub (1, std::memory_order_relaxed);
    _active_application_callbacks.fetch_add (1, std::memory_order_relaxed);
}

void mesh_node_runtime_t::application_work_finished () noexcept
{
    _active_application_callbacks.fetch_sub (1, std::memory_order_relaxed);
}

void mesh_node_runtime_t::note_local_node_submit_attempt ()
{
}

void mesh_node_runtime_t::local_application_work_finished () noexcept
{
    application_work_finished ();
}

std::uint64_t mesh_node_runtime_t::pending_application_callbacks () const noexcept
{
    return _pending_application_callbacks.load (std::memory_order_relaxed);
}

std::uint64_t mesh_node_runtime_t::active_application_callbacks () const noexcept
{
    return _active_application_callbacks.load (std::memory_order_relaxed);
}

std::size_t mesh_node_runtime_t::pending_transport_operations () const noexcept
{
    return _node ? _node->pending_operation_count () : 0;
}

std::uint64_t mesh_node_runtime_t::active_completion_waiters () const noexcept
{
    return _active_completion_waiters.load (std::memory_order_acquire);
}

std::shared_ptr<mesh_node_runtime_t> mesh_node_runtime_t::from (zlink_builder_t &builder,
                                                                const std::string &mesh_name)
{
    const auto found = builder._state->mesh_nodes.find (mesh_name);
    if (found == builder._state->mesh_nodes.end ()) {
        return {};
    }
    return std::make_shared<mesh_node_runtime_t> (found->second);
}

std::vector<std::shared_ptr<mesh_node_builder_state_t>>
mesh_node_runtime_t::registrations (zlink_builder_t &builder)
{
    std::vector<std::shared_ptr<mesh_node_builder_state_t>> registrations;
    registrations.reserve (builder._state->mesh_nodes.size ());
    for (const auto &[_, registration] : builder._state->mesh_nodes)
        registrations.push_back (registration);
    return registrations;
}

} // namespace zlink::framework::detail

namespace zlink::framework
{

mesh_peer_connections_t::mesh_peer_connections_t (
  std::shared_ptr<detail::mesh_node_builder_state_t> state) :
    _state (std::move (state))
{
}

void mesh_peer_connections_t::connect (std::string endpoint)
{
    if (endpoint.empty ()) {
        throw detail::configuration_error ("peer endpoint is required");
    }
    mesh_peer_connection_t connection{
      detail::next_connection_intent_id (), {}, std::move (endpoint)};
    std::function<void (const mesh_peer_connection_t &)> activate;
    {
        std::lock_guard lock (_state->mutex);
        _state->peer_connections.push_back (connection);
        activate = _state->runtime_peer_connect;
    }
    if (activate)
        activate (connection);
}

void mesh_peer_connections_t::connect (zlink::routing_id_t expected_routing_id,
                                       std::string endpoint)
{
    if (endpoint.empty ()) {
        throw detail::configuration_error ("peer endpoint is required");
    }
    mesh_peer_connection_t connection{detail::next_connection_intent_id (),
                                      std::move (expected_routing_id), std::move (endpoint)};
    std::function<void (const mesh_peer_connection_t &)> activate;
    {
        std::lock_guard lock (_state->mutex);
        _state->peer_connections.push_back (connection);
        activate = _state->runtime_peer_connect;
    }
    if (activate)
        activate (connection);
}

void mesh_peer_connections_t::disconnect (std::string endpoint)
{
    std::vector<mesh_peer_connection_t> removed;
    std::function<void (const mesh_peer_connection_t &)> deactivate;
    {
        std::lock_guard lock (_state->mutex);
        for (auto it = _state->peer_connections.begin (); it != _state->peer_connections.end ();) {
            if (it->endpoint != endpoint) {
                ++it;
                continue;
            }
            removed.push_back (*it);
            it = _state->peer_connections.erase (it);
        }
        deactivate = _state->runtime_peer_disconnect;
    }
    if (deactivate)
        for (const auto &connection : removed)
            deactivate (connection);
}

std::vector<mesh_peer_connection_t> mesh_peer_connections_t::list_connections () const
{
    std::lock_guard lock (_state->mutex);
    return _state->peer_connections;
}

mesh_channel_builder_t::mesh_channel_builder_t (
  std::shared_ptr<detail::mesh_node_builder_state_t> state, std::string channel_name) :
    _state (std::move (state)), _channel_name (std::move (channel_name))
{
}

mesh_channel_client_builder_t mesh_channel_builder_t::client ()
{
    std::lock_guard lock (_state->mutex);
    auto &channel = _state->channels[_channel_name];
    if (channel.role_selected)
        throw detail::configuration_error ("RouteMesh channel role is already selected: "
                                           + _channel_name);
    channel.role_selected = true;
    channel.server = false;
    return {};
}

mesh_channel_server_builder_t mesh_channel_builder_t::server ()
{
    std::lock_guard lock (_state->mutex);
    auto &channel = _state->channels[_channel_name];
    if (channel.role_selected)
        throw detail::configuration_error ("RouteMesh channel role is already selected: "
                                           + _channel_name);
    channel.role_selected = true;
    channel.server = true;
    return mesh_channel_server_builder_t (_state, _channel_name);
}

mesh_channel_server_builder_t::mesh_channel_server_builder_t (
  std::shared_ptr<detail::mesh_node_builder_state_t> state, std::string channel_name) :
    _state (std::move (state)), _channel_name (std::move (channel_name))
{
}

mesh_channel_server_builder_t &mesh_channel_server_builder_t::set_weight (int weight)
{
    if (weight < 0 || weight > 10000) {
        throw detail::configuration_error ("ChannelName weight must be in range 0..10000");
    }
    std::lock_guard lock (_state->mutex);
    _state->channels[_channel_name].weight = weight;
    return *this;
}

mesh_channel_server_builder_t &
mesh_channel_server_builder_t::use_handler_group (std::string group_name)
{
    if (group_name.empty ()) {
        throw detail::configuration_error ("handler group name is required");
    }
    std::shared_ptr<detail::handler_group_options_state_t> handler_groups;
    {
        std::lock_guard lock (_state->mutex);
        _state->channels[_channel_name].handler_group = group_name;
        handler_groups = _state->handler_groups;
    }
    if (handler_groups) {
        handler_groups->add_mesh_channel (
          group_name, _channel_name,
          {detail::handler_group_kind_t::request, detail::handler_group_kind_t::send},
          "MeshNode channel");
    }
    return *this;
}

mesh_channel_server_builder_t &
mesh_channel_server_builder_t::add_handler_group (std::string group_name)
{
    return use_handler_group (std::move (group_name));
}

mesh_channel_server_builder_t &mesh_channel_server_builder_t::add_handler_registration (
  detail::mesh_handler_registration_t registration)
{
    detail::route_handler_descriptor_t descriptor{registration.request
                                                    ? runtime::messaging::message_kind_t::request
                                                    : runtime::messaging::message_kind_t::command,
                                                  registration.dispatch_name,
                                                  registration.packet_name,
                                                  registration.owner_type,
                                                  registration.message_type,
                                                  registration.reply_type};
    std::lock_guard lock (_state->mutex);
    _state->handlers.add_handler (std::move (descriptor), std::move (registration.invoke));
    return *this;
}

mesh_node_builder_t::mesh_node_builder_t (
  std::shared_ptr<detail::mesh_node_builder_state_t> state) :
    _state (std::move (state)), _peer_connections (_state)
{
}

mesh_channel_builder_t mesh_node_builder_t::channel_name (std::string channel_name)
{
    if (channel_name.empty ()) {
        throw detail::configuration_error ("ChannelName is required");
    }
    std::function<void (const std::string &)> observer;
    {
        std::lock_guard lock (_state->mutex);
        _state->channels.try_emplace (channel_name);
        observer = _state->channel_name_observer;
    }
    if (observer) {
        observer (channel_name);
    }
    return mesh_channel_builder_t (_state, std::move (channel_name));
}

mesh_channel_builder_t mesh_node_builder_t::channel (std::string channel_name)
{
    return this->channel_name (std::move (channel_name));
}

mesh_node_builder_t &mesh_node_builder_t::listen (std::string endpoint)
{
    if (endpoint.empty ()) {
        throw detail::configuration_error ("MeshNode listen endpoint is required");
    }
    std::lock_guard lock (_state->mutex);
    _state->listen_port.reset ();
    _state->listen_endpoint = mesh_endpoint_notation::normalize_endpoint (endpoint);
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::listen (std::uint16_t port)
{
    std::lock_guard lock (_state->mutex);
    _state->listen_port = port;
    _state->listen_endpoint = mesh_endpoint_notation::normalize_endpoint (
      "tcp://" + mesh_endpoint_notation::bracket_ipv6_host (_state->bind_host) + ":"
        + std::to_string (port));
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_bind_host (std::string host)
{
    if (host.empty ())
        throw detail::configuration_error ("MeshNode bind host is required");
    std::lock_guard lock (_state->mutex);
    _state->bind_host_override = host;
    _state->bind_host = std::move (host);
    if (_state->listen_port) {
        _state->listen_endpoint = mesh_endpoint_notation::normalize_endpoint (
          "tcp://" + mesh_endpoint_notation::bracket_ipv6_host (_state->bind_host) + ":"
            + std::to_string (*_state->listen_port));
    }
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_advertise_host (std::string host)
{
    if (host.empty ())
        throw detail::configuration_error ("MeshNode advertise host is required");
    std::lock_guard lock (_state->mutex);
    _state->advertise_host_override = host;
    _state->advertise_host = std::move (host);
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_routing_id (zlink::routing_id_t routing_id)
{
    std::lock_guard lock (_state->mutex);
    if (_state->automatic_routing_id_prefix)
        throw detail::configuration_error (
          "MeshNode cannot configure both a fixed routing id and an automatic routing id prefix");
    _state->spot_state->snapshot.routing_id = routing_id;
    _state->routing_id = std::move (routing_id);
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_automatic_routing_id_prefix (std::string prefix)
{
    if (!detail::valid_routing_id_prefix (prefix))
        throw detail::configuration_error ("MeshNode automatic routing id prefix must contain "
                                           "1..64 ASCII letters, digits, '.', '_' or '-'");
    std::lock_guard lock (_state->mutex);
    if (_state->routing_id)
        throw detail::configuration_error (
          "MeshNode cannot configure both a fixed routing id and an automatic routing id prefix");
    _state->automatic_routing_id_prefix = prefix;
    _state->routing_id = zlink::routing_id_t::from (prefix + "-" + detail::new_uuid_v4 ());
    _state->spot_state->snapshot.routing_id = *_state->routing_id;
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_object_role (object_role_t role)
{
    std::lock_guard lock (_state->mutex);
    _state->object_role = role;
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_placement_weight (int weight)
{
    if (weight < 0 || weight > 10000)
        throw detail::configuration_error ("placement weight must be in range 0..10000");
    std::lock_guard lock (_state->mutex);
    _state->placement_weight = weight;
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_actor_limit (std::int32_t limit)
{
    if (limit < 0)
        throw detail::configuration_error ("Actor capacity limit must be non-negative");
    std::lock_guard lock (_state->mutex);
    _state->actor_limit = limit;
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_spot_limit (std::int32_t limit)
{
    if (limit < 0)
        throw detail::configuration_error ("Spot capacity limit must be non-negative");
    std::lock_guard lock (_state->mutex);
    _state->spot_limit = limit;
    return *this;
}

mesh_node_builder_t &
mesh_node_builder_t::set_instance_spot_idle_timeout (std::chrono::milliseconds timeout)
{
    if (timeout < std::chrono::milliseconds::zero ())
        throw detail::configuration_error ("Instance Spot idle timeout must not be negative");
    std::lock_guard lock (_state->mutex);
    _state->instance_spot_idle_timeout = timeout;
    _state->spot_state->instance_spot_idle_timeout = timeout;
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_activation_concurrency (std::int32_t limit)
{
    if (limit <= 0)
        throw detail::configuration_error ("Activation concurrency limit must be positive");
    std::lock_guard lock (_state->mutex);
    _state->activation_concurrency_limit = limit;
    return *this;
}

mesh_node_socket_config_t &mesh_node_builder_t::configure_router_socket ()
{
    return _state->socket;
}

mesh_peer_connections_t &mesh_node_builder_t::peer_connections ()
{
    return _peer_connections;
}

mesh_node_builder_t &
mesh_node_builder_t::set_default_request_timeout (std::chrono::milliseconds timeout)
{
    if (timeout <= std::chrono::milliseconds::zero ()) {
        throw detail::configuration_error ("request timeout must be greater than zero");
    }
    std::lock_guard lock (_state->mutex);
    _state->default_request_timeout = timeout;
    return *this;
}

void mesh_node_builder_t::mark_node_direct_handler ()
{
    std::lock_guard lock (_state->mutex);
    _state->has_node_direct_handler = true;
}

spot_node_builder_t &mesh_node_builder_t::spot_builder ()
{
    return _state->spot_builder;
}

std::string mesh_node_builder_t::route_dispatch_name () const
{
    std::lock_guard lock (_state->mutex);
    return _state->mesh_name;
}

} // namespace zlink::framework
