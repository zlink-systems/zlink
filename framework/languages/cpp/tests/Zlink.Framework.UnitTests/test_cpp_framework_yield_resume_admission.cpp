/* SPDX-License-Identifier: Elastic-2.0 */

#include "manual_internal_scheduler.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"

#include <zlink/framework/contracts/workers/worker.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <optional>

namespace
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;

task_t<int> yield_for_worker (task_completion_source_t<int> &reply)
{
    worker_call_t<int> call ([&] (std::stop_token) { return reply.task (); });
    co_return co_await call.yield ();
}

enum class admission_boundary_t
{
    close,
    relocation_cut,
    relocation_cut_with_rejected_wake
};

class yield_resume_admission_t : public testing::TestWithParam<admission_boundary_t>
{
};

TEST_P (yield_resume_admission_t, SettlesStartedHandlerOnce)
{
    completion_owner_t completion_owner;
    test::manual_internal_scheduler_t executor;
    serial_execution_queue_t queue (executor, {}, {}, serial_lane_policy_t::spot_wide ());
    task_completion_source_t<int> reply;
    task_completion_source_t<void> handler_terminal;
    std::atomic_int terminals{0};
    std::optional<task_t<int>> handler;
    std::shared_ptr<detail::serial_turn_t> turn;
    queue.post_async ("yielding-handler", [&] (auto complete) {
        turn = detail::capture_current_serial_turn ();
        handler.emplace (yield_for_worker (reply));
        detail::observe_task_terminal (*handler, [&, complete] (const auto &) {
            complete ([&] {
                ++terminals;
                handler_terminal.complete (result_t<void>::success ());
            });
        });
    });
    executor.run_all ();
    ASSERT_TRUE (turn && turn->released ());
    ASSERT_EQ (0, terminals.load ());

    const auto boundary = GetParam ();
    if (boundary == admission_boundary_t::close)
        queue.close ();
    else {
        auto binding = std::make_shared<stateful::relocation_seal_binding_t> ();
        auto cut = queue.take_application_suffix (
          [binding] { binding->bound.store (true, std::memory_order_release); },
          [] (const auto &take) {
              take ();
              return true;
          },
          binding);
        ASSERT_TRUE (executor.run_until (cut, 2s));
        ASSERT_TRUE (cut.result ());
        ASSERT_TRUE (binding->bound.load (std::memory_order_acquire));
    }
    if (boundary == admission_boundary_t::relocation_cut_with_rejected_wake)
        executor.retire ();

    auto terminal = handler_terminal.task ();
    reply.complete (result_t<int>::success (7));
    ASSERT_TRUE (executor.run_until (terminal, 2s));
    executor.run_all ();
    queue.drain ();
    EXPECT_EQ (1, terminals.load ());
    const auto &result = handler->result ();
    if (boundary == admission_boundary_t::relocation_cut) {
        ASSERT_TRUE (result);
        EXPECT_EQ (7, result.value ());
    } else {
        ASSERT_FALSE (result);
        EXPECT_EQ (framework_error_kind_t::shutting_down, result.error_kind ());
    }
}

INSTANTIATE_TEST_SUITE_P (SpotWide,
                          yield_resume_admission_t,
                          testing::Values (admission_boundary_t::close,
                                           admission_boundary_t::relocation_cut,
                                           admission_boundary_t::relocation_cut_with_rejected_wake),
                          [] (const testing::TestParamInfo<admission_boundary_t> &test) {
                              switch (test.param) {
                                  case admission_boundary_t::close:
                                      return "Closed";
                                  case admission_boundary_t::relocation_cut:
                                      return "RelocationCut";
                                  case admission_boundary_t::relocation_cut_with_rejected_wake:
                                      return "RelocationCutWithRejectedWake";
                              }
                              return "";
                          });
} // namespace
