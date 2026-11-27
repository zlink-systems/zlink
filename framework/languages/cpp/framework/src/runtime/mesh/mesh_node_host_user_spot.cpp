/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service_internal.hpp"

#include "runtime/execution/task_result.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/locations/relocation_authority_payload.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/locations/source_creation_cleanup.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/timers/async_delay.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <utility>

namespace zlink::framework::runtime
{

using mesh_node_host_service_detail::choose_placement_target;
using mesh_node_host_service_detail::placement_candidates;

namespace
{
constexpr auto creation_authority_poll_interval = std::chrono::milliseconds (10);

std::atomic_uint64_t next_user_spot_operation{1};
} // namespace

task_t<spot_create_result_t> mesh_node_host_service_t::create_user_spot (
  std::shared_ptr<detail::mesh_node_runtime_t> request_source,
  bool exclusive,
  std::optional<spot_id_t> spot_id,
  std::string stable_type,
  std::optional<std::string> mesh_name,
  std::optional<message_t> request,
  std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    if (!_location_store || stable_type.empty ())
        co_return result_t<spot_create_result_t>::failure (
          framework_error_kind_t::not_configured,
          "User Spot creation requires a Location Store and stable type");
    std::shared_ptr<detail::mesh_node_runtime_t> source;
    if (mesh_name) {
        const auto found = std::find_if (_nodes.begin (), _nodes.end (), [&] (const auto &node) {
            return node && node->mesh_name () == *mesh_name;
        });
        if (found == _nodes.end ())
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::not_found,
              "The selected Mesh is not registered in this process");
        source = *found;
    } else {
        if (_nodes.empty ())
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::not_configured,
              "No object Client or Server Mesh is registered");
        if (_nodes.size () != 1)
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::invalid_operation,
              "More than one object Mesh is registered; select one with in_mesh");
        source = request_source;
    }
    if (!spot_id) {
        spot_id = detail::new_user_spot_id ();
    } else {
        try {
            detail::require_spot_id (*spot_id);
        }
        catch (const std::invalid_argument &error) {
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::not_configured, error.what ());
        }
        if (!exclusive && detail::is_framework_entry_spot_id (*spot_id)) {
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::not_configured,
              "caller-provided SpotId uses the reserved Entry Spot format");
        }
    }
    const auto selected_mesh = mesh_name.value_or (source->mesh_name ());
    std::set<std::pair<std::string, std::uint64_t>> excluded_targets;
    std::optional<mesh_node_descriptor_t> placement_target;
    std::vector<std::byte> application_bytes;
    if (request) {
        const auto raw = detail::message_to_raw (*request, *_serializers);
        const auto bytes = raw.bytes ();
        if (bytes.size () > 1024u * 1024u)
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::not_configured, "User Spot creation request exceeds 1 MiB");
        application_bytes.assign (bytes.begin (), bytes.end ());
    }
    const object_creation_key_t key{placement_object_kind_t::user_spot, *spot_id};
    object_reservation_fence_t fence;
    const std::string creating_text =
      "zlink:user-spot:creating:v1\n" + stable_type + "\n" + *spot_id;
    std::vector<std::byte> creating_payload;
    creating_payload.reserve (creating_text.size ());
    for (const auto value : creating_text)
        creating_payload.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
    object_reserve_request_t reserve_request;
    reserve_request.operation_deadline =
      std::chrono::time_point_cast<std::chrono::system_clock::duration> (
        std::chrono::system_clock::now () + (deadline - std::chrono::steady_clock::now ()));
    reserve_request.key = key;
    reserve_request.intent.stable_type = stable_type;
    reserve_request.intent.request_content_reference =
      encode_inline_creation_content (application_bytes);
    reserve_request.intent.request_sha256 = sha256 (application_bytes);
    reserve_request.intent.request_encoded_size =
      static_cast<std::uint64_t> (application_bytes.size ());
    reserve_request.creating_payload = creating_payload;
    reserve_request.capacity_bundle = {
      0, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, stable_type, 1}};
    // Static coroutine with by-value inputs: a capturing coroutine lambda would read
    // its captures after suspension (CP.51).
    struct select_user_spot_target_t
    {
        static task_t<std::optional<mesh_node_descriptor_t>>
        run (mesh_node_host_service_t *self,
             std::string mesh,
             std::string type,
             std::shared_ptr<detail::mesh_node_runtime_t> origin,
             std::chrono::steady_clock::time_point until,
             std::set<std::pair<std::string, std::uint64_t>> excluded,
             spot_id_t id)
        {
            const auto candidates = co_await placement_candidates (
              self->_services->get_required<live_location_reader_t> (), mesh,
              placement_object_kind_t::user_spot, type, origin, until,
              [&excluded] (const auto &candidate) {
                  return excluded.contains (
                    {candidate.rid.to_hex (), candidate.lifecycle_generation});
              });
            if (candidates.empty ())
                co_return std::nullopt;
            co_return choose_placement_target (candidates, id);
        }
    };
    const auto use_target = [&] {
        reserve_request.target = {placement_target->mesh_name,
                                  node_rid_t::from_string (placement_target->rid.to_string ()),
                                  placement_target->lifecycle_generation,
                                  {placement_target->owner_id, placement_target->lease_generation}};
    };
    object_reserve_result_t reserved;
    live_location_reader_t authority_reader (*_location_store);
    while (true) {
        if (std::chrono::steady_clock::now () >= deadline)
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::deadline_exceeded,
              "Timed out waiting for User Spot authority");
        auto read = co_await _location_store->read_authority (spot_authority_key (*spot_id));
        if (auto *current = std::get_if<authority_snapshot_t> (&read)) {
            if (exclusive)
                co_return result_t<spot_create_result_t>::failure (
                  framework_error_kind_t::already_exists,
                  "Framework-generated User SpotId already has an authority claim");
            if (current->allocation.object_kind != placement_object_kind_t::user_spot
                || current->allocation.stable_type != stable_type)
                co_return result_t<spot_create_result_t>::failure (
                  framework_error_kind_t::type_mismatch, "User Spot stable type does not match");
            const bool reservation_pending =
              current->allocation.state == placement_allocation_state_t::reserved
              && current->pending_creation;
            bool owner_unavailable = false;
            if (!reservation_pending) {
                const bool owner_available =
                  co_await authority_reader.owner_available (current->owner);
                owner_unavailable = !owner_available;
            }
            if (reservation_pending || owner_unavailable) {
                if (current->allocation.state == placement_allocation_state_t::active) {
                    if (!placement_target) {
                        placement_target = co_await select_user_spot_target_t::run (
                          this, selected_mesh, stable_type, source, deadline, excluded_targets,
                          *spot_id);
                        if (placement_target)
                            use_target ();
                    }
                    if (!placement_target)
                        co_return result_t<spot_create_result_t>::failure (
                          framework_error_kind_t::unavailable,
                          "User Spot authority cannot be released after its owner ended");
                }
                const auto released = co_await _location_store->release_ended_reservation (
                  spot_authority_key (*spot_id), current->store_version, {}, reserve_request);
                if (released)
                    continue;
                placement_target.reset ();
                auto after_release =
                  co_await _location_store->read_authority (spot_authority_key (*spot_id));
                const auto *same = std::get_if<authority_snapshot_t> (&after_release);
                if (!same || same->store_version != current->store_version)
                    continue;
                if (!(co_await authority_reader.owner_available (same->owner)))
                    co_return result_t<spot_create_result_t>::failure (
                      framework_error_kind_t::unavailable,
                      "User Spot authority cannot be released after its owner ended");
            }
            stateful::authority_relocation_payload_codec_t::project_application_payload (*current);
            const auto authority = decode_direct_user_spot_authority_payload (current->payload);
            if (current->allocation.state == placement_allocation_state_t::active && authority
                && authority->state == user_spot_authority_state_t::ready)
                co_return result_t<spot_create_result_t>::success (
                  {spot_ref_t{*spot_id, current->object_generation,
                              current->allocation.target.mesh_name,
                              current->allocation.target.node_rid},
                   spot_create_state_t::existing, std::nullopt});
            if (!(current->allocation.state == placement_allocation_state_t::reserved
                  && current->pending_creation)
                && !(current->allocation.state == placement_allocation_state_t::active && authority
                     && authority->state == user_spot_authority_state_t::closing))
                co_return result_t<spot_create_result_t>::failure (
                  framework_error_kind_t::unavailable, "User Spot creation authority changed");
            co_await detail::delay (
              _services->get_required<detail::delay_owner_t> (),
              std::min (creation_authority_poll_interval,
                        std::chrono::duration_cast<std::chrono::milliseconds> (
                          deadline - std::chrono::steady_clock::now ())));
            continue;
        }
        if (!placement_target) {
            placement_target = co_await select_user_spot_target_t::run (
              this, selected_mesh, stable_type, source, deadline, excluded_targets, *spot_id);
            if (placement_target)
                use_target ();
        }
        if (!placement_target) {
            // A winner may have committed between the Missing observation and
            // placement lookup. Only the outer observation classifies it.
            auto raced = co_await _location_store->read_authority (spot_authority_key (*spot_id));
            if (!std::holds_alternative<authority_missing_t> (raced))
                continue;
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::unavailable, "No eligible User Spot target is ready");
        }
        reserved = co_await _location_store->reserve (reserve_request);
        if (exclusive
            && (std::holds_alternative<object_already_exists_t> (reserved)
                || std::holds_alternative<object_reserve_conflict_t> (reserved)))
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::already_exists,
              "Framework-generated User SpotId already has an authority claim");
        if (std::holds_alternative<object_placement_capacity_exhausted_t> (reserved)) {
            excluded_targets.emplace (placement_target->rid.to_hex (),
                                      placement_target->lifecycle_generation);
            placement_target.reset ();
            continue;
        }
        if (std::holds_alternative<object_already_exists_t> (reserved)
            || std::holds_alternative<object_reserve_conflict_t> (reserved)) {
            placement_target.reset ();
            continue;
        }
        break;
    }
    auto target = std::move (*placement_target);
    if (const auto *mismatch = std::get_if<object_type_mismatch_t> (&reserved)) {
        if (exclusive && mismatch->current.allocation.state == placement_allocation_state_t::active)
            co_return result_t<spot_create_result_t>::failure (
              framework_error_kind_t::already_exists,
              "Framework-generated User SpotId already exists");
        co_return result_t<spot_create_result_t>::failure (framework_error_kind_t::type_mismatch,
                                                           "User Spot stable type does not match");
    }
    if (const auto *created = std::get_if<object_reserved_t> (&reserved)) {
        fence = created->fence;
    } else {
        co_return result_t<spot_create_result_t>::failure (framework_error_kind_t::internal_failure,
                                                           "User Spot reservation failed");
    }
    timeout = std::chrono::duration_cast<std::chrono::milliseconds> (
      deadline - std::chrono::steady_clock::now ());
    if (timeout <= std::chrono::milliseconds::zero ()) {
        (void) co_await await_result (_location_store->abort ({key, fence}));
        co_return result_t<spot_create_result_t>::failure (
          framework_error_kind_t::deadline_exceeded, "User Spot creation deadline elapsed");
    }
    const auto source_status = source->native_node ().status ();
    protocol::user_spot_create_header_t command{
      0,
      {source_status.lifecycle_generation (),
       next_user_spot_operation.fetch_add (1, std::memory_order_relaxed)},
      source_status.routing_id ().to_bytes (),
      source_status.lifecycle_generation (),
      *spot_id,
      stable_type,
      {fence.reservation_id, fence.expected_store_version, fence.object_generation,
       fence.authority_owner_generation, target.rid.to_bytes (), target.lifecycle_generation,
       fence.target.owner.owner_id,
       static_cast<std::uint64_t> (fence.target.owner.lease_generation),
       fence.capacity_bundle.spot_slots},
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    reserve_request.operation_deadline.time_since_epoch ())
                                    .count ())};
    auto completion = std::make_shared<task_completion_source_t<spot_create_result_t>> ();
    auto output = completion->task ();
    auto on_complete = [completion, target, serializers = _serializers, store = _location_store,
                        key,
                        fence] (foundation::operation_terminal_t terminal,
                                result_t<protocol::user_spot_create_reply_t> outcome,
                                std::optional<protocol::application_payload_t> application_reply) {
        if (!outcome) {
            (void) cleanup_source_created_reservation (store, key, fence, true);
            completion->complete (detail::propagate_failure<spot_create_result_t> (
              outcome, "User Spot creation failed"));
            return;
        }
        const auto &reply = outcome.value ();
        if (terminal != foundation::operation_terminal_t::completed
            || reply.header.terminal_result != 0) {
            if (terminal == foundation::operation_terminal_t::completed)
                (void) cleanup_source_created_reservation (store, key, fence, true);
            //  Spec 16 §9 / 26 — preserve the mapped kind and the exact
            //  operation/wire terminal in the message instead of
            //  collapsing every failure into one opaque string.
            const auto mapped_kind =
              user_spot_terminal::map_user_spot_operation_failure (terminal, reply.header, true);
            completion->complete (result_t<spot_create_result_t>::failure (
              mapped_kind, "Remote User Spot creation failed (kind="
                             + std::to_string (static_cast<int> (mapped_kind))
                             + ", operationTerminal=" + std::to_string (static_cast<int> (terminal))
                             + ", terminalResult=" + std::to_string (reply.header.terminal_result)
                             + ", failureCode=" + std::to_string (reply.header.failure_code)
                             + ")"));
            return;
        }
        if (reply.result == protocol::user_spot_create_result_t::rejected)
            (void) cleanup_source_created_reservation (store, key, fence, true);
        std::optional<message_t> decoded_reply;
        if (application_reply)
            decoded_reply = message_t::from_raw (
              zlink::message_t::from (application_reply->payload_bytes ()), serializers);
        completion->complete (result_t<spot_create_result_t>::success (
          {{spot_id_t (reply.spot_id), reply.object_generation, target.mesh_name,
            node_rid_t::from_string (target.rid.to_string ())},
           reply.result == protocol::user_spot_create_result_t::existing
             ? spot_create_state_t::existing
           : reply.result == protocol::user_spot_create_result_t::created
             ? spot_create_state_t::created
             : spot_create_state_t::rejected,
           std::move (decoded_reply)}));
    };
    try {
        auto accepted = source->native_node ().create_user_spot_remote (target.rid, command,
                                                                        timeout, on_complete);
        detail::observe_task_completion (accepted, [completion, store = _location_store, key,
                                                    fence] (const result_t<bool> &result) mutable {
            if (result && result.value ())
                return;
            (void) cleanup_source_created_reservation (store, key, fence, true);
            completion->complete (result_t<spot_create_result_t>::failure (
              result ? framework_error_kind_t::rejected : result.error_kind (),
              result && !result.value ()
                ? "User Spot create operation was not admitted"
                : (result.error () ? result.error ()->what ()
                                   : "User Spot create submission failed")));
        });
    }
    catch (const framework_exception_t &error) {
        (void) cleanup_source_created_reservation (_location_store, key, fence, true);
        completion->complete (detail::result_access_t::failure<spot_create_result_t> (error));
    }
    catch (const std::exception &error) {
        (void) cleanup_source_created_reservation (_location_store, key, fence, true);
        completion->complete (result_t<spot_create_result_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    co_return co_await await_result (std::move (output));
}

task_t<std::optional<spot_ref_t>> mesh_node_host_service_t::find_user_spot (spot_id_t spot_id)
{
    if (!_location_store)
        co_return result_t<std::optional<spot_ref_t>>::failure (
          framework_error_kind_t::not_configured, "Spot manager requires a Location Store");
    const auto read = co_await _location_store->read_authority (spot_authority_key (spot_id));
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
    if (!snapshot || snapshot->allocation.object_kind != placement_object_kind_t::user_spot
        || snapshot->allocation.state != placement_allocation_state_t::active)
        co_return result_t<std::optional<spot_ref_t>>::success (std::nullopt);
    co_return result_t<std::optional<spot_ref_t>>::success (
      spot_ref_t{std::move (spot_id), snapshot->object_generation,
                 snapshot->allocation.target.mesh_name, snapshot->allocation.target.node_rid});
}

task_t<bool>
mesh_node_host_service_t::close_user_spot (std::shared_ptr<detail::mesh_node_runtime_t>,
                                           spot_ref_t spot,
                                           std::chrono::steady_clock::time_point deadline,
                                           std::uint64_t wire_deadline)
{
    if (!_location_store)
        co_return result_t<bool>::failure (framework_error_kind_t::not_configured,
                                           "Spot manager requires a Location Store");
    const auto source_node = std::find_if (_nodes.begin (), _nodes.end (), [&] (const auto &node) {
        return node && node->mesh_name () == spot.mesh_name ();
    });
    if (source_node == _nodes.end ())
        co_return result_t<bool>::failure (framework_error_kind_t::not_found,
                                           "The SpotRef Mesh is not registered in this process");
    const auto source = *source_node;
    const auto read =
      co_await _location_store->read_authority (spot_authority_key (spot.spot_id ()));
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
    // Without an authority no owner can hold this incarnation. Otherwise the
    // row only addresses the owner, which classifies the request (§7.1).
    if (!snapshot)
        co_return result_t<bool>::success (false);
    const auto source_status = source->native_node ().status ();
    protocol::user_spot_close_header_t command{
      0,
      {source_status.lifecycle_generation (),
       next_user_spot_operation.fetch_add (1, std::memory_order_relaxed)},
      source_status.routing_id ().to_bytes (),
      source_status.lifecycle_generation (),
      {spot.spot_id (), spot.object_generation (),
       zlink::routing_id_t::from (std::string (snapshot->allocation.target.node_rid.value ()))
         .to_bytes (),
       snapshot->allocation.target.node_lifecycle_generation, snapshot->authority_owner_generation,
       snapshot->store_version},
      wire_deadline};
    auto completion = std::make_shared<task_completion_source_t<bool>> ();
    auto output = completion->task ();
    auto accepted = source->native_node ().close_user_spot_remote (
      zlink::routing_id_t::from (std::string (snapshot->allocation.target.node_rid.value ())),
      std::move (command), deadline,
      [completion] (foundation::operation_terminal_t terminal,
                    protocol::user_spot_close_reply_t reply) {
          if (terminal != foundation::operation_terminal_t::completed
              || reply.header.terminal_result != 0) {
              completion->complete (result_t<bool>::failure (
                user_spot_terminal::map_user_spot_operation_failure (terminal, reply.header, false),
                "Remote User Spot close failed"));
              return;
          }
          completion->complete (result_t<bool>::success (reply.closed));
      });
    detail::observe_task_completion (accepted, [completion] (const result_t<bool> &result) mutable {
        if (result && result.value ())
            return;
        completion->complete (result_t<bool>::failure (
          result ? framework_error_kind_t::rejected : result.error_kind (),
          result && !result.value ()
            ? "User Spot close operation was not admitted"
            : (result.error () ? result.error ()->what () : "User Spot close submission failed")));
    });
    co_return co_await output;
}

} // namespace zlink::framework::runtime
