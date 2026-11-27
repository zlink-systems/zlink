/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"

#include "runtime/actors/actor_manager_access.hpp"
#include "runtime/execution/task_result.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/store_location_resolvers.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace zlink::framework::runtime
{

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
    if (!projection) {
        co_return result_t<std::optional<actor_ref_t>>::success (std::nullopt);
    }
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
        if (projection && projection->actor.actor_id ().value () == actor_id.value ())
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

task_t<result_t<void>> mesh_node_host_service_t::finalize_local_actor_destroy (actor_ref_t actor)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        co_return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                           "Actor destroy requires an exact ActorRef");
    }

    const auto node = co_await find_local_actor_node_async (actor.node_rid ());
    const auto cleaned = co_await detail::cleanup_local_actor_resources (
      {}, _services ? std::optional<service_provider_t>{*_services} : std::nullopt, actor);
    if (!cleaned)
        co_return cleaned;
    if (node) {
        const auto native_cleaned = co_await node->cleanup_application_actor_stateful (actor);
        if (!native_cleaned)
            co_return native_cleaned;
    }

    std::optional<authority_snapshot_t> removed_snapshot;
    if (_location_store) {
        const auto key = actor_authority_key (actor.actor_id ().value ());
        const auto current = co_await runtime::await_result (_location_store->read_authority (key));
        if (!current) {
            co_return detail::propagate_failure<void> (current,
                                                       "Actor destroy authority read failed");
        }
        if (const auto *snapshot = std::get_if<authority_snapshot_t> (&current.value ())) {
            const bool exact =
              snapshot->allocation.object_kind == placement_object_kind_t::actor
              && snapshot->allocation.stable_type
                   == ::zlink::framework::detail::actor_ref_access_t::actor_type (actor)
              && snapshot->object_generation == actor.object_generation ()
              && snapshot->allocation.target.node_rid.value () == actor.node_rid ().value ();
            if (exact && snapshot->allocation.state == placement_allocation_state_t::active) {
                const auto removed =
                  co_await runtime::await_result (_location_store->compare_exchange_authority (
                    key, snapshot->store_version, authority_delete_t{}));
                if (!removed) {
                    co_return detail::propagate_failure<void> (
                      removed, "Actor destroy authority delete failed");
                }
                if (std::holds_alternative<authority_deleted_t> (removed.value ())) {
                    removed_snapshot = *snapshot;
                } else {
                    co_return result_t<void>::failure (
                      framework_error_kind_t::unavailable,
                      "Actor destroy authority changed before deletion");
                }
            }
        }
    }

    if (removed_snapshot && _services) {
        if (auto actor_resolver = _services->get<actor_address_resolver_t> ()) {
            const auto &snapshot = *removed_snapshot;
            const auto expected = spot_address_t{
              {},
              zlink::routing_id_t::from (
                std::string (snapshot.allocation.target.node_rid.value ())),
              {},
              0,
              {},
              snapshot.object_generation,
              snapshot.authority_owner_generation,
              location_owner_token_t{snapshot.allocation.target.owner.owner_id,
                                     snapshot.allocation.target.owner.lease_generation},
              snapshot.allocation.target.node_lifecycle_generation};
            (void) co_await actor_resolver->get ().invalidate_actor_address_if_matches_async (
              std::string (actor.actor_id ().value ()), expected);
        }
    }

    co_return result_t<void>::success ();
}

task_t<bool> mesh_node_host_service_t::destroy_actor (actor_ref_t actor)
{
    try {
        if (!_location_store || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
            co_return result_t<bool>::failure (
              framework_error_kind_t::invalid_operation,
              "Actor destroy requires a Location Store and an exact ActorRef");
        }

        const auto key = actor_authority_key (actor.actor_id ().value ());
        const auto read = co_await _location_store->read_authority (key);
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        if (!snapshot) {
            co_return result_t<bool>::success (false);
        }
        if (snapshot->allocation.object_kind != placement_object_kind_t::actor
            || snapshot->allocation.stable_type
                 != ::zlink::framework::detail::actor_ref_access_t::actor_type (actor)
            || snapshot->object_generation != actor.object_generation ()
            || snapshot->allocation.target.node_rid.value () != actor.node_rid ().value ()) {
            co_return result_t<bool>::failure (
              framework_error_kind_t::invalid_operation,
              "ActorRef does not match the current actor incarnation");
        }
        if (snapshot->allocation.state != placement_allocation_state_t::active) {
            co_return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                               "Actor creation or transfer is in progress");
        }

        const auto node = co_await find_local_actor_node_async (actor.node_rid ());
        if (node) {
            if (co_await node->application_actor_transfer_in_progress (actor)) {
                co_return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                                   "Actor transfer is in progress");
            }

            /* The local Spot owns the first destructive step. Its cleanup
             * callback runs after local state is detached and only then
             * removes Location authority. This keeps a failed local cleanup
             * from leaving a deleted authority with retained capacity. */
            const auto local_destroyed = co_await node->destroy_application_actor (actor);
            if (!local_destroyed) {
                co_return result_t<bool>::failure (local_destroyed.error_kind (),
                                                   local_destroyed.error ()
                                                     ? local_destroyed.error ()->what ()
                                                     : "Actor runtime cleanup failed");
            }
            /* The Spot destroy callback owns the authority and gateway
             * finalization for a local Actor. Keep this outer operation as
             * the result boundary so the finalizer runs exactly once. */
            co_return result_t<bool>::success (true);
        }

        const auto removed = co_await _location_store->compare_exchange_authority (
          key, snapshot->store_version, authority_delete_t{});
        if (!std::holds_alternative<authority_deleted_t> (removed)) {
            if (const auto *conflict = std::get_if<authority_conflict_t> (&removed)) {
                if (std::holds_alternative<authority_missing_t> (conflict->current)) {
                    co_return result_t<bool>::success (false);
                }
                const auto &current = std::get<authority_snapshot_t> (conflict->current);
                if (current.object_generation != actor.object_generation ()) {
                    co_return result_t<bool>::failure (
                      framework_error_kind_t::invalid_operation,
                      "Actor generation changed while destroy was pending");
                }
            }
            co_return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                               "Actor authority changed while destroy was pending");
        }
        /* A destroy removes the current authority before the next same-Id
         * incarnation can be created. Invalidate the resolver entry that was
         * derived from that authority so a new incarnation cannot reuse the
         * previous node route when its generation is reused. */
        if (_services) {
            if (auto actor_resolver = _services->get<actor_address_resolver_t> ()) {
                const auto expected = spot_address_t{
                  {},
                  zlink::routing_id_t::from (
                    std::string (snapshot->allocation.target.node_rid.value ())),
                  {},
                  0,
                  {},
                  snapshot->object_generation,
                  snapshot->authority_owner_generation,
                  location_owner_token_t{snapshot->allocation.target.owner.owner_id,
                                         snapshot->allocation.target.owner.lease_generation},
                  snapshot->allocation.target.node_lifecycle_generation};
                (void) co_await actor_resolver->get ().invalidate_actor_address_if_matches_async (
                  std::string (actor.actor_id ().value ()), expected);
            }
        }

        const auto finalized = co_await finalize_local_actor_destroy (actor);
        if (!finalized) {
            co_return detail::result_access_t::failure<bool> (framework_exception_t (
              finalized.error_kind (),
              finalized.error () ? finalized.error ()->what () : "Actor destroy cleanup failed"));
        }
        co_return result_t<bool>::success (true);
    }
    catch (const framework_exception_t &error) {
        co_return detail::result_access_t::failure<bool> (error);
    }
    catch (const std::exception &error) {
        co_return result_t<bool>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
}

} // namespace zlink::framework::runtime
