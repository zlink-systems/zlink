/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework.hpp>

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace
{

using namespace zlink::framework;
using namespace std::chrono_literals;

class started_service_t final : public hosted_service_t
{
  public:
    explicit started_service_t (std::promise<void> &started) : _started (started) {}
    task_t<void> start (service_provider_t &) override
    {
        _started.set_value ();
        co_return;
    }
    void request_stop () noexcept override {}
    void stop () noexcept override {}

  private:
    std::promise<void> &_started;
};

// Submit/completion §1 :69: a timed observation inside a runtime execution
// context fails with InvalidOperation.
bool runtime_turn_rejects_result_for ()
{
    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor);
    bool rejected = false;
    queue.run ("host-context-hooks", [&] {
        const task_t<void> ready (result_t<void>::success ());
        try {
            (void) ready.result_for (1ms);
        }
        catch (const framework_exception_t &error) {
            rejected = error.kind () == framework_error_kind_t::invalid_operation;
        }
    });
    return rejected;
}

// The host runtime start is the one place that installs the runtime context
// hooks; this process has started no host before the first observation.
TEST (ZLinkFrameworkHostContextHooks, HostStartInstallsRuntimeContextHooks)
{
    EXPECT_FALSE (runtime_turn_rejects_result_for ());

    std::promise<void> started;
    auto running = started.get_future ();
    auto app = app_t::create ();
    app.add_hosted_service (std::make_unique<started_service_t> (started));
    char command[] = "host-context-hooks";
    char *argv[] = {command};
    int exit_code = -1;
    std::thread host ([&] { exit_code = app.run (1, argv); });
    ASSERT_EQ (std::future_status::ready, running.wait_for (5s));
    app.stop ();
    host.join ();
    EXPECT_EQ (0, exit_code);

    EXPECT_TRUE (runtime_turn_rejects_result_for ());
}

// Host relocation §14: accepted callbacks drain before session/resource teardown.
class callback_barrier_service_t final : public hosted_service_t,
                                         public runtime::hosted_service_lifecycle_t
{
  public:
    explicit callback_barrier_service_t (app_t &app) : _app (app) {}

    task_t<void> start (service_provider_t &) override
    {
        _app.stop ();
        co_return;
    }

    void stop () noexcept override {}
    void seal_application_dispatch () noexcept override { sealed = true; }

    bool wait_for_accepted_callbacks_until (std::chrono::steady_clock::time_point) noexcept override
    {
        ++callback_waits;
        return true;
    }

    bool drain_sessions_until (std::chrono::steady_clock::time_point) noexcept override
    {
        waits_at_session_drain = callback_waits;
        return true;
    }

    bool sealed = false;
    int callback_waits = 0;
    int waits_at_session_drain = 0;

  private:
    app_t &_app;
};

TEST (ZLinkFrameworkHostContextHooks, ShutdownWaitsOnceForAcceptedCallbacksBeforeSessionDrain)
{
    auto app = app_t::create ();
    auto service = std::make_unique<callback_barrier_service_t> (app);
    auto &barrier = *service;
    app.add_hosted_service (std::move (service));
    char command[] = "callback-barrier";
    char *argv[] = {command};
    EXPECT_EQ (0, app.run (1, argv));
    EXPECT_TRUE (barrier.sealed);
    const auto termination = app.shutdown ().result ().value ();
    EXPECT_EQ (termination_outcome_t::stopped, termination.outcome);
    EXPECT_EQ (termination_reason_t::none, termination.reason);
    EXPECT_EQ (1, barrier.callback_waits);
    EXPECT_EQ (1, barrier.waits_at_session_drain);
}

// Host relocation §14 bounds closing observation by the supplied teardown end.
TEST (ZLinkFrameworkHostContextHooks, ShutdownClosingRequestsCancellationAtTeardownEnd)
{
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("closing-cancellation");
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->spot_id = "closing-cancellation";
    context->spot_instance = std::make_shared<int> (1);
    node->spot_contexts_by_id.emplace (context->spot_id,
                                       detail::spot_context_access_t::create (context));
    node->teardown_end = std::chrono::system_clock::time_point::min ();
    std::stop_source host_cleanup;
    node->cleanup_cancellation = host_cleanup.get_token ();
    task_completion_source_t<void> release;
    std::stop_token closing_token;
    bool stopped_at_invocation = true;
    context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &,
                                         std::stop_token token) {
        closing_token = token;
        stopped_at_invocation = token.stop_requested ();
        return release.task ();
    };
    task_completion_source_t<bool> closed;
    context->request_close (
      {}, [&] (result_t<bool> result) { closed.complete (result); },
      spot_close_reason_t::host_shutdown);
    detail::spot_node_runtime_t runtime (node);
    EXPECT_FALSE (
      runtime.close_all_user_spots (std::chrono::steady_clock::time_point{}, host_cleanup));
    EXPECT_FALSE (stopped_at_invocation);
    EXPECT_TRUE (closing_token.stop_possible ());
    EXPECT_TRUE (closing_token.stop_requested ());
    // Always settle the application source; this reproduction owns no timer or sleeper.
    release.complete (result_t<void>::success ());
    EXPECT_TRUE (closed.task ().result ().value ());
}

TEST (ZLinkFrameworkHostContextHooks, ShutdownSharesCancellationAcrossNodesAndLateCallbacks)
{
    std::stop_source host_cleanup;
    task_completion_source_t<void> release;
    std::stop_token first_token;
    std::stop_token late_token;
    auto first = std::make_shared<detail::spot_node_builder_state_t> ("first-closing");
    auto late = std::make_shared<detail::spot_node_builder_state_t> ("late-closing");
    runtime::host::host_options_t native_options;
    native_options.mesh.descriptor.mesh_name = "closing-token-test";
    native_options.mesh.descriptor.node_routing_id =
      zlink::routing_id_t::from ("closing-token-test-node").to_bytes ();
    native_options.mesh.descriptor.lifecycle_generation = 1;
    native_options.mesh.descriptor.descriptor_revision = 1;
    native_options.mesh.descriptor.advertised_endpoint = "tcp://127.0.0.1:0";
    native_options.mesh.runtime_failures =
      std::make_shared<runtime::runtime_failure_collector_t> ();
    native_options.object_stable_types = {"framework.spot"};
    auto native_host = std::make_shared<runtime::host::public_host_runtime_t> (native_options);
    auto first_native =
      std::make_shared<runtime::host::spot_t> (native_host->get_or_create_spot ("first"));
    auto late_native =
      std::make_shared<runtime::host::spot_t> (native_host->get_or_create_spot ("late"));
    auto first_context = std::make_shared<detail::spot_context_state_t> ();
    auto late_context = std::make_shared<detail::spot_context_state_t> ();
    for (const auto &[node, context] :
         {std::pair{first, first_context}, std::pair{late, late_context}}) {
        context->node = node;
        context->lane_owner = node;
        context->spot_id = node == first ? "first" : "late";
        context->spot_instance = std::make_shared<int> (1);
        node->spot_contexts_by_id.emplace (context->spot_id,
                                           detail::spot_context_access_t::create (context));
    }
    first_context->native_spot = first_native;
    late_context->native_spot = late_native;
    first_context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &,
                                               std::stop_token token) {
        first_token = token;
        EXPECT_FALSE (token.stop_requested ());
        return release.task ();
    };
    late_context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &,
                                              std::stop_token token) {
        late_token = token;
        EXPECT_TRUE (token.stop_requested ());
        return task_t<void> (result_t<void>::success ());
    };
    detail::spot_node_runtime_t first_runtime (first);
    detail::spot_node_runtime_t late_runtime (late);
    EXPECT_FALSE (
      first_runtime.close_all_user_spots (std::chrono::steady_clock::time_point{}, host_cleanup));
    EXPECT_TRUE (first_token.stop_requested ());
    EXPECT_TRUE (
      late_runtime.close_all_user_spots (std::chrono::steady_clock::time_point{}, host_cleanup));
    EXPECT_EQ (host_cleanup.get_token (), first_token);
    EXPECT_EQ (first_token, late_token);
    release.complete (result_t<void>::success ());
    first_runtime.release_native_handles ();
    late_runtime.release_native_handles ();
}

TEST (ZLinkFrameworkHostContextHooks, ShutdownObservesEntryClosingFailureBeforeResourceRelease)
{
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("entry-closing-failure");
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->spot_id = "entry-closing-failure";
    context->lifecycle_domain = detail::spot_lifecycle_domain_t::entry ();
    context->spot_instance = std::make_shared<int> (1);
    node->spot_contexts_by_id.emplace (context->spot_id,
                                       detail::spot_context_access_t::create (context));
    int calls = 0;
    auto observed_deadline = std::chrono::system_clock::time_point::max ();
    context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &closing,
                                         std::stop_token) -> task_t<void> {
        ++calls;
        observed_deadline = closing.deadline;
        throw std::runtime_error ("entry closing failure");
    };
    detail::spot_node_runtime_t runtime (node);
    std::stop_source host_cleanup;
    EXPECT_THROW (
      runtime.close_all_user_spots (std::chrono::steady_clock::time_point{}, host_cleanup),
      std::runtime_error);
    EXPECT_EQ (1, calls);
    // Resource release must not be the first observer of application closing.
    runtime.release_native_handles ();
    EXPECT_EQ (1, calls);
    EXPECT_NE (std::chrono::system_clock::time_point::max (), observed_deadline);
}

struct startup_closing_evidence_t
{
    bool initialized = false;
    bool startup_failed = false;
    bool cleanup_has_cancellation = false;
    int closing_calls = 0;
    std::chrono::system_clock::time_point deadline = std::chrono::system_clock::time_point::max ();
};

class startup_entry_spot_t final : public entry_spot_t<actor_t>
{
  public:
    startup_entry_spot_t (entry_spot_context_t context, startup_closing_evidence_t &evidence) :
        _context (std::move (context)), _evidence (evidence)
    {
    }
    entry_spot_context_t &context () noexcept override { return _context; }
    const entry_spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    task_t<void> on_initialize () override
    {
        _evidence.initialized = true;
        co_return;
    }
    task_t<void> on_actor_joined (actor_t &) override { co_return; }
    task_t<void> on_leave_actor (actor_t &) override { co_return; }
    task_t<void> on_closing (const spot_closing_context_t &closing, std::stop_token token) override
    {
        ++_evidence.closing_calls;
        _evidence.deadline = closing.deadline;
        _evidence.cleanup_has_cancellation = token.stop_possible ();
        EXPECT_EQ (spot_close_reason_t::host_shutdown, closing.reason);
        throw std::runtime_error ("startup Entry closing failed");
    }

  private:
    entry_spot_context_t _context;
    startup_closing_evidence_t &_evidence;
};

class startup_failure_service_t final : public hosted_service_t
{
  public:
    explicit startup_failure_service_t (startup_closing_evidence_t &evidence) : _evidence (evidence)
    {
    }
    task_t<void> start (service_provider_t &) override
    {
        _evidence.startup_failed = true;
        throw std::runtime_error ("hosted service startup failed");
    }
    void request_stop () noexcept override {}
    void stop () noexcept override {}

  private:
    startup_closing_evidence_t &_evidence;
};

TEST (ZLinkFrameworkHostContextHooks, StartupFailureUsesSharedClosingCoordinator)
{
    startup_closing_evidence_t evidence;
    auto app = app_t::create ();
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.add_location_store (std::make_shared<runtime::in_memory_location_store_t> ());
        options.add_relocation_store (std::make_shared<runtime::in_memory_relocation_store_t> ());
        options.add_route_mesh ("startup-closing")
          .set_object_role (object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from ("startup-closing"))
          .listen ("tcp://127.0.0.1:0")
          .add_entry_spot<startup_entry_spot_t> ([&] (entry_spot_context_t context) {
              return std::make_shared<startup_entry_spot_t> (std::move (context), evidence);
          });
    });
    app.add_hosted_service (std::make_unique<startup_failure_service_t> (evidence));
    testing::internal::CaptureStderr ();
    const auto exit_code = app.run (0, nullptr);
    const auto error = testing::internal::GetCapturedStderr ();
    EXPECT_EQ (1, exit_code);
    EXPECT_NE (std::string::npos, error.find ("hosted service startup failed"));
    EXPECT_TRUE (evidence.initialized);
    EXPECT_TRUE (evidence.startup_failed);
    EXPECT_EQ (1, evidence.closing_calls);
    ASSERT_TRUE (evidence.cleanup_has_cancellation);
    EXPECT_NE (std::chrono::system_clock::time_point::max (), evidence.deadline);
    const auto terminal = app.shutdown ().result ().value ();
    EXPECT_EQ (termination_outcome_t::force_stopped, terminal.outcome);
    EXPECT_EQ (termination_reason_t::teardown_failed, terminal.reason);
    EXPECT_EQ (framework_runtime_state_t::stopped, app.runtime_state ());
    EXPECT_EQ (1, evidence.closing_calls);
}

TEST (ZLinkFrameworkHostContextHooks, ShutdownDrainsCommittedLifecycleAndCancelsPendingCapabilities)
{
    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor);
    std::promise<std::function<void ()>> held;
    auto holder = held.get_future ();
    int deferred_runs = 0;
    int deferred_cancelled = 0;
    int lifecycle_runs = 0;
    int cancellable_runs = 0;
    int cancellable_cancelled = 0;
    ASSERT_TRUE (queue.try_post_async ("held-turn", [&] (auto complete) {
        EXPECT_TRUE (detail::defer_current_serial_turn ([&] { ++deferred_runs; },
                                                        [&] { ++deferred_cancelled; }));
        held.set_value ([complete] () mutable { complete ([] {}); });
    }));
    auto finish = holder.get ();
    EXPECT_TRUE (queue.try_post (
      "committed-lifecycle", [&] { ++lifecycle_runs; },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle}));
    EXPECT_TRUE (queue.try_post_cancellable_async (
      "cancellable-lifecycle",
      [&] (auto complete) {
          ++cancellable_runs;
          complete ([] {});
      },
      [&] { ++cancellable_cancelled; },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle}));
    queue.cancel_pending ();
    finish ();
    queue.drain ();
    EXPECT_EQ (1, lifecycle_runs);
    EXPECT_EQ (0, cancellable_runs);
    EXPECT_EQ (1, cancellable_cancelled);
    EXPECT_EQ (0, deferred_runs);
    EXPECT_EQ (1, deferred_cancelled);
}


TEST (ZLinkFrameworkHostContextHooks, ShutdownPreservesLifecycleFifoThroughEntryClose)
{
    runtime::offload_executor_t executor (1);
    auto queue = std::make_shared<runtime::serial_execution_queue_t> (executor);
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("entry-closing-fifo");
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->spot_id = "entry-closing-fifo";
    context->lifecycle_domain = detail::spot_lifecycle_domain_t::entry ();
    context->spot_instance = std::make_shared<int> (1);
    context->serial_queue = queue;
    node->spot_contexts_by_id.emplace (context->spot_id,
                                       detail::spot_context_access_t::create (context));
    std::vector<int> order;
    std::promise<std::function<void ()>> held;
    auto holder = held.get_future ();
    ASSERT_TRUE (queue->try_post_async (
      "held-lifecycle",
      [&] (auto complete) { held.set_value ([complete] () mutable { complete ([] {}); }); },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle}));
    auto finish = holder.get ();
    ASSERT_TRUE (queue->try_post (
      "before-close", [&] { order.push_back (1); },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle}));
    context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &closing,
                                         std::stop_token) -> task_t<void> {
        EXPECT_EQ (spot_close_reason_t::host_shutdown, closing.reason);
        EXPECT_EQ ((std::vector<int>{1}), order);
        order.push_back (2);
        co_return;
    };
    task_completion_source_t<bool> closed;
    context->request_close (
      {}, [&] (result_t<bool> result) { closed.complete (result); },
      spot_close_reason_t::host_shutdown);
    ASSERT_TRUE (queue->try_post (
      "after-close", [&] { order.push_back (3); },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle}));
    queue->cancel_pending ();
    finish ();
    queue->drain ();
    EXPECT_TRUE (closed.task ().result ().value ());
    EXPECT_EQ ((std::vector<int>{1, 2, 3}), order);
}

TEST (ZLinkFrameworkHostContextHooks, IdleClosingFailureStillReleasesLocalActivation)
{
    auto node = std::make_shared<detail::spot_node_builder_state_t> ("idle-closing-failure");
    auto context = std::make_shared<detail::spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->spot_id = "idle-closing-failure";
    context->lifecycle_domain = detail::spot_lifecycle_domain_t::instance ();
    context->spot_instance = std::make_shared<int> (1);
    node->spot_contexts_by_id.emplace (context->spot_id,
                                       detail::spot_context_access_t::create (context));
    int calls = 0;
    context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &closing,
                                         std::stop_token) -> task_t<void> {
        ++calls;
        EXPECT_EQ (spot_close_reason_t::idle_evicted, closing.reason);
        EXPECT_EQ (context, detail::spot_context_access_t::state (
                              node->spot_contexts_by_id.at (context->spot_id)));
        throw std::runtime_error ("idle closing failed");
    };
    const auto reservation = context->begin_idle_close_reservation ();
    ASSERT_NE (0u, reservation);
    context->commit_idle_close (reservation);
    EXPECT_EQ (1, calls);
    EXPECT_TRUE (context->closed);
    EXPECT_FALSE (context->spot_instance);
    EXPECT_FALSE (context->node);
    EXPECT_TRUE (node->spot_contexts_by_id.empty ());
}

} // namespace
