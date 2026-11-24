/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/telemetry_observation.hpp"
#include "../support/execution_cases.hpp"
#include "../support/execution_actor_join_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/timers/core_timer_drain_loop.hpp"
#include "../support/scope_exit.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using zlink::framework::test::scope_exit_t;

namespace zlink::framework::test::execution
{
class actor_cutover_authority_t final
    : public zlink::framework::runtime::stateful::authority_relocation_port_t
{
  public:
    zlink::framework::runtime::stateful::source_relocation_settlement_t settle_source_relocation (
      const zlink::framework::runtime::stateful::relocation_authority_fence_t &fence,
      const std::vector<zlink::framework::runtime::stateful::object_ref_t> &participants,
      bool preserve) override
    {
        namespace sf = zlink::framework::runtime::stateful;
        (void) preserve;
        sf::source_relocation_settlement_t settled{observe_relocation (fence), {}};
        if (settled.outcome == sf::relocation_authority_t::target_committed) {
            for (const auto &participant : participants) {
                if (const auto current = read (participant.kind, participant.key))
                    settled.follow_generations.emplace (
                      std::pair{participant.kind, participant.key},
                      current->target.authority_owner_generation);
            }
        }
        return settled;
    }

    using authority_publish_result_t =
      zlink::framework::runtime::stateful::authority_publish_result_t;
    using authority_relocation_reference_t =
      zlink::framework::runtime::stateful::authority_relocation_reference_t;
    using inventory_digest_t = zlink::framework::runtime::stateful::inventory_digest_t;
    using object_kind_t = zlink::framework::runtime::stateful::object_kind_t;
    using object_ref_t = zlink::framework::runtime::stateful::object_ref_t;
    using authority_publish_status_t =
      zlink::framework::runtime::stateful::authority_publish_status_t;

    authority_publish_result_t
    publish (const object_ref_t &source,
             const object_ref_t &target,
             zlink::framework::location_owner_token_t target_owner,
             zlink::framework::object_creation_target_t,
             std::string relocation_reference,
             std::uint32_t checksum_crc32c,
             inventory_digest_t inventory_digest,
             std::vector<std::byte> target_application_payload = {},
             std::string expected_store_version = {},
             zlink::framework::runtime::protocol::relocation_id_t = {},
             zlink::framework::location_owner_token_t = {},
             const zlink::framework::runtime::protocol::relocation_prepare_t * = nullptr) override
    {
        if (on_publish)
            on_publish ();
        auto issued_target = target;
        issued_target.authority_owner_generation = source.authority_owner_generation + 1;
        authority_relocation_reference_t reference{
          .source = source,
          .target = issued_target,
          .relocation_reference = std::move (relocation_reference),
          .checksum_crc32c = checksum_crc32c,
          .inventory_digest = inventory_digest,
          .target_owner = std::move (target_owner),
          .application_payload = std::move (target_application_payload)};
        {
            std::lock_guard lock (mutex);
            current = reference;
            last_expected_store_version = std::move (expected_store_version);
        }
        return {authority_publish_status_t::published, std::move (reference)};
    }

    std::optional<authority_relocation_reference_t> read (object_kind_t,
                                                          const std::string &) override
    {
        std::lock_guard lock (mutex);
        return current;
    }

    std::function<void ()> on_publish;
    std::mutex mutex;
    std::optional<authority_relocation_reference_t> current;
    std::string last_expected_store_version;
};

bool verify_join_commit_does_not_wait_for_joined_callback (bool lifecycle_failure,
                                                           bool completion_failure)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace stateful = zlink::framework::runtime::stateful;

    constexpr const char *transfer_id = "join-commit-transfer";
    auto source_leave_failures_owner = std::make_shared<std::atomic_int> (0);
    auto source_leave_identity_owner = std::make_shared<std::atomic_bool> (false);
    auto commit_failures_owner = std::make_shared<std::atomic_int> (0);
    auto commit_identity_owner = std::make_shared<std::atomic_bool> (false);
    auto &source_leave_failures = *source_leave_failures_owner;
    auto &source_leave_identity = *source_leave_identity_owner;
    auto &commit_failures = *commit_failures_owner;
    auto &commit_identity = *commit_identity_owner;
    // Declared before the node so work queued on its Spot never outlives them.
    std::atomic_int completions{0};
    std::atomic_bool completion_identity{false};
    std::promise<void> joined_started;
    auto started = joined_started.get_future ();
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("join-commit-target");
    node->dispatch.message_flow (message_flow_log_mode_t::errors);
    auto leave_logs = std::make_shared<test::telemetry_observations_t<log_record_t>> ();
    logging_builder_t logging;
    logging.use_provider ("join-commit-cause", [leave_logs] (const log_record_t &record) {
        leave_logs->record (record);
    });
    dispatch_options_access_t::set_logger (node->dispatch, logging.create_logger ("join-commit"));
    dispatch_options_access_t::set_observer_for_tests (
      node->dispatch, [source_leave_failures_owner, source_leave_identity_owner,
                       transfer_id] (const message_flow_event_t &event) {
          if (event.packet_name != spot_actor_leave_route_command_t::packet_name)
              return;
          ++*source_leave_failures_owner;
          if (!event.exception)
              return;
          try {
              std::rethrow_exception (event.exception);
          }
          catch (const framework_exception_t &error) {
              source_leave_identity_owner->store (
                event.outcome == message_flow_outcome_t::completed
                && event.surface == dispatch_error_surface_t::spot_actor
                && event.message_kind == dispatch_message_kind_t::control
                && event.result == message_flow_result_t::failed && !event.reason
                && event.correlation_id == transfer_id
                && error.kind () == framework_error_kind_t::not_configured);
          }
      });
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (2);
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("join-commit-target");
    spot->spot_id = spot_id_t ("target-spot");
    spot->spot_name = "target";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime = node->channel_runtime;
    spot->serial_executor = node->worker_executor;
    constexpr std::uint64_t operation_high = 31;
    constexpr std::uint64_t operation_low = 37;
    constexpr std::string_view lifecycle_error = "materialized lifecycle failure";
    constexpr std::string_view completion_error = "materialized completion failure";
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      node->dispatch,
      [commit_failures_owner, commit_identity_owner, completion_failure, lifecycle_error,
       completion_error, transfer_id] (const message_dispatch_error_event_t &event) {
          if (event.packet_name != "JoinSpot" || event.correlation_id != transfer_id)
              return;
          ++*commit_failures_owner;
          if (!event.exception)
              return;
          try {
              std::rethrow_exception (event.exception);
          }
          catch (const framework_exception_t &failure) {
              if (failure.kind ()
                    == (completion_failure ? framework_error_kind_t::deadline_exceeded
                                           : framework_error_kind_t::unavailable)
                  && std::string_view (failure.what ())
                       == (completion_failure ? completion_error : lifecycle_error))
                  commit_identity_owner->store (true);
          }
      });
    spot->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *spot->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.on_join_completed =
      [&completions, &completion_identity, lifecycle_failure, completion_failure,
       completion_error] (void *, actor_join_completion_outcome_t outcome, std::uint64_t high,
                          std::uint64_t low, const actor_ref_t *, const std::optional<message_t> &,
                          framework_error_kind_t kind, bool) -> task_t<void> {
        completion_identity.store (high == operation_high && low == operation_low
                                   && (lifecycle_failure
                                         ? outcome == actor_join_completion_outcome_t::failed
                                             && kind == framework_error_kind_t::unavailable
                                         : outcome == actor_join_completion_outcome_t::accepted));
        ++completions;
        if (completion_failure)
            throw framework_exception_t (framework_error_kind_t::deadline_exceeded,
                                         std::string (completion_error));
        co_return;
    };
    node->actor_factories.emplace ("player", std::move (factory));
    auto release_joined = std::make_shared<task_completion_source_t<void>> ();
    spot_actor_admission_callbacks_t callbacks;
    callbacks.join = [] (void *, std::string_view, const zlink::message_t &,
                         serializer_registry_t &) { return spot_actor_join_result_t::accept (); };
    callbacks.on_actor_joined = [release_joined, &joined_started] (void *, void *) -> task_t<void> {
        joined_started.set_value ();
        co_await release_joined->task ();
    };
    spot->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    const auto source_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("join-commit-source"), "player", "joined-actor", 7);
    auto store = std::make_shared<wire_actor_join_authority_store_t> ();
    store->snapshot = authority_snapshot_t{
      .store_version = "join-commit-v1",
      .payload = runtime::encode_actor_authority_payload (source_actor, "source-spot", 1),
      .object_generation = 7,
      .authority_owner_generation = 19,
      .owner = location_owner_token_t{"source-owner", 5},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {.state = placement_allocation_state_t::active,
                     .object_kind = placement_object_kind_t::actor,
                     .stable_type = "player",
                     .target = {.mesh_name = "join-commit",
                                .node_rid = node_rid_t::from_string ("join-commit-source"),
                                .node_lifecycle_generation = 3,
                                .owner = location_owner_token_t{"source-owner", 5}}}};
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [store] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*store);
      },
      service_lifetime_t::singleton);
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    spot_node_runtime_t owner (node);
    owner.bind_service_provider (provider);
    auto authority = std::make_shared<actor_cutover_authority_t> ();
    const stateful::object_ref_t target{.kind = stateful::object_kind_t::actor,
                                        .key = "joined-actor",
                                        .object_generation = 7,
                                        .authority_owner_generation = 20,
                                        .mesh_name = "join-commit",
                                        .node_id = "join-commit-target"};
    auto source = target;
    source.authority_owner_generation = 19;
    source.node_id = "join-commit-source";
    authority->publish (source, target, location_owner_token_t{"target-owner", 6},
                        object_creation_target_t{}, "join-commit-ref", 0, {});
    owner.bind_relocation_authority (authority);
    const auto admitted =
      owner
        .admit_remote_actor_to_spot (transfer_id, source_actor, spot_id_t ("source-spot"),
                                     spot->spot_id, zlink::message_t{}, operation_high,
                                     operation_low, 19, 3, 5, true, 1, 1)
        .result ();
    const auto pending = node->actor_transfer_coordinator->admission (transfer_id);
    if (!admitted || !admitted.value ().accepted || !pending)
        return false;
    std::vector<handoff_packet_t> discarded;
    if (!node->actor_transfer_coordinator->begin_commit (transfer_id, source_actor, spot->spot_id,
                                                         discarded)
        || !discarded.empty ())
        return false;
    const std::string key = "player:joined-actor";
    node->actor_types_by_id.emplace ("joined-actor", "player");
    node->actor_instances.emplace (key, std::make_shared<int> (1));
    node->actor_authority_fences.emplace (
      key, runtime::protocol::actor_route_fence_t{"joined-actor", 7, {}, 1, 20, 6});
    node->actor_join_relocation_recoveries.emplace (
      key, spot_node_builder_state_t::actor_join_relocation_recovery_t{
             .handoff_id = transfer_id,
             .source_spot_id = spot_id_t ("source-spot"),
             .target_spot_id = spot->spot_id,
             .target_node_generation = 1,
             .source_actor = source_actor,
             .completion_operation_id_high = operation_high,
             .completion_operation_id_low = operation_low});
    std::promise<void> following_frame;
    auto following = following_frame.get_future ();
    std::atomic<bool> committed{false};
    std::thread receiver ([&] {
        committed.store (owner.commit_relocation_materialization ({target}));
        owner.dispatch_wire_actor_join_admission (
          spot_id_t ("following-target"), std::string_view{}, [] () -> task_t<void> { co_return; },
          [&] (result_t<void>) { following_frame.set_value (); });
    });
    const bool entered = started.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    const bool progressed =
      entered && following.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    release_joined->complete (lifecycle_failure
                                ? result_t<void>::failure (framework_error_kind_t::unavailable,
                                                           std::string (lifecycle_error))
                                : result_t<void>::success ());
    receiver.join ();
    spot->serial_queue->drain ();
    node->worker_executor->drain ();
    test::acknowledge_delivery (std::chrono::steady_clock::now () + std::chrono::seconds (2));
    const auto source_logs = leave_logs->snapshot ();
    const bool source_cause =
      std::any_of (source_logs.begin (), source_logs.end (), [transfer_id] (const auto &record) {
          const auto has = [&] (std::string_view key, std::string_view value) {
              return std::any_of (
                record.fields.begin (), record.fields.end (),
                [&] (const auto &field) { return field.key == key && field.value == value; });
          };
          return has (dispatch_event_field::packet, spot_actor_leave_route_command_t::packet_name)
                 && has (dispatch_event_field::corr, transfer_id)
                 && has (dispatch_event_field::error_type, typeid (framework_exception_t).name ())
                 && has (dispatch_event_field::error_message,
                         "Actor OnLeave node notification transport is not configured");
      });
    const bool failure_preserved =
      completions.load () == 1 && completion_identity.load () && commit_failures.load () == 1
      && commit_identity.load () && source_leave_failures.load () == 0
      && node->actor_transfer_coordinator->phase (key) == actor_move_phase_t::reconcile
      && node->actor_transfer_coordinator->blocks_dispatch (key)
      && authority->read (target.kind, target.key)->target == target;
    if (lifecycle_failure && !failure_preserved) {
        std::cerr << "materialized Join failure: callbacks=" << completions.load ()
                  << " identity=" << completion_identity.load ()
                  << " commit-errors=" << commit_failures.load ()
                  << " dispatch-blocked=" << node->actor_transfer_coordinator->blocks_dispatch (key)
                  << " callback-failure=" << completion_failure << '\n';
    }
    const bool accepted_completed =
      completions.load () == 1 && completion_identity.load () && commit_failures.load () == 0
      && source_leave_failures.load () == 1 && source_leave_identity.load () && source_cause;
    if (!lifecycle_failure && !accepted_completed) {
        std::cerr << "materialized Join SourceLeave: observations=" << source_leave_failures.load ()
                  << " identity=" << source_leave_identity.load () << '\n';
    }
    const auto passed = progressed && committed.load ()
                        && (lifecycle_failure ? failure_preserved : accepted_completed);
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}
bool verify_actor_join_finalize_replies_after_target_activation ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-finalize-node");
    auto leave_submit_unavailable_events = std::make_shared<std::atomic_int> (0);
    node->dispatch.message_flow (message_flow_log_mode_t::errors);
    auto leave_submit_logs = std::make_shared<test::telemetry_observations_t<log_record_t>> ();
    logging_builder_t logging;
    logging.use_provider ("leave-submit-cause", [leave_submit_logs] (const log_record_t &record) {
        leave_submit_logs->record (record);
    });
    dispatch_options_access_t::set_logger (node->dispatch, logging.create_logger ("leave-submit"));
    dispatch_options_access_t::set_observer_for_tests (
      node->dispatch, [leave_submit_unavailable_events] (const message_flow_event_t &event) {
          if (event.packet_name != "spot_actor_leave_submit"
              || event.result != message_flow_result_t::failed || !event.exception)
              return;
          try {
              std::rethrow_exception (event.exception);
          }
          catch (const framework_exception_t &error) {
              if (error.kind () == framework_error_kind_t::unavailable)
                  leave_submit_unavailable_events->fetch_add (1, std::memory_order_release);
          }
      });
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1, "actor-finalize");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    // The single finalize request keeps its original Join deadline while the
    // target finishes lifecycle and retained backlog publication.
    node->channel_runtime->default_request_timeout = std::chrono::milliseconds (200);
    auto target = std::make_shared<spot_context_state_t> ();
    target->node = node;
    target->node_rid = node_rid_t::from_string ("actor-finalize-node");
    target->spot_id = spot_id_t ("target-spot");
    target->spot_name = "target";
    target->spot_instance = std::make_shared<int> (1);
    target->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    target->channel_runtime->serializers = &serializers;
    target->serial_executor = node->worker_executor;
    target->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *target->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    node->spot_contexts_by_id.emplace (target->spot_id, spot_context_access_t::create (target));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    auto join_completion = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool join_completion_entered{false};
    std::atomic_int join_completion_calls{0};
    std::atomic_uint64_t join_completion_operation_high{0};
    std::atomic_uint64_t join_completion_operation_low{0};
    factory.on_join_completed = [join_completion, &join_completion_entered, &join_completion_calls,
                                 &join_completion_operation_high, &join_completion_operation_low] (
                                  void *, actor_join_completion_outcome_t outcome,
                                  std::uint64_t operation_high, std::uint64_t operation_low,
                                  const actor_ref_t *, const std::optional<message_t> &,
                                  framework_error_kind_t, bool) -> task_t<void> {
        if (outcome != actor_join_completion_outcome_t::accepted)
            throw std::runtime_error ("deferred completion was not accepted");
        ++join_completion_calls;
        join_completion_operation_high.store (operation_high, std::memory_order_release);
        join_completion_operation_low.store (operation_low, std::memory_order_release);
        join_completion_entered.store (true, std::memory_order_release);
        co_await join_completion->task ();
    };
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor =
      actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player", "actor-c2", 7);
    const auto committed_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("actor-finalize-node"), "player", "actor-c2", 7);
    actor_gateway_runtime_t gateway;
    gateway.bind_serializers (serializers);
    auto joined_bound_delivery = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool joined_bound_delivery_started{false};
    std::atomic_bool joined_bound_push_submitted{false};
    spot_actor_admission_callbacks_t callbacks;
    std::atomic_int actor_joined_calls{0};
    std::atomic_bool replay_before_joined{false};
    callbacks.join = [] (void *, std::string_view, const zlink::message_t &,
                         serializer_registry_t &) {
        return spot_actor_join_result_t::accept (message_t::from (std::string ("accepted")));
    };
    callbacks.on_actor_joined = [&actor_joined_calls, &gateway, committed_actor,
                                 &joined_bound_push_submitted] (void *, void *) -> task_t<void> {
        if (actor_joined_calls.fetch_add (1, std::memory_order_acq_rel) == 0) {
            const auto submitted = gateway.actor_context (committed_actor)
                                     .bound_session ()
                                     .send (std::string ("joined-before-completion"))
                                     .async ()
                                     .result ();
            joined_bound_push_submitted.store (static_cast<bool> (submitted),
                                               std::memory_order_release);
        }
        co_return;
    };
    target->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    spot_node_runtime_t owner (node);
    auto native =
      std::make_shared<runtime::host::public_host_runtime_t> (runtime::host::host_options_t{
        .mesh = {.descriptor = {.mesh_name = "actor-finalize-mesh",
                                .node_routing_id =
                                  zlink::routing_id_t::from ("actor-finalize-node").to_bytes (),
                                .lifecycle_generation = 1,
                                .descriptor_revision = 1,
                                .advertised_endpoint = "tcp://127.0.0.1:0"},
                 .runtime_failures =
                   std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
        .object_stable_types = {"framework.spot"}});
    native->start ();
    owner.attach_native_node (native);
    node->actor_leave_notification_sender =
      [] (const zlink::routing_id_t &,
          std::vector<zlink::message_t>) -> task_t<zlink::submit_result_t> {
        co_return zlink::submit_result_t::not_connected;
    };
    auto authority = std::make_shared<actor_cutover_authority_t> ();
    owner.bind_relocation_authority (authority);
    // The private cutover path still enters the same Store-fenced admission
    // boundary as canonical actorJoin(28).  Model its source Authority row
    // rather than reviving the retired local actor-type cache.
    auto store = std::make_shared<wire_actor_join_authority_store_t> ();
    const auto set_source_authority = [store] (const actor_ref_t &source_actor,
                                               std::uint64_t authority_owner_generation) {
        store->snapshot = authority_snapshot_t{
          .store_version = "actor-cutover-v1",
          .payload = runtime::encode_actor_authority_payload (source_actor, "source-spot", 1),
          .object_generation = source_actor.object_generation (),
          .authority_owner_generation = authority_owner_generation,
          .owner = location_owner_token_t{"source-owner", 29},
          .store_now = std::chrono::system_clock::now (),
          .allocation = {.state = placement_allocation_state_t::active,
                         .object_kind = placement_object_kind_t::actor,
                         .stable_type = "player",
                         .target = {.mesh_name = "source-mesh",
                                    .node_rid = node_rid_t::from_string (
                                      std::string (source_actor.node_rid ().value ())),
                                    .node_lifecycle_generation = 1,
                                    .owner = location_owner_token_t{"source-owner", 29}}}};
    };
    set_source_authority (actor, 19);
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [store] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (*store);
      },
      service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    owner.bind_service_provider (provider);
    const auto bound = gateway.bind_session_sink (
      actor,
      [joined_bound_delivery, &joined_bound_delivery_started] (
        std::string, stream_codec_t, const zlink::message_t &) -> task_t<void> {
          joined_bound_delivery_started.store (true, std::memory_order_release);
          co_await joined_bound_delivery->task ();
      });
    if (!bound) {
        return false;
    }
    std::atomic_int replayed{0};
    std::atomic_bool successor_join_reserved{false};
    auto first_replay = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool first_replay_entered{false};
    std::mutex delivery_order_mutex;
    std::vector<std::string> delivery_order;
    target->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "BacklogPacket", "",
                                std::type_index (typeid (int)), std::type_index (typeid (int)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    target->handler_invokers.push_back (
      [&owner, actor, &replayed, &successor_join_reserved, &actor_joined_calls,
       &replay_before_joined, first_replay, &first_replay_entered, &delivery_order_mutex,
       &delivery_order] (void *, void *, service_provider_t &, serializer_registry_t &,
                         const zlink::message_t &,
                         const spot_inbound_message_t &metadata) -> task_t<zlink::message_t> {
          const auto sequence = metadata.find ("sequence");
          if (!sequence)
              throw std::runtime_error ("handoff replay sequence is missing");
          if (actor_joined_calls.load (std::memory_order_acquire) == 0)
              replay_before_joined.store (true, std::memory_order_release);
          if (*sequence == "B1") {
              first_replay_entered.store (true, std::memory_order_release);
              co_await first_replay->task ();
          }
          ++replayed;
          {
              std::lock_guard lock (delivery_order_mutex);
              delivery_order.push_back (std::string (*sequence));
          }
          if (*sequence == "B1") {
              auto reserved = owner.reserve_actor_join_barrier (actor);
              successor_join_reserved.store (static_cast<bool> (reserved),
                                             std::memory_order_release);
              if (reserved)
                  reserved.value ()->cancel ();
          }
          co_return zlink::message_t{};
      });

    const std::string transfer_id = "transfer-c2";
    const std::string key = "player:actor-c2";
    // Completes the blockers the queued Join work waits for and drains before the locals above
    // are destroyed, also when a check fails early.
    scope_exit_t drain_on_exit ([&] {
        (void) join_completion->complete (result_t<void>::success ());
        (void) joined_bound_delivery->complete (result_t<void>::success ());
        (void) first_replay->complete (result_t<void>::success ());
        target->serial_queue->drain ();
        node->worker_executor->drain ();
    });
    const auto admitted = owner
                            .admit_remote_actor_to_spot (
                              transfer_id, actor, spot_id_t ("source-spot"), target->spot_id,
                              zlink::message_t::from (std::string ("prepare")), 11, 13, 19, 1, 29)
                            .result ();
    if (!admitted || !admitted.value ().accepted) {
        return false;
    }
    const auto prepared = owner.prepare_remote_actor_to_spot (
      transfer_id, actor, target->spot_id, zlink::message_t{}, gateway.actor_context (actor), true);
    if (!prepared) {
        return false;
    }

    const auto admission_deadline =
      node->actor_transfer_coordinator->admission (transfer_id)->deadline;
    const auto actor_queue = target->spot_serial_executor->actor_executor (key)->queue ();
    spot_actor_commit_route_request_t cutover{
      .transfer_id = transfer_id,
      .actor_node_rid = "source-node",
      .actor_type = "player",
      .actor_id = "actor-c2",
      .actor_generation = 7,
      .actor_authority_owner_generation = 19,
      .expected_actor_store_version = "source-version",
      .target_spot_id = "target-spot",
      .target_spot_generation = 1,
      .source_mesh_name = "source-mesh",
      .target_mesh_name = "actor-finalize-mesh",
      .target_node_lifecycle_generation = native->status ().lifecycle_generation (),
      .target_owner_id = "target-owner",
      .target_owner_lease_generation = 23,
      .source_spot_id = "source-spot",
      .source_spot_generation = 1,
      .handoff_backlog = {spot_actor_handoff_packet_t{.packet_name_value = "BacklogPacket",
                                                      .payload = {1},
                                                      .content_type = "application/x-test",
                                                      .metadata = {{"sequence", "B1"}}},
                          spot_actor_handoff_packet_t{.packet_name_value = "BacklogPacket",
                                                      .payload = {2},
                                                      .content_type = "application/x-test",
                                                      .metadata = {{"sequence", "B2"}}}},
      .finalize = true};
    authority->on_publish = [&] {
        const auto appended = node->actor_transfer_coordinator->try_append_backlog (
          key, handoff_packet_t{
                 "BacklogPacket", {3}, "application/x-test", {{"sequence", "D1"}}, false});
        if (appended != handoff_append_result_t::appended)
            throw std::runtime_error ("direct packet was not retained at the authority boundary");
    };
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::command;
    header.channel_name = "actor-route";
    header.message_name = spot_actor_commit_route_request_t::packet_name;
    const auto parts =
      runtime::messaging::envelope_codec_t{}.encode_parts (header, cutover, serializers);
    spot_route_internal_dispatcher_t dispatcher (owner, gateway, route_client_t{}, serializers);
    if (!dispatcher.can_handle_send (spot_actor_commit_route_request_t::packet_name)) {
        return false;
    }
    const auto submitted =
      dispatcher
        .dispatch_send (
          route_received_packet_t{zlink::routing_id_t::from ("source-node"), 1, parts}, header,
          provider)
        .result ()
        .value ();
    const auto join_completion_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!join_completion_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < join_completion_deadline) {
        std::this_thread::yield ();
    }
    while (!joined_bound_delivery_started.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < join_completion_deadline) {
        std::this_thread::yield ();
    }
    if (!submitted || !join_completion_entered.load (std::memory_order_acquire)
        || !joined_bound_push_submitted.load (std::memory_order_acquire)
        || !joined_bound_delivery_started.load (std::memory_order_acquire)
        || actor_joined_calls.load (std::memory_order_acquire) != 1
        || replayed.load (std::memory_order_acquire) != 0
        || !node->actor_transfer_coordinator->blocks_dispatch (key)) {
        std::cerr << "actor cutover dispatch diagnostic submitted=" << static_cast<bool> (submitted)
                  << " error=" << (submitted.error () ? submitted.error ()->what () : "<none>")
                  << " completion-entered=" << join_completion_entered.load ()
                  << " bound-submitted=" << joined_bound_push_submitted.load ()
                  << " bound-started=" << joined_bound_delivery_started.load ()
                  << " joined=" << actor_joined_calls.load () << " replayed=" << replayed.load ()
                  << " blocked=" << node->actor_transfer_coordinator->blocks_dispatch (key) << '\n';
        joined_bound_delivery->complete (result_t<void>::success ());
        return false;
    }
    join_completion->complete (result_t<void>::success ());
    const auto bound_delivery_observation_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (100);
    while (!first_replay_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < bound_delivery_observation_deadline) {
        std::this_thread::yield ();
    }
    const auto completion_waited_for_joined_bound_delivery =
      !first_replay_entered.load (std::memory_order_acquire)
      && node->actor_transfer_coordinator->blocks_dispatch (key);
    joined_bound_delivery->complete (result_t<void>::success ());
    if (!completion_waited_for_joined_bound_delivery)
        return false;
    const auto replay_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!first_replay_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < replay_deadline) {
        std::this_thread::yield ();
    }
    if (!first_replay_entered.load (std::memory_order_acquire))
        return false;
    // The commit record expires at the admission deadline even while the
    // accepted Actor turn still owns replay. Completion is the FIFO terminal,
    // independent of that replay cache's retention.
    (void) node->actor_transfer_coordinator->cleanup_expired_async (admission_deadline, {})
      .result ();
    first_replay->complete (result_t<void>::success ());
    const auto finalized_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while ((actor_queue->pending_count () != 0 || replayed.load (std::memory_order_acquire) != 3)
           && std::chrono::steady_clock::now () < finalized_deadline) {
        std::this_thread::yield ();
    }
    std::vector<std::string> observed_delivery_order;
    {
        std::lock_guard lock (delivery_order_mutex);
        observed_delivery_order = delivery_order;
    }
    if (actor_queue->pending_count () != 0
        || node->actor_transfer_coordinator->blocks_dispatch (key) || replayed.load () != 3
        || replay_before_joined.load (std::memory_order_acquire)
        || !join_completion_entered.load (std::memory_order_acquire)
        || join_completion_calls.load () != 1
        || join_completion_operation_high.load (std::memory_order_acquire) != 11
        || join_completion_operation_low.load (std::memory_order_acquire) != 13
        || actor_joined_calls.load (std::memory_order_acquire) != 1
        || !completion_waited_for_joined_bound_delivery
        || !first_replay_entered.load (std::memory_order_acquire)
        || !successor_join_reserved.load (std::memory_order_acquire)
        || observed_delivery_order != std::vector<std::string>{"B1", "B2", "D1"}) {
        std::cerr << "actor cutover replay diagnostic blocked="
                  << node->actor_transfer_coordinator->blocks_dispatch (key)
                  << " replayed=" << replayed.load ()
                  << " completion-calls=" << join_completion_calls.load ()
                  << " joined=" << actor_joined_calls.load () << " order=";
        for (const auto &item : observed_delivery_order)
            std::cerr << item << ',';
        std::cerr << '\n';
        return false;
    }
    const auto leave_submit_observation_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (leave_submit_unavailable_events->load (std::memory_order_acquire) == 0
           && std::chrono::steady_clock::now () < leave_submit_observation_deadline)
        std::this_thread::yield ();
    test::acknowledge_delivery (leave_submit_observation_deadline);
    const auto submit_logs = leave_submit_logs->snapshot ();
    const bool submit_cause =
      std::any_of (submit_logs.begin (), submit_logs.end (), [] (const auto &record) {
          const auto has = [&] (std::string_view key, std::string_view value) {
              return std::any_of (
                record.fields.begin (), record.fields.end (),
                [&] (const auto &field) { return field.key == key && field.value == value; });
          };
          return has (dispatch_event_field::packet, "spot_actor_leave_submit")
                 && has (dispatch_event_field::error_type, typeid (framework_exception_t).name ())
                 && std::any_of (record.fields.begin (), record.fields.end (),
                                 [] (const auto &field) {
                                     return field.key == dispatch_event_field::error_message
                                            && !field.value.empty ();
                                 });
      });
    if (!submit_cause || leave_submit_unavailable_events->load (std::memory_order_acquire) != 1)
        return false;
    {
        std::lock_guard lock (authority->mutex);
        if (authority->last_expected_store_version != "source-version") {
            std::cerr << "remote Actor authority commit omitted the source StoreVersion fence\n";
            return false;
        }
    }
    // A duplicate internal finalize cannot reopen a completed target or
    // dispatch its retained backlog twice.
    const auto duplicate = owner
                             .finalize_remote_actor_to_spot (transfer_id, actor, target->spot_id,
                                                             provider, nullptr, std::nullopt)
                             .result ();
    if (duplicate || node->actor_transfer_coordinator->blocks_dispatch (key)
        || replayed.load () != 3 || join_completion_calls.load (std::memory_order_acquire) != 1
        || actor_joined_calls.load (std::memory_order_acquire) != 1) {
        return false;
    }

    // The combined internal commit path must defer OnActorJoined into the same
    // fenced finalizer instead of reopening the pre-callback delivery window.
    const auto combined_actor = actor_ref_access_t::make (node_rid_t::from_string ("source-node"),
                                                          "player", "actor-combined", 7);
    const auto combined_committed_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("actor-finalize-node"), "player", "actor-combined", 7);
    const std::string combined_transfer_id = "transfer-combined";
    set_source_authority (combined_actor, 39);
    const auto combined_admitted =
      owner
        .admit_remote_actor_to_spot (
          combined_transfer_id, combined_actor, spot_id_t ("source-spot"), target->spot_id,
          zlink::message_t::from (std::string ("prepare")), 31, 33, 39, 1, 29)
        .result ();
    auto combined_bound_delivery = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool combined_bound_delivery_started{false};
    const auto combined_bound = gateway.bind_session_sink (
      combined_actor,
      [combined_bound_delivery, &combined_bound_delivery_started] (
        std::string, stream_codec_t, const zlink::message_t &) -> task_t<void> {
          combined_bound_delivery_started.store (true, std::memory_order_release);
          co_await combined_bound_delivery->task ();
      });
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [&actor_joined_calls, &gateway, combined_committed_actor] (void *, void *) -> task_t<void> {
        actor_joined_calls.fetch_add (1, std::memory_order_release);
        const auto submitted = gateway.actor_context (combined_committed_actor)
                                 .bound_session ()
                                 .send (std::string ("combined-joined"))
                                 .async ()
                                 .result ();
        if (!submitted)
            throw std::runtime_error ("combined Join bound push was not submitted");
        co_return;
    };
    if (!combined_admitted || !combined_admitted.value ().accepted || !combined_bound)
        return false;
    auto combined = std::async (std::launch::async, [&] {
        return owner
          .commit_remote_actor_to_spot (combined_transfer_id, combined_actor, target->spot_id,
                                        zlink::message_t{}, gateway.actor_context (combined_actor),
                                        {}, &provider)
          .result ();
    });
    const auto combined_delivery_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!combined_bound_delivery_started.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < combined_delivery_deadline) {
        std::this_thread::yield ();
    }
    const auto combined_waited_for_delivery =
      combined_bound_delivery_started.load (std::memory_order_acquire)
      && combined.wait_for (std::chrono::milliseconds (100)) != std::future_status::ready;
    combined_bound_delivery->complete (result_t<void>::success ());
    const auto combined_finished =
      combined.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    const auto combined_result = combined_finished
                                   ? combined.get ()
                                   : result_t<actor_join_reply_t>::failure (
                                       framework_error_kind_t::deadline_exceeded,
                                       "combined Actor commit did not finish after bound delivery");
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [&actor_joined_calls] (void *, void *) -> task_t<void> {
        actor_joined_calls.fetch_add (1, std::memory_order_release);
        co_return;
    };
    if (!combined_waited_for_delivery || !combined_result)
        return false;

    // If the deadline wins while the replay owner is still queued, no Join
    // completion, location publication or completed admission may escape.
    const auto queued_actor =
      actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player", "actor-c3", 7);
    const std::string queued_transfer_id = "transfer-c3";
    const std::string queued_key = "player:actor-c3";
    set_source_authority (queued_actor, 29);
    const auto queued_admitted =
      owner
        .admit_remote_actor_to_spot (
          queued_transfer_id, queued_actor, spot_id_t ("source-spot"), target->spot_id,
          zlink::message_t::from (std::string ("prepare")), 21, 23, 29, 1, 29)
        .result ();
    const auto queued_prepared = owner.prepare_remote_actor_to_spot (
      queued_transfer_id, queued_actor, target->spot_id, zlink::message_t{},
      actor_gateway_runtime_t{}.actor_context (queued_actor), true);
    if (!queued_admitted || !queued_admitted.value ().accepted || !queued_prepared) {
        return false;
    }

    runtime::serial_execution_queue_options_t queued_options;
    auto queued_actor_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *node->worker_executor, queued_options, runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::actor_delivery ());
    target->spot_serial_executor->replace_actor_queue (queued_key, queued_actor_queue);
    std::mutex queued_gate;
    std::condition_variable queued_changed;
    std::optional<runtime::serial_execution_queue_t::async_completion_t> release_predecessor;
    std::mutex cancelled_mutex;
    std::condition_variable cancelled_changed;
    std::optional<result_t<actor_join_reply_t>> cancelled;
    bool finalize_started = false;
    if (!queued_actor_queue->try_post_async ("deadline-predecessor", [&] (auto complete) {
            {
                std::lock_guard lock (queued_gate);
                release_predecessor.emplace (std::move (complete));
            }
            queued_changed.notify_all ();
        })) {
        return false;
    }
    // Releases the predecessor, waits for the finalize callback and drains before the locals
    // above are destroyed.
    scope_exit_t drain_queued ([&] {
        runtime::serial_execution_queue_t::async_completion_t release;
        {
            std::unique_lock lock (queued_gate);
            queued_changed.wait (lock, [&] { return release_predecessor.has_value (); });
            release = std::move (*release_predecessor);
        }
        if (release)
            release ([] {});
        if (finalize_started) {
            std::unique_lock lock (cancelled_mutex);
            cancelled_changed.wait (lock, [&] { return cancelled.has_value (); });
        }
        if (queued_actor_queue)
            queued_actor_queue->drain ();
    });
    {
        std::unique_lock lock (queued_gate);
        if (!queued_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return release_predecessor.has_value (); })) {
            queued_actor_queue->cancel_pending ();
            return false;
        }
    }
    const auto completion_calls_before_cancel = join_completion_calls.load ();
    finalize_started = true;
    owner.finalize_remote_actor_to_spot_async (
      queued_transfer_id, queued_actor, target->spot_id, provider, nullptr,
      std::chrono::steady_clock::now () + std::chrono::milliseconds (10),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (cancelled_mutex);
              cancelled.emplace (std::move (result));
          }
          cancelled_changed.notify_all ();
      });
    {
        std::unique_lock lock (cancelled_mutex);
        if (!cancelled_changed.wait_for (lock, std::chrono::seconds (1),
                                         [&] { return cancelled.has_value (); })) {
            return false;
        }
    }
    runtime::serial_execution_queue_t::async_completion_t finish_predecessor;
    {
        std::lock_guard lock (queued_gate);
        finish_predecessor = std::move (*release_predecessor);
    }
    finish_predecessor ([] {});
    queued_actor_queue->drain ();
    if (*cancelled || cancelled->error_kind () != framework_error_kind_t::deadline_exceeded
        || node->actor_transfer_coordinator->phase (queued_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (queued_transfer_id, queued_actor, target->spot_id)
        || join_completion_calls.load () != completion_calls_before_cancel) {
        return false;
    }
    target->spot_serial_executor->erase_actor_queue (queued_key);
    queued_actor_queue.reset ();

    // Destroying the deadline owner must leave timer readiness draining usable.
    // A second timer proves the polling path remains available.
    std::mutex probe_mutex;
    std::condition_variable probe_changed;
    bool probe_fired = false;
    zlink::framework::detail::core_timer_drain_loop_t probe_timer (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    probe_timer.start (std::chrono::milliseconds (1), 1, [&] (std::uint64_t) {
        {
            std::lock_guard lock (probe_mutex);
            probe_fired = true;
        }
        probe_changed.notify_all ();
    });
    {
        std::unique_lock lock (probe_mutex);
        if (!probe_changed.wait_for (lock, std::chrono::seconds (1), [&] { return probe_fired; })) {
            return false;
        }
    }
    probe_timer.close ();

    // A deadline that expires while on_actor_joined is queued removes that
    // lifecycle submission. The failed transfer must not run the callback
    // later when the predecessor releases the Spot queue.
    const auto lifecycle_actor =
      actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player", "actor-c4", 7);
    const std::string lifecycle_transfer_id = "transfer-c4";
    const std::string lifecycle_key = "player:actor-c4";
    set_source_authority (lifecycle_actor, 41);
    const auto lifecycle_admitted =
      owner
        .admit_remote_actor_to_spot (
          lifecycle_transfer_id, lifecycle_actor, spot_id_t ("source-spot"), target->spot_id,
          zlink::message_t::from (std::string ("prepare")), 31, 37, 41, 1, 29)
        .result ();
    const auto lifecycle_prepared = owner.prepare_remote_actor_to_spot (
      lifecycle_transfer_id, lifecycle_actor, target->spot_id, zlink::message_t{},
      gateway.actor_context (lifecycle_actor), true);
    if (!lifecycle_admitted || !lifecycle_admitted.value ().accepted || !lifecycle_prepared) {
        return false;
    }
    std::mutex lifecycle_gate;
    std::condition_variable lifecycle_gate_changed;
    std::optional<runtime::serial_execution_queue_t::async_completion_t>
      release_lifecycle_predecessor;
    std::mutex lifecycle_cancel_mutex;
    std::condition_variable lifecycle_cancel_changed;
    std::optional<result_t<actor_join_reply_t>> lifecycle_cancelled;
    bool lifecycle_finalize_started = false;
    if (!target->serial_queue->try_post_async (
          "deadline-lifecycle-predecessor",
          [&] (auto complete) {
              {
                  std::lock_guard lock (lifecycle_gate);
                  release_lifecycle_predecessor.emplace (std::move (complete));
              }
              lifecycle_gate_changed.notify_all ();
          },
          runtime::serial_work_options_t{
            runtime::serial_work_lane_t::lifecycle,
            runtime::serial_execution_queue_t::fixed_work_byte_cost})) {
        return false;
    }
    // Releases the predecessor, waits for the finalize callback and drains before the locals
    // above are destroyed.
    scope_exit_t drain_lifecycle ([&] {
        runtime::serial_execution_queue_t::async_completion_t release;
        {
            std::unique_lock lock (lifecycle_gate);
            lifecycle_gate_changed.wait (
              lock, [&] { return release_lifecycle_predecessor.has_value (); });
            release = std::move (*release_lifecycle_predecessor);
        }
        if (release)
            release ([] {});
        if (lifecycle_finalize_started) {
            std::unique_lock lock (lifecycle_cancel_mutex);
            lifecycle_cancel_changed.wait (lock, [&] { return lifecycle_cancelled.has_value (); });
        }
        target->serial_queue->drain ();
    });
    {
        std::unique_lock lock (lifecycle_gate);
        if (!lifecycle_gate_changed.wait_for (lock, std::chrono::seconds (1), [&] {
                return release_lifecycle_predecessor.has_value ();
            })) {
            return false;
        }
    }
    const auto joined_calls_before_lifecycle_cancel =
      actor_joined_calls.load (std::memory_order_acquire);
    lifecycle_finalize_started = true;
    owner.finalize_remote_actor_to_spot_async (
      lifecycle_transfer_id, lifecycle_actor, target->spot_id, provider, &gateway,
      std::chrono::steady_clock::now () + std::chrono::milliseconds (10),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (lifecycle_cancel_mutex);
              lifecycle_cancelled.emplace (std::move (result));
          }
          lifecycle_cancel_changed.notify_all ();
      });
    {
        std::unique_lock lock (lifecycle_cancel_mutex);
        if (!lifecycle_cancel_changed.wait_for (lock, std::chrono::seconds (1),
                                                [&] { return lifecycle_cancelled.has_value (); })) {
            return false;
        }
    }
    runtime::serial_execution_queue_t::async_completion_t finish_lifecycle_predecessor;
    {
        std::lock_guard lock (lifecycle_gate);
        finish_lifecycle_predecessor = std::move (*release_lifecycle_predecessor);
    }
    finish_lifecycle_predecessor ([] {});
    target->serial_queue->drain ();
    if (*lifecycle_cancelled
        || lifecycle_cancelled->error_kind () != framework_error_kind_t::deadline_exceeded
        || actor_joined_calls.load (std::memory_order_acquire)
             != joined_calls_before_lifecycle_cancel
        || node->actor_transfer_coordinator->phase (lifecycle_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (lifecycle_transfer_id, lifecycle_actor,
                                                target->spot_id)) {
        return false;
    }

    // If the deadline expires after on_actor_joined has started, the transfer
    // keeps its terminal owner until that callback settles. The callback's
    // terminal is converted into the admitted Join failure completion before
    // the transfer enters reconciliation.
    auto active_lifecycle = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool active_lifecycle_entered{false};
    std::atomic_int active_lifecycle_failure_calls{0};
    std::mutex active_lifecycle_mutex;
    std::condition_variable active_lifecycle_changed;
    std::optional<result_t<actor_join_reply_t>> active_lifecycle_result;
    bool active_lifecycle_finalize_started = false;
    // Completes the lifecycle callback, waits for the finalize callback and drains before the
    // locals above are destroyed.
    scope_exit_t drain_active_lifecycle ([&] {
        (void) active_lifecycle->complete (result_t<void>::success ());
        if (active_lifecycle_finalize_started) {
            std::unique_lock lock (active_lifecycle_mutex);
            active_lifecycle_changed.wait (lock,
                                           [&] { return active_lifecycle_result.has_value (); });
        }
        target->serial_queue->drain ();
    });
    node->actor_factories.at ("player").on_join_completed =
      [&active_lifecycle_failure_calls] (void *, actor_join_completion_outcome_t outcome,
                                         std::uint64_t operation_high, std::uint64_t operation_low,
                                         const actor_ref_t *, const std::optional<message_t> &,
                                         framework_error_kind_t error_kind, bool) -> task_t<void> {
        if (outcome != actor_join_completion_outcome_t::failed || operation_high != 59
            || operation_low != 61 || error_kind != framework_error_kind_t::deadline_exceeded) {
            throw std::runtime_error ("active lifecycle deadline completion lost its OperationId");
        }
        ++active_lifecycle_failure_calls;
        co_return;
    };
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [active_lifecycle, &active_lifecycle_entered] (void *, void *) -> task_t<void> {
        active_lifecycle_entered.store (true, std::memory_order_release);
        co_await active_lifecycle->task ();
    };
    const auto active_lifecycle_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c4-active", 7);
    const std::string active_lifecycle_transfer_id = "transfer-c4-active";
    const std::string active_lifecycle_key = "player:actor-c4-active";
    set_source_authority (active_lifecycle_actor, 67);
    const auto active_lifecycle_admitted =
      owner
        .admit_remote_actor_to_spot (
          active_lifecycle_transfer_id, active_lifecycle_actor, spot_id_t ("source-spot"),
          target->spot_id, zlink::message_t::from (std::string ("prepare")), 59, 61, 67, 1, 29)
        .result ();
    const auto active_lifecycle_prepared = owner.prepare_remote_actor_to_spot (
      active_lifecycle_transfer_id, active_lifecycle_actor, target->spot_id, zlink::message_t{},
      gateway.actor_context (active_lifecycle_actor), true);
    if (!active_lifecycle_admitted || !active_lifecycle_admitted.value ().accepted
        || !active_lifecycle_prepared) {
        return false;
    }
    active_lifecycle_finalize_started = true;
    owner.finalize_remote_actor_to_spot_async (
      active_lifecycle_transfer_id, active_lifecycle_actor, target->spot_id, provider, &gateway,
      std::chrono::steady_clock::now () + std::chrono::milliseconds (20),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (active_lifecycle_mutex);
              active_lifecycle_result.emplace (std::move (result));
          }
          active_lifecycle_changed.notify_all ();
      });
    const auto active_lifecycle_start_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!active_lifecycle_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < active_lifecycle_start_deadline) {
        std::this_thread::yield ();
    }
    if (!active_lifecycle_entered.load (std::memory_order_acquire))
        return false;
    {
        std::unique_lock lock (active_lifecycle_mutex);
        if (active_lifecycle_changed.wait_for (lock, std::chrono::milliseconds (80), [&] {
                return active_lifecycle_result.has_value ();
            })) {
            return false;
        }
    }
    if (active_lifecycle_failure_calls.load (std::memory_order_acquire) != 0)
        return false;
    active_lifecycle->complete (result_t<void>::success ());
    {
        std::unique_lock lock (active_lifecycle_mutex);
        if (!active_lifecycle_changed.wait_for (lock, std::chrono::seconds (1), [&] {
                return active_lifecycle_result.has_value ();
            })) {
            return false;
        }
    }
    if (*active_lifecycle_result
        || active_lifecycle_result->error_kind () != framework_error_kind_t::deadline_exceeded
        || active_lifecycle_failure_calls.load (std::memory_order_acquire) != 1
        || node->actor_transfer_coordinator->phase (active_lifecycle_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (active_lifecycle_transfer_id,
                                                active_lifecycle_actor, target->spot_id)) {
        return false;
    }

    // Host shutdown ends the registered source waiter once with ShuttingDown.
    // Its original source remains pending until the application completes it.
    auto shutdown_lifecycle = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool shutdown_lifecycle_entered{false};
    std::atomic_int shutdown_lifecycle_failure_calls{0};
    std::mutex shutdown_lifecycle_mutex;
    std::condition_variable shutdown_lifecycle_changed;
    std::optional<result_t<actor_join_reply_t>> shutdown_lifecycle_result;
    std::atomic_int shutdown_lifecycle_terminals{0};
    bool shutdown_lifecycle_finalize_started = false;
    // Completes the lifecycle callback, waits for the finalize callback and drains before the
    // locals above are destroyed.
    scope_exit_t drain_shutdown_lifecycle ([&] {
        (void) shutdown_lifecycle->complete (result_t<void>::success ());
        if (shutdown_lifecycle_finalize_started) {
            std::unique_lock lock (shutdown_lifecycle_mutex);
            shutdown_lifecycle_changed.wait (
              lock, [&] { return shutdown_lifecycle_result.has_value (); });
        }
        target->serial_queue->drain ();
    });
    node->actor_factories.at ("player").on_join_completed =
      [&shutdown_lifecycle_failure_calls] (
        void *, actor_join_completion_outcome_t outcome, std::uint64_t operation_high,
        std::uint64_t operation_low, const actor_ref_t *, const std::optional<message_t> &,
        framework_error_kind_t error_kind, bool) -> task_t<void> {
        if (outcome != actor_join_completion_outcome_t::failed || operation_high != 71
            || operation_low != 73 || error_kind != framework_error_kind_t::shutting_down) {
            throw std::runtime_error (
              "active lifecycle shutdown completion lost its terminal reason");
        }
        ++shutdown_lifecycle_failure_calls;
        co_return;
    };
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [shutdown_lifecycle, &shutdown_lifecycle_entered] (void *, void *) -> task_t<void> {
        shutdown_lifecycle_entered.store (true, std::memory_order_release);
        co_await shutdown_lifecycle->task ();
    };
    const auto shutdown_lifecycle_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("source-node"), "player", "actor-c4-shutdown", 7);
    const std::string shutdown_lifecycle_transfer_id = "transfer-c4-shutdown";
    const std::string shutdown_lifecycle_key = "player:actor-c4-shutdown";
    set_source_authority (shutdown_lifecycle_actor, 79);
    const auto shutdown_lifecycle_admitted =
      owner
        .admit_remote_actor_to_spot (
          shutdown_lifecycle_transfer_id, shutdown_lifecycle_actor, spot_id_t ("source-spot"),
          target->spot_id, zlink::message_t::from (std::string ("prepare")), 71, 73, 79, 1, 29)
        .result ();
    const auto shutdown_lifecycle_prepared = owner.prepare_remote_actor_to_spot (
      shutdown_lifecycle_transfer_id, shutdown_lifecycle_actor, target->spot_id, zlink::message_t{},
      gateway.actor_context (shutdown_lifecycle_actor), true);
    if (!shutdown_lifecycle_admitted || !shutdown_lifecycle_admitted.value ().accepted
        || !shutdown_lifecycle_prepared) {
        return false;
    }
    auto shutdown_source_task = shutdown_lifecycle->task ();
    shutdown_lifecycle_finalize_started = true;
    owner.finalize_remote_actor_to_spot_async (
      shutdown_lifecycle_transfer_id, shutdown_lifecycle_actor, target->spot_id, provider, &gateway,
      std::chrono::steady_clock::now () + std::chrono::seconds (1),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (shutdown_lifecycle_mutex);
              ++shutdown_lifecycle_terminals;
              shutdown_lifecycle_result.emplace (std::move (result));
          }
          shutdown_lifecycle_changed.notify_all ();
      });
    const auto shutdown_lifecycle_start_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!shutdown_lifecycle_entered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < shutdown_lifecycle_start_deadline) {
        std::this_thread::yield ();
    }
    if (!shutdown_lifecycle_entered.load (std::memory_order_acquire))
        return false;
    target->serial_queue->cancel_pending ();
    {
        std::unique_lock lock (shutdown_lifecycle_mutex);
        if (!shutdown_lifecycle_changed.wait_for (lock, std::chrono::seconds (1), [&] {
                return shutdown_lifecycle_result.has_value ();
            })) {
            return false;
        }
    }
    if (shutdown_source_task.result_for (std::chrono::milliseconds (0))
        || shutdown_lifecycle_terminals.load () != 1
        || !shutdown_lifecycle->complete (result_t<void>::success ())
        || shutdown_lifecycle->complete (result_t<void>::success ())
        || !shutdown_source_task.result_for (std::chrono::milliseconds (0))
        || shutdown_lifecycle_terminals.load () != 1) {
        return false;
    }
    if (*shutdown_lifecycle_result
        || shutdown_lifecycle_result->error_kind () != framework_error_kind_t::shutting_down
        || shutdown_lifecycle_failure_calls.load (std::memory_order_acquire) != 1
        || node->actor_transfer_coordinator->phase (shutdown_lifecycle_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (shutdown_lifecycle_transfer_id,
                                                shutdown_lifecycle_actor, target->spot_id)) {
        return false;
    }
    target->serial_queue->drain ();
    target->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *target->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());

    // An admitted lifecycle failure still owns a Join completion terminal.
    // The source OperationId must observe exactly one failed completion before
    // the target transfer enters reconciliation.
    std::atomic_int failed_join_completion_calls{0};
    node->actor_factories.at ("player").on_join_completed =
      [&failed_join_completion_calls] (void *, actor_join_completion_outcome_t outcome,
                                       std::uint64_t operation_high, std::uint64_t operation_low,
                                       const actor_ref_t *, const std::optional<message_t> &,
                                       framework_error_kind_t error_kind, bool) -> task_t<void> {
        if (outcome != actor_join_completion_outcome_t::failed || operation_high != 43
            || operation_low != 47 || error_kind != framework_error_kind_t::internal_failure) {
            throw std::runtime_error (
              "lifecycle failure completion did not preserve its OperationId");
        }
        ++failed_join_completion_calls;
        co_return;
    };
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [] (void *, void *) -> task_t<void> {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "deterministic lifecycle failure");
        co_return;
    };
    const auto failed_actor =
      actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player", "actor-c5", 7);
    const std::string failed_transfer_id = "transfer-c5";
    const std::string failed_key = "player:actor-c5";
    set_source_authority (failed_actor, 53);
    const auto failed_admitted =
      owner
        .admit_remote_actor_to_spot (
          failed_transfer_id, failed_actor, spot_id_t ("source-spot"), target->spot_id,
          zlink::message_t::from (std::string ("prepare")), 43, 47, 53, 1, 29)
        .result ();
    const auto failed_prepared = owner.prepare_remote_actor_to_spot (
      failed_transfer_id, failed_actor, target->spot_id, zlink::message_t{},
      gateway.actor_context (failed_actor), true);
    if (!failed_admitted || !failed_admitted.value ().accepted || !failed_prepared) {
        return false;
    }
    std::mutex failed_mutex;
    std::condition_variable failed_changed;
    std::optional<result_t<actor_join_reply_t>> failed_result;
    owner.finalize_remote_actor_to_spot_async (
      failed_transfer_id, failed_actor, target->spot_id, provider, &gateway,
      std::chrono::steady_clock::now () + std::chrono::seconds (1),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (failed_mutex);
              failed_result.emplace (std::move (result));
          }
          failed_changed.notify_all ();
      });
    // Waits for the finalize callback and drains before the locals above are destroyed.
    scope_exit_t drain_failed ([&] {
        {
            std::unique_lock lock (failed_mutex);
            failed_changed.wait (lock, [&] { return failed_result.has_value (); });
        }
        target->serial_queue->drain ();
    });
    {
        std::unique_lock lock (failed_mutex);
        if (!failed_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return failed_result.has_value (); })) {
            return false;
        }
    }
    if (*failed_result || failed_result->error_kind () != framework_error_kind_t::internal_failure
        || failed_join_completion_calls.load (std::memory_order_acquire) != 1
        || node->actor_transfer_coordinator->phase (failed_key)
             != std::make_optional (actor_move_phase_t::reconcile)
        || owner.completed_remote_actor_commit (failed_transfer_id, failed_actor,
                                                target->spot_id)) {
        return false;
    }

    // The target completes Join after OnJoined even while the one-way source
    // leave submission has no transport terminal.
    std::atomic_int cutover_order{0};
    std::atomic_int target_joined_order{0};
    std::atomic_int source_leave_submit_order{0};
    std::atomic_int target_accepted_order{0};
    node->actor_factories.at ("player").on_join_completed =
      [&cutover_order, &target_accepted_order] (
        void *, actor_join_completion_outcome_t outcome, std::uint64_t operation_high,
        std::uint64_t operation_low, const actor_ref_t *, const std::optional<message_t> &,
        framework_error_kind_t, bool) -> task_t<void> {
        if (outcome != actor_join_completion_outcome_t::accepted || operation_high != 83
            || operation_low != 89) {
            throw std::runtime_error ("source leave ordering lost the target OperationId");
        }
        target_accepted_order.store (++cutover_order, std::memory_order_release);
        co_return;
    };
    target->actor_admissions.at (std::type_index (typeid (int))).on_actor_joined =
      [&cutover_order, &target_joined_order] (void *, void *) -> task_t<void> {
        target_joined_order.store (++cutover_order, std::memory_order_release);
        co_return;
    };
    const auto leave_order_actor =
      actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player", "actor-c6", 7);
    const std::string leave_order_transfer_id = "transfer-c6";
    set_source_authority (leave_order_actor, 97);
    const auto leave_order_admitted =
      owner
        .admit_remote_actor_to_spot (
          leave_order_transfer_id, leave_order_actor, spot_id_t ("source-spot"), target->spot_id,
          zlink::message_t::from (std::string ("prepare")), 83, 89, 97, 1, 29)
        .result ();
    const auto leave_order_prepared = owner.prepare_remote_actor_to_spot (
      leave_order_transfer_id, leave_order_actor, target->spot_id, zlink::message_t{},
      gateway.actor_context (leave_order_actor), true);
    if (!leave_order_admitted || !leave_order_admitted.value ().accepted || !leave_order_prepared) {
        return false;
    }
    std::mutex leave_order_mutex;
    std::condition_variable leave_order_changed;
    std::optional<result_t<actor_join_reply_t>> leave_order_result;
    task_completion_source_t<void> leave_submit_terminal;
    owner.finalize_remote_actor_to_spot_async (
      leave_order_transfer_id, leave_order_actor, target->spot_id, provider, &gateway,
      std::chrono::steady_clock::now () + std::chrono::seconds (1),
      [&] (result_t<actor_join_reply_t> result) {
          {
              std::lock_guard lock (leave_order_mutex);
              leave_order_result.emplace (std::move (result));
          }
          leave_order_changed.notify_all ();
      },
      [&cutover_order, &source_leave_submit_order, &leave_submit_terminal] {
          source_leave_submit_order.store (++cutover_order, std::memory_order_release);
          return leave_submit_terminal.task ();
      });
    // Settles the source leave submission, waits for the finalize callback and drains before the
    // locals above are destroyed.
    scope_exit_t drain_leave_order ([&] {
        (void) leave_submit_terminal.complete (result_t<void>::failure (
          framework_error_kind_t::internal_failure, "deterministic source leave submit failure"));
        {
            std::unique_lock lock (leave_order_mutex);
            leave_order_changed.wait (lock, [&] { return leave_order_result.has_value (); });
        }
        target->serial_queue->drain ();
    });
    {
        std::unique_lock lock (leave_order_mutex);
        if (!leave_order_changed.wait_for (lock, std::chrono::seconds (1),
                                           [&] { return leave_order_result.has_value (); })) {
            return false;
        }
    }
    if (!*leave_order_result || target_joined_order.load (std::memory_order_acquire) != 1
        || source_leave_submit_order.load (std::memory_order_acquire) != 2
        || target_accepted_order.load (std::memory_order_acquire) != 3) {
        return false;
    }
    leave_submit_terminal.complete (result_t<void>::failure (
      framework_error_kind_t::internal_failure, "deterministic source leave submit failure"));

    // The actual Actor FIFO owns an accepted Join's terminal. Unrelated
    // target Spot lifecycle and application work progress before it settles.
    const auto held_actor =
      actor_ref_access_t::make (node_rid_t::from_string ("source-node"), "player", "actor-c7", 7);
    const std::string held_transfer_id = "transfer-c7";
    set_source_authority (held_actor, 101);
    const auto lifecycle_before =
      target->serial_queue->pending_count (runtime::serial_work_lane_t::lifecycle);
    const auto held_admitted =
      owner
        .admit_remote_actor_to_spot (
          held_transfer_id, held_actor, spot_id_t ("source-spot"), target->spot_id,
          zlink::message_t::from (std::string ("prepare")), 91, 93, 101, 1, 29, true, 1, 1)
        .result ();
    if (!held_admitted || !held_admitted.value ().accepted)
        return false;
    std::mutex held_gate;
    std::condition_variable held_changed;
    bool application_progressed = false;
    std::atomic_bool later_lifecycle_ran{false};
    target->serial_queue->post (
      "later-lifecycle-after-accepted-join",
      [&] {
          std::lock_guard lock (held_gate);
          later_lifecycle_ran.store (true);
          held_changed.notify_all ();
      },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
    target->serial_queue->post ("application-during-join-placement", [&] {
        std::lock_guard lock (held_gate);
        application_progressed = true;
        held_changed.notify_all ();
    });
    bool progressed_before_cancel = false;
    {
        std::unique_lock lock (held_gate);
        progressed_before_cancel = held_changed.wait_for (lock, std::chrono::seconds (1), [&] {
            return application_progressed && later_lifecycle_ran.load ();
        });
    }
    node->actor_transfer_coordinator->fail_commit (held_transfer_id, false);
    target->serial_queue->drain ();
    if (!progressed_before_cancel || !application_progressed || !later_lifecycle_ran.load ()
        || target->serial_queue->pending_count (runtime::serial_work_lane_t::lifecycle)
             != lifecycle_before)
        return false;

    target->serial_queue->close ();
    target->serial_queue->drain ();
    target->serial_executor->drain ();
    const auto passed = true;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}
}
