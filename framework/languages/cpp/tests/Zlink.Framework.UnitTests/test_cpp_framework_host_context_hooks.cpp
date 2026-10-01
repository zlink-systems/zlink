/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"

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

} // namespace
