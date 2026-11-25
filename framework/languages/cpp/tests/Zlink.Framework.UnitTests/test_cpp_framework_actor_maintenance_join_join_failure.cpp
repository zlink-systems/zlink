/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_native_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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
