/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework.hpp>

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
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

} // namespace
