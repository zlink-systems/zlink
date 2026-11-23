/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/execution/serial_execution_queue.hpp"

#include <zlink/framework/contracts/workers/worker.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <system_error>

namespace
{
using namespace zlink::framework;
using namespace zlink::framework::runtime;

class manual_executor_t final : public offload_executor_t
{
  public:
    manual_executor_t () : offload_executor_t (1) {}

    bool try_submit_internal (std::function<void ()> work) override
    {
        std::lock_guard lock (_mutex);
        if (_reject != 0 && (++_attempt == 1 || (_reject == 2 && _attempt == 3))) {
            ++rejected;
            return false;
        }
        _ready.push_back (std::move (work));
        return true;
    }

    void reject_resume (int reject)
    {
        std::lock_guard lock (_mutex);
        _reject = reject;
    }

    void run_ready ()
    {
        for (;;) {
            std::unique_lock lock (_mutex);
            if (_ready.empty ())
                return;
            auto work = std::move (_ready.front ());
            _ready.pop_front ();
            lock.unlock ();
            work ();
        }
    }

    std::atomic_int rejected{0};

  private:
    std::mutex _mutex;
    std::deque<std::function<void ()>> _ready;
    int _reject = 0;
    int _attempt = 0;
};

struct terminal_t
{
    std::shared_ptr<detail::serial_turn_t> before;
    std::shared_ptr<detail::serial_turn_t> after;
    std::optional<int> value;
    std::optional<framework_error_kind_t> failure;
    std::error_code cancellation;
    std::atomic_int caught{0};
    std::atomic_int cleaned{0};
};

task_t<void> await_reply (task_completion_source_t<int> &reply, terminal_t &terminal)
{
    struct cleanup_t
    {
        std::atomic_int &count;
        ~cleanup_t () { ++count; }
    } cleanup{terminal.cleaned};
    worker_call_t<int> call ([&] (std::stop_token) { return reply.task (); });
    terminal.before = detail::capture_current_serial_turn ();
    try {
        terminal.value = co_await call.yield ();
    }
    catch (const framework_exception_t &error) {
        terminal.failure = error.kind ();
        ++terminal.caught;
    }
    catch (const std::system_error &error) {
        terminal.cancellation = error.code ();
        ++terminal.caught;
    }
    terminal.after = detail::capture_current_serial_turn ();
}

enum scenario_t
{
    success,
    reject_resume,
    reject_resume_and_direct_worker,
    cancelled,
    closed
};

class yield_terminal_test_t : public testing::TestWithParam<scenario_t>
{
};

TEST_P (yield_terminal_test_t, DeliversOnceInFreshTurnAfterCurrentHandler)
{
    completion_owner_t completion_owner;
    manual_executor_t executor;
    serial_execution_queue_t queue (executor, {}, {}, serial_lane_policy_t::spot_wide ());
    task_completion_source_t<int> reply;
    terminal_t terminal;
    std::optional<task_t<void>> awaiting;
    serial_execution_queue_t::async_completion_t finish_current;
    std::atomic_bool current_finished{false};
    queue.post_async ("yielding-handler", [&] (auto complete) {
        awaiting.emplace (await_reply (reply, terminal));
        detail::observe_task_terminal (*awaiting, [complete] (const auto &) { complete ([] {}); });
    });
    queue.post_async ("current-handler",
                      [&] (auto complete) { finish_current = std::move (complete); });
    executor.run_ready ();
    ASSERT_TRUE (terminal.before && terminal.before->released ());
    ASSERT_TRUE (finish_current);
    ASSERT_EQ (0, terminal.cleaned.load ());

    // The original implementation posts a terminal control between the
    // rejected gate wake and its attempt to dispatch the continuation directly.
    const auto scenario = GetParam ();
    const int rejections =
      scenario == reject_resume || scenario == reject_resume_and_direct_worker ? scenario : 0;
    executor.reject_resume (rejections);
    if (scenario == closed)
        queue.close ();
    auto result = scenario == cancelled
                    ? detail::result_access_t::failure<int> (
                        detail::make_cancellation_exception ("yield operation cancelled"))
                    : result_t<int>::success (7);
    reply.complete (std::move (result));
    executor.run_ready ();
    EXPECT_EQ (rejections != 0, executor.rejected.load () > 0);
    EXPECT_FALSE (current_finished.load ());
    EXPECT_EQ (0, terminal.caught.load ());
    EXPECT_EQ (0, terminal.cleaned.load ());

    executor.reject_resume (0);
    finish_current ([&] { current_finished = true; });
    executor.run_ready ();
    queue.drain ();
    EXPECT_TRUE (current_finished.load ());
    EXPECT_EQ (1, terminal.cleaned.load ());
    ASSERT_TRUE (terminal.after);
    EXPECT_NE (terminal.before, terminal.after);
    EXPECT_TRUE (terminal.after->belongs_to (&queue));
    if (scenario == success) {
        EXPECT_EQ (7, terminal.value);
        EXPECT_EQ (0, terminal.caught.load ());
    } else if (scenario == cancelled) {
        EXPECT_EQ (std::errc::operation_canceled, terminal.cancellation);
        EXPECT_EQ (1, terminal.caught.load ());
    } else {
        EXPECT_EQ (framework_error_kind_t::shutting_down, terminal.failure);
        EXPECT_EQ (1, terminal.caught.load ());
    }
}

INSTANTIATE_TEST_SUITE_P (
  WorkerAdmission,
  yield_terminal_test_t,
  testing::Values (success, reject_resume, reject_resume_and_direct_worker, cancelled, closed));

} // namespace
