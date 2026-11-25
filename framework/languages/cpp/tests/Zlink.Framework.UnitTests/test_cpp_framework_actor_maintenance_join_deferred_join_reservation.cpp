/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/manual_internal_scheduler.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace deferred_join_reservation_probe
{
using namespace std::chrono_literals;
namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace detail = fw::detail;
constexpr auto packet_name = "deferred-join-reservation-probe";
constexpr auto actor_key = "probe:actor";
constexpr auto first_transfer = "first-remote-join";
constexpr std::size_t replay_worker_count = 2;

std::shared_ptr<rt::serial_application_record_t>
accepted_record (const std::shared_ptr<rt::application_job_queue_t> &jobs, std::uint64_t sequence)
{
    rt::messaging::envelope_header_t envelope;
    envelope.kind = rt::messaging::message_kind_t::command;
    envelope.channel_name = "actor";
    envelope.message_name = packet_name;
    envelope.content_type = "application/json";
    auto parts = fw::detail::encode_actor_packet (envelope, zlink::message_t::from ("{}"));
    const auto routing_id = zlink::routing_id_t::from ("probe-source").to_bytes ();
    rt::protocol::actor_message_header_t header;
    header.operation = {1, sequence};
    header.target = {"actor", 1, routing_id, 1, 1, 1};
    auto record = std::make_shared<rt::serial_application_record_t> ();
    record->payload ().emplace ();
    record->payload ()->sequence = sequence;
    record->payload ()->application_payload_bytes = 2;
    // This isolated fixture injects the accepted authority and journal input.
    record->payload ()->application_record = rt::stateful::accepted_actor_record (
      header, {"probe-owner", 1, routing_id, 1},
      rt::protocol::application_payload_t::from_parts (std::move (parts).take_items ()), false);
    record->admission.jobs = jobs;
    return record;
}

enum class replay_case_t
{
    remote_without_barrier,
    remote_with_barrier,
    local_target,
    rejected_deferred
};

void restore_probe (replay_case_t test_case, bool multiple_packets = false)
{
    const bool with_source_barrier = test_case != replay_case_t::remote_without_barrier;
    const bool local_target = test_case == replay_case_t::local_target;
    const bool rejected_deferred = test_case == replay_case_t::rejected_deferred;
    auto mesh = fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("probe-mesh");
    auto node = mesh->spot_state;
    node->monitoring = fw::test::runtime_failure_monitoring ();
    auto scheduler = std::make_shared<fw::test::manual_internal_scheduler_t> ();
    scheduler->publication_fence (fw::test::state_lane_publication_fence (
      node->lane, *::zlink::framework::runtime::infrastructure_executor ()));
    node->worker_executor = scheduler;
    fw::serializer_registry_t serializers;
    fw::service_collection_t services;
    node->root_services = services.build_provider ();
    mesh->application_jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{
        fw::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    auto native = std::make_shared<rt::host::public_host_runtime_t> (rt::host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "probe-mesh",
                              .node_routing_id =
                                zlink::routing_id_t::from ("probe-source").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .application_jobs = mesh->application_jobs,
               .runtime_failures = node->runtime_failures ()},
      .object_stable_types = {"framework.spot", "probe"}});
    native->start ();
    node->native_node = native;
    auto held = mesh->application_jobs->try_reserve_supply ();
    ASSERT_TRUE (held);
    held->mark_queued ();

    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = fw::node_rid_t::from_string ("probe-source");
    spot->spot_id = fw::spot_id_t ("probe-source-spot");
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime = fw::test::runtime_failure_fixture<detail::channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.configure_instance = [] (void *, const fw::actor_ref_t &, void *) {};
    node->actor_factories.emplace ("probe", std::move (factory));
    node->actor_instances.emplace (actor_key, std::make_shared<int> (1));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 1);
    node->actor_types_by_id.emplace ("actor", "probe");
    const auto actor = detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string ("probe-source"), "probe", "actor", 1);

    struct observation_t
    {
        bool next_reserved;
        std::string error;
        std::uint32_t permits;
        std::string transfer_id;
    };
    std::vector<std::string> order;
    bool sequence_seen = false;
    std::promise<void> sequence_finished;
    auto sequence_done = sequence_finished.get_future ();
    std::shared_ptr<detail::deferred_barrier_t> next_barrier;
    bool handler_seen = false;
    std::vector<std::shared_ptr<detail::serial_turn_t>> replay_turns;
    fw::task_completion_source_t<observation_t> observed;
    auto handler_finished = observed.task ();
    spot->handlers.push_back (fw::spot_handler_descriptor_t{
      fw::spot_handler_kind_t::actor_send, packet_name, "", std::type_index (typeid (int)),
      std::type_index (typeid (void)), std::type_index (typeid (int)),
      std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&, node] (void *, void *, fw::service_provider_t &, fw::serializer_registry_t &,
                 const zlink::message_t &,
                 const fw::spot_inbound_message_t &metadata) -> fw::task_t<zlink::message_t> {
          if (auto turn = detail::capture_current_serial_turn ())
              replay_turns.push_back (std::move (turn));
          if (multiple_packets && metadata.values.contains ("second")) {
              order.push_back ("second");
              co_return zlink::message_t{};
          }
          if (multiple_packets)
              order.push_back ("first");
          try {
              auto next = detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
              observation_t value{
                static_cast<bool> (next), next ? "" : next.error ()->what (),
                mesh->application_jobs->snapshot ().permits_in_use,
                node->actor_transfer_coordinator->transfer_id (actor_key).value_or ("")};
              if (next) {
                  if (multiple_packets) {
                      next_barrier = next.value ();
                      EXPECT_TRUE (detail::defer_current_serial_turn (
                        [&] {
                            order.push_back ("next-barrier");
                            sequence_finished.set_value ();
                            sequence_seen = true;
                            return fw::task_t<void> (fw::result_t<void>::success ());
                        },
                        [&] { next_barrier->cancel (); }, next_barrier));
                  } else if (local_target) {
                      next_barrier = next.value ();
                  } else {
                      next.value ()->cancel ();
                  }
              }
              handler_seen = true;
              observed.complete (fw::result_t<observation_t>::success (std::move (value)));
          }
          catch (...) {
              handler_seen = true;
              observed.complete (detail::current_exception_result<observation_t> ());
          }
          co_return zlink::message_t{};
      });
    node->dispatch.message_flow (fw::message_flow_log_mode_t::detailed);
    const auto observe_flow = [] (const fw::message_flow_event_t &event) {
        std::fprintf (
          stderr, "FLOW packet=%s stage=%s result=%s\n", event.packet_name.value_or ("").c_str (),
          event.detail_stage.value_or ("").c_str (), event.detail_result.value_or ("").c_str ());
    };
    if (!rejected_deferred)
        detail::dispatch_options_access_t::set_observer_for_tests (node->dispatch, observe_flow);
    std::shared_ptr<detail::deferred_barrier_t> source_barrier;
    detail::deferred_barrier_t::async_completion_t finish_barrier;
    bool source_entered = false;
    std::promise<void> barrier_entered;
    auto entered = barrier_entered.get_future ();
    struct cleanup_t
    {
        std::function<void ()> action;
        ~cleanup_t ()
        {
            if (action)
                action ();
        }
    } cleanup{[&] {
        if (next_barrier)
            next_barrier->cancel ();
        if (finish_barrier) {
            auto complete = std::exchange (finish_barrier, {});
            complete (fw::result_t<void>::success ());
        }
        held.reset ();
        mesh->application_jobs->stop ();
        scheduler->retire ();
        native->close ();
        node->native_node.reset ();
        detail::dispatch_options_access_t::set_observer_for_tests (node->dispatch, {});
        (void) node->actor_transfer_coordinator->cancel_move (actor_key);
        node->spot_contexts_by_id.clear ();
        spot->node.reset ();
    }};
    if (with_source_barrier) {
        if (local_target) {
            auto previous = std::make_shared<detail::spot_context_state_t> ();
            previous->node = node;
            previous->spot_id = fw::spot_id_t ("probe-previous-spot");
            previous->serial_executor = node->worker_executor;
            node->spot_contexts_by_id.emplace (previous->spot_id,
                                               detail::spot_context_access_t::create (previous));
            node->actor_spot_ids[actor_key] = previous->spot_id;
        }
        auto reserved = detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
        ASSERT_TRUE (reserved);
        source_barrier = std::move (reserved.value ());
        ASSERT_TRUE (source_barrier->activate_async ([&] (auto complete) {
            finish_barrier = std::move (complete);
            barrier_entered.set_value ();
            source_entered = true;
        }));
        scheduler->run_all ();
        ASSERT_TRUE (source_entered);
        node->lane.run ([&] { node->actor_spot_ids[actor_key] = spot->spot_id; }).get ();
    } else {
        ASSERT_TRUE (
          node->actor_transfer_coordinator->try_reserve_source (actor_key, first_transfer));
    }
    if (!rejected_deferred) {
        ASSERT_TRUE (local_target ? node->actor_transfer_coordinator->try_begin_local (actor_key)
                                  : node->actor_transfer_coordinator->try_begin_source_remote (
                                      actor_key, first_transfer));
    }
    std::optional<fw::task_t<std::optional<zlink::message_t>>> rejected_work;
    if (rejected_deferred) {
        auto initial = std::make_shared<rt::application_job_queue_t::permit_t> (std::move (*held));
        held.reset ();
        rejected_work.emplace (detail::spot_node_runtime_t (node).relay_actor_packet (
          actor, detail::actor_gateway_runtime_t{}.actor_context (actor),
          detail::stream_message_kind_t::send, packet_name, zlink::message_t::from ("{}"),
          *node->root_services, serializers, {}, nullptr,
          [initial] { initial->release_for_handler_entry (); }, {},
          zlink::routing_id_t::from ("probe-source"), {1, 1}, 0, std::nullopt, {}, 0, {},
          accepted_record (mesh->application_jobs, 1)));
        detail::dispatch_options_access_t::set_observer_for_tests (node->dispatch, observe_flow);
    } else {
        held.reset ();
        ASSERT_EQ (
          detail::handoff_append_result_t::appended,
          node->actor_transfer_coordinator->try_append_backlog (
            actor_key, detail::handoff_packet_t{packet_name,
                                                {},
                                                "application/json",
                                                {},
                                                false,
                                                accepted_record (mesh->application_jobs, 1)}));
    }

    if (multiple_packets) {
        ASSERT_EQ (
          detail::handoff_append_result_t::appended,
          node->actor_transfer_coordinator->try_append_backlog (
            actor_key, detail::handoff_packet_t{packet_name,
                                                {},
                                                "application/json",
                                                {{"second", "true"}},
                                                false,
                                                accepted_record (mesh->application_jobs, 2)}));
        auto queue = spot->ensure_spot_serial_executor ()->actor_executor (actor_key)->queue ();
        ASSERT_TRUE (queue->try_post ("ordinary-suffix", [&] { order.push_back ("ordinary"); }));
    }

    if (rejected_deferred) {
        auto complete = std::exchange (finish_barrier, {});
        complete (fw::result_t<void>::success ());
    } else {
        detail::spot_node_runtime_t (node).fail_remote_actor_transfer (actor, false, std::nullopt);
    }
    scheduler->run_all ();
    if (local_target) {
        scheduler->run_until (handler_finished);
        scheduler->run_all ();
        EXPECT_TRUE (handler_seen);
    }
    if (finish_barrier) {
        auto complete = std::exchange (finish_barrier, {});
        complete (fw::result_t<void>::success ());
    }
    if (source_barrier) {
        // The previous Spot's barrier settles on another lane. An empty manual
        // queue does not establish that its final continuation has been published.
        auto terminal = source_barrier->terminal ();
        scheduler->run_until (terminal);
        ASSERT_TRUE (terminal.result ());
    }
    scheduler->run_all ();
    scheduler->run_until (handler_finished);
    scheduler->run_all ();
    const auto handler_before_cleanup = handler_seen;
    EXPECT_TRUE (handler_before_cleanup);
    if (!handler_before_cleanup) {
        // A bad nested post can leave an accepted replay holding its own
        // execution claim. Release those fixture-owned turns for cleanup.
        for (std::size_t index = 0; index < replay_turns.size (); ++index) {
            auto turn = replay_turns[index];
            (void) turn->release ();
            scheduler->run_all ();
        }
    }
    ASSERT_TRUE (handler_seen);
    const auto &value = handler_finished.result ().value ();
    if (multiple_packets && value.next_reserved) {
        scheduler->run_all ();
        EXPECT_TRUE (sequence_seen);
        auto terminal = next_barrier->terminal ();
        EXPECT_TRUE (terminal.await_ready ());
        if (terminal.await_ready ())
            EXPECT_TRUE (terminal.result ());
        auto completed = node->actor_transfer_coordinator->wait_for_source_completion (actor_key);
        EXPECT_TRUE (completed.await_ready ());
        if (completed.await_ready ())
            completed.result ().value ();
        EXPECT_EQ (spot->ensure_spot_serial_executor ()
                     ->actor_executor (actor_key)
                     ->queue ()
                     ->pending_count (),
                   0u);
    }
    if (local_target && value.next_reserved) {
        EXPECT_FALSE (value.transfer_id.empty ());
        EXPECT_EQ (value.transfer_id, node->actor_transfer_coordinator->transfer_id (actor_key));
        EXPECT_EQ (detail::actor_move_phase_t::source_reserved,
                   node->actor_transfer_coordinator->phase (actor_key));
        next_barrier->cancel ();
        next_barrier.reset ();
    }
    scheduler->run_all ();
    EXPECT_EQ (spot->ensure_spot_serial_executor ()->spot_queue ()->pending_count (), 0u);
    EXPECT_EQ (
      spot->ensure_spot_serial_executor ()->actor_executor (actor_key)->queue ()->pending_count (),
      0u);
    std::fprintf (stdout,
                  "prefix-terminal handler_before_cleanup=%d sequence=%d replay_turns=%zu\n",
                  handler_before_cleanup, sequence_seen, replay_turns.size ());
    const auto phase_after_restore = node->actor_transfer_coordinator->phase (actor_key);
    auto control = detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
    const bool after_restore_reserved = static_cast<bool> (control);
    if (control)
        control.value ()->cancel ();
    cleanup.action ();
    cleanup.action = {};
    const auto remaining = mesh->application_jobs->snapshot ().permits_in_use;
    std::fprintf (stderr,
                  "OBSERVATION source_barrier=%d local_target=%d rejected_deferred=%d "
                  "next_reserved=%d error=%s "
                  "permits_during_handler=%u "
                  "after_restore_reserved=%d final_permits=%u\n",
                  with_source_barrier, local_target, rejected_deferred, value.next_reserved,
                  value.error.c_str (), value.permits, after_restore_reserved, remaining);
    EXPECT_FALSE (phase_after_restore.has_value ());
    EXPECT_TRUE (after_restore_reserved);
    EXPECT_EQ (0u, value.permits);
    EXPECT_EQ (0u, remaining);
    EXPECT_TRUE (value.next_reserved) << value.error;
    if (multiple_packets)
        EXPECT_EQ ((std::vector<std::string>{"first", "next-barrier", "second", "ordinary"}),
                   order);
}
TEST (DeferredJoinReservation, RemoteRestoreWithoutSourceBarrier)
{
    restore_probe (replay_case_t::remote_without_barrier);
}
TEST (DeferredJoinReservation, RemoteRestoreWithSourceBarrier)
{
    restore_probe (replay_case_t::remote_with_barrier);
}
TEST (DeferredJoinReservation, LocalTargetWithPreviousSpotBarrier)
{
    restore_probe (replay_case_t::local_target);
}
TEST (DeferredJoinReservation, PrefixHandlersKeepIndependentJoinBoundary)
{
    restore_probe (replay_case_t::remote_with_barrier, true);
}
TEST (DeferredJoinReservation, RejectedDeferredBarrierReleasesBeforeReservation)
{
    restore_probe (replay_case_t::rejected_deferred);
}
void capacity_probe (bool handler_failure = false, bool extra_supply = false)
{
    auto mesh = fw::test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("probe-mesh");
    auto node = mesh->spot_state;
    node->monitoring = fw::test::runtime_failure_monitoring ();
    auto scheduler = std::make_shared<fw::test::manual_internal_scheduler_t> ();
    node->worker_executor = scheduler;
    scheduler->publication_fence (fw::test::state_lane_publication_fence (
      node->lane, *::zlink::framework::runtime::infrastructure_executor ()));
    fw::serializer_registry_t serializers;
    fw::service_collection_t services;
    node->root_services = services.build_provider ();
    mesh->application_jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{
        fw::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    auto native = std::make_shared<rt::host::public_host_runtime_t> (rt::host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "probe-mesh",
                              .node_routing_id =
                                zlink::routing_id_t::from ("probe-source").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .application_jobs = mesh->application_jobs,
               .runtime_failures = node->runtime_failures ()},
      .object_stable_types = {"framework.spot", "probe"}});
    native->start ();
    node->native_node = native;
    auto held = mesh->application_jobs->try_reserve_supply ();
    ASSERT_TRUE (held);
    held->mark_queued ();

    auto spot = std::make_shared<detail::spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = fw::node_rid_t::from_string ("probe-source");
    spot->spot_id = fw::spot_id_t ("probe-source-spot");
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime = fw::test::runtime_failure_fixture<detail::channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, detail::spot_context_access_t::create (spot));
    detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.configure_instance = [] (void *, const fw::actor_ref_t &, void *) {};
    node->actor_factories.emplace ("probe", std::move (factory));
    node->actor_instances.emplace (actor_key, std::make_shared<int> (1));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 1);
    node->actor_types_by_id.emplace ("actor", "probe");
    const auto actor = detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string ("probe-source"), "probe", "actor", 1);

    std::atomic<bool> first_started{false}, second_started{false};
    fw::task_completion_source_t<void> release_first;
    int join_activated = 0, join_cancelled = 0;
    std::shared_ptr<detail::deferred_barrier_t> failed_handler_join;
    std::mutex order_mutex;
    std::vector<std::string> order;
    std::shared_ptr<rt::application_job_queue_t::permit_t> ordinary_permit;
    const auto until = [&] (auto condition) {
        node->lane.run ([] {}).get ();
        scheduler->run_all ();
        scheduler->run_until (condition);
        return condition ();
    };
    spot->handlers.push_back (fw::spot_handler_descriptor_t{
      fw::spot_handler_kind_t::actor_send, packet_name, "", std::type_index (typeid (int)),
      std::type_index (typeid (void)), std::type_index (typeid (int)),
      std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&] (void *, void *, fw::service_provider_t &, fw::serializer_registry_t &,
           const zlink::message_t &,
           const fw::spot_inbound_message_t &metadata) -> fw::task_t<zlink::message_t> {
          const auto name = metadata.values.contains ("ordinary") ? "ordinary"
                            : metadata.values.contains ("second") ? "second"
                                                                  : "first";
          {
              std::lock_guard lock (order_mutex);
              order.push_back (name);
          }
          if (name == std::string_view ("first")) {
              if (handler_failure) {
                  auto reserved =
                    detail::spot_node_runtime_t (node).reserve_actor_join_barrier (actor);
                  if (!reserved)
                      throw fw::framework_exception_t (reserved.error_kind (),
                                                       "test Join reservation failed");
                  failed_handler_join = reserved.value ();
                  const auto registered = detail::defer_current_serial_turn (
                    [&] {
                        ++join_activated;
                        return fw::task_t<void> (fw::result_t<void>::success ());
                    },
                    [&] {
                        ++join_cancelled;
                        failed_handler_join->cancel ();
                    },
                    failed_handler_join);
                  if (!registered)
                      throw fw::framework_exception_t (registered.error_kind (),
                                                       "test Join registration failed");
              }
              first_started.store (true);
              co_await release_first.task ();
              if (handler_failure)
                  throw fw::framework_exception_t (fw::framework_error_kind_t::internal_failure,
                                                   "deterministic handler failure");
          } else if (name == std::string_view ("second")) {
              second_started.store (true);
          }
          co_return zlink::message_t{};
      });
    struct cleanup_t
    {
        std::function<void ()> action;
        ~cleanup_t ()
        {
            if (action)
                action ();
        }
    } cleanup{[&] {
        held.reset ();
        ordinary_permit.reset ();
        release_first.complete (fw::result_t<void>::success ());
        mesh->application_jobs->stop ();
        scheduler->run_all ();
        node->worker_executor->request_stop ();
        node->worker_executor->drain ();
        native->close ();
        node->native_node.reset ();
        (void) node->actor_transfer_coordinator->cancel_move (actor_key);
        node->spot_contexts_by_id.clear ();
        spot->node.reset ();
    }};
    if (handler_failure) {
        auto initial = std::make_shared<rt::application_job_queue_t::permit_t> (std::move (*held));
        held.reset ();
        auto record = accepted_record (mesh->application_jobs, 1);
        record->admission.retain_initial ([initial] { initial->release_for_handler_entry (); });
        auto failed = detail::spot_node_runtime_t (node).relay_actor_packet (
          actor, detail::actor_gateway_runtime_t{}.actor_context (actor),
          detail::stream_message_kind_t::send, packet_name, zlink::message_t::from ("{}"),
          *node->root_services, serializers, {}, nullptr, {}, {},
          zlink::routing_id_t::from ("probe-source"), {1, 1}, 0, std::nullopt, {}, 0, {}, record);
        scheduler->run_all ();
        const auto entered = first_started.load ();
        release_first.complete (fw::result_t<void>::success ());
        scheduler->run_until (failed);
        const auto activated = join_activated;
        const auto cancelled = join_cancelled;
        const auto failed_ready = failed.await_ready ();
        if (failed_handler_join)
            failed_handler_join->cancel ();
        scheduler->run_all ();
        EXPECT_TRUE (entered);
        EXPECT_EQ (activated, 0);
        EXPECT_EQ (cancelled, 1);
        EXPECT_TRUE (failed_ready);
        if (failed_ready) {
            EXPECT_FALSE (failed.result ());
            EXPECT_EQ (failed.result ().error_kind (),
                       fw::framework_error_kind_t::internal_failure);
        }
        EXPECT_EQ (mesh->application_jobs->snapshot ().permits_in_use, 0u);
        EXPECT_EQ (spot->ensure_spot_serial_executor ()->spot_queue ()->pending_count (), 0u);
        EXPECT_EQ (spot->ensure_spot_serial_executor ()
                     ->actor_executor (actor_key)
                     ->queue ()
                     ->pending_count (),
                   0u);
        std::fprintf (stdout,
                      "handler-task-failure entered=%d activated=%d cancelled=%d ready=%d\n",
                      entered, activated, cancelled, failed_ready);
        return;
    }
    ASSERT_TRUE (node->actor_transfer_coordinator->try_reserve_source (actor_key, first_transfer));
    ASSERT_TRUE (
      node->actor_transfer_coordinator->try_begin_source_remote (actor_key, first_transfer));
    for (const auto &metadata : {std::map<std::string, std::string>{},
                                 std::map<std::string, std::string>{{"second", "true"}}})
        ASSERT_EQ (
          detail::handoff_append_result_t::appended,
          node->actor_transfer_coordinator->try_append_backlog (
            actor_key, detail::handoff_packet_t{
                         packet_name,
                         {},
                         "application/json",
                         metadata,
                         false,
                         accepted_record (mesh->application_jobs, metadata.empty () ? 1 : 2)}));
    detail::spot_node_runtime_t (node).fail_remote_actor_transfer (actor, false, std::nullopt);
    ASSERT_TRUE (until ([&] {
        return first_started.load () || mesh->application_jobs->snapshot ().capacity_waiters == 1;
    }));
    // The held initial reservation must prevent the first replay callback.
    EXPECT_FALSE (first_started.load ());
    EXPECT_EQ (mesh->application_jobs->snapshot ().capacity_waiters, 1u);
    if (first_started.load ())
        return;
    held.reset ();
    ASSERT_TRUE (until ([&] { return first_started.load (); }));
    auto supply = mesh->application_jobs->try_reserve_supply ();
    ASSERT_TRUE (supply);
    ordinary_permit = std::make_shared<rt::application_job_queue_t::permit_t> (std::move (*supply));
    ordinary_permit->mark_queued ();
    auto ordinary = detail::spot_node_runtime_t (node).relay_actor_packet (
      actor, detail::actor_gateway_runtime_t{}.actor_context (actor),
      detail::stream_message_kind_t::send, packet_name, zlink::message_t::from ("{}"),
      *node->root_services, serializers,
      fw::spot_inbound_message_t{.values = {{"ordinary", "true"}}}, nullptr,
      [ordinary_permit] { ordinary_permit->release_for_handler_entry (); }, {},
      zlink::routing_id_t::from ("probe-source"), {1, 3}, 0, std::nullopt, {}, 0, {},
      accepted_record (mesh->application_jobs, 3));
    ASSERT_TRUE (until ([&] { return mesh->application_jobs->snapshot ().permits_in_use == 0; }));
    held = mesh->application_jobs->try_reserve_supply ();
    ASSERT_TRUE (held);
    held->mark_queued ();
    release_first.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (until ([&] {
        return second_started.load () || mesh->application_jobs->snapshot ().capacity_waiters == 1;
    }));
    EXPECT_FALSE (second_started.load ());
    EXPECT_EQ (mesh->application_jobs->snapshot ().capacity_waiters, 1u);
    bool lifecycle_ran = false;
    rt::serial_work_options_t lifecycle{rt::serial_work_lane_t::lifecycle};
    ASSERT_TRUE (spot->ensure_spot_serial_executor ()->spot_queue ()->try_post (
      "permit-wait-lifecycle", [&] { lifecycle_ran = true; }, lifecycle));
    scheduler->run_all ();
    ASSERT_TRUE (lifecycle_ran);
    held.reset ();
    scheduler->run_all ();
    ASSERT_TRUE (ordinary.result ());
    spot->ensure_spot_serial_executor ()->actor_executor (actor_key)->queue ()->drain ();
    if (extra_supply) {
        auto extra = mesh->application_jobs->try_reserve_supply ();
        ASSERT_TRUE (extra);
        extra.reset ();
    }
    const auto final = mesh->application_jobs->snapshot ();
    EXPECT_EQ (final.peak_permits_in_use, 1u);
    EXPECT_EQ (final.permits_in_use, 0u);
    EXPECT_EQ (final.capacity_waiters, 0u);
    EXPECT_EQ (final.capacity_wait_count, 2u);
    // At capacity one with no cancellations, a supply acquisition either
    // starts a pause or fulfills a capacity wait without a state transition.
    const auto supply_acquisitions =
      mesh->application_jobs->pressure_metrics_snapshot ().paused_transition_count
      + final.capacity_wait_count;
    EXPECT_EQ (supply_acquisitions, 6u);
    std::lock_guard lock (order_mutex);
    EXPECT_EQ (order, (std::vector<std::string>{"first", "second", "ordinary"}));
}
TEST (DeferredJoinReservation, FailedHandlerTaskDiscardsDeferredJoin)
{
    capacity_probe (true);
}

TEST (DeferredJoinReservation, DurablePrefixTakesLateIngressPermitAtCapacityOne)
{
    capacity_probe ();
}

TEST (DeferredJoinReservation, DurablePrefixRejectsExtraSupplyAcquisition)
{
    EXPECT_NONFATAL_FAILURE (capacity_probe (false, true), "supply_acquisitions");
}


} // namespace deferred_join_reservation_probe
