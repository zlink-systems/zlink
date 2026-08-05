/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/mesh_node_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/sha256.hpp"
#include "runtime/messaging/async_submit_runtime.hpp"

#include "runtime/channels/channel_runtime.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/spots/spot_route_packets.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/errors/error.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <set>
#include <thread>
#include <utility>

namespace zlink::framework::detail
{
namespace
{

std::chrono::milliseconds one_way_send_timeout (const mesh_node_builder_state_t &state)
{
    return state.socket.send_timeout.value_or (std::chrono::seconds (1));
}

bool framework_owned_node_message (
  const std::vector<zlink::message_t> &parts)
{
    try {
        runtime::messaging::message_parts_t encoded (parts);
        const auto header =
          runtime::messaging::envelope_codec_t{}.decode_header (encoded);
        return header && header.value ().message_name.starts_with ("__zlink.");
    }
    catch (...) {
        return false;
    }
}

std::size_t message_follow_payload_bytes (
  const runtime::messaging::envelope_header_t &header,
  const zlink::message_t &payload)
{
    auto total = payload.to_bytes ().size () + header.message_name.size ()
                 + header.content_type.size ();
    for (const auto &[key, value] : header.metadata)
        total += key.size () + value.size ();
    return total;
}

class actor_message_follow_lease_t
{
  public:
    actor_message_follow_lease_t (
      spot_node_runtime_t runtime,
      actor_ref_t actor,
      std::size_t payload_bytes) :
        _runtime (std::move (runtime)), _actor (std::move (actor)),
        _payload_bytes (payload_bytes)
    {
    }

    ~actor_message_follow_lease_t ()
    {
        _runtime.release_actor_message_follow (_actor, _payload_bytes);
    }

  private:
    spot_node_runtime_t _runtime;
    actor_ref_t _actor;
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

std::string spot_submit_target (const zlink::routing_id_t &node,
                                const std::string &spot)
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
    return framework_exception_t (framework_error_kind_t::protocol_error,
                                  std::move (message));
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
  std::function<void (const std::map<std::string, int> &,
                      int,
                      std::uint64_t)> publisher)
{
    std::lock_guard lock (_state->mutex);
    _descriptor_publisher = std::move (publisher);
}

void mesh_node_runtime_t::start ()
{
    if (_node) {
        return;
    }
    _stopping.store (false, std::memory_order_release);
    runtime::messaging::activate_submit_owner (this);

    std::lock_guard lock (_state->mutex);
    _state->spot_state->one_way_send_timeout = one_way_send_timeout (*_state);
    _state->spot_state->instance_spot_idle_timeout =
      _state->instance_spot_idle_timeout;
    if (_state->mesh_name.empty ()) {
        throw configuration_error ("MeshName is required");
    }
    if (_state->listen_endpoint.empty ()) {
        throw configuration_error ("MeshNode listen endpoint is required");
    }
    if (!_state->routing_id) {
        throw configuration_error ("MeshNode routing id is required");
    }
    for (const auto &[channel_name, channel] : _state->channels) {
        if (!channel.role_selected)
            throw configuration_error (
              "RouteMesh channel requires a Client or Server role: "
              + channel_name);
    }
    if (_state->object_role == object_role_t::client
        && _state->has_node_direct_handler) {
        throw configuration_error (
          "Object Client cannot register application Node direct handlers");
    }
    if (_state->socket.send_timeout
        && (_state->socket.send_timeout->count () <= 0
            || _state->socket.send_timeout->count ()
                 > std::numeric_limits<int>::max ())) {
        throw configuration_error (
          "MeshNode send timeout must be between 1 and INT_MAX milliseconds");
    }

    std::vector<runtime::mesh::service_channel_descriptor_t> channels;
    channels.reserve (_state->channels.size ());
    for (const auto &[channel_name, channel] : _state->channels) {
        if (channel.server)
            channels.push_back (
              runtime::mesh::service_channel_descriptor_t{
                channel_name, channel.weight});
    }
    std::sort (channels.begin (), channels.end (),
               [] (const auto &left, const auto &right) {
                   return left.name < right.name;
               });
    std::set<std::string> object_stable_types (
      _state->spot_state->snapshot.actor_types.begin (),
      _state->spot_state->snapshot.actor_types.end ());
    object_stable_types.insert ("framework.spot");
    const auto normalize_mailbox_budget = [] (std::uint64_t configured,
                                              std::size_t fallback,
                                              const char *name) {
        const auto value = configured == 0 ? fallback : configured;
        if (value > std::numeric_limits<std::size_t>::max ()) {
            throw configuration_error (
              std::string ("MeshNode ") + name
              + " exceeds the platform mailbox budget range");
        }
        return static_cast<std::size_t> (value);
    };
    const auto application_message_budget = normalize_mailbox_budget (
      _state->socket.mailbox_message_budget,
      runtime::dispatch_limits::application_mailbox_messages,
      "mailbox message budget");
    const auto application_byte_budget = normalize_mailbox_budget (
      _state->socket.mailbox_byte_budget,
      runtime::dispatch_limits::application_mailbox_bytes,
      "mailbox byte budget");
    auto node = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{
        runtime::mesh::raw_mesh_node_options_t{
          runtime::mesh::service_node_descriptor_t{
            .mesh_name = _state->mesh_name,
            .node_routing_id = _state->routing_id->to_bytes (),
            .lifecycle_generation = 1,
            .descriptor_revision = 1,
            .advertised_endpoint = _state->listen_endpoint,
            .channels = std::move (channels),
            .state = runtime::mesh::service_node_state_t::preparing,
            .effective_max_message_bytes =
              _state->socket.max_message_size > 0
                ? static_cast<std::uint32_t> (_state->socket.max_message_size)
                : 4u * 1024u * 1024u,
            .object_role =
              _state->object_role == object_role_t::client
                ? runtime::mesh::service_object_role_t::client
                : _state->object_role == object_role_t::server
                    ? runtime::mesh::service_object_role_t::server
                    : runtime::mesh::service_object_role_t::none,
            .placement_weight = _state->placement_weight},
          application_message_budget,
          application_byte_budget,
          runtime::dispatch_limits::control_mailbox_messages,
          runtime::dispatch_limits::control_mailbox_bytes,
          _state->socket.send_high_water_mark.bytes (),
          _state->socket.receive_high_water_mark.bytes (),
          _state->advertise_host,
          _state->auto_hwm_profile},
        _state->spot_state->snapshot.entry_spot_name.value_or ("entry"),
        std::move (object_stable_types),
        _route_cache_max_age});
    if (_spot_route_fence_resolver)
        node->configure_spot_route_fence_resolver (
          _spot_route_fence_resolver);
    if (_user_spot_store && _user_spot_materializer) {
        node->configure_user_spot_operations (
          _user_spot_store, _user_spot_materializer);
    }
    if (_actor_create_target)
        node->configure_actor_create_operations (
          _actor_create_target);
    if (_instance_spot_materializer) {
        node->configure_instance_spot_operations (
          _user_spot_store, _instance_spot_relocations,
          _instance_spot_owner, _instance_spot_materializer);
    }
    if (_session_route_owner_resolver)
        node->configure_session_route_owner (
          _session_route_owner_resolver);
    if (_stateful_dispatch_resolver)
        node->configure_stateful_dispatch (
          _stateful_dispatch_resolver);
    node->configure_message_follow_handler ([this] (const auto &notice) {
        dispatch_message_follow (notice);
    });
    if (_relocation_authority && _relocation_store)
        node->configure_relocation (
          _relocation_authority, _relocation_store,
          _aggregate_relocation_authority);
    node->transport ().set_send_ready_handler ([this] {
        runtime::messaging::notify_submit_ready (this);
    });
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
            const auto intent =
              peer.expected_routing_id
                ? node->connect_peer (peer.endpoint, *peer.expected_routing_id)
                : node->connect_peer (peer.endpoint);
            if (intent)
                _peer_connection_intents.emplace (
                  peer.endpoint, next_connection_intent_id ());
        }
    }
    _node = std::move (node);
    spot_node_runtime_t spot_runtime (_state->spot_state);
    spot_runtime.attach_native_node (_node);
    if (_state->spot_state->snapshot.entry_spot_name) {
        auto native_entry = _node->entry_spot ();
        (void) spot_runtime.get_or_create_spot (
          *_state->spot_state->snapshot.entry_spot_name,
          spot_id_t (native_entry.spot_id ()), zlink::message_t{},
          native_entry.status ().lifecycle_generation (),
          _state->mesh_name);
    }
}

void mesh_node_runtime_t::configure_user_spot_operations (
  std::shared_ptr<location_repository_t> store,
  host::user_spot_materializer_t materializer)
{
    if (_node)
        throw configuration_error (
          "User Spot operations must be configured before MeshNode start");
    _user_spot_store = std::move (store);
    _user_spot_materializer = std::move (materializer);
}

void mesh_node_runtime_t::configure_spot_route_fence_resolver (
  host::spot_route_fence_resolver_t resolver,
  std::chrono::milliseconds route_cache_max_age)
{
    if (_node)
        throw configuration_error (
          "Spot route fence resolver must be configured before MeshNode start");
    if (route_cache_max_age < std::chrono::milliseconds::zero ())
        throw configuration_error (
          "Spot route cache age must not be negative");
    _spot_route_fence_resolver = std::move (resolver);
    _route_cache_max_age = route_cache_max_age;
}

void mesh_node_runtime_t::configure_actor_route_resolver (
  std::function<std::optional<runtime::spot_address_t> (
    const actor_ref_t &)> resolver,
  std::function<void (const runtime::protocol::actor_route_fence_t &)>
    invalidator)
{
    if (_node)
        throw configuration_error (
          "Actor route resolver must be configured before MeshNode start");
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
    if (!store || !relocations || owner.owner_id.empty ()
        || owner.lease_generation <= 0 || !materializer)
        throw configuration_error (
          "Instance Spot operations require Location and Relocation Stores, an owner lease, and a materializer");
    _user_spot_store = std::move (store);
    _instance_spot_relocations = std::move (relocations);
    _instance_spot_owner = std::move (owner);
    _instance_spot_materializer = std::move (materializer);
}

void mesh_node_runtime_t::configure_relocation_runtime (
  std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority,
  std::shared_ptr<runtime::stateful::relocation_store_port_t> relocations,
  std::shared_ptr<runtime::stateful::aggregate_authority_port_t>
    aggregate_authority)
{
    if (_node)
        throw configuration_error (
          "Relocation runtime must be configured before MeshNode start");
    if (!authority || !relocations)
        throw configuration_error (
          "Relocation runtime requires Location and Relocation Stores");
    _relocation_authority = std::move (authority);
    _relocation_store = std::move (relocations);
    _aggregate_relocation_authority =
      std::move (aggregate_authority);
}

runtime::stateful::relocation_result_t
mesh_node_runtime_t::relocate_application_actor (
  const actor_ref_t &actor,
  const mesh_node_descriptor_t &target,
  const authority_snapshot_t &authority,
  relocation_capacity_fence_t capacity_fence)
{
    const auto blocked = [] {
        return runtime::stateful::relocation_result_t{
          runtime::stateful::relocation_terminal_t::blocked,
          runtime::stateful::relocation_reason_t::restore_failed,
          std::nullopt};
    };
    if (!_node || target.lifecycle_generation == 0
        || target.owner_id.empty () || target.lease_generation <= 0
        || capacity_fence.value.empty ())
        return blocked ();
    auto *maintenance = _node->maintenance ();
    const auto source = _node->resolve_actor (actor);
    const auto status = _node->status ();
    if (!maintenance || !source
        || authority.object_generation != source->object_generation
        || authority.authority_owner_generation
             != source->authority_owner_generation
        || authority.owner.owner_id.empty ()
        || authority.owner.lease_generation <= 0
        || authority.store_version.empty ()
        || authority.allocation.target.node_rid.value ()
             != status.routing_id ().to_string ())
        return blocked ();

    static std::atomic<std::uint64_t> next_relocation{1};
    const auto sequence =
      next_relocation.fetch_add (1, std::memory_order_relaxed);
    const auto relocation = runtime::protocol::relocation_id_t{
      status.lifecycle_generation (), sequence == 0 ? 1 : sequence};
    const runtime::protocol::relocation_coordinator_fence_t coordinator{
      authority.owner.owner_id,
      static_cast<std::uint64_t> (
        authority.owner.lease_generation),
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      authority.store_version};
    const auto target_routing_id = target.rid;
    const runtime::protocol::request_source_fence_t
      source_cleanup_fence{
        authority.owner.owner_id,
        static_cast<std::uint64_t> (
          authority.owner.lease_generation),
        status.routing_id ().to_bytes (),
        status.lifecycle_generation ()};
    const runtime::protocol::request_source_fence_t
      target_completion_fence{
        target.owner_id,
        static_cast<std::uint64_t> (target.lease_generation),
        target.rid.to_bytes (),
        target.lifecycle_generation};

    runtime::stateful::eligible_relocation_unit_t::
      canonical_wire_context_t wire{
      .relocation = relocation,
      .target_attempt_generation = target.lifecycle_generation,
      .coordinator = coordinator,
      .target_node_routing_id = target.rid.to_bytes (),
      .target_node_generation = target.lifecycle_generation,
      .participant_ids = {1},
      .prepare_target =
        [this,
         target,
         source_status = status,
         source = *source,
         stable_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
         relocation,
         coordinator] (const std::vector<
              runtime::stateful::frozen_object_state_t> &,
            const std::vector<
              runtime::protocol::relocation_data_t> &records,
            const runtime::stateful::relocation_stored_t &stored) {
            std::size_t required_bytes = 0;
            for (const auto &record : records)
                required_bytes +=
                  runtime::protocol::encode_relocation_control (
                    record)
                    .size ();
            runtime::protocol::relocation_object_kind_t kind;
            switch (source.kind) {
            case runtime::stateful::object_kind_t::actor:
                kind =
                  runtime::protocol::relocation_object_kind_t::
                    actor;
                break;
            case runtime::stateful::object_kind_t::user_spot:
                kind =
                  runtime::protocol::relocation_object_kind_t::
                    user_spot;
                break;
            case runtime::stateful::object_kind_t::instance_spot:
                kind =
                  runtime::protocol::relocation_object_kind_t::
                    instance_spot;
                break;
            default:
                return false;
            }
            return _node->prepare_relocation_remote (
              target.rid,
              runtime::protocol::relocation_prepare_t{
                relocation,
                target.lifecycle_generation,
                runtime::protocol::relocation_round_t::initial,
                coordinator,
                {target.rid.to_bytes (),
                 target.lifecycle_generation,
                 target.owner_id,
                 static_cast<std::uint64_t> (
                   target.lease_generation)},
                runtime::protocol::relocation_role_t::source,
                {kind,
                 stable_type,
                 source.key,
                 source.object_generation,
                 source.authority_owner_generation},
                source_status.routing_id ().to_bytes (),
                source_status.lifecycle_generation (),
                records.size (),
                required_bytes,
                {runtime::protocol::relocation_participant_t{
                  1,
                  runtime::protocol::
                    relocation_participant_kind_t::
                      object_mailbox,
                  {},
                  0,
                  {},
                  0,
                  {},
                  0,
                  records.size (),
                  required_bytes}},
                runtime::protocol::relocation_root_t{
                  stored.reference,
                  stored.checksum_crc32c},
                static_cast<std::uint64_t> (
                  std::max<std::int64_t> (
                    0, target.application_version))},
              std::chrono::seconds (5));
        },
      .acknowledged = [] (std::uint64_t, std::uint64_t) {},
      .acknowledged_records =
        [this, source = *source] (
          std::uint64_t,
          const std::vector<runtime::protocol::relocation_data_t> &records,
          std::uint64_t high_water) {
            for (const auto &record : records) {
                if (record.sequence > high_water
                    || !record.frozen_record
                    || record.frozen_record->reply_route_id)
                    continue;
                (void) _node->acknowledge_relocated_source (
                  source, record.frozen_record->operation);
            }
        },
      .complete_source_terminal =
        [this, source = *source] (
            std::uint64_t,
            std::uint64_t sequence,
            const runtime::protocol::reply_relay_t &relay,
            const std::optional<
              runtime::protocol::application_payload_t> &reply) {
            return _node->complete_relocated_source (
              source, sequence, relay, reply);
        },
      .complete_target =
        [this,
         target_routing_id,
         relocation,
         coordinator,
         target_attempt_generation =
           target.lifecycle_generation,
         source_cleanup_fence,
         target_completion_fence] {
            return _node->complete_relocation_remote (
              target_routing_id,
              runtime::protocol::relocation_complete_t{
                relocation,
                target_attempt_generation,
                coordinator,
                runtime::protocol::relocation_role_t::source,
                source_cleanup_fence,
                runtime::protocol::source_cleanup_state_t::
                  completed},
              target_completion_fence,
              std::chrono::seconds (5), true);
        },
      .abort_target =
        [this,
         target_routing_id,
         relocation,
         coordinator,
         target_attempt_generation =
           target.lifecycle_generation,
         source_cleanup_fence,
         target_completion_fence] {
            (void) _node->complete_relocation_remote (
              target_routing_id,
              runtime::protocol::relocation_complete_t{
                relocation,
                target_attempt_generation,
                coordinator,
                runtime::protocol::relocation_role_t::source,
                source_cleanup_fence,
                runtime::protocol::source_cleanup_state_t::
                  pending},
              target_completion_fence,
              std::chrono::seconds (1), false);
        }};

    std::vector<std::byte> inventory_bytes;
    inventory_bytes.reserve (
      source->key.size () + sizeof (source->object_generation)
      + sizeof (source->authority_owner_generation));
    for (const auto value : source->key)
        inventory_bytes.push_back (
          static_cast<std::byte> (
            static_cast<unsigned char> (value)));
    for (int shift = 56; shift >= 0; shift -= 8) {
        inventory_bytes.push_back (
          static_cast<std::byte> (
            (source->object_generation >> shift) & 0xffu));
        inventory_bytes.push_back (
          static_cast<std::byte> (
            (source->authority_owner_generation >> shift)
            & 0xffu));
    }
    const auto public_digest = runtime::sha256 (inventory_bytes);
    runtime::stateful::inventory_digest_t inventory_digest{};
    for (std::size_t index = 0; index != inventory_digest.size (); ++index)
        inventory_digest[index] =
          std::to_integer<std::uint8_t> (public_digest[index]);

    return maintenance->relocate (
      *source, target.rid.to_string (),
      {target.owner_id, target.lease_generation},
      std::move (capacity_fence),
      256u * 1024u * 1024u,
      inventory_digest, wire);
}

bool mesh_node_runtime_t::application_actor_transfer_in_progress (
  const actor_ref_t &actor) const
{
    return spot_node_runtime_t (_state->spot_state)
      .actor_transfer_in_progress (actor);
}

result_t<bool> mesh_node_runtime_t::destroy_application_actor (
  const actor_ref_t &actor)
{
    return spot_node_runtime_t (_state->spot_state).destroy_actor (actor);
}

runtime::stateful::aggregate_relocation_result_t
mesh_node_runtime_t::relocate_application_unit (
  std::vector<runtime::stateful::object_ref_t> sources,
  std::vector<std::string> stable_types,
  const mesh_node_descriptor_t &target,
  const std::vector<authority_snapshot_t> &authorities,
  std::vector<relocation_capacity_fence_t> capacity_fences)
{
    using namespace runtime::stateful;
    const auto blocked = [] {
        return runtime::stateful::aggregate_relocation_result_t{
          runtime::stateful::relocation_terminal_t::blocked,
          runtime::stateful::relocation_reason_t::restore_failed,
          {}};
    };
    if (!_node || sources.empty ()
        || sources.size () != stable_types.size ()
        || sources.size () != authorities.size ()
        || sources.size () != capacity_fences.size ()
        || target.lifecycle_generation == 0
        || target.owner_id.empty ()
        || target.lease_generation <= 0)
        return blocked ();
    auto *maintenance = _node->maintenance ();
    const auto status = _node->status ();
    if (!maintenance)
        return blocked ();
    for (std::size_t index = 0; index != sources.size (); ++index) {
        if (stable_types[index].empty ()
            || capacity_fences[index].value.empty ()
            || authorities[index].object_generation
                 != sources[index].object_generation
            || authorities[index].authority_owner_generation
                 != sources[index].authority_owner_generation
            || authorities[index].owner.owner_id.empty ()
            || authorities[index].owner.lease_generation <= 0
            || authorities[index].store_version.empty ()
            || authorities[index].allocation.target.node_rid.value ()
                 != status.routing_id ().to_string ())
            return blocked ();
    }

    struct participant_input_t
    {
        object_ref_t source;
        std::string stable_type;
        authority_snapshot_t authority;
        relocation_capacity_fence_t capacity;
    };
    std::vector<participant_input_t> input;
    input.reserve (sources.size ());
    for (std::size_t index = 0; index != sources.size (); ++index) {
        input.push_back (
          {std::move (sources[index]),
           std::move (stable_types[index]),
           authorities[index],
           std::move (capacity_fences[index])});
    }
    std::sort (
      input.begin (), input.end (),
      [] (const auto &left, const auto &right) {
          if (left.source.kind != right.source.kind)
              return left.source.kind < right.source.kind;
          return left.source.key < right.source.key;
      });
    sources.clear ();
    stable_types.clear ();
    capacity_fences.clear ();
    std::vector<std::uint64_t> participant_ids;
    for (std::size_t index = 0; index != input.size (); ++index) {
        sources.push_back (input[index].source);
        stable_types.push_back (input[index].stable_type);
        capacity_fences.push_back (
          std::move (input[index].capacity));
        participant_ids.push_back (index + 1);
    }
    const auto principal =
      std::find_if (
        input.begin (), input.end (), [] (const auto &participant) {
            return participant.source.kind
                   == object_kind_t::user_spot;
        });
    const auto principal_index =
      principal != input.end ()
        ? static_cast<std::size_t> (
            std::distance (input.begin (), principal))
        : 0u;

    static std::atomic<std::uint64_t> next_relocation{1};
    const auto sequence =
      next_relocation.fetch_add (1, std::memory_order_relaxed);
    const runtime::protocol::relocation_id_t relocation{
      status.lifecycle_generation (), sequence == 0 ? 1 : sequence};
    const auto &coordinator_authority =
      input[principal_index].authority;
    const runtime::protocol::relocation_coordinator_fence_t coordinator{
      coordinator_authority.owner.owner_id,
      static_cast<std::uint64_t> (
        coordinator_authority.owner.lease_generation),
      status.routing_id ().to_bytes (),
      status.lifecycle_generation (),
      coordinator_authority.store_version};
    const runtime::protocol::request_source_fence_t
      source_cleanup_fence{
        coordinator.owner_id,
        coordinator.lease_generation,
        coordinator.node_routing_id,
        coordinator.node_generation};
    const runtime::protocol::request_source_fence_t
      target_completion_fence{
        target.owner_id,
        static_cast<std::uint64_t> (target.lease_generation),
        target.rid.to_bytes (),
        target.lifecycle_generation};

    eligible_relocation_unit_t::canonical_wire_context_t wire{
      .relocation = relocation,
      .target_attempt_generation = target.lifecycle_generation,
      .coordinator = coordinator,
      .target_node_routing_id = target.rid.to_bytes (),
      .target_node_generation = target.lifecycle_generation,
      .participant_ids = participant_ids,
      .prepare_target =
        [this, target, status, sources, stable_types,
         principal_index,
         participant_ids, relocation, coordinator] (
          const std::vector<frozen_object_state_t> &,
          const std::vector<
            runtime::protocol::relocation_data_t> &records,
          const runtime::stateful::relocation_stored_t &stored) {
            std::map<std::uint64_t,
                     std::pair<std::size_t, std::size_t>>
              progress;
            for (const auto id : participant_ids)
                progress.emplace (id, std::pair{0u, 0u});
            for (const auto &record : records) {
                auto found = progress.find (
                  record.participant_id);
                if (found == progress.end ())
                    return false;
                ++found->second.first;
                found->second.second +=
                  runtime::protocol::
                    encode_relocation_control (record)
                    .size ();
            }
            std::vector<
              runtime::protocol::relocation_participant_t>
              participants;
            for (const auto id : participant_ids) {
                const auto value = progress.at (id);
                participants.push_back (
                  {id,
                   runtime::protocol::
                     relocation_participant_kind_t::
                       object_mailbox,
                   {}, 0, {}, 0, {}, 0,
                   value.first, value.second});
            }
            runtime::protocol::relocation_object_kind_t kind;
            switch (sources[principal_index].kind) {
            case object_kind_t::actor:
                kind = runtime::protocol::
                  relocation_object_kind_t::actor;
                break;
            case object_kind_t::user_spot:
                kind = runtime::protocol::
                  relocation_object_kind_t::user_spot;
                break;
            case object_kind_t::instance_spot:
                kind = runtime::protocol::
                  relocation_object_kind_t::instance_spot;
                break;
            default:
                return false;
            }
            std::size_t required_bytes = 0;
            for (const auto &[_, value] : progress)
                required_bytes += value.second;
            return _node->prepare_relocation_remote (
              target.rid,
              runtime::protocol::relocation_prepare_t{
                relocation,
                target.lifecycle_generation,
                runtime::protocol::relocation_round_t::initial,
                coordinator,
                {target.rid.to_bytes (),
                 target.lifecycle_generation,
                 target.owner_id,
                 static_cast<std::uint64_t> (
                   target.lease_generation)},
                runtime::protocol::relocation_role_t::source,
                {kind, stable_types[principal_index],
                 sources[principal_index].key,
                 sources[principal_index].object_generation,
                 sources[principal_index]
                   .authority_owner_generation},
                status.routing_id ().to_bytes (),
                status.lifecycle_generation (),
                records.size (), required_bytes,
                std::move (participants),
                runtime::protocol::relocation_root_t{
                  stored.reference, stored.checksum_crc32c},
                static_cast<std::uint64_t> (
                  std::max<std::int64_t> (
                    0, target.application_version))},
              std::chrono::seconds (5));
        },
      .acknowledged = [] (std::uint64_t, std::uint64_t) {},
      .acknowledged_records =
        [this, sources] (
          std::uint64_t participant,
          const std::vector<runtime::protocol::relocation_data_t> &records,
          std::uint64_t high_water) {
            if (participant == 0 || participant > sources.size ())
                return;
            for (const auto &record : records) {
                if (record.sequence > high_water
                    || !record.frozen_record
                    || record.frozen_record->reply_route_id)
                    continue;
                (void) _node->acknowledge_relocated_source (
                  sources[participant - 1],
                  record.frozen_record->operation);
            }
        },
      .complete_source_terminal =
        [this, sources] (
          std::uint64_t participant,
          std::uint64_t sequence,
          const runtime::protocol::reply_relay_t &relay,
          const std::optional<
            runtime::protocol::application_payload_t> &reply) {
            if (participant == 0
                || participant > sources.size ())
                return false;
            return _node->complete_relocated_source (
              sources[participant - 1], sequence, relay, reply);
        },
      .complete_target =
        [this, target, relocation, coordinator,
         source_cleanup_fence, target_completion_fence] {
            return _node->complete_relocation_remote (
              target.rid,
              runtime::protocol::relocation_complete_t{
                relocation, target.lifecycle_generation,
                coordinator,
                runtime::protocol::relocation_role_t::source,
                source_cleanup_fence,
                runtime::protocol::source_cleanup_state_t::
                  completed},
              target_completion_fence,
              std::chrono::seconds (5), true);
        },
      .abort_target =
        [this, target, relocation, coordinator,
         source_cleanup_fence, target_completion_fence] {
            (void) _node->complete_relocation_remote (
              target.rid,
              runtime::protocol::relocation_complete_t{
                relocation, target.lifecycle_generation,
                coordinator,
                runtime::protocol::relocation_role_t::source,
                source_cleanup_fence,
                runtime::protocol::source_cleanup_state_t::
                  pending},
              target_completion_fence,
              std::chrono::seconds (1), false);
        }};

    std::vector<std::byte> inventory;
    for (const auto &source : sources) {
        for (const auto value : source.key)
            inventory.push_back (
              static_cast<std::byte> (
                static_cast<unsigned char> (value)));
        for (int shift = 56; shift >= 0; shift -= 8) {
            inventory.push_back (
              static_cast<std::byte> (
                source.object_generation >> shift));
            inventory.push_back (
              static_cast<std::byte> (
                source.authority_owner_generation >> shift));
        }
    }
    const auto public_digest = runtime::sha256 (inventory);
    runtime::stateful::inventory_digest_t digest{};
    for (std::size_t index = 0; index != digest.size (); ++index)
        digest[index] =
          std::to_integer<std::uint8_t> (
            public_digest[index]);
    if (sources.size () == 1) {
        const auto result = maintenance->relocate (
          sources.front (), target.rid.to_string (),
          {target.owner_id, target.lease_generation},
          std::move (capacity_fences.front ()),
          256u * 1024u * 1024u, digest, wire);
        std::vector<
          runtime::stateful::authority_relocation_reference_t>
          published;
        if (result.authority)
            published.push_back (*result.authority);
        return runtime::stateful::aggregate_relocation_result_t{
          result.terminal, result.reason, std::move (published),
          result.replay_records};
    }
    return maintenance->relocate_aggregate (
      sources, target.rid.to_string (),
      {target.owner_id, target.lease_generation},
      std::move (capacity_fences),
      256u * 1024u * 1024u, digest, wire);
}

void mesh_node_runtime_t::configure_session_route_owner (
  std::function<std::optional<location_owner_token_t> ()>
    owner_resolver)
{
    if (!owner_resolver)
        throw configuration_error (
          "Session route owner resolver is required");
    _session_route_owner_resolver = std::move (owner_resolver);
    if (_node)
        _node->configure_session_route_owner (
          _session_route_owner_resolver);
}

void mesh_node_runtime_t::configure_stateful_dispatch (
  runtime::stateful::accepted_record_authority_resolver_t resolver)
{
    if (!resolver)
        throw configuration_error (
          "Stateful dispatch authority resolver is required");
    if (_node)
        throw configuration_error (
          "Stateful dispatch must be configured before MeshNode start");
    _stateful_dispatch_resolver = std::move (resolver);
}

void mesh_node_runtime_t::set_message_follow_invalidation_handler (
  std::function<void (const runtime::protocol::message_follow_notice_t &)>
    handler)
{
    {
        std::lock_guard lock (_message_follow_mutex);
        _message_follow_handler = std::move (handler);
    }
    if (_node) {
        _node->configure_message_follow_handler ([this] (const auto &notice) {
            dispatch_message_follow (notice);
        });
    }
}

void mesh_node_runtime_t::dispatch_message_follow (
  const runtime::protocol::message_follow_notice_t &notice)
{
    spot_node_runtime_t spot (_state->spot_state);
    spot.invalidate_message_follow_route (notice);
    std::function<void (const runtime::protocol::message_follow_notice_t &)>
      handler;
    std::function<void (const runtime::protocol::actor_route_fence_t &)>
      actor_invalidator;
    {
        std::lock_guard lock (_message_follow_mutex);
        handler = _message_follow_handler;
        actor_invalidator = _actor_route_invalidator;
    }
    if (actor_invalidator) {
        if (const auto *source = std::get_if<
              runtime::protocol::actor_route_fence_t> (&notice.source))
            actor_invalidator (*source);
    }
    if (handler)
        handler (notice);
}

bool mesh_node_runtime_t::activate_instance_spot_remote (
  const zlink::routing_id_t &target_node,
  runtime::protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  runtime::protocol::application_payload_t application_payload,
  std::chrono::milliseconds timeout,
  host::instance_spot_activation_completion_t completion)
{
    if (!_node)
        return false;
    return _node->activate_instance_spot_remote (
      target_node, std::move (request), std::move (metadata),
      std::move (application_payload), timeout,
      std::move (completion));
}

bool mesh_node_runtime_t::send_instance_spot_activation_remote (
  const zlink::routing_id_t &target_node,
  runtime::protocol::instance_spot_activation_header_t request,
  std::optional<std::vector<std::uint8_t>> metadata,
  runtime::protocol::application_payload_t application_payload)
{
    return _node && _node->send_instance_spot_activation_remote (
      target_node, std::move (request), std::move (metadata),
      std::move (application_payload));
}

void mesh_node_runtime_t::stop () noexcept
{
    _stopping.store (true, std::memory_order_release);
    _completion_ready.notify_all ();
    runtime::messaging::shutdown_submit_owner (this);
    {
        std::lock_guard lock (_completion_mutex);
        _actor_join_continuations.clear ();
        _completed_operations.clear ();
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
        spot_node_runtime_t (_state->spot_state)
          .detach_native_node ();
        _node->close ();
    }
    catch (...) {
    }
    _node.reset ();
}

void mesh_node_runtime_t::connect_peer (
  const zlink::routing_id_t &expected_routing_id,
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
          endpoint, expected_routing_id,
          expected_lifecycle_generation,
          std::move (security_identity));
    if (submitted)
        _peer_connection_intents.emplace (
          endpoint, next_connection_intent_id ());
}

void mesh_node_runtime_t::expect_peer (
  const zlink::routing_id_t &expected_routing_id,
  const std::string &endpoint,
  std::uint64_t expected_lifecycle_generation,
  std::string security_identity)
{
    if (!_node || endpoint.empty ())
        return;
    _node->expect_peer (
      endpoint, expected_routing_id,
      expected_lifecycle_generation,
      std::move (security_identity));
}

void mesh_node_runtime_t::forget_peer (
  const zlink::routing_id_t &expected_routing_id,
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
        if (found == _peer_connection_intents.end ())
            return;
        _node->disconnect_peer (endpoint);
        _peer_connection_intents.erase (found);
    }
    catch (...) {
    }
}

bool mesh_node_runtime_t::wait_for_peer_ready (
  const zlink::routing_id_t &target,
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

host::spot_handle_t
mesh_node_runtime_t::get_or_create_spot (std::string spot_id)
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

zlink::submit_result_t mesh_node_runtime_t::send_to_spot (
  const std::string &source_spot_id,
  const zlink::routing_id_t &target_node_rid,
  const std::string &target_spot_id,
  std::uint64_t target_spot_generation,
  const std::vector<zlink::message_t> &parts,
  std::vector<std::uint8_t> metadata)
{
    runtime::messaging::note_submit_attempt (
      spot_submit_target (target_node_rid, target_spot_id), this,
      one_way_send_timeout (*_state), _state->max_pending);
    return get_or_create_spot (source_spot_id)
      .send_to_spot (target_node_rid, target_spot_id,
                     target_spot_generation, parts,
                     zlink::send_flags_t::dontwait, metadata);
}

zlink::submit_result_t mesh_node_runtime_t::request_to_spot (
  const std::string &source_spot_id,
  const zlink::routing_id_t &target_node_rid,
  const std::string &target_spot_id,
  std::uint64_t target_spot_generation,
  const std::vector<zlink::message_t> &parts,
  host::operation_id_t &operation_id,
  std::chrono::milliseconds timeout,
  std::vector<std::uint8_t> metadata)
{
    return get_or_create_spot (source_spot_id)
      .request_to_spot (target_node_rid, target_spot_id,
                        target_spot_generation, parts, operation_id,
                        zlink::send_flags_t::none, timeout, metadata);
}

host::actor_handle_t mesh_node_runtime_t::create_actor (
  std::string actor_type,
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
    auto actor = _node->create_actor (
      std::move (actor_type), actor_id);
    _actors.emplace (std::move (actor_id), actor);
    return actor;
}

zlink::submit_result_t mesh_node_runtime_t::send_to_actor (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  std::vector<std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    runtime::messaging::note_submit_attempt (
      actor_submit_target (target), this, one_way_send_timeout (*_state),
      _state->max_pending);
    return _node->send_to_actor (
      target, parts, metadata, authority_owner_generation,
      owner_lease_generation);
}

zlink::submit_result_t mesh_node_runtime_t::request_to_actor (
  const actor_ref_t &target,
  const std::vector<zlink::message_t> &parts,
  host::operation_id_t &operation_id,
  std::chrono::milliseconds timeout,
  std::vector<std::uint8_t> metadata,
  std::uint64_t authority_owner_generation,
  std::uint64_t owner_lease_generation)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    return _node->request_to_actor (
      target, parts, operation_id, timeout, metadata,
      authority_owner_generation, owner_lease_generation);
}

zlink::submit_result_t mesh_node_runtime_t::send_actor_bound_session (
  const actor_ref_t &actor,
  std::uint64_t expected_binding_generation,
  const std::vector<zlink::message_t> &parts)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    runtime::messaging::note_submit_attempt (
      actor_submit_target (actor), this, one_way_send_timeout (*_state),
      _state->max_pending);
    (void) expected_binding_generation;
    return _node->send_to_actor (actor, parts);
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

bool mesh_node_runtime_t::has_admitted_peer (
  const zlink::routing_id_t &peer_rid,
  std::uint64_t lifecycle_generation) const
{
    if (!_node || lifecycle_generation == 0)
        return false;
    const auto peer = _node->transport ().topology ().peer (
      peer_rid.to_bytes ());
    return peer
           && peer->descriptor.lifecycle_generation
                == lifecycle_generation
           && peer->descriptor.state
                == runtime::mesh::service_node_state_t::serving;
}

host::public_host_runtime_t &mesh_node_runtime_t::native_node ()
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    return *_node;
}

bool mesh_node_runtime_t::prepare_actor_transfer (
  const host::actor_transfer_prepare_t &prepare,
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
        return result_t<actor_ref_t>::failure (
          framework_error_kind_t::internal_failure, error.what ());
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
          runtime::stateful::object_ref_t{
            runtime::stateful::object_kind_t::actor,
            actor_id,
            object_generation,
            authority_owner_generation,
            _state->mesh_name,
            _state->routing_id->to_string ()});
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

result_t<actor_join_reply_t> mesh_node_runtime_t::join_application_actor_to_entry_spot (
  const actor_ref_t &actor,
  const node_rid_t &target_node,
  const zlink::message_t &request,
  std::chrono::milliseconds timeout)
{
    const auto found = _actors.find (std::string (actor.actor_id ().value ()));
    if (found == _actors.end ()) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "local Actor handle was not found");
    }
    host::operation_id_t operation;
    const std::vector<zlink::message_t> parts{request};
    const auto submitted = found->second.join_entry_spot (
      zlink::routing_id_t::from (std::string (target_node.value ())), parts, operation, timeout);
    if (submitted != zlink::submit_result_t::ok) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::internal_failure, "Actor entry Spot join was not submitted");
    }
    auto joined = wait_for_join_completion (operation, actor, timeout);
    return joined;
}

result_t<void> mesh_node_runtime_t::submit_application_actor_entry_spot_join (
  const actor_ref_t &actor,
  const node_rid_t &target_node,
  const zlink::message_t &request,
  std::chrono::milliseconds timeout,
  actor_join_completion_t completion)
{
    if (!completion)
        return result_t<void>::failure (
          framework_error_kind_t::internal_failure,
          "Actor entry Spot join completion is required");
    const auto found = _actors.find (std::string (actor.actor_id ().value ()));
    if (found == _actors.end ())
        return result_t<void>::failure (
          framework_error_kind_t::not_found,
          "local Actor handle was not found");

    host::operation_id_t operation;
    const std::vector<zlink::message_t> parts{request};
    std::unique_lock lock (_completion_mutex);
    const auto submitted = found->second.join_entry_spot (
      zlink::routing_id_t::from (std::string (target_node.value ())),
      parts, operation, timeout);
    if (submitted != zlink::submit_result_t::ok)
        return result_t<void>::failure (
          framework_error_kind_t::internal_failure,
          "Actor entry Spot join was not submitted");
    const auto [_, inserted] = _actor_join_continuations.emplace (
      operation,
      actor_join_continuation_t{actor, std::move (completion)});
    if (!inserted)
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "Actor entry Spot join operation was duplicated");
    return result_t<void>::success ();
}

bool mesh_node_runtime_t::complete_application_actor_entry_spot_join (
  const host::receive_record_t &record,
  const std::vector<zlink::message_t> &parts)
{
    actor_join_completion_t completion;
    std::optional<actor_ref_t> actor;
    {
        std::lock_guard lock (_completion_mutex);
        const auto found = _actor_join_continuations.find (
          record.operation_id);
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

result_t<actor_join_reply_t> mesh_node_runtime_t::join_application_actor_to_spot (
  actor_ref_t actor,
  const node_rid_t &target_node,
  const spot_id_t &target_spot,
  std::uint64_t target_spot_generation,
  const zlink::message_t &request,
  std::chrono::milliseconds timeout,
  std::optional<zlink::routing_id_t> bound_session_node_rid,
  std::optional<zlink::routing_id_t> bound_session_rid)
{
    spot_node_runtime_t spot_runtime (_state->spot_state);
    const auto completion_source_spot =
      spot_runtime.actor_spot (actor);
    auto deliver_completion =
      [&] (std::uint64_t operation_high,
           std::uint64_t operation_low,
           const result_t<actor_join_reply_t> &joined)
        -> result_t<void> {
          if (!joined) {
              return spot_runtime.deliver_actor_join_completion (
                actor,
                actor_join_failed_t{
                  operation_high, operation_low,
                  joined.error_kind ()},
                completion_source_spot);
          }
          const auto reply =
            joined.value ().reply.is_empty ()
              ? std::optional<message_t>{}
              : std::make_optional (
                  message_t::from_raw (
                    joined.value ().reply, _serializers));
          if (joined.value ().result_code == 0) {
              return spot_runtime.deliver_actor_join_completion (
                actor,
                actor_join_accepted_t{
                  operation_high, operation_low,
                  joined.value ().actor, reply},
                completion_source_spot);
          }
          return spot_runtime.deliver_actor_join_completion (
            actor,
            actor_join_rejected_t{
              operation_high, operation_low, reply},
            completion_source_spot);
      };
    const auto local_routing_id = routing_id ();
    const bool remote =
      local_routing_id
      && local_routing_id->to_hex ()
           != zlink::routing_id_t::from (std::string (target_node.value ())).to_hex ();
    if (!remote) {
        const auto found = _actors.find (std::string (actor.actor_id ().value ()));
        if (found == _actors.end ()) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found,
              "local Actor handle was not found");
        }
        host::operation_id_t operation;
        const std::vector<zlink::message_t> parts{request};
        const auto submitted = found->second.join_spot (
          zlink::routing_id_t::from (std::string (target_node.value ())),
          target_spot,
          target_spot_generation, parts, operation, timeout);
        if (submitted != zlink::submit_result_t::ok) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::internal_failure,
              "Actor Spot join was not submitted");
        }
        auto joined = wait_for_join_completion (operation, actor, timeout);
        const auto delivered =
          deliver_completion (operation.high, operation.low, joined);
        if (!delivered)
            return detail::propagate_failure<actor_join_reply_t> (
              delivered,
              "local Actor Join completion callback failed");
        return joined;
    }
    if (!_serializers) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "MeshNode serializers are not configured");
    }
    runtime::messaging::client_call_codec_t codec;
    auto request_route =
      [&] (const auto &route_request, std::string packet_name)
        -> result_t<runtime::messaging::message_parts_t> {
          const auto header = codec.create_envelope (
            runtime::messaging::message_kind_t::request, "spot",
            std::move (packet_name), timeout);
          auto encoded =
            codec.encode_envelope_parts (header, route_request, *_serializers);
          auto origin = get_or_create_spot (
            "__zlink-route-origin-" + routing_id ()->to_hex ());
          host::operation_id_t operation;
          const auto submitted = origin.request_to_spot (
            zlink::routing_id_t::from (std::string (target_node.value ())),
            target_spot,
            target_spot_generation, encoded.items (), operation,
            zlink::send_flags_t::none, timeout);
          if (submitted != zlink::submit_result_t::ok) {
              return result_t<runtime::messaging::message_parts_t>::failure (
                framework_error_kind_t::internal_failure,
                "Actor transfer route request was not submitted");
          }
          auto completed = wait_for_completion (operation, timeout);
          if (!completed)
              return detail::propagate_failure<runtime::messaging::message_parts_t> (
                completed, "Actor transfer route request failed");
          if (completed.value ().record.terminal_result
              != static_cast<int> (zlink::request_result_t::ok)) {
              return result_t<runtime::messaging::message_parts_t>::failure (
                framework_error_kind_t::internal_failure,
                "Actor transfer route request returned an error");
          }
          return result_t<runtime::messaging::message_parts_t>::success (
            runtime::messaging::message_parts_t (
              std::move (completed.value ().parts)));
      };

    const auto source_spot = completion_source_spot;
    if (!source_spot) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found,
          "source Actor is not joined to a local Spot");
    }
    const auto transfer_id = spot_runtime.next_actor_transfer_id ();
    const auto completion_operation_id_low =
      _state->next_join_completion_operation.fetch_add (
        1, std::memory_order_relaxed);
    const auto completion_operation_id_high =
      static_cast<std::uint64_t> (
        std::hash<std::string>{} (_state->mesh_name))
      | 1ULL;
    auto fail_remote_join =
      [&] (const auto &failed, std::string message)
        -> result_t<actor_join_reply_t> {
          const auto failure = detail::propagate_failure<actor_join_reply_t> (
            failed, std::move (message));
          const auto delivered = deliver_completion (
            completion_operation_id_high,
            completion_operation_id_low,
            failure);
          if (!delivered)
              return detail::propagate_failure<actor_join_reply_t> (
                delivered,
                "remote Actor Join failure completion callback failed");
          return failure;
      };
    const auto source_actor = _node->resolve_actor (actor);
    if (!source_actor || source_actor->authority_owner_generation == 0) {
        const auto failure = result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found,
          "source Framework Actor authority is unavailable");
        return fail_remote_join (failure, "source Framework Actor authority is unavailable");
    }
    const auto actor_authority_owner_generation =
      source_actor->authority_owner_generation;
    const auto admission_request = spot_actor_admission_route_request_t{
      .transfer_id = transfer_id,
      .actor_node_rid = std::string (actor.node_rid ().value ()),
      .actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_generation = actor.object_generation (),
      .actor_authority_owner_generation =
        actor_authority_owner_generation,
      .completion_operation_id_high =
        completion_operation_id_high,
      .completion_operation_id_low =
        completion_operation_id_low,
      .source_spot_id = *source_spot,
      .target_spot_id = target_spot,
      .payload = request.to_bytes ()};
    auto admission_parts = request_route (
      admission_request, spot_actor_admission_route_request_t::packet_name);
    if (!admission_parts)
        return fail_remote_join (admission_parts, "remote Actor admission failed");
    auto admission = codec.decode_envelope_reply<spot_actor_admission_route_reply_t> (
      admission_parts.value (), *_serializers,
      "remote Actor admission reply is empty",
      "remote Actor admission reply decode failed", "ActorTransferAdmission");
    if (!admission)
        return fail_remote_join (admission, "remote Actor admission failed");
    if (!admission.value ().accepted) {
        const auto rejected =
          result_t<actor_join_reply_t>::success (
          actor_join_reply_t{
            1, actor, zlink::message_t::from (admission.value ().payload)});
        const auto delivered =
          deliver_completion (
            completion_operation_id_high,
            completion_operation_id_low,
            rejected);
        if (!delivered)
            return detail::propagate_failure<actor_join_reply_t> (
              delivered,
              "remote Actor Join rejected completion callback failed");
        return rejected;
    }

    auto prepared = spot_runtime.transfer_actor_out (actor, transfer_id);
    if (!prepared)
        return fail_remote_join (prepared, "Actor transfer-out failed");
    auto left = spot_runtime.leave_actor_for_remote_transfer (actor);
    if (!left) {
        spot_runtime.fail_remote_actor_transfer (actor, false);
        return fail_remote_join (left, "source Actor leave failed");
    }

    spot_runtime.emit_actor_transfer_marker (
      "commit_request", actor, transfer_id, target_spot, target_node);
    const auto prepare_request = spot_actor_commit_route_request_t{
      .transfer_id = transfer_id,
      .actor_node_rid = std::string (actor.node_rid ().value ()),
      .actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_generation = actor.object_generation (),
      .actor_authority_owner_generation =
        actor_authority_owner_generation,
      .completion_root_reference =
        admission.value ().completion_root_reference,
      .completion_root_checksum =
        admission.value ().completion_root_checksum,
      .target_spot_id = target_spot,
      .transfer_state = prepared.value ().state.to_bytes (),
      .core_transfer = true,
      .prepare = true};
    auto prepare_parts = request_route (
      prepare_request, spot_actor_commit_route_request_t::packet_name);
    if (!prepare_parts) {
        spot_runtime.fail_remote_actor_transfer (actor, true);
        return fail_remote_join (prepare_parts, "remote Actor prepare failed");
    }
    auto prepared_reply = codec.decode_envelope_reply<spot_actor_join_route_reply_t> (
      prepare_parts.value (), *_serializers,
      "remote Actor prepare reply is empty",
      "remote Actor prepare reply decode failed", "ActorTransferPrepare");
    if (!prepared_reply) {
        spot_runtime.fail_remote_actor_transfer (actor, true);
        return fail_remote_join (prepared_reply, "remote Actor prepare failed");
    }

    const auto native_actor = actor;
    std::uint64_t membership_epoch = 1;
    {
        std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
        const auto epoch = _state->spot_state->core_actor_membership_epochs.find (
          std::string (actor.actor_id ().value ()));
        if (epoch != _state->spot_state->core_actor_membership_epochs.end ())
            membership_epoch = epoch->second;
    }
    host::actor_transfer_prepare_t core_prepare{
      .role = host::actor_transfer_role_t::source,
      .transfer_id = transfer_id,
      .actor = native_actor,
      .source_spot_id = *source_spot,
      .target_spot_id = target_spot,
      .target_spot_generation = target_spot_generation,
      .target_node_rid =
        zlink::routing_id_t::from (std::string (target_node.value ())) };
    host::actor_transfer_token_t core_token;
    host::actor_transfer_prepare_result_t core_result{native_actor,
                                                      membership_epoch};
    const auto core_prepared =
      prepare_actor_transfer (core_prepare, timeout, core_token, core_result);
    if (!core_prepared) {
        spot_runtime.fail_remote_actor_transfer (actor, true);
        const auto failure = result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::internal_failure,
          "source Framework Actor relocation prepare failed");
        return fail_remote_join (failure, "source Framework Actor relocation prepare failed");
    }

    std::vector<spot_actor_handoff_packet_t> backlog;
    for (auto &packet : spot_runtime.take_actor_handoff_backlog (actor)) {
        backlog.push_back (spot_actor_handoff_packet_t{
          std::move (packet.packet_name), std::move (packet.payload),
          std::move (packet.content_type), std::move (packet.metadata),
          packet.is_request});
    }
    const auto finalize_request = spot_actor_commit_route_request_t{
      .transfer_id = transfer_id,
      .actor_node_rid = std::string (actor.node_rid ().value ()),
      .actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_generation = actor.object_generation (),
      .actor_authority_owner_generation =
        actor_authority_owner_generation,
      .completion_root_reference =
        admission.value ().completion_root_reference,
      .completion_root_checksum =
        admission.value ().completion_root_checksum,
      .target_spot_id = target_spot,
      .bound_session_node_rid =
        bound_session_node_rid ? bound_session_node_rid->to_string () : std::string{},
      .bound_session_rid =
        bound_session_rid ? bound_session_rid->to_string () : std::string{},
      .transfer_state = prepared.value ().state.to_bytes (),
      .handoff_backlog = std::move (backlog),
      .core_transfer = true,
      .core_transfer_id_high = 0,
      .core_transfer_id_low = 0,
      .core_membership_epoch = membership_epoch,
      .core_final_sequence = 0,
      .core_reserve_message_count = 0,
      .core_reserve_byte_count = 0,
      .finalize = true};
    auto finalize_parts = request_route (
      finalize_request, spot_actor_commit_route_request_t::packet_name);
    if (!finalize_parts) {
        spot_runtime.fail_remote_actor_transfer (actor, true);
        return fail_remote_join (finalize_parts, "remote Actor finalize failed");
    }
    auto finalized = codec.decode_envelope_reply<spot_actor_join_route_reply_t> (
      finalize_parts.value (), *_serializers,
      "remote Actor finalize reply is empty",
      "remote Actor finalize reply decode failed", "ActorTransferFinalize");
    if (!finalized) {
        spot_runtime.fail_remote_actor_transfer (actor, true);
        return fail_remote_join (finalized, "remote Actor finalize failed");
    }
    const auto next_membership_epoch = membership_epoch + 1;
    const auto core_committed = core_token.commit (next_membership_epoch);
    if (!core_committed) {
        spot_runtime.fail_remote_actor_transfer (actor, true);
        const auto failure = result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::internal_failure,
          "source Framework Actor relocation commit failed");
        return fail_remote_join (failure, "source Framework Actor relocation commit failed");
    }
    {
        std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
        _state->spot_state->core_actor_membership_epochs[std::string (actor.actor_id ().value ())]
          = next_membership_epoch;
    }
    const auto joined = actor_join_reply_from_spot_route (finalized.value ());
    spot_runtime.complete_remote_actor_transfer (
      actor, joined.actor,
      spot_route_t{target_node, target_spot, {}}, transfer_id);
    spot_runtime.emit_actor_transfer_marker (
      "commit_ack", actor, transfer_id, target_spot, target_node);
    const auto accepted =
      result_t<actor_join_reply_t>::success (
        actor_join_reply_t{
          joined.result_code, joined.actor,
          zlink::message_t::from (admission.value ().payload)});
    const auto delivered =
      deliver_completion (
        completion_operation_id_high,
        completion_operation_id_low,
        accepted);
    if (!delivered)
        return detail::propagate_failure<actor_join_reply_t> (
          delivered,
          "remote Actor Join accepted completion callback failed");
    spot_runtime.emit_actor_transfer_marker (
      "message_follow_registered", actor, transfer_id, target_spot, target_node);
    return accepted;
}

result_t<std::shared_ptr<deferred_barrier_t>>
mesh_node_runtime_t::reserve_application_actor_join_barrier (
  const actor_ref_t &actor)
{
    spot_node_runtime_t spot_runtime (_state->spot_state);
    if (!spot_runtime.actor_spot (actor)) {
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::not_found,
          "Deferred Actor join source runtime was not found");
    }
    return spot_runtime.reserve_actor_join_barrier (actor);
}

result_t<actor_join_reply_t> mesh_node_runtime_t::actor_join_reply_from_completion (
  const host::receive_record_t &record,
  const std::vector<zlink::message_t> &parts,
  const actor_ref_t &actor)
{
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
        return result_t<actor_join_reply_t>::success (
          actor_join_reply_t{1, actor, reply});
    }
    const auto &native = joined.current_actor;
    {
        std::lock_guard<std::recursive_mutex> lock (_state->spot_state->mutex);
        ++_state->spot_state
            ->core_actor_membership_epochs[std::string (actor.actor_id ().value ())];
    }
    return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{
        0,
        ::zlink::framework::detail::actor_ref_access_t::make (
          native.node_rid (), std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
                     std::string (native.actor_id ().value ()), native.object_generation ()),
        reply});
}

result_t<actor_join_reply_t> mesh_node_runtime_t::wait_for_join_completion (
  const host::operation_id_t &operation,
  const actor_ref_t &actor,
  std::chrono::milliseconds timeout)
{
    auto completed = wait_for_completion (operation, timeout);
    if (!completed) {
        return result_t<actor_join_reply_t>::failure (
          completed.error_kind (),
          completed.error () ? completed.error ()->what () : "Actor Spot join failed");
    }
    auto completion = std::move (completed.value ());
    return actor_join_reply_from_completion (
      completion.record, completion.parts, actor);
}

result_t<std::optional<zlink::message_t>>
mesh_node_runtime_t::relay_application_actor (
  const actor_ref_t &actor,
  const stream_header_t &header,
  const zlink::message_t &payload,
  std::chrono::milliseconds timeout)
{
    runtime::messaging::client_call_codec_t codec;
    const auto kind =
      header.kind () == stream_message_kind_t::send
        ? runtime::messaging::message_kind_t::command
        : runtime::messaging::message_kind_t::request;
    auto envelope =
      codec.create_envelope (kind, "actor", std::string (header.packet_name ()), timeout);
    envelope.metadata = header.metadata ().values ();
    if (const auto correlation = header.correlation_id ())
        envelope.correlation_id = std::string (*correlation);
    return relay_application_actor (actor, envelope, payload, timeout);
}

result_t<std::optional<zlink::message_t>>
mesh_node_runtime_t::relay_application_actor (
  const actor_ref_t &actor,
  const runtime::messaging::envelope_header_t &header,
  const zlink::message_t &payload,
  std::chrono::milliseconds timeout)
{
    return relay_application_actor (
      actor, header, payload, timeout,
      zlink::routing_id_t::from (std::uint32_t{0}),
      runtime::protocol::actor_route_fence_t{}, 0,
      runtime::protocol::wire_operation_id_t{}, 0);
}

result_t<std::optional<zlink::message_t>>
mesh_node_runtime_t::relay_application_actor (
  const actor_ref_t &actor,
  const runtime::messaging::envelope_header_t &header,
  const zlink::message_t &payload,
  std::chrono::milliseconds timeout,
  const zlink::routing_id_t &source_node,
  const runtime::protocol::actor_route_fence_t &stale_route,
  std::uint8_t incoming_hop_count,
  const runtime::protocol::wire_operation_id_t &original_operation,
  std::uint64_t original_reply_route_id)
{
    try {
        spot_node_runtime_t spot_runtime (_state->spot_state);
        if (spot_runtime.actor_message_follow_target (actor)) {
            const auto payload_bytes =
              message_follow_payload_bytes (header, payload);
            const auto follow_target =
              spot_runtime.try_acquire_actor_message_follow (
                actor, payload_bytes, incoming_hop_count);
            if (!follow_target) {
                return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::unavailable,
                  "Actor Message Follow bound was exceeded");
            }
            actor_message_follow_lease_t lease (
              spot_runtime, actor, payload_bytes);
            spot_runtime.emit_actor_transfer_marker (
              "message_follow_relay", actor, {},
              follow_target->route.spot_id,
              follow_target->route.node_rid);
            spot_inbound_message_t metadata;
            metadata.content_type = header.content_type;
            metadata.values = header.metadata;
            metadata.values["__zlink.messageFollowHopCount"] =
              std::to_string (static_cast<unsigned int> (
                incoming_hop_count + 1));
            if (header.kind
                == runtime::messaging::message_kind_t::command)
                metadata.values["__zlink.actorRelayKind"] = "send";
            runtime::messaging::client_call_codec_t codec;
            auto request_header = codec.create_envelope (
              runtime::messaging::message_kind_t::request, "spot",
              spot_actor_packet_route_request_t::packet_name,
              timeout);
            auto request = make_spot_actor_packet_route_request (
              follow_target->actor, follow_target->route.spot_id,
              header.message_name, payload, metadata);
            auto request_parts = codec.encode_envelope_parts (
              request_header, request, *_serializers);
            const auto target_node = zlink::routing_id_t::from (
              std::string (follow_target->route.node_rid.value ()));
            const auto target_generation =
              spot_runtime.resolve_spot_generation (
                target_node, follow_target->route.spot_id);
            if (!target_generation) {
                return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::not_found,
                  "Actor message follow target Spot generation is unavailable");
            }
            auto origin = get_or_create_spot (
              "__zlink-route-origin-" + routing_id ()->to_hex ());
            host::operation_id_t operation;
            const auto submitted = origin.request_to_spot (
              target_node, follow_target->route.spot_id,
              *target_generation, request_parts.items (), operation,
              zlink::send_flags_t::none, timeout);
            if (submitted != zlink::submit_result_t::ok) {
                return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::internal_failure,
                  "Actor message follow route request was not submitted");
            }
            auto completed = wait_for_completion (operation, timeout);
            if (!completed) {
                return detail::propagate_failure<
                  std::optional<zlink::message_t>> (
                    completed, "Actor message follow route request failed");
            }
            if (completed.value ().record.terminal_result
                != static_cast<int> (zlink::request_result_t::ok)) {
                return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::internal_failure,
                  "Actor message follow route request returned an error");
            }
            runtime::messaging::message_parts_t reply_parts (
              std::move (completed.value ().parts));
            auto decoded = codec.decode_envelope_reply<
              spot_actor_packet_route_reply_t> (
                reply_parts, *_serializers,
                "Actor message follow reply is empty",
                "Actor message follow reply decode failed",
                header.message_name);
            if (!decoded)
                return detail::propagate_failure<
                  std::optional<zlink::message_t>> (
                    decoded, "Actor message follow relay failed");
            std::optional<authority_snapshot_t> target_snapshot;
            if (_user_spot_store) {
                const auto authority = _user_spot_store
                  ->read_authority (authority_key_t{
                    "1:" + std::string (
                      follow_target->actor.actor_id ().value ())})
                  .result ();
                if (authority) {
                    if (const auto *snapshot =
                          std::get_if<authority_snapshot_t> (
                            &authority.value ()))
                        target_snapshot = *snapshot;
                }
            }
            const auto target_peer = _node->transport ().topology ().peer (
              target_node.to_bytes ());
            const auto local_descriptor =
              _node->transport ().topology ().local_descriptor ();
            const auto target_node_generation =
              target_node.to_bytes () == local_descriptor.node_routing_id
                ? local_descriptor.lifecycle_generation
                : target_peer
                    ? target_peer->descriptor.lifecycle_generation
                    : 0;
            if ((original_operation.high != 0
                    || original_operation.low != 0)
                && !source_node.to_bytes ().empty ()
                && stale_route.owner_lease_generation != 0
                && target_snapshot
                && target_snapshot->object_generation
                     == follow_target->actor.object_generation ()
                && target_snapshot->authority_owner_generation != 0
                && target_snapshot->owner.lease_generation > 0
                && target_node_generation != 0
                && spot_runtime.mark_actor_message_follow_notified (
                     actor, source_node)) {
                runtime::protocol::actor_route_fence_t target_route{
                  std::string (follow_target->actor.actor_id ().value ()),
                  follow_target->actor.object_generation (),
                  target_node.to_bytes (),
                  target_node_generation,
                  target_snapshot->authority_owner_generation,
                  static_cast<std::uint64_t> (
                    target_snapshot->owner.lease_generation)};
                (void) _node->send_message_follow (
                  source_node.to_bytes (),
                  runtime::protocol::message_follow_notice_t{
                    stale_route,
                    std::move (target_route),
                    static_cast<std::uint8_t> (incoming_hop_count + 1),
                    1,
                    static_cast<std::uint32_t> (
                      std::min<std::size_t> (
                        payload_bytes,
                        runtime::protocol::messageFollowBytes)),
                    original_operation,
                    original_reply_route_id});
            }
            return result_t<std::optional<zlink::message_t>>::success (
              decoded.value ().has_reply
                ? std::make_optional (
                    zlink::message_t::from (
                      decoded.value ().payload))
                : std::nullopt);
        }
        const auto &target_actor = actor;
        auto target_node_rid = zlink::routing_id_t::from (
          std::string (target_actor.node_rid ().value ()));
        std::uint64_t authority_owner_generation = 0;
        std::uint64_t owner_lease_generation = 0;
        if (_actor_route_resolver) {
            const auto resolved = _actor_route_resolver (target_actor);
            if (!resolved
                || resolved->object_generation != target_actor.object_generation ()
                || resolved->authority_owner_generation == 0
                || resolved->owner.lease_generation <= 0) {
                return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::not_found,
                  "Actor route fence is unavailable");
            }
            // The ActorRef may describe the source node from before a move.
            // The resolver owns the current placement, so route the message
            // to that node while retaining the requested object generation.
            target_node_rid = resolved->node_rid;
            authority_owner_generation =
              resolved->authority_owner_generation;
            owner_lease_generation = static_cast<std::uint64_t> (
              resolved->owner.lease_generation);
        } else if (_user_spot_store) {
            const auto authority = _user_spot_store
              ->read_authority (
                authority_key_t{
                  "1:" + std::string (target_actor.actor_id ().value ())})
              .result ();
            if (authority) {
                if (const auto *snapshot =
                      std::get_if<authority_snapshot_t> (
                        &authority.value ()))
                    authority_owner_generation =
                      snapshot->authority_owner_generation;
            }
            const auto local_descriptor =
              _node->transport ().topology ().local_descriptor ();
            if (!target_actor.node_rid ().empty ()
                && target_actor.node_rid ().value ()
                     == zlink::routing_id_t::from (
                          local_descriptor.node_routing_id).to_string ()
                && !spot_runtime.actor_route (target_actor)) {
                spot_runtime.emit_actor_transfer_marker (
                  "message_follow_expired", target_actor, {},
                  std::nullopt, std::nullopt);
                return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::unavailable,
                  "Actor Message Follow route has expired");
            }
        }
        const auto kind = header.kind;
        auto encoded =
          runtime::messaging::envelope_codec_t{}.encode_raw_body_parts (header, payload);
        const auto native_actor = host::mesh_node_t::remote_actor_ref (
          target_node_rid,
          std::string (target_actor.actor_id ().value ()),
          target_actor.object_generation ());
        if (kind == runtime::messaging::message_kind_t::command) {
            const auto submitted = send_to_actor (
              native_actor, encoded.items (), {},
              authority_owner_generation, owner_lease_generation);
            if (submitted != zlink::submit_result_t::ok) {
                return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::internal_failure,
                  "Actor relay send was not accepted");
            }
            return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
        }

        host::operation_id_t operation;
        const auto submitted =
          request_to_actor (
            native_actor, encoded.items (), operation, timeout, {},
            authority_owner_generation, owner_lease_generation);
        if (submitted != zlink::submit_result_t::ok) {
            return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::internal_failure,
              "Actor relay request was not accepted");
        }
        auto completed = wait_for_completion (operation, timeout);
        if (!completed) {
            return detail::propagate_failure<std::optional<zlink::message_t>> (
              completed, "Actor relay request completion failed");
        }
        runtime::messaging::message_parts_t reply (std::move (completed.value ().parts));
        auto reply_header = runtime::messaging::envelope_codec_t{}.decode_header (reply);
        if (!reply_header) {
            return result_t<std::optional<zlink::message_t>>::failure (
              reply_header.error_kind (),
              reply_header.error () ? reply_header.error ()->what ()
                                     : "Actor relay reply header decode failed");
        }
        if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
            const auto message =
              reply_header.value ().error_message.value_or ("Actor relay request failed");
            runtime::messaging::request_failure_mapper_t failure_mapper;
            const auto mapped = failure_mapper.error_header_exception (
              reply_header.value ().error_code.value_or ("request_failed"), message,
              "Actor relay request");
            return result_t<std::optional<zlink::message_t>>::failure (
              mapped.kind (), message);
        }
        auto body = runtime::messaging::envelope_codec_t{}.decode_body (reply);
        if (!body)
            return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
        return result_t<std::optional<zlink::message_t>>::success (
          std::make_optional (std::move (body.value ())));
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<std::optional<zlink::message_t>> (error);
    }
    catch (const std::exception &error) {
        return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
}

result_t<void> mesh_node_runtime_t::bind_application_actor_session (
  const actor_ref_t &actor,
  const node_rid_t &session_node,
  std::chrono::milliseconds timeout)
{
    if (!_serializers) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "MeshNode serializers are not configured");
    }
    try {
        auto actor_node = zlink::routing_id_t::from (
          std::string (actor.node_rid ().value ()));
        std::uint64_t authority_owner_generation = 0;
        std::uint64_t owner_lease_generation = 0;
        if (_actor_route_resolver) {
            const auto resolved = _actor_route_resolver (actor);
            if (!resolved
                || resolved->object_generation != actor.object_generation ()
                || resolved->authority_owner_generation == 0
                || resolved->owner.lease_generation <= 0) {
                return result_t<void>::failure (
                  framework_error_kind_t::not_found,
                  "Remote Actor session binding route is unavailable");
            }
            actor_node = resolved->node_rid;
            authority_owner_generation =
              resolved->authority_owner_generation;
            owner_lease_generation = static_cast<std::uint64_t> (
              resolved->owner.lease_generation);
        }
        runtime::messaging::client_call_codec_t codec;
        auto header = codec.create_envelope (
          runtime::messaging::message_kind_t::request, "actor",
          actor_bound_session_bind_route_request_t::packet_name, timeout);
        auto request = actor_bound_session_bind_route_request_t{
          .actor_node_rid = actor_node.to_string (),
          .actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
          .actor_id = std::string (actor.actor_id ().value ()),
          .actor_generation = actor.object_generation (),
          .session_node_rid = std::string (session_node.value ())};
        auto encoded =
          codec.encode_envelope_parts (header, request, *_serializers);
        const auto native_actor = host::mesh_node_t::remote_actor_ref (
          actor_node,
          std::string (actor.actor_id ().value ()), actor.object_generation ());
        if (!wait_for_peer_ready (actor_node, timeout)) {
            return result_t<void>::failure (
              framework_error_kind_t::unavailable,
              "Remote Actor session binding target RouteMesh peer is not ready");
        }
        host::operation_id_t operation;
        const auto submitted =
          request_to_actor (native_actor, encoded.items (), operation, timeout,
                            {}, authority_owner_generation,
                            owner_lease_generation);
        if (submitted != zlink::submit_result_t::ok) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Remote Actor session binding was not accepted");
        }
        auto completed = wait_for_completion (operation, timeout);
        if (!completed) {
            return detail::propagate_failure<void> (
              completed, "Remote Actor session binding did not complete");
        }
        if (completed.value ().record.terminal_result
            != static_cast<int> (zlink::request_result_t::ok)) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Remote Actor session binding completed with result "
                + std::to_string (
                  completed.value ().record.terminal_result)
                + " (errno "
                + std::to_string (
                  completed.value ().record.failure_errno)
                + ")");
        }
        runtime::messaging::message_parts_t reply (
          std::move (completed.value ().parts));
        auto decoded =
          codec.decode_envelope_reply<actor_bound_session_route_reply_t> (
            reply, *_serializers, "Remote Actor session binding reply is empty",
            "Remote Actor session binding reply decode failed",
            "BindActorSession");
        if (!decoded) {
            return detail::propagate_failure<void> (
              decoded, "Remote Actor session binding failed");
        }
        return decoded.value ().accepted
                 ? result_t<void>::success ()
                 : result_t<void>::failure (
                     framework_error_kind_t::not_configured,
                     "Remote Actor session binding was rejected");
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
}

result_t<void> mesh_node_runtime_t::notify_application_actor_disconnected (
  const actor_ref_t &actor,
  const node_rid_t &target_node,
  std::chrono::milliseconds timeout)
{
    if (!_serializers) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "MeshNode serializers are not configured");
    }
    try {
        runtime::messaging::client_call_codec_t codec;
        auto envelope = codec.create_envelope (
          runtime::messaging::message_kind_t::request, "spot",
          spot_actor_disconnect_route_request_t::packet_name, timeout);
        auto encoded = codec.encode_envelope_parts (
          envelope, make_spot_actor_disconnect_route_request (actor), *_serializers);
        host::operation_id_t operation;
        const auto submitted = request_to_node (
          zlink::routing_id_t::from (std::string (target_node.value ())),
          encoded.items (), operation, timeout);
        if (submitted != zlink::submit_result_t::ok) {
            return result_t<void>::failure (
              framework_error_kind_t::internal_failure,
              "Actor disconnect notification was not submitted");
        }
        auto completed = wait_for_completion (operation, timeout);
        if (!completed) {
            return detail::propagate_failure<void> (
              completed, "Actor disconnect notification failed");
        }
        if (completed.value ().record.terminal_result
            != static_cast<int> (zlink::request_result_t::ok)) {
            return result_t<void>::failure (
              framework_error_kind_t::internal_failure,
              "Actor disconnect notification returned an error");
        }
        runtime::messaging::message_parts_t reply (
          std::move (completed.value ().parts));
        auto decoded =
          codec.decode_envelope_reply<spot_actor_disconnect_route_reply_t> (
            reply, *_serializers, "Actor disconnect reply is empty",
            "Actor disconnect reply decode failed", "DisconnectActor");
        return decoded
                 ? result_t<void>::success ()
                 : detail::propagate_failure<void> (
                     decoded, "Actor disconnect notification failed");
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
}

std::optional<actor_ref_t>
mesh_node_runtime_t::follow_relocated_actor (const actor_ref_t &actor)
{
    spot_node_runtime_t runtime (_state->spot_state);
    const auto follow_target = runtime.actor_message_follow_target (actor);
    if (!follow_target) {
        runtime.emit_actor_transfer_marker (
          "message_follow_expired", actor, {}, std::nullopt, std::nullopt);
        return std::nullopt;
    }
    runtime.emit_actor_transfer_marker (
      "message_follow_relay", actor, {}, follow_target->route.spot_id,
      follow_target->route.node_rid);
    return follow_target->actor;
}

result_t<mesh_node_runtime_t::operation_completion_t>
mesh_node_runtime_t::wait_for_completion (
  const host::operation_id_t &operation,
  std::chrono::milliseconds timeout)
{
    std::unique_lock lock (_completion_mutex);
    if (!_completion_ready.wait_for (
          lock, timeout, [&] {
              return _completed_operations.contains (operation)
                     || _stopping.load (std::memory_order_acquire);
          })) {
        return detail::boundary_failure<operation_completion_t> (
          detail::boundary_error_t::timed_out,
          "MeshNode operation timed out");
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

std::optional<zlink::submit_result_t>
mesh_node_runtime_t::classify_node_direct_target (
  const zlink::routing_id_t &target) const
{
    if (!_user_spot_store)
        return std::nullopt;
    try {
        location_page_request_t page;
        for (;;) {
            const auto listed =
              _user_spot_store
                ->list_mesh_nodes (_state->mesh_name, page)
                .result ()
                .value ();
            const auto found = std::find_if (
              listed.items.begin (), listed.items.end (),
              [&target] (const mesh_node_descriptor_t &descriptor) {
                  return descriptor.rid == target;
              });
            if (found != listed.items.end ()) {
                return found->object_role == object_role_t::client
                  ? std::optional<zlink::submit_result_t> (
                      zlink::submit_result_t::not_found)
                  : std::nullopt;
            }
            if (!listed.continuation_token)
                return zlink::submit_result_t::not_found;
            page.continuation_token = listed.continuation_token;
        }
    }
    catch (...) {
        return std::nullopt;
    }
}

zlink::submit_result_t
mesh_node_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                   const std::vector<zlink::message_t> &parts,
                                   std::vector<std::uint8_t> metadata)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    runtime::messaging::note_submit_attempt (
      node_submit_target (target), this, one_way_send_timeout (*_state),
      _state->max_pending);
    if (!framework_owned_node_message (parts)) {
        if (const auto classified = classify_node_direct_target (target))
            return *classified;
    }
    (void) metadata;
    return _node->send_to_node (target, parts);
}

zlink::submit_result_t
mesh_node_runtime_t::send_to_node (const zlink::routing_id_t &target,
                                   const std::vector<zlink::message_t> &parts,
                                   const std::map<std::string, std::string> &metadata)
{
    const auto encoded = mesh_metadata_codec_t::encode (metadata);
    return send_to_node (target, parts, std::vector<std::uint8_t> (encoded));
}

zlink::submit_result_t mesh_node_runtime_t::request_to_node (
  const zlink::routing_id_t &target,
  const std::vector<zlink::message_t> &parts,
  host::operation_id_t &operation_id,
  std::chrono::milliseconds timeout,
  std::vector<std::uint8_t> metadata)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    if (!framework_owned_node_message (parts)) {
        if (const auto classified = classify_node_direct_target (target))
            return *classified;
    }
    (void) metadata;
    return _node->request_to_node (
      target, parts, operation_id, timeout);
}

zlink::submit_result_t mesh_node_runtime_t::request_to_node (
  const zlink::routing_id_t &target,
  const std::vector<zlink::message_t> &parts,
  host::operation_id_t &operation_id,
  std::chrono::milliseconds timeout,
  const std::map<std::string, std::string> &metadata)
{
    const auto encoded = mesh_metadata_codec_t::encode (metadata);
    return request_to_node (
      target, parts, operation_id, timeout, std::vector<std::uint8_t> (encoded));
}

zlink::submit_result_t
mesh_node_runtime_t::send_to_channel (const std::string &channel_name,
                                      const std::vector<zlink::message_t> &parts,
                                      std::vector<std::uint8_t> metadata)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    runtime::messaging::note_submit_attempt (
      channel_submit_target (channel_name), this, one_way_send_timeout (*_state),
      _state->max_pending);
    (void) metadata;
    return _node->send_to_channel (channel_name, parts);
}

zlink::submit_result_t
mesh_node_runtime_t::send_to_channel (const std::string &channel_name,
                                      const std::vector<zlink::message_t> &parts,
                                      const std::map<std::string, std::string> &metadata)
{
    const auto encoded = mesh_metadata_codec_t::encode (metadata);
    return send_to_channel (channel_name, parts, std::vector<std::uint8_t> (encoded));
}

zlink::submit_result_t mesh_node_runtime_t::request_to_channel (
  const std::string &channel_name,
  const std::vector<zlink::message_t> &parts,
  host::operation_id_t &operation_id,
  std::chrono::milliseconds timeout,
  std::vector<std::uint8_t> metadata)
{
    if (!_node) {
        throw configuration_error ("MeshNode has not started");
    }
    (void) metadata;
    return _node->request_to_channel (
      channel_name, parts, operation_id, timeout);
}

zlink::submit_result_t mesh_node_runtime_t::request_to_channel (
  const std::string &channel_name,
  const std::vector<zlink::message_t> &parts,
  host::operation_id_t &operation_id,
  std::chrono::milliseconds timeout,
  const std::map<std::string, std::string> &metadata)
{
    const auto encoded = mesh_metadata_codec_t::encode (metadata);
    return request_to_channel (
      channel_name, parts, operation_id, timeout, std::vector<std::uint8_t> (encoded));
}

std::size_t mesh_node_runtime_t::dispatch_ready (
  const std::function<void (const host::ready_record_t &,
                            const host::receive_record_t &,
                            std::vector<zlink::message_t>)> &dispatch,
  bool accept_application_receive)
{
    if (!dispatch)
        throw configuration_error ("MeshNode dispatch callback is required");

    return _node->dispatch_ready (
      [&] (const host::ready_record_t &ready_record,
           const host::receive_record_t &record,
           std::vector<zlink::message_t> parts) {
            if (record.kind == host::record_kind_t::completion) {
                {
                    std::lock_guard lock (_completion_mutex);
                    (void) _completed_operations.complete (
                      record.operation_id, operation_completion_t{record, parts});
                }
                _completion_ready.notify_all ();
            }
            if (record.kind == host::record_kind_t::send_ready
                && record.send_ready) {
                const auto target = send_ready_target (*record.send_ready);
                if (!target.empty ()) {
                    runtime::messaging::notify_submit_ready (target, this);
                }
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

std::int32_t
mesh_node_runtime_t::activation_concurrency_limit () const
{
    std::lock_guard lock (_state->mutex);
    return _state->activation_concurrency_limit;
}

void mesh_node_runtime_t::set_placement_weight (int weight)
{
    if (!_node)
        throw configuration_error ("MeshNode has not started");
    if (weight < 0 || weight > 10000)
        throw configuration_error (
          "placement weight must be in range 0..10000");
    auto descriptor =
      native_node ().transport ().topology ()
        .local_descriptor ();
    if (descriptor.descriptor_revision
          == std::numeric_limits<std::uint64_t>::max ())
        throw configuration_error (
          "MeshNode descriptor revision is exhausted");
    descriptor.placement_weight = weight;
    ++descriptor.descriptor_revision;
    std::function<void (const std::map<std::string, int> &,
                        int,
                        std::uint64_t)> publisher;
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
    native_node ().transport ().topology ().publish_local (
      std::move (descriptor));
    std::lock_guard lock (_state->mutex);
    _state->placement_weight = weight;
}

std::size_t mesh_node_runtime_t::max_pending () const noexcept
{
    return _state->max_pending;
}

void mesh_node_runtime_t::set_channel_weight (const std::string &channel_name,
                                              int weight)
{
    if (!_node)
        throw configuration_error ("MeshNode has not started");
    if (weight < 0 || weight > 10000)
        throw configuration_error (
          "channel weight must be in range 0..10000");
    auto descriptor =
      native_node ().transport ().topology ().local_descriptor ();
    const auto descriptor_channel = std::find_if (
      descriptor.channels.begin (), descriptor.channels.end (),
      [&] (const auto &candidate) { return candidate.name == channel_name; });
    if (descriptor_channel == descriptor.channels.end ())
        throw configuration_error ("RouteMesh channel is not configured: "
                                   + mesh_name () + "/" + channel_name);
    descriptor_channel->weight = weight;
    if (descriptor.descriptor_revision
          == std::numeric_limits<std::uint64_t>::max ())
        throw configuration_error (
          "MeshNode descriptor revision is exhausted");
    ++descriptor.descriptor_revision;
    std::function<void (const std::map<std::string, int> &,
                        int,
                        std::uint64_t)> publisher;
    std::map<std::string, int> channel_weights;
    int placement_weight = 100;
    {
        std::lock_guard lock (_state->mutex);
        const auto found = _state->channels.find (channel_name);
        if (found == _state->channels.end () || !found->second.server)
            throw configuration_error ("RouteMesh channel is not configured: "
                                       + _state->mesh_name + "/" + channel_name);
        for (const auto &[name, registration] : _state->channels)
            if (registration.server)
                channel_weights.emplace (
                  name, name == channel_name ? weight : registration.weight);
        placement_weight = _state->placement_weight;
        publisher = _descriptor_publisher;
    }
    if (publisher)
        publisher (
          channel_weights, placement_weight, descriptor.descriptor_revision);
    native_node ().transport ().topology ().publish_local (
      std::move (descriptor));
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
    const auto rid = routing_id ();
    if (!rid)
        return;
    runtime::messaging::note_submit_attempt (
      node_submit_target (*rid), this, one_way_send_timeout (*_state),
      _state->max_pending);
}

void mesh_node_runtime_t::local_application_work_finished () noexcept
{
    application_work_finished ();
    if (const auto rid = routing_id ()) {
        runtime::messaging::notify_submit_ready (node_submit_target (*rid), this);
    }
}

std::uint64_t mesh_node_runtime_t::pending_application_callbacks () const noexcept
{
    return _pending_application_callbacks.load (std::memory_order_relaxed);
}

std::uint64_t mesh_node_runtime_t::active_application_callbacks () const noexcept
{
    return _active_application_callbacks.load (std::memory_order_relaxed);
}

std::shared_ptr<mesh_node_runtime_t>
mesh_node_runtime_t::from (zlink_builder_t &builder, const std::string &mesh_name)
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
    std::lock_guard lock (_state->mutex);
    _state->peer_connections.push_back (
      mesh_peer_connection_t{detail::next_connection_intent_id (), {}, std::move (endpoint)});
}

void mesh_peer_connections_t::connect (zlink::routing_id_t expected_routing_id,
                                       std::string endpoint)
{
    if (endpoint.empty ()) {
        throw detail::configuration_error ("peer endpoint is required");
    }
    std::lock_guard lock (_state->mutex);
    _state->peer_connections.push_back (mesh_peer_connection_t{
      detail::next_connection_intent_id (), std::move (expected_routing_id), std::move (endpoint)});
}

void mesh_peer_connections_t::disconnect (std::string endpoint)
{
    std::lock_guard lock (_state->mutex);
    std::erase_if (_state->peer_connections,
                   [&endpoint] (const mesh_peer_connection_t &connection) {
                       return connection.endpoint == endpoint;
                   });
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
        throw detail::configuration_error (
          "RouteMesh channel role is already selected: " + _channel_name);
    channel.role_selected = true;
    channel.server = false;
    return {};
}

mesh_channel_server_builder_t mesh_channel_builder_t::server ()
{
    std::lock_guard lock (_state->mutex);
    auto &channel = _state->channels[_channel_name];
    if (channel.role_selected)
        throw detail::configuration_error (
          "RouteMesh channel role is already selected: " + _channel_name);
    channel.role_selected = true;
    channel.server = true;
    return mesh_channel_server_builder_t (_state, _channel_name);
}

mesh_channel_server_builder_t::mesh_channel_server_builder_t (
  std::shared_ptr<detail::mesh_node_builder_state_t> state,
  std::string channel_name) :
    _state (std::move (state)), _channel_name (std::move (channel_name))
{
}

mesh_channel_server_builder_t &
mesh_channel_server_builder_t::set_weight (int weight)
{
    if (weight < 0 || weight > 10000) {
        throw detail::configuration_error (
          "ChannelName weight must be in range 0..10000");
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
    std::lock_guard lock (_state->mutex);
    _state->channels[_channel_name].handler_group = std::move (group_name);
    return *this;
}

mesh_channel_server_builder_t &
mesh_channel_server_builder_t::add_handler_registration (
  detail::mesh_handler_registration_t registration)
{
    detail::route_handler_descriptor_t descriptor{
      registration.request ? runtime::messaging::message_kind_t::request
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

mesh_node_builder_t &mesh_node_builder_t::listen (std::string endpoint)
{
    if (endpoint.empty ()) {
        throw detail::configuration_error ("MeshNode listen endpoint is required");
    }
    std::lock_guard lock (_state->mutex);
    _state->listen_endpoint = std::move (endpoint);
    return *this;
}

mesh_node_builder_t &
mesh_node_builder_t::set_advertise_host (std::string host)
{
    if (host.empty ())
        throw detail::configuration_error (
          "MeshNode advertise host is required");
    std::lock_guard lock (_state->mutex);
    _state->advertise_host = std::move (host);
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_routing_id (zlink::routing_id_t routing_id)
{
    std::lock_guard lock (_state->mutex);
    _state->spot_state->snapshot.routing_id = routing_id;
    _state->routing_id = std::move (routing_id);
    return *this;
}

mesh_node_builder_t &mesh_node_builder_t::set_object_role (object_role_t role)
{
    std::lock_guard lock (_state->mutex);
    _state->object_role = role;
    return *this;
}

mesh_node_builder_t &
mesh_node_builder_t::set_placement_weight (int weight)
{
    if (weight < 0 || weight > 10000)
        throw detail::configuration_error (
          "placement weight must be in range 0..10000");
    std::lock_guard lock (_state->mutex);
    _state->placement_weight = weight;
    return *this;
}

mesh_node_builder_t &
mesh_node_builder_t::set_actor_limit (std::int32_t limit)
{
    if (limit < 0)
        throw detail::configuration_error (
          "Actor capacity limit must be non-negative");
    std::lock_guard lock (_state->mutex);
    _state->actor_limit = limit;
    return *this;
}

mesh_node_builder_t &
mesh_node_builder_t::set_spot_limit (std::int32_t limit)
{
    if (limit < 0)
        throw detail::configuration_error (
          "Spot capacity limit must be non-negative");
    std::lock_guard lock (_state->mutex);
    _state->spot_limit = limit;
    return *this;
}

mesh_node_builder_t &
mesh_node_builder_t::set_instance_spot_idle_timeout (
  std::chrono::milliseconds timeout)
{
    if (timeout < std::chrono::milliseconds::zero ())
        throw detail::configuration_error (
          "Instance Spot idle timeout must not be negative");
    std::lock_guard lock (_state->mutex);
    _state->instance_spot_idle_timeout = timeout;
    _state->spot_state->instance_spot_idle_timeout = timeout;
    return *this;
}

mesh_node_builder_t &
mesh_node_builder_t::set_activation_concurrency (
  std::int32_t limit)
{
    if (limit <= 0)
        throw detail::configuration_error (
          "Activation concurrency limit must be positive");
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
