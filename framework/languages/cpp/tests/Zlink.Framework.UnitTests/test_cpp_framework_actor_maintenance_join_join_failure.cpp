/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_native_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/scope_exit.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>

namespace zlink::framework::test::actor_maintenance_join
{
constexpr auto factory_failed_spot = "factory-failed";

class ActorDeferredJoinFailure : public testing::TestWithParam<std::tuple<bool, const char *>>
{
};
TEST_P (ActorDeferredJoinFailure, FailureAndRejectionPreserveSourceActor)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.completion_to_hold = 1;
    evidence.fail_remote_factory =
      std::string_view (std::get<1> (GetParam ())) == factory_failed_spot;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t first, second;
    release_callbacks_t release{evidence};
    configure (first, evidence_owner, first_rid, store, relocations);
    fw::detail::dispatch_options_access_t::set_observer_for_tests (
      first.app.add_zlink_framework ().configure_dispatch (),
      [owner = evidence_owner] (const fw::message_flow_event_t &event) {
          if (event.packet_name != probe_t::packet_name)
              return;
          if (event.actor_id != actor_id || event.outcome != fw::message_flow_outcome_t::admitted
              || event.detail_stage != "canonical_application.commit_accepted_ingress")
              return;
          {
              std::lock_guard lock (owner->mutex);
              owner->probe_admission_observed = true;
          }
          owner->changed.notify_all ();
      });
    first.start ();
    ASSERT_TRUE (ready (first));
    auto services = first.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    configure (second, evidence_owner, second_rid, store, relocations);
    second.start ();
    ASSERT_TRUE (ready (second));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto second_services = second.app.advanced ().services ().build_provider ();
    const auto [remote, target] = GetParam ();
    auto &target_services = remote ? second_services : services;
    auto &other_services = remote ? services : second_services;
    ASSERT_TRUE (set_published_placement_weight (other_services, 0, &target_services));
    auto &spots = target_services.get_required<fw::spot_manager_t> ();
    ASSERT_TRUE (spots.get_or_create (fw::spot_id_t (target), room_type)
                   .timeout (wait_budget)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    std::size_t initial;
    {
        std::lock_guard lock (evidence.mutex);
        initial = evidence.completions.size ();
    }
    ASSERT_TRUE (move_to (client, evidence, target, initial + 1));
    {
        std::lock_guard lock (evidence.mutex);
        EXPECT_EQ (evidence.completion_owners.back (), first_rid);
        if (evidence.fail_remote_factory)
            EXPECT_GT (evidence.remote_factory_failure_count, 0);
        if (std::string_view (target) == rejected_spot) {
            const auto *rejected =
              std::get_if<fw::actor_join_rejected_t> (&evidence.completions.back ());
            ASSERT_NE (rejected, nullptr);
            ASSERT_TRUE (rejected->reply);
            EXPECT_EQ (rejected->reply->decode<std::string> (), rejection_reply);
        } else {
            const auto *failed =
              std::get_if<fw::actor_join_failed_t> (&evidence.completions.back ());
            ASSERT_NE (failed, nullptr)
              << "completion variant=" << evidence.completions.back ().index ();
            EXPECT_EQ (failed->error_kind, std::string_view (target) == framework_failed_spot
                                             ? fw::framework_error_kind_t::unavailable
                                             : fw::framework_error_kind_t::internal_failure);
        }
    }
    // Keep completion's barrier active until the public probe reaches source
    // admission. Existing Detailed flow events fix the order without a sleep.
    auto pending_probe = client.request (fw::actor_id_t (actor_id), probe_t{17})
                           .timeout (wait_budget)
                           .async<probe_t> ();
    bool reached_admission;
    {
        std::unique_lock lock (evidence.mutex);
        reached_admission = evidence.changed.wait_for (
          lock, wait_budget, [&] { return evidence.probe_admission_observed; });
    }
    evidence.release_completion.complete (fw::result_t<void>::success ());
    ASSERT_TRUE (reached_admission);
    auto probe = pending_probe.result ();
    ASSERT_TRUE (probe) << "kind=" << static_cast<int> (probe.error_kind ())
                        << " message=" << (probe.error () ? probe.error ()->what () : "");
    EXPECT_EQ (probe.value ().value, 17);
}

INSTANTIATE_TEST_SUITE_P (LocalAndRemote,
                          ActorDeferredJoinFailure,
                          testing::Values (std::make_tuple (false, failed_spot),
                                           std::make_tuple (false, framework_failed_spot),
                                           std::make_tuple (false, rejected_spot),
                                           std::make_tuple (true, failed_spot),
                                           std::make_tuple (true, framework_failed_spot),
                                           std::make_tuple (true, rejected_spot),
                                           std::make_tuple (true, factory_failed_spot)));

TEST (ActorMaintenanceJoin, NotifyFailureDoesNotChangeSuccessfulJoinCompletion)
{
    native_intake_fixture_t fixture;
    const auto native = fixture.host;
    const auto actor = native->create_actor (actor_type, actor_id).ref ();
    auto node =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
    node->native_node = native;
    node->actor_instances.emplace (std::string (actor_type) + ":" + actor_id,
                                   std::make_shared<int> (0));
    fw::detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    node->actor_factories.emplace (actor_type, std::move (factory));
    std::promise<std::exception_ptr> reported;
    auto failure = reported.get_future ();
    node->runtime_failures ()->bind_runtime_reporter (
      [&reported] (std::exception_ptr error) { reported.set_value (std::move (error)); });
    native->transport ().mailbox ().bind_application_dispatch (
      {}, {}, [] (const auto &, auto) -> fw::task_t<bool> {
          throw std::runtime_error ("notify failed");
          co_return false;
      });
    fw::detail::spot_node_runtime_t runtime (node);
    const auto result =
      runtime
        .deliver_actor_join_completion (actor, fw::actor_join_accepted_t{1, 2, actor, std::nullopt},
                                        std::nullopt)
        .result ()
        .value ();
    native->close ();
    ASSERT_TRUE (result) << (result.error () ? result.error ()->what () : "");
    const auto error = failure.get ();
    ASSERT_TRUE (error);
    try {
        std::rethrow_exception (error);
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ (error.what (), "notify failed");
    }
}

#ifdef ZLINK_FRAMEWORK_TEST_CREATION_PROBE
TEST (ActorMaintenanceJoin, SpotDrainWaitsForNativeReadyAfterJoinCompletion)
{
    native_intake_fixture_t fixture;
    const auto native = fixture.host;
    const auto actor = native->create_actor (actor_type, actor_id).ref ();
    const auto worker = std::make_shared<fw::runtime::offload_executor_t> (1, "join-ready");
    auto node =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
    node->native_node = native;
    node->worker_executor = worker;
    auto spot = std::make_shared<fw::detail::spot_context_state_t> ();
    spot->lane_owner = node;
    spot->spot_id = fw::spot_id_t ("join-ready-spot");
    ASSERT_TRUE (spot->ensure_spot_serial_executor ());
    node->spot_contexts_by_id.emplace (spot->spot_id,
                                       fw::detail::spot_context_access_t::create (spot));
    node->actor_instances.emplace (std::string (actor_type) + ":" + actor_id,
                                   std::make_shared<int> (0));
    node->actor_factories.try_emplace (actor_type);
    fw::detail::spot_node_runtime_t runtime (node);

    struct ready_work_t
    {
        std::promise<std::string> entered;
        fw::task_completion_source_t<void> release;
        std::promise<void> finished;
    };
    const auto ready = std::make_shared<ready_work_t> ();
    auto ready_entered = ready->entered.get_future ();
    const auto ready_finished = ready->finished.get_future ().share ();
    std::promise<void> drain_progress;
    auto progressed = drain_progress.get_future ();
    std::once_flag progress_recorded;
    const auto record_progress = [&] {
        std::call_once (progress_recorded, [&] { drain_progress.set_value (); });
    };
    node->join_delivery_drain_wait_probe = record_progress;
    std::jthread drain_thread;
    scope_exit_t release_ready ([&] { ready->release.complete (fw::result_t<void>::success ()); });
    native->transport ().mailbox ().bind_application_dispatch (
      {}, {}, [ready] (const std::string &owner, auto) {
          // The mailbox can destroy its callback copy after this call returns.
          return
            [] (std::shared_ptr<ready_work_t> work, std::string actor_key) -> fw::task_t<bool> {
                work->entered.set_value (std::move (actor_key));
                co_await work->release.task ();
                work->finished.set_value ();
                co_return true;
            }(ready, owner);
      });

    auto delivery = runtime.deliver_actor_join_completion (
      actor, fw::actor_join_accepted_t{1, 2, actor, std::nullopt}, std::nullopt);
    ASSERT_EQ (ready_entered.wait_for (wait_budget), std::future_status::ready);
    ASSERT_EQ (ready_entered.get (), actor_id);
    std::packaged_task<bool ()> drain ([&] {
        const auto drain_deadline = std::chrono::steady_clock::now () + 2 * wait_budget;
        fw::detail::drain_spot_node_executors (*node, drain_deadline);
        const bool ready_at_return =
          ready_finished.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready;
        record_progress ();
        return ready_at_return;
    });
    auto drained = drain.get_future ();
    drain_thread = std::jthread (std::move (drain));
    ASSERT_EQ (progressed.wait_for (wait_budget), std::future_status::ready);

    const auto joined = delivery.result_for (wait_budget);
    EXPECT_TRUE (joined && *joined && joined->value ())
      << "Join did not publish while ready waited";
    EXPECT_EQ (ready_finished.wait_for (std::chrono::milliseconds::zero ()),
               std::future_status::timeout);
    release_ready.run ();
    ASSERT_EQ (ready_finished.wait_for (wait_budget), std::future_status::ready);
    ASSERT_EQ (drained.wait_for (wait_budget), std::future_status::ready);
    drain_thread.join ();

    const bool ready_finished_at_drain_return = drained.get ();
    const auto events = ready_finished_at_drain_return
                          ? "ready_entered -> drain_waiting -> ready_finished -> "
                            "drain_returned"
                          : "ready_entered -> drain_returned -> "
                            "ready_finished";
    RecordProperty ("events", events);
    EXPECT_TRUE (ready_finished_at_drain_return) << events;
}

#endif

namespace
{
struct hosted_ready_fixture_t
{
    std::shared_ptr<fw::runtime::offload_executor_t> worker =
      std::make_shared<fw::runtime::offload_executor_t> (1, "hosted-ready");
    std::shared_ptr<fw::detail::spot_node_builder_state_t> node =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (mesh_name);
    fw::detail::spot_node_runtime_t runtime{node};

    hosted_ready_fixture_t ()
    {
        node->worker_executor = worker;
        auto spot = std::make_shared<fw::detail::spot_context_state_t> ();
        spot->lane_owner = node;
        spot->spot_id = fw::spot_id_t ("hosted-ready-spot");
        node->spot_contexts_by_id.emplace (spot->spot_id,
                                           fw::detail::spot_context_access_t::create (spot));
        node->actor_types_by_id.emplace (actor_id, actor_type);
        node->actor_spot_ids.emplace (std::string (actor_type) + ":" + actor_id, spot->spot_id);
    }
};
} // namespace

TEST (ActorMaintenanceJoin, HostedReadyWaitsForClaim)
{
    namespace st = fw::runtime::stateful;
    native_intake_fixture_t intake;
    hosted_ready_fixture_t fixture;
    const st::object_ref_t object{st::object_kind_t::actor,
                                  actor_id,
                                  1,
                                  1,
                                  mesh_name,
                                  zlink::routing_id_t::from (intake.rid).to_string ()};
    const auto created =
      fw::test::begin_reserved_object (intake.host->objects (), object, actor_type, {});
    ASSERT_EQ (created.error, st::stateful_error_t::none);
    ASSERT_EQ (intake.host->objects ().commit_create (created.attempt), st::stateful_error_t::none);
    const auto raw = std::make_shared<st::raw_stateful_dispatch_t> (
      intake.host->objects (), intake.host->transport (), intake.resolver);
    const auto slot =
      fw::test::publish_stateful_claim (intake.host->transport (), object, intake.actor_record (1));
    const auto accepted =
      raw
        ->ingest_claimed (object, slot,
                          fw::test::retained_ingress_snapshot (intake.host->objects (), object))
        .result ();
    ASSERT_TRUE (accepted);
    ASSERT_EQ (accepted.value (), st::stateful_error_t::none);

    struct claim_work_t
    {
        std::promise<bool> entered;
        std::shared_future<void> release;
        std::promise<bool> finished;
    };
    std::promise<void> release;
    auto claim = std::make_shared<claim_work_t> ();
    claim->release = release.get_future ().share ();
    auto entered = claim->entered.get_future ();
    auto finished = claim->finished.get_future ().share ();
    std::promise<void> drain_started;
    auto started = drain_started.get_future ();
    std::future<bool> drained;
    scope_exit_t release_claim ([&] { release.set_value (); });

    auto ready = fixture.runtime.notify_actor_application_ready (
      actor_id, [claim, raw, object] () -> std::optional<fw::runtime::serial_prefix_work_t> {
          auto [error, delivery] = raw->try_claim (object);
          claim->entered.set_value (error == st::stateful_error_t::none && delivery.has_value ());
          claim->release.wait ();
          claim->finished.set_value (delivery
                                     && raw->complete (*delivery) == st::stateful_error_t::none);
          return std::nullopt;
      });
    ASSERT_EQ (entered.wait_for (wait_budget), std::future_status::ready);
    ASSERT_TRUE (entered.get ());
    EXPECT_FALSE (ready.await_ready ()) << "Hosted ready returned before the claim finished";
    drained = std::async (std::launch::async, [&] {
        drain_started.set_value ();
        fw::detail::drain_spot_node_executors (*fixture.node,
                                               std::chrono::steady_clock::now () + 2 * wait_budget);
        return finished.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready;
    });
    ASSERT_EQ (started.wait_for (wait_budget), std::future_status::ready);
    release_claim.run ();
    const auto routed = ready.result_for (wait_budget);
    ASSERT_TRUE (routed && *routed);
    EXPECT_TRUE (routed->value ());
    ASSERT_EQ (finished.wait_for (wait_budget), std::future_status::ready);
    EXPECT_TRUE (finished.get ());
    ASSERT_EQ (drained.wait_for (wait_budget), std::future_status::ready);
    EXPECT_TRUE (drained.get ()) << "Drain returned before the hosted claim finished";
    EXPECT_EQ (slot.claim->outstanding (), 0u);
}

TEST (ActorMaintenanceJoin, HostedReadyRejectedControlSettlesJoinDrain)
{
    for (const bool retire_worker : {false, true}) {
        SCOPED_TRACE (retire_worker ? "control submission rejected" : "Spot queue closed");
        hosted_ready_fixture_t fixture;
        native_intake_fixture_t intake;
        const auto actor = intake.host->create_actor (actor_type, actor_id).ref ();
        fixture.node->native_node = intake.host;
        fixture.node->actor_instances.emplace (std::string (actor_type) + ":" + actor_id,
                                               std::make_shared<int> (0));
        fixture.node->actor_factories.try_emplace (actor_type);
        ASSERT_TRUE (fixture.runtime.actor_application_queue (actor_id).result ().value ());

        struct ready_state_t
        {
            std::promise<void> arrived;
            fw::task_completion_source_t<void> release;
            std::atomic_bool claimed = false;
            fw::task_completion_source_t<bool> routed;
        };
        const auto ready = std::make_shared<ready_state_t> ();
        auto arrived = ready->arrived.get_future ();
        scope_exit_t release_ready (
          [&] { ready->release.complete (fw::result_t<void>::success ()); });
        intake.host->transport ().mailbox ().bind_application_dispatch (
          {}, {}, [node = fixture.node, ready] (const std::string &owner, auto) {
              return
                [] (fw::detail::spot_node_runtime_t runtime, std::shared_ptr<ready_state_t> ready,
                    std::string owner) -> fw::task_t<bool> {
                    ready->arrived.set_value ();
                    co_await ready->release.task ();
                    auto notified = runtime.notify_actor_application_ready (
                      std::move (owner),
                      [ready] () -> std::optional<fw::runtime::serial_prefix_work_t> {
                          ready->claimed.store (true);
                          return std::nullopt;
                      });
                    fw::detail::observe_task_terminal (notified,
                                                       [ready] (const fw::result_t<bool> &result) {
                                                           ready->routed.complete (result);
                                                       });
                    co_return co_await notified;
                }(fw::detail::spot_node_runtime_t (node), ready, owner);
          });

        auto joined = fixture.runtime.deliver_actor_join_completion (
          actor, fw::actor_join_accepted_t{1, 2, actor, std::nullopt}, std::nullopt);
        ASSERT_EQ (arrived.wait_for (wait_budget), std::future_status::ready);
        const auto result = joined.result_for (wait_budget);
        ASSERT_TRUE (result && *result && result->value ());

        fixture.runtime.request_stop ();
        fixture.runtime.cancel_pending_dispatch ();
        if (retire_worker)
            fixture.worker->drain ();
        release_ready.run ();

        const auto notification = ready->routed.task ().result_for (wait_budget);
        ASSERT_TRUE (notification && *notification);
        EXPECT_TRUE (notification->value ());
        fw::detail::drain_spot_node_executors (*fixture.node,
                                               std::chrono::steady_clock::now () + wait_budget);
        EXPECT_FALSE (ready->claimed.load ());
        EXPECT_NO_THROW (fixture.node->runtime_failures ()->rethrow_if_failed ());
    }
}

TEST (ActorMaintenanceJoin, CompletionCallbackPreservesStandardException)
{
    auto node =
      fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (first_rid);
    fw::detail::spot_node_runtime_t runtime (node);
    const auto actor = fw::detail::actor_ref_access_t::make (
      fw::node_rid_t::from_string (first_rid), actor_type, actor_id, 1);
    node->actor_instances.emplace (std::string (actor_type) + ":" + actor_id,
                                   std::make_shared<int> (0));
    fw::detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    auto release = std::make_shared<fw::task_completion_source_t<void>> ();
    factory.on_join_completed = [release] (void *, auto, auto, auto, const auto *, const auto &,
                                           auto, bool) -> fw::task_t<void> {
        co_await release->task ();
        throw std::runtime_error ("completion callback failed");
    };
    node->actor_factories.emplace (actor_type, std::move (factory));
    auto delivery = runtime.deliver_actor_join_completion (
      actor, fw::actor_join_failed_t{1, 2, fw::framework_error_kind_t::unavailable}, std::nullopt);
    release->complete (fw::result_t<void>::success ());
    const auto result = delivery.result ().value ();
    ASSERT_FALSE (result);
    ASSERT_TRUE (result.exception ());
    try {
        std::rethrow_exception (result.exception ());
        FAIL () << "callback exception was lost";
    }
    catch (const fw::framework_exception_t &) {
        FAIL () << "callback exception was replaced";
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ (error.what (), "completion callback failed");
    }
}
} // namespace zlink::framework::test::actor_maintenance_join
