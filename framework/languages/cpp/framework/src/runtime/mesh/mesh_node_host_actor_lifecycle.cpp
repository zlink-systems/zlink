/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"

#include "runtime/actors/actor_manager_access.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/mesh_node_host_service_internal.hpp"
#include "runtime/timers/async_delay.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace zlink::framework::runtime
{
using mesh_node_host_service_detail::next_mesh_operation;

actor_manager_t mesh_node_host_service_t::actor_manager ()
{
    if (_nodes.empty ())
        return actor_manager_t{};
    return detail::actor_manager_access_t::create (
      [this] (bool exclusive, actor_id_t actor_id, std::string stable_type,
              std::optional<std::string> mesh_name, std::optional<message_t> request,
              std::chrono::milliseconds timeout, creation_operation_id_t operation_id) {
          const auto source_node =
            mesh_name ? std::find_if (_nodes.begin (), _nodes.end (),
                                      [&] (const auto &node) {
                                          return node && node->mesh_name () == *mesh_name;
                                      })
                      : _nodes.begin ();
          if (source_node == _nodes.end ())
              return task_t<actor_create_result_t> (result_t<actor_create_result_t>::failure (
                framework_error_kind_t::not_found, "The selected Actor Mesh is not registered"));
          const auto source = (*source_node)->status ();
          return create_actor (exclusive, std::move (actor_id), std::move (stable_type),
                               std::move (mesh_name), std::move (request), timeout,
                               creation_operation_identity_t{
                                 node_rid_t::from_string (source.routing_id ().to_string ()),
                                 source.lifecycle_generation (), operation_id});
      },
      [this] (actor_id_t actor_id) { return find_actor (std::move (actor_id)); },
      [this] (actor_id_t actor_id) { return find_actor_spot (std::move (actor_id)); },
      [this] (actor_ref_t actor) { return destroy_actor (std::move (actor)); });
}

task_t<std::optional<actor_ref_t>> mesh_node_host_service_t::find_actor (actor_id_t actor_id)
{
    if (!_location_store)
        co_return result_t<std::optional<actor_ref_t>>::success (std::nullopt);
    const auto current =
      co_await _location_store->read_authority (actor_authority_key (actor_id.value ()));
    const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
    if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active)
        co_return result_t<std::optional<actor_ref_t>>::success (std::nullopt);
    const auto projection =
      decode_actor_authority_payload (snapshot->payload, snapshot->object_generation);
    if (!projection || projection->state != actor_authority_state_t::ready)
        co_return result_t<std::optional<actor_ref_t>>::success (std::nullopt);
    co_return result_t<std::optional<actor_ref_t>>::success (
      ::zlink::framework::detail::actor_ref_access_t::make (
        snapshot->allocation.target.node_rid, snapshot->allocation.stable_type,
        std::string (actor_id.value ()), snapshot->object_generation));
}

task_t<std::optional<spot_ref_t>> mesh_node_host_service_t::find_actor_spot (actor_id_t actor_id)
{
    if (!_location_store)
        co_return result_t<std::optional<spot_ref_t>>::success (std::nullopt);
    const auto read =
      co_await _location_store->read_authority (actor_authority_key (actor_id.value ()));
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
    if (snapshot && snapshot->allocation.object_kind == placement_object_kind_t::actor
        && snapshot->allocation.state == placement_allocation_state_t::active) {
        const auto projection =
          decode_actor_authority_payload (snapshot->payload, snapshot->object_generation);
        if (projection && projection->state == actor_authority_state_t::ready
            && projection->actor.actor_id ().value () == actor_id.value ())
            co_return result_t<std::optional<spot_ref_t>>::success (spot_ref_t{
              projection->spot_id, projection->spot_generation,
              snapshot->allocation.target.mesh_name, snapshot->allocation.target.node_rid});
    }
    co_return result_t<std::optional<spot_ref_t>>::success (std::nullopt);
}

task_t<std::shared_ptr<detail::mesh_node_runtime_t>>
mesh_node_host_service_t::find_local_actor_node_async (node_rid_t actor_node)
{
    for (const auto &node : _nodes) {
        if (!node)
            continue;
        const auto rid = co_await node->routing_id_async ();
        if (rid && rid->to_string () == actor_node.value ())
            co_return node;
    }
    co_return nullptr;
}

task_t<void> mesh_node_host_service_t::update_actor_destroy_authority (
  actor_ref_t actor, bool closing, std::string expected_store_version)
{
    if (!_location_store)
        co_return;
    const auto key = actor_authority_key (actor.actor_id ().value ());
    const auto current = co_await _location_store->read_authority (key);
    const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
    if (!snapshot || snapshot->object_generation != actor.object_generation ()
        || snapshot->allocation.target.node_rid.value () != actor.node_rid ().value ()
        || snapshot->allocation.stable_type != detail::actor_ref_access_t::actor_type (actor))
        throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                     "Actor authority no longer names the destroy incarnation");
    if (!expected_store_version.empty () && snapshot->store_version != expected_store_version)
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "Actor owner changed before destroy admission");
    const auto owner = current_location_owner ();
    if (!owner || owner->owner_id != snapshot->owner.owner_id
        || owner->lease_generation != snapshot->owner.lease_generation)
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "Actor destroy authority belongs to another owner");
    auto payload = decode_direct_actor_authority_payload (snapshot->payload);
    const auto expected =
      closing ? actor_authority_state_t::ready : actor_authority_state_t::closing;
    if (!payload || payload->state != expected || payload->has_relocation_state
        || snapshot->aggregate
        || snapshot->allocation.state != placement_allocation_state_t::active)
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "Actor authority cannot advance destroy");

    if (closing) {
        payload->state = actor_authority_state_t::closing;
        const auto changed = co_await _location_store->compare_exchange_authority (
          key, snapshot->store_version, authority_put_t{encode_actor_authority_payload (*payload)});
        if (!std::holds_alternative<authority_stored_t> (changed))
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "Actor authority changed before Closing");
        co_return;
    }
    if (_services) {
        if (auto resolver = _services->get<actor_address_resolver_t> ()) {
            const auto expected_address =
              spot_address_t{{},
                             zlink::routing_id_t::from (
                               std::string (snapshot->allocation.target.node_rid.value ())),
                             {},
                             0,
                             {},
                             snapshot->object_generation,
                             snapshot->authority_owner_generation,
                             snapshot->allocation.target.owner,
                             snapshot->allocation.target.node_lifecycle_generation};
            (void) co_await resolver->get ().invalidate_actor_address_if_matches_async (
              std::string (actor.actor_id ().value ()), expected_address);
        }
    }
    const auto removed = co_await _location_store->compare_exchange_authority (
      key, snapshot->store_version, authority_delete_t{});
    if (!std::holds_alternative<authority_deleted_t> (removed))
        throw framework_exception_t (framework_error_kind_t::unavailable,
                                     "Closing Actor authority changed before deletion");
}

task_t<bool> mesh_node_host_service_t::destroy_actor (actor_ref_t actor)
{
    const auto started = std::chrono::steady_clock::now ();
    const auto cancellation = _descriptor_stop.get_token ();
    try {
        if (!_location_store || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
            co_return result_t<bool>::failure (
              framework_error_kind_t::invalid_operation,
              "Actor destroy requires a Location Store and an exact ActorRef");
        }

        const auto key = actor_authority_key (actor.actor_id ().value ());
        const auto read = co_await _location_store->read_authority (key);
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        if (!snapshot)
            co_return false;
        if (snapshot->allocation.object_kind != placement_object_kind_t::actor
            || snapshot->allocation.stable_type
                 != ::zlink::framework::detail::actor_ref_access_t::actor_type (actor)
            || snapshot->object_generation != actor.object_generation ()) {
            co_return result_t<bool>::failure (
              framework_error_kind_t::invalid_operation,
              "ActorRef does not match the current actor incarnation");
        }
        const auto payload = decode_direct_actor_authority_payload (snapshot->payload);
        if (snapshot->allocation.state != placement_allocation_state_t::active || !payload
            || payload->has_relocation_state || snapshot->aggregate) {
            co_return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                               "Actor creation or transfer is in progress");
        }
        if (payload->state == actor_authority_state_t::closing) {
            const auto released =
              co_await _location_store->release_ended_reservation (key, snapshot->store_version);
            if (released)
                co_return true;
        }
        const auto &target = snapshot->allocation.target;
        const auto node = co_await find_local_actor_node_async (target.node_rid);
        if (node)
            co_return co_await node->destroy_application_actor (
              detail::actor_ref_access_t::make (target.node_rid, snapshot->allocation.stable_type,
                                                std::string (actor.actor_id ().value ()),
                                                actor.object_generation (), target.mesh_name),
              snapshot->store_version);

        const auto source = std::find_if (_nodes.begin (), _nodes.end (), [&] (const auto &entry) {
            return entry && entry->mesh_name () == target.mesh_name;
        });
        if (source == _nodes.end ())
            co_return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                               "Actor owner Mesh is unavailable");
        const auto deadline = started + (*source)->default_request_timeout ();
        if (std::chrono::steady_clock::now () >= deadline)
            co_return result_t<bool>::failure (framework_error_kind_t::deadline_exceeded,
                                               "Actor destroy deadline elapsed");
        const auto target_rid = zlink::routing_id_t::from (std::string (target.node_rid.value ()));
        if (target.owner.lease_generation <= 0
            || !(*source)->has_admitted_peer (target_rid, target.node_lifecycle_generation))
            co_return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                               "Actor owner is not RouteMesh-admitted");
        protocol::actor_destroy_command_t command{
          next_mesh_operation.fetch_add (1, std::memory_order_relaxed),
          {std::string (actor.actor_id ().value ()), actor.object_generation (),
           target_rid.to_bytes (), target.node_lifecycle_generation,
           snapshot->authority_owner_generation,
           static_cast<std::uint64_t> (target.owner.lease_generation)}};
        if (!co_await (*source)->native_node ().transport ().send_actor_destroy (
              std::move (command)))
            co_return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                               "Actor destroy command was not admitted");

        while (std::chrono::steady_clock::now () < deadline && !cancellation.stop_requested ()) {
            const auto observed = co_await _location_store->read_authority (key, cancellation);
            if (std::chrono::steady_clock::now () >= deadline || cancellation.stop_requested ())
                break;
            const auto *current = std::get_if<authority_snapshot_t> (&observed);
            if (!current || current->object_generation > actor.object_generation ())
                co_return true;
            // Location runtime 6.1: an observer reclaims a Closing record whose
            // owner lease ended, with the same CAS the owner-side reclaim uses.
            if (current->object_generation == actor.object_generation ()) {
                const auto observed_payload =
                  decode_direct_actor_authority_payload (current->payload);
                if (observed_payload
                    && observed_payload->state == actor_authority_state_t::closing) {
                    const auto released = co_await _location_store->release_ended_reservation (
                      key, current->store_version, cancellation);
                    if (released)
                        co_return true;
                }
            }
            co_await detail::delay (_services->get_required<detail::delay_owner_t> (),
                                    std::chrono::milliseconds (1));
        }
        if (cancellation.stop_requested ())
            co_return result_t<bool>::failure (framework_error_kind_t::shutting_down,
                                               "Actor destroy interrupted by Mesh shutdown");
        co_return result_t<bool>::failure (framework_error_kind_t::deadline_exceeded,
                                           "Actor destroy deadline elapsed");
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<bool> (error);
    }
    catch (const std::exception &error) {
        co_return result_t<bool>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
}

} // namespace zlink::framework::runtime
