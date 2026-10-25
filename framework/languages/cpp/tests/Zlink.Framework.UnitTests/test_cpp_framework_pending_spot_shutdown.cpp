/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <typeindex>

namespace
{
namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace detail = fw::detail;
using namespace std::chrono_literals;

constexpr auto observation_timeout = 2s;

[[noreturn]] void fail_child (const char *reason)
{
    std::cerr << "[pending-spot-shutdown] " << reason << std::endl;
    // The failing input can retain the creation thread indefinitely. Exiting the
    // test subprocess preserves that failure without blocking a future destructor.
    std::_Exit (EXIT_FAILURE);
}

template <typename Future> void require_ready (Future &future, const char *reason)
{
    if (future.wait_for (observation_timeout) != std::future_status::ready)
        fail_child (reason);
}

void pending_creation_shutdown (bool with_child = false, bool settle_before_drain = false)
{
    auto node = fw::test::runtime_failure_fixture<detail::spot_node_builder_state_t> (
      "pending-creation-shutdown");
    node->worker_executor = std::make_shared<rt::offload_executor_t> (1);
    const auto worker = node->worker_executor;
    fw::serializer_registry_t serializers;
    node->channel_runtime = fw::test::runtime_failure_fixture<detail::channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    node->spot_factories.emplace ("preparing", std::type_index (typeid (int)));

    std::shared_ptr<detail::spot_context_state_t> context;
    std::promise<void> entered;
    auto creation_entered = entered.get_future ();
    fw::task_completion_source_t<fw::spot_create_response_t> release;
    detail::spot_lifecycle_callbacks_t lifecycle;
    lifecycle.create_spot_context_instance =
      [&] (fw::spot_context_t value, fw::service_provider_t &) -> std::shared_ptr<void> {
        context = detail::spot_context_access_t::state (value);
        return std::make_shared<int> (1);
    };
    lifecycle.on_create = [&] (void *, const zlink::message_t &, fw::serializer_registry_t &) {
        entered.set_value ();
        return release.task ();
    };
    node->spot_lifecycles.emplace ("preparing", std::move (lifecycle));

    detail::spot_node_runtime_t owner (node);
    std::promise<std::string> creation_terminal;
    auto created = creation_terminal.get_future ();
    std::thread creation ([&] {
        try {
            const auto result =
              owner.get_or_create_spot ("preparing", fw::spot_id_t ("pending-creation"), {});
            creation_terminal.set_value (
              result.state == fw::spot_create_state_t::created ? "created" : "not-created");
        }
        catch (const std::exception &error) {
            creation_terminal.set_value (error.what ());
        }
    });
    require_ready (creation_entered, "creation callback did not enter");
    creation_entered.get ();

    // This marker follows the callback on the only worker. Its completion proves
    // that the callback suspended and no longer occupies that worker.
    std::promise<void> observed_idle_worker;
    auto callback_suspended = observed_idle_worker.get_future ();
    if (!worker->try_submit_internal ([&] { observed_idle_worker.set_value (); }))
        fail_child ("worker rejected the suspension marker");
    require_ready (callback_suspended, "creation callback retained the worker");
    callback_suspended.get ();

    struct creation_snapshot_t
    {
        bool unpublished = false;
        std::shared_future<void> terminal;
    };
    const auto pending = node->lane
                           .run ([&] {
                               const auto found =
                                 node->pending_spot_creations_by_id.find ("pending-creation");
                               if (found == node->pending_spot_creations_by_id.end ())
                                   return creation_snapshot_t{};
                               return creation_snapshot_t{
                                 found->second.context.lock () == context
                                   && !node->spot_contexts_by_id.contains ("pending-creation"),
                                 found->second.future};
                           })
                           .get ();
    if (!pending.unpublished || !pending.terminal.valid () || !context || !context->serial_queue)
        fail_child ("creation did not reach its unpublished pending state");

    std::shared_ptr<rt::serial_execution_queue_t> child_queue;
    std::atomic_bool child_called{false};
    if (with_child) {
        child_queue = context->spot_serial_executor->actor_executor ("pending-child")->queue ();
        if (!child_queue->try_post ("pending-child-application",
                                    [&] { child_called.store (true, std::memory_order_release); }))
            fail_child ("Actor child rejected application work during preparation");

        // Child publication schedules its parent gate before this marker. The
        // only worker imports that publication and tests its preparation policy
        // before it can run the marker.
        std::promise<void> observed_child_publication;
        auto child_suspended = observed_child_publication.get_future ();
        if (!worker->try_submit_internal ([&] { observed_child_publication.set_value (); }))
            fail_child ("worker rejected the Actor child suspension marker");
        require_ready (child_suspended, "Actor child did not reach preparation suspension");
        child_suspended.get ();
        if (child_queue->pending_count (rt::serial_work_lane_t::application) != 1
            || child_called.load (std::memory_order_acquire))
            fail_child ("preparation did not retain Actor child application work");
        std::cerr << "[pending-spot-shutdown] child_preparation_pending=1 child_called=0"
                  << std::endl;
    }

    auto preparation_terminal =
      context->spot_serial_executor->preparation_lifecycle ()->terminal ();
    auto first_shutdown = std::async (std::launch::async, [&] {
        owner.request_stop ();
        owner.cancel_timers ();
        owner.cancel_pending_dispatch ();
        if (settle_before_drain) {
            require_ready (pending.terminal, "early cancellation retained creation");
            const bool removed =
              node->lane
                .run (
                  [&] { return !node->pending_spot_creations_by_id.contains ("pending-creation"); })
                .get ();
            if (!removed || !child_queue || !child_queue->closed ()
                || child_queue->pending_count () != 0
                || child_called.load (std::memory_order_acquire))
                fail_child ("creation released its reservation before child resources");
            std::cerr << "[pending-spot-shutdown] early_creation_terminal=1 registry_removed=1 "
                         "child_closed=1 child_pending=0 child_called=0"
                      << std::endl;
        }
        owner.cancel_pending_work ();
    });
    require_ready (first_shutdown, "first shutdown did not return");
    first_shutdown.get ();

    const bool creation_settled_before_worker_shutdown =
      pending.terminal.wait_for (0s) == std::future_status::ready;
    const bool caller_returned = created.wait_for (0s) == std::future_status::ready;
    const bool preparation_settled_before_worker_shutdown = preparation_terminal.await_ready ();
    const auto remaining_lifecycle =
      context->serial_queue->pending_count (rt::serial_work_lane_t::lifecycle);
    const auto live_workers = worker->live_worker_count ();
    const auto owned_threads = worker->owned_thread_count ();
    const auto remaining_child = child_queue ? child_queue->pending_count () : 0;
    const bool child_ran = child_called.load (std::memory_order_acquire);
    std::cerr << "[pending-spot-shutdown] first_returned=1 creation_terminal="
              << creation_settled_before_worker_shutdown
              << " preparation_terminal=" << preparation_settled_before_worker_shutdown
              << " lifecycle=" << remaining_lifecycle << " live_workers=" << live_workers
              << " owned_threads=" << owned_threads << " caller_returned=" << caller_returned
              << " child_pending=" << remaining_child << " child_called=" << child_ran << std::endl;

    // The application callback may complete after shutdown has cancelled its
    // accepted creation. That late completion must not retain either teardown.
    release.complete (
      fw::result_t<fw::spot_create_response_t>::success (fw::spot_create_response_t::accept ()));
    auto second_shutdown = std::async (std::launch::async, [&] { owner.cancel_pending_work (); });
    const bool second_returned =
      second_shutdown.wait_for (observation_timeout) == std::future_status::ready;
    const bool creation_returned =
      created.wait_for (observation_timeout) == std::future_status::ready;
    std::cerr << "[pending-spot-shutdown] late_completion=1 second_returned=" << second_returned
              << " creation_returned=" << creation_returned << std::endl;

    if (!second_returned || !creation_returned)
        fail_child ("late callback completion retained creation or teardown");
    second_shutdown.get ();
    const auto creation_outcome = created.get ();
    creation.join ();
    std::cerr << "[pending-spot-shutdown] creation_outcome=" << creation_outcome << std::endl;

    if (!creation_settled_before_worker_shutdown || !preparation_settled_before_worker_shutdown
        || remaining_lifecycle != 0 || live_workers != 0 || owned_threads != 0)
        fail_child ("shutdown stopped workers before accepted preparation became terminal");
    if (remaining_child != 0 || child_ran || (child_queue && child_queue->pending_count () != 0)
        || child_called.load (std::memory_order_acquire))
        fail_child ("shutdown retained or executed Actor child application work");
}

TEST (ZLinkFrameworkPendingSpotShutdown, SettlesUnpublishedCreationBeforeStoppingWorkers)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT (
      {
          try {
              pending_creation_shutdown ();
              std::_Exit (EXIT_SUCCESS);
          }
          catch (const std::exception &error) {
              std::cerr << "[pending-spot-shutdown] unexpected exception: " << error.what ()
                        << std::endl;
              std::_Exit (EXIT_FAILURE);
          }
      },
      ::testing::ExitedWithCode (EXIT_SUCCESS), "");
}

TEST (ZLinkFrameworkPendingSpotShutdown,
      SettlesUnpublishedCreationAndActorChildBeforeStoppingWorkers)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT (
      {
          try {
              pending_creation_shutdown (true);
              std::_Exit (EXIT_SUCCESS);
          }
          catch (const std::exception &error) {
              std::cerr << "[pending-spot-shutdown] unexpected exception: " << error.what ()
                        << std::endl;
              std::_Exit (EXIT_FAILURE);
          }
      },
      ::testing::ExitedWithCode (EXIT_SUCCESS), "");
}
TEST (ZLinkFrameworkPendingSpotShutdown, ReleasesChildBeforeEarlyCreationUnregisters)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EXIT (
      {
          try {
              pending_creation_shutdown (true, true);
              std::_Exit (EXIT_SUCCESS);
          }
          catch (const std::exception &error) {
              std::cerr << "[pending-spot-shutdown] unexpected exception: " << error.what ()
                        << std::endl;
              std::_Exit (EXIT_FAILURE);
          }
      },
      ::testing::ExitedWithCode (EXIT_SUCCESS), "");
}
} // namespace
