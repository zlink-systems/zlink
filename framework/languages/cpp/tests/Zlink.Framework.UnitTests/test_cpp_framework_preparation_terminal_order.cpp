/* SPDX-License-Identifier: Elastic-2.0 */
// Preparation §terminal order: the preparation terminal is published only after the lifecycle
// barrier has completed. A waiter resumed at the terminal is the host closing its executors
// after a failed startup (#5860); barrier completion still schedules the next drain, and that
// tail must not run against executors the waiter has already closed.
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <future>
#include <memory>

namespace
{
using namespace zlink::framework;

struct terminal_observation_t
{
    bool barrier_terminal_ready = false;
    bool succeeded = false;
};

terminal_observation_t
observe_preparation_terminal (const detail::deferred_barrier_t::async_work_t &cleanup)
{
    runtime::state_lane_t lane;
    auto worker = std::make_shared<runtime::offload_executor_t> (1);
    auto executor = std::make_shared<detail::spot_serial_executor_t> (
      worker, lane, runtime::serial_lane_policy_t::spot_wide ());
    const auto preparation = executor->begin_preparation ();
    const auto barrier = preparation->lifecycle;
    std::promise<terminal_observation_t> observed;
    auto terminal = std::make_shared<task_t<void>> (executor->finish_preparation (cleanup));
    detail::observe_task_terminal (
      *terminal, [&, terminal, barrier] (const result_t<void> &result) {
          // Runs inline at the terminal publication, on the publishing thread.
          observed.set_value (terminal_observation_t{barrier->terminal ().await_ready (),
                                                     static_cast<bool> (result)});
      });
    const auto observation = observed.get_future ().get ();
    executor->close ();
    worker->drain ();
    return observation;
}

TEST (ZLinkFrameworkPreparationTerminalOrder, FailedCleanupPublishesTerminalAfterBarrierCompletion)
{
    const auto observation =
      observe_preparation_terminal ([] (std::function<void (result_t<void>)> complete) {
          complete (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                             "preparation cleanup fixture"));
      });
    EXPECT_FALSE (observation.succeeded);
    EXPECT_TRUE (observation.barrier_terminal_ready);
}

TEST (ZLinkFrameworkPreparationTerminalOrder,
      ReadyPreparationPublishesTerminalAfterBarrierCompletion)
{
    const auto observation = observe_preparation_terminal ({});
    EXPECT_TRUE (observation.succeeded);
    EXPECT_TRUE (observation.barrier_terminal_ready);
}
} // namespace
