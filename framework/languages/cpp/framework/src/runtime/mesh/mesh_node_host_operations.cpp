/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service_internal.hpp"
#include "runtime/mesh/mesh_node_host_actor_creation_internal.hpp"

#include "runtime/execution/task_result.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/timers/async_delay.hpp"


#include <algorithm>
#include <chrono>
#include <functional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace zlink::framework::runtime
{

using mesh_node_host_service_detail::actor_result_from_terminal;
using mesh_node_host_service_detail::actor_terminal_envelope;
using mesh_node_host_service_detail::choose_placement_target;
using mesh_node_host_service_detail::encode_actor_creation_intent;
using mesh_node_host_service_detail::placement_candidates;
using mesh_node_host_service_detail::target_actor_authority_payload;

namespace
{
namespace terminal_codec = zlink::framework::runtime::protocol::generated::detail;
} // namespace

task_t<actor_create_result_t>
mesh_node_host_service_t::create_actor (bool exclusive,
                                        actor_id_t actor_id,
                                        std::string stable_type,
                                        std::optional<std::string> mesh_name,
                                        std::optional<message_t> request,
                                        std::chrono::milliseconds timeout,
                                        creation_operation_identity_t operation)
{
    if (!_location_store || actor_id.value ().empty () || stable_type.empty ())
        co_return result_t<actor_create_result_t>::failure (
          framework_error_kind_t::not_configured,
          "Actor creation requires Location Store, ActorId and stable type");
    if (_nodes.empty ())
        co_return result_t<actor_create_result_t>::failure (
          framework_error_kind_t::not_configured, "No object Client or Server Mesh is registered");
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    const auto operation_deadline = std::chrono::system_clock::now () + timeout;
    std::string selected_mesh;
    if (mesh_name)
        selected_mesh = *mesh_name;
    else {
        const auto identity = co_await _nodes.front ()->identity_async ();
        selected_mesh = identity.first;
    }
    const auto source_node = co_await find_local_actor_node_async (operation.source_node_rid);
    if (!source_node)
        co_return result_t<actor_create_result_t>::failure (
          framework_error_kind_t::unavailable, "Actor creation source MeshNode is not available");
    const auto source_runtime = std::find (_nodes.begin (), _nodes.end (), source_node);

    const auto source_rid = (*source_runtime)->native_node ().status ().routing_id ();
    // A missing epoch excludes the whole host run; a value excludes just a replaced peer.
    using unavailable_target_t =
      std::tuple<std::string, std::uint64_t, std::optional<std::uint64_t>>;
    std::set<unavailable_target_t> unavailable_targets;
    const auto complete_existing =
      [&] (const authority_snapshot_t &current) -> std::optional<result_t<actor_create_result_t>> {
        if (current.allocation.stable_type != stable_type)
            return result_t<actor_create_result_t>::failure (framework_error_kind_t::type_mismatch,
                                                             "Actor stable type does not match");
        if (exclusive)
            return result_t<actor_create_result_t>::failure (framework_error_kind_t::already_exists,
                                                             "Actor already exists");
        const auto payload = decode_direct_actor_authority_payload (current.payload);
        if (!payload)
            return result_t<actor_create_result_t>::failure (framework_error_kind_t::unavailable,
                                                             "Actor authority is unavailable");
        if (payload->state != actor_authority_state_t::ready)
            return std::nullopt;
        return result_t<actor_create_result_t>::success (
          actor_create_existing_t{::zlink::framework::detail::actor_ref_access_t::make (
            current.allocation.target.node_rid, current.allocation.stable_type,
            std::string (actor_id.value ()), current.object_generation)});
    };
    const auto terminal = co_await _location_store->read_creation_terminal (operation);
    if (terminal || !exclusive) {
        live_location_reader_t reader (*_location_store);
        const auto current =
          co_await reader.read_authority (actor_authority_key (actor_id.value ()));
        const auto *snapshot = std::get_if<authority_snapshot_t> (&current);
        if (terminal)
            co_return result_t<actor_create_result_t>::success (actor_result_from_terminal (
              *terminal, snapshot ? snapshot->allocation.target.node_rid : node_rid_t{},
              stable_type, [this] (zlink::message_t raw) {
                  return message_t::from_raw (std::move (raw), _serializers);
              }));
        if (snapshot && snapshot->allocation.state == placement_allocation_state_t::active) {
            const auto owner_available = co_await reader.owner_available (snapshot->owner);
            if (owner_available) {
                if (auto existing = complete_existing (*snapshot))
                    co_return std::move (*existing);
            }
        }
    }
    auto refresh_candidates = [&] {
        return placement_candidates (
          _services->get_required<live_location_reader_t> (), selected_mesh,
          placement_object_kind_t::actor, stable_type, source_node, deadline,
          [&] (const mesh_node_descriptor_t &descriptor) {
              if (unavailable_targets.contains (
                    {descriptor.rid.to_hex (), descriptor.lifecycle_generation, std::nullopt}))
                  return true;
              const auto epoch =
                source_node->admitted_peer_epoch (descriptor.rid, descriptor.lifecycle_generation);
              return epoch
                     && unavailable_targets.contains (
                       {descriptor.rid.to_hex (), descriptor.lifecycle_generation, *epoch});
          });
    };
    auto candidates = co_await refresh_candidates ();
    if (candidates.empty ())
        co_return result_t<actor_create_result_t>::failure (
          framework_error_kind_t::unavailable, "No eligible Actor placement target is ready");
    const auto choose_target = [&] {
        return choose_placement_target (candidates, actor_id.value ());
    };
    mesh_node_descriptor_t target;
    using actor_target_selection_t =
      std::tuple<mesh_node_descriptor_t, decltype (_nodes)::iterator, std::optional<std::uint64_t>,
                 object_reserve_request_t>;
    struct find_actor_target_t
    {
        static task_t<decltype (_nodes)::iterator> run (mesh_node_host_service_t *self,
                                                        std::decay_t<decltype (target)> target)
        {
            for (auto it = self->_nodes.begin (); it != self->_nodes.end (); ++it) {
                const auto &node = *it;
                const auto [node_mesh, rid] = co_await node->identity_async ();
                if (node_mesh == target.mesh_name && rid && rid->to_hex () == target.rid.to_hex ())
                    co_return it;
            }
            co_return self->_nodes.end ();
        }
    };
    auto target_runtime = _nodes.end ();
    std::optional<std::uint64_t> target_peer_epoch;
    std::vector<std::uint8_t> request_body;
    std::string request_content_type;
    if (request) {
        request_body = detail::message_to_raw (*request, *_serializers).to_bytes ();
        if (!request->_packet_name.empty ())
            request_content_type = _serializers->content_type (request->_type);
    }
    const auto request_bytes =
      encode_actor_creation_intent (request_content_type, std::move (request_body));
    object_reserve_request_t reserve{
      .key = {placement_object_kind_t::actor, std::string (actor_id.value ())},
      .intent = {.stable_type = stable_type,
                 .request_content_reference = encode_inline_creation_content (request_bytes),
                 .request_sha256 = sha256 (request_bytes),
                 .request_encoded_size = request_bytes.size ()},

      .capacity_bundle = {.actor_slots = 1},
      .operation_deadline = operation_deadline};
    struct select_actor_target_t
    {
        static task_t<actor_target_selection_t>
        run (mesh_node_host_service_t *self,
             std::decay_t<decltype (choose_target ())> target,
             std::decay_t<decltype (source_node)> source_node,
             std::decay_t<decltype (source_rid)> source_rid,
             std::decay_t<decltype (reserve)> reserve,
             std::decay_t<decltype (stable_type)> stable_type,
             std::decay_t<decltype (actor_id)> actor_id)
        {
            auto target_runtime = co_await find_actor_target_t::run (self, target);
            auto target_peer_epoch =
              target.rid == source_rid
                ? std::optional<std::uint64_t>{}
                : source_node->admitted_peer_epoch (target.rid, target.lifecycle_generation);
            reserve.target = {.mesh_name = target.mesh_name,
                              .node_rid = node_rid_t::from_string (target.rid.to_string ()),
                              .node_lifecycle_generation = target.lifecycle_generation,
                              .owner = {target.owner_id, target.lease_generation}};
            //  The authority payload names the placement target, so it is
            //  rewritten with the reservation target it belongs to.
            reserve.creating_payload = target_actor_authority_payload (
              actor_authority_state_t::creating, stable_type, actor_id, target);

            co_return std::tuple{std::move (target), target_runtime, target_peer_epoch,
                                 std::move (reserve)};
        }
    };
    const auto select_target = [&] () -> task_t<actor_target_selection_t> {
        return select_actor_target_t::run (this, choose_target (), source_node, source_rid, reserve,
                                           stable_type, actor_id);
    };
    std::tie (target, target_runtime, target_peer_epoch, reserve) = co_await select_target ();
    //  Location runtime §7 and Actor model §6.3: a target that cannot take this creation (no
    //  capacity, no longer serving, or no Entry Spot accepting the type) is excluded for its host
    //  execution generation and another candidate is chosen. No task when none remains.
    const auto exclude_target = [&] () -> std::optional<task_t<actor_target_selection_t>> {
        unavailable_targets.insert (
          {target.rid.to_hex (), target.lifecycle_generation, std::nullopt});
        candidates.erase (std::remove_if (candidates.begin (), candidates.end (),
                                          [&] (const auto &candidate) {
                                              return candidate.mesh_name == target.mesh_name
                                                     && candidate.rid == target.rid
                                                     && candidate.lifecycle_generation
                                                          == target.lifecycle_generation;
                                          }),
                          candidates.end ());
        if (candidates.empty ())
            return std::nullopt;
        return select_target ();
    };
    while (std::chrono::steady_clock::now () < deadline) {
        const auto reserved = co_await _location_store->reserve (reserve);
        if (const auto *existing = std::get_if<object_already_exists_t> (&reserved)) {
            if (auto result = complete_existing (existing->current))
                co_return std::move (*result);
        }
        if (std::holds_alternative<object_type_mismatch_t> (reserved))
            co_return result_t<actor_create_result_t>::failure (
              framework_error_kind_t::type_mismatch, "Actor stable type does not match");
        const auto *reserve_conflict = std::get_if<object_reserve_conflict_t> (&reserved);
        const bool target_unavailable =
          reserve_conflict
          && std::holds_alternative<authority_missing_t> (reserve_conflict->current);
        if (std::holds_alternative<object_placement_capacity_exhausted_t> (reserved)
            || target_unavailable) {
            auto has_next_target = exclude_target ();
            if (!has_next_target)
                co_return result_t<actor_create_result_t>::failure (
                  framework_error_kind_t::unavailable, "Actor placement candidates were exhausted");
            std::tie (target, target_runtime, target_peer_epoch, reserve) =
              co_await *has_next_target;
            continue;
        }
        if (const auto *winner = std::get_if<object_reserved_t> (&reserved)) {
            if (target_runtime == _nodes.end ()) {
                const auto peer_epoch =
                  (*source_runtime)->admitted_peer_epoch (target.rid, target.lifecycle_generation);
                if (!peer_epoch || !target_peer_epoch || *peer_epoch != *target_peer_epoch) {
                    // The Store descriptor can outlive a replaced peer.  This
                    // reservation has not reached transport admission, so it
                    // is safe to abandon it and re-resolve within the original
                    // deadline rather than letting raw submit retry its stale RID.
                    if (target_peer_epoch) {
                        unavailable_targets.insert (
                          {target.rid.to_hex (), target.lifecycle_generation, *target_peer_epoch});
                    }
                    (void) co_await await_result (_location_store->abort (
                      {reserve.key, winner->fence}, {}, operation_deadline));
                    candidates = co_await refresh_candidates ();
                    if (candidates.empty ())
                        co_return result_t<actor_create_result_t>::failure (
                          framework_error_kind_t::unavailable,
                          "Actor placement target is not RouteMesh-admitted");
                    std::tie (target, target_runtime, target_peer_epoch, reserve) =
                      co_await select_target ();
                    continue;
                }
                const auto source_status = (*source_runtime)->native_node ().status ();
                protocol::actor_create_header_t command;
                command.operation = {operation.operation_id.high, operation.operation_id.low};
                command.source_node_routing_id = source_status.routing_id ().to_bytes ();
                command.source_node_generation = source_status.lifecycle_generation ();
                command.actor_id = std::string (actor_id.value ());
                command.stable_type = stable_type;
                command.reservation = {
                  winner->fence.reservation_id,
                  winner->fence.expected_store_version,
                  winner->fence.object_generation,
                  winner->fence.authority_owner_generation,
                  target.rid.to_bytes (),
                  target.lifecycle_generation,
                  winner->fence.target.owner.owner_id,
                  static_cast<std::uint64_t> (winner->fence.target.owner.lease_generation),
                  static_cast<std::uint32_t> (winner->fence.capacity_bundle.actor_slots)};
                command.deadline_unix_ms = static_cast<std::uint64_t> (
                  std::chrono::duration_cast<std::chrono::milliseconds> (
                    operation_deadline.time_since_epoch ())
                    .count ());
                auto remote = co_await await_result (complete_remote_actor_creation (
                  *source_runtime, target, std::move (command), timeout, actor_id, stable_type,
                  reserve.key, winner->fence, operation));
                if (!remote)
                    co_return detail::propagate_failure<actor_create_result_t> (
                      remote, "Remote Actor creation failed");
                if (remote.value ())
                    co_return result_t<actor_create_result_t>::success (
                      std::move (*remote.value ()));
                auto has_next_target = exclude_target ();
                if (!has_next_target)
                    co_return result_t<actor_create_result_t>::failure (
                      framework_error_kind_t::unavailable,
                      "Actor placement candidates were exhausted");
                std::tie (target, target_runtime, target_peer_epoch, reserve) =
                  co_await *has_next_target;
                continue;
            }
            //  Actor model §6.3: this local target has no Entry Spot accepting the type, so the
            //  creation ends before the factory and another candidate is chosen.
            if (!(co_await detail::spot_node_runtime_t (
                    _registrations[static_cast<std::size_t> (
                                     std::distance (_nodes.begin (), target_runtime))]
                      ->spot_state)
                    .entry_accepts_actor (stable_type))) {
                const auto aborted = co_await await_result (
                  _location_store->abort ({reserve.key, winner->fence}, {}, operation_deadline));
                if (!aborted)
                    co_return detail::propagate_failure<actor_create_result_t> (
                      aborted, "Actor reservation abort failed");
                auto has_next_target = exclude_target ();
                if (!has_next_target)
                    co_return result_t<actor_create_result_t>::failure (
                      framework_error_kind_t::unavailable,
                      "Actor placement candidates were exhausted");
                std::tie (target, target_runtime, target_peer_epoch, reserve) =
                  co_await *has_next_target;
                continue;
            }
            std::optional<zlink::message_t> raw_request;
            if (request)
                raw_request = detail::message_to_raw (*request, *_serializers);
            // MeshNode §5.1: the target MeshNode holds one activation admission for this Actor
            // creation until Ready, rejection or failure.
            std::shared_ptr<void> activation_admission;
            std::optional<result_t<actor_ref_t>> factory_result;
            try {
                activation_admission = (*target_runtime)
                                         ->activation_admission ()
                                         .enter_scoped (detail::activation_admission_t::actor_key (
                                           std::string (actor_id.value ())));
            }
            catch (const framework_exception_t &error) {
                factory_result.emplace (detail::result_access_t::failure<actor_ref_t> (error));
            }
            if (!factory_result)
                factory_result.emplace (co_await await_result (
                  (*target_runtime)
                    ->create_application_actor_async (stable_type, std::string (actor_id.value ()),
                                                      winner->fence.object_generation,
                                                      winner->fence.authority_owner_generation)));
            const auto created = std::move (*factory_result);
            if (!created) {
                const auto failed_envelope = actor_terminal_envelope (
                  terminal_codec::request_terminal_result_t::internalError,
                  terminal_codec::framework_error_code_t::actorCreateFailed, std::nullopt,
                  std::nullopt, std::nullopt);
                const creation_terminal_publication_t failed_publication{operation, failed_envelope,
                                                                         operation_deadline};
                (void) co_await await_result (_location_store->complete_creation (
                  {reserve.key, winner->fence, object_creation_failed_t{failed_publication}}));
                co_return detail::propagate_failure<actor_create_result_t> (created,
                                                                            "Actor factory failed");
            }
            // Initial Entry membership belongs to creation, without an Actor
            // Join completion callback (membership §2).
            const auto node = *target_runtime;
            auto joining = std::make_shared<
              task_completion_source_t<zlink::framework::detail::actor_join_reply_t>> ();
            auto join_operation = std::make_shared<host::pending_operation_t> ();
            auto submitted = node->submit_application_actor_entry_spot_join (
              created.value (), node_rid_t::from_string (target.rid.to_string ()),
              raw_request.value_or (zlink::message_t{}), timeout, *join_operation,
              [joining] (result_t<zlink::framework::detail::actor_join_reply_t> reply) mutable {
                  joining->complete (std::move (reply));
              });
            detail::observe_task_completion (
              submitted,
              [node, join_operation, joining] (const result_t<result_t<void>> &submitted) {
                  if (!submitted)
                      joining->complete (detail::propagate_failure<detail::actor_join_reply_t> (
                        submitted, "Initial Entry membership submission failed"));
                  else if (!submitted.value ())
                      joining->complete (detail::propagate_failure<detail::actor_join_reply_t> (
                        submitted.value (), "Initial Entry membership was not submitted"));
              });
            auto creation = complete_local_actor_creation (
              joining->task (), target, created.value (), std::move (actor_id),
              std::move (stable_type), reserve.key, winner->fence, operation, operation_deadline);
            auto published = std::make_shared<task_completion_source_t<actor_create_result_t>> ();
            auto terminal = published->task ();
            detail::observe_task_completion (
              creation, [node, actor = created.value (), published,
                         activation_admission = std::move (activation_admission)] (
                          const result_t<actor_create_result_t> &result) mutable {
                  // complete_creation has published the Ready/Created terminal.
                  // Publish the creation completion before opening Actor ingress.
                  activation_admission.reset ();
                  const bool ready =
                    result && !std::holds_alternative<actor_create_rejected_t> (result.value ());
                  if (!ready)
                      node->settle_application_actor_creation (actor, false);
                  published->complete (result);
                  if (ready)
                      node->settle_application_actor_creation (actor, true);
              });
            co_return co_await terminal;
        }
        if (const auto terminal = co_await _location_store->read_creation_terminal (operation))
            co_return result_t<actor_create_result_t>::success (actor_result_from_terminal (
              *terminal, node_rid_t::from_string (target.rid.to_string ()), stable_type,
              [this] (zlink::message_t raw) {
                  return message_t::from_raw (std::move (raw), _serializers);
              }));
        co_await detail::delay (_services->get_required<detail::delay_owner_t> (),
                                std::chrono::milliseconds (1));
    }
    co_return result_t<actor_create_result_t>::failure (framework_error_kind_t::deadline_exceeded,
                                                        "Actor creation deadline elapsed");
}

} // namespace zlink::framework::runtime
