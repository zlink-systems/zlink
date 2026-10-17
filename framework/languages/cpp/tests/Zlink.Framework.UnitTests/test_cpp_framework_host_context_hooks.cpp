/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework.hpp>

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"

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

} // namespace
