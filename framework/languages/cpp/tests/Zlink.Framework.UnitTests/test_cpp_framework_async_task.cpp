/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include "runtime/timers/async_delay.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace
{

thread_local int native_await_ambient_value = 0;
thread_local bool inside_native_completion = false;

struct ambient_guard_t
{
    explicit ambient_guard_t (int value) : previous (native_await_ambient_value)
    {
        native_await_ambient_value = value;
    }
    ~ambient_guard_t () { native_await_ambient_value = previous; }
    int previous;
};

std::shared_ptr<void> capture_test_ambient ()
{
    return std::make_shared<int> (native_await_ambient_value);
}

std::shared_ptr<void> enter_test_ambient (const std::shared_ptr<void> &snapshot)
{
    return std::make_shared<ambient_guard_t> (
      *std::static_pointer_cast<int> (snapshot));
}

class test_serial_turn_t final : public zlink::framework::detail::serial_turn_t
{
  public:
    bool release () override
    {
        released_value = true;
        return true;
    }
    bool released () const override { return released_value; }
    zlink::framework::detail::task_scheduler_t resume_scheduler () override
    {
        return [] (std::function<void ()> work) { work (); };
    }
    bool belongs_to (const void *owner) const noexcept override
    {
        return owner == this;
    }
    bool is_after_active_phase () const noexcept override { return false; }
    bool allows_yield () const noexcept override { return true; }
    zlink::framework::result_t<void> defer (
      std::function<void ()> work, std::function<void ()>) override
    {
        work ();
        return zlink::framework::result_t<void>::success ();
    }
    void cancel_deferred () noexcept override {}

  private:
    bool released_value = false;
};

class native_async_state_t final : public zlink::detail::async_result_state_t<int>
{
  public:
    bool ready () const noexcept override
    {
        std::lock_guard<std::mutex> lock (mutex);
        return terminal;
    }

    bool suspend (
      std::coroutine_handle<> continuation_,
      zlink::detail::async_continuation_scheduler_t scheduler_) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (terminal)
            return false;
        continuation = continuation_;
        scheduler = std::move (scheduler_);
        return true;
    }

    int take () override { return value; }
    bool cancel () noexcept override { return false; }
    void abandon (std::coroutine_handle<> continuation_) noexcept override
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (continuation == continuation_)
            continuation = {};
    }

    void complete (int value_)
    {
        std::coroutine_handle<> next;
        zlink::detail::async_continuation_scheduler_t next_scheduler;
        {
            std::lock_guard<std::mutex> lock (mutex);
            value = value_;
            terminal = true;
            next = std::exchange (continuation, {});
            next_scheduler = std::move (scheduler);
        }
        auto work = [next] { next.resume (); };
        if (next_scheduler)
            next_scheduler (std::move (work));
        else
            work ();
    }

  private:
    mutable std::mutex mutex;
    std::coroutine_handle<> continuation{};
    zlink::detail::async_continuation_scheduler_t scheduler;
    int value = 0;
    bool terminal = false;
};

zlink::framework::task_t<int> await_native_binding_result (
  const std::shared_ptr<native_async_state_t> &state,
  const std::shared_ptr<test_serial_turn_t> &turn,
  std::atomic<int> &observed_ambient,
  std::atomic<bool> &observed_serial_turn,
  std::atomic<bool> &resumed_inside_completion)
{
    const int value = co_await zlink::detail::async_result_access_t::make<int> (state);
    resumed_inside_completion.store (
      inside_native_completion, std::memory_order_release);
    observed_ambient.store (native_await_ambient_value, std::memory_order_release);
    observed_serial_turn.store (
      zlink::framework::detail::capture_current_serial_turn () == turn,
      std::memory_order_release);
    co_return value;
}

zlink::framework::task_t<int> await_native_binding_outside_completion (
  const std::shared_ptr<native_async_state_t> &state,
  std::atomic<bool> &resumed_inside_completion)
{
    const int value = co_await zlink::detail::async_result_access_t::make<int> (state);
    resumed_inside_completion.store (
      inside_native_completion, std::memory_order_release);
    co_return value;
}

zlink::framework::task_t<int> delayed_value (int value)
{
    struct delay_awaiter_t
    {
        int value;

        bool await_ready () const noexcept { return false; }
        void await_suspend (std::coroutine_handle<> continuation) const
        {
            std::thread ([continuation] {
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
                continuation.resume ();
            }).detach ();
        }
        int await_resume () const noexcept { return value; }
    };

    co_return co_await delay_awaiter_t{value};
}

zlink::framework::task_t<int> await_shared (zlink::framework::task_t<int> &task, int offset)
{
    const auto value = co_await task;
    co_return value + offset;
}

zlink::framework::task_t<int> timeout_task ()
{
    co_return zlink::framework::detail::boundary_failure<int> (zlink::framework::detail::boundary_error_t::timed_out, "timeout preserved");
}

zlink::framework::task_t<int> await_timeout ()
{
    auto task = timeout_task ();
    co_return co_await task;
}

zlink::framework::task_t<int> await_async_delay (
  std::chrono::milliseconds duration, int value)
{
    co_await zlink::framework::detail::delay (duration);
    co_return value;
}

} // namespace

int main ()
{
    auto shared = delayed_value (40);
    auto first_waiter = await_shared (shared, 1);
    auto second_waiter = await_shared (shared, 2);
    if (first_waiter.result ().value () != 41 || second_waiter.result ().value () != 42) {
        return 1;
    }

    zlink::framework::detail::task_completion_source_t<int> completion;
    auto first_complete_wins = completion.task ();
    int callback_count = 0;
    int callback_value = 0;
    zlink::framework::detail::observe_task_completion (
      first_complete_wins,
      [&callback_count, &callback_value] (const zlink::framework::result_t<int> &result) {
          ++callback_count;
          callback_value = result.value ();
      });
    completion.complete (zlink::framework::result_t<int>::success (100));
    completion.complete (zlink::framework::result_t<int>::success (200));
    if (first_complete_wins.result ().value () != 100 || callback_count != 1
        || callback_value != 100) {
        return 2;
    }

    zlink::framework::request_call_t<int> call (zlink::framework::detail::boundary_failure<int> (zlink::framework::detail::boundary_error_t::timed_out, "timeout"));
    auto coroutine_result = call.async ().result ();
    if ((coroutine_result.error () != nullptr
         && zlink::framework::detail::boundary_state (*coroutine_result.error ()) != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 3;
    }

    const auto preserved_failure = await_timeout ().result ();
    if (preserved_failure
        || (preserved_failure.error () != nullptr
         && zlink::framework::detail::boundary_state (*preserved_failure.error ()) != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 4;
    }

    zlink::framework::request_call_t<int> shutdown_call (zlink::framework::detail::boundary_failure<int> (zlink::framework::detail::boundary_error_t::shutdown, "shutdown"));
    const auto shutdown_result = shutdown_call.async ().result ();
    if (shutdown_result || shutdown_result.error () == nullptr
        || zlink::framework::detail::boundary_state (*shutdown_result.error ())
             != zlink::framework::detail::boundary_error_t::shutdown) {
        return 5;
    }

    std::deque<std::function<void ()>> scheduled;
    zlink::framework::detail::task_completion_source_t<int> scheduled_completion (
      [&scheduled] (std::function<void ()> work) { scheduled.push_back (std::move (work)); });
    auto scheduled_task = scheduled_completion.task ();
    int scheduled_callback_count = 0;
    zlink::framework::detail::observe_task_completion (
      scheduled_task, [&scheduled_callback_count] (const zlink::framework::result_t<int> &result) {
          if (result.value () == 300) {
              ++scheduled_callback_count;
          }
      });
    scheduled_completion.complete (zlink::framework::result_t<int>::success (300));
    scheduled_completion.complete (zlink::framework::result_t<int>::success (400));
    if (scheduled_callback_count != 0 || scheduled.size () != 1) {
        return 6;
    }
    scheduled.front () ();
    scheduled.pop_front ();
    if (scheduled_callback_count != 1 || scheduled_task.result ().value () != 300) {
        return 7;
    }

    std::deque<std::function<void ()>> rescheduled_work;
    auto immediate_task = zlink::framework::task_t<int> (
      zlink::framework::result_t<int>::success (500));
    auto rescheduled_task = zlink::framework::detail::reschedule_task (
      std::move (immediate_task),
      [&rescheduled_work] (std::function<void ()> work) {
          rescheduled_work.push_back (std::move (work));
      });
    int rescheduled_callback_count = 0;
    zlink::framework::detail::observe_task_completion (
      rescheduled_task,
      [&rescheduled_callback_count] (const zlink::framework::result_t<int> &result) {
          if (result.value () == 500) {
              ++rescheduled_callback_count;
          }
      });
    if (rescheduled_callback_count != 0 || rescheduled_work.size () != 1) {
        return 8;
    }
    rescheduled_work.front () ();
    rescheduled_work.pop_front ();
    if (rescheduled_callback_count != 1 || rescheduled_task.result ().value () != 500) {
        return 9;
    }

    bool one_way_invoked = false;
    zlink::framework::send_call_t accepted (
      "test.command",
      [&one_way_invoked] (const std::string &,
                          const zlink::framework::send_call_t::metadata_map_t &) {
          one_way_invoked = true;
          return zlink::framework::result_t<void>::success ();
      });
    const auto accepted_result = accepted.async ().result ();
    if (!accepted_result || !one_way_invoked) {
        return 10;
    }

    zlink::framework::send_call_t timed_out (
      zlink::framework::detail::boundary_failure<void> (
        zlink::framework::detail::boundary_error_t::timed_out, "send timed out"));
    const auto timed_out_result = timed_out.async ().result ();
    if (timed_out_result
        || timed_out_result.error_kind ()
             != zlink::framework::framework_error_kind_t::deadline_exceeded) {
        return 11;
    }

    zlink::framework::send_call_t disconnected (
      zlink::framework::detail::boundary_failure<void> (
        zlink::framework::detail::boundary_error_t::disconnected, "route unavailable"));
    const auto disconnected_result = disconnected.async ().result ();
    if (disconnected_result
        || disconnected_result.error_kind ()
             != zlink::framework::framework_error_kind_t::unavailable) {
        return 12;
    }

    std::mutex publish_mutex;
    std::condition_variable publish_changed;
    bool publish_started = false;
    bool release_target = false;
    zlink::framework::publish_call_t blocked_publish (
      [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
          std::unique_lock lock (publish_mutex);
          publish_started = true;
          publish_changed.notify_all ();
          publish_changed.wait (lock, [&] { return release_target; });
          return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "target failed after handoff");
      });
    const auto blocked_terminal = blocked_publish.async ().result ();
    if (!blocked_terminal) {
        return 13;
    }
    {
        std::unique_lock lock (publish_mutex);
        if (!publish_changed.wait_for (
              lock, std::chrono::seconds (1), [&] { return publish_started; })) {
            return 14;
        }
        release_target = true;
    }
    publish_changed.notify_all ();

    std::mutex failed_mutex;
    std::condition_variable failed_changed;
    bool failed_target_ran = false;
    zlink::framework::publish_call_t failed_publish (
      [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
          {
              std::lock_guard lock (failed_mutex);
              failed_target_ran = true;
          }
          failed_changed.notify_all ();
          return zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "post-start failure is internal");
      });
    const auto failed_terminal = failed_publish.async ().result ();
    if (!failed_terminal) {
        return 15;
    }
    {
        std::unique_lock lock (failed_mutex);
        if (!failed_changed.wait_for (
              lock, std::chrono::seconds (1), [&] { return failed_target_ran; })) {
            return 16;
        }
    }

    const zlink::framework::detail::ambient_context_hooks_t test_ambient_hooks{
      &capture_test_ambient, &enter_test_ambient};
    zlink::framework::runtime::configure_handler_coroutine_executor (1);
    zlink::framework::detail::ambient_context_hooks.store (
      &test_ambient_hooks, std::memory_order_release);
    native_await_ambient_value = 42;
    auto native_state = std::make_shared<native_async_state_t> ();
    auto serial_turn = std::make_shared<test_serial_turn_t> ();
    std::atomic<int> observed_ambient{0};
    std::atomic<bool> observed_serial_turn{false};
    std::atomic<bool> serial_resumed_inside_completion{true};
    auto native_task = [&] {
        zlink::framework::detail::serial_turn_scope_t scope (serial_turn);
        return await_native_binding_result (
          native_state, serial_turn, observed_ambient,
          observed_serial_turn, serial_resumed_inside_completion);
    } ();
    std::thread completion_thread ([native_state] {
        native_await_ambient_value = 999;
        inside_native_completion = true;
        native_state->complete (700);
        inside_native_completion = false;
    });
    completion_thread.join ();
    if (native_task.result ().value () != 700
        || observed_ambient.load (std::memory_order_acquire) != 42
        || !observed_serial_turn.load (std::memory_order_acquire)
        || serial_resumed_inside_completion.load (std::memory_order_acquire)
        || serial_turn->released ()) {
        return 17;
    }
    zlink::framework::detail::ambient_context_hooks.store (
      nullptr, std::memory_order_release);

    auto unowned_native_state = std::make_shared<native_async_state_t> ();
    std::atomic<bool> resumed_inside_completion{true};
    auto unowned_native_task = await_native_binding_outside_completion (
      unowned_native_state, resumed_inside_completion);
    std::thread unowned_completion_thread ([unowned_native_state] {
        inside_native_completion = true;
        unowned_native_state->complete (701);
        inside_native_completion = false;
    });
    unowned_completion_thread.join ();
    if (unowned_native_task.result ().value () != 701
        || resumed_inside_completion.load (std::memory_order_acquire)) {
        return 18;
    }

    /* Regression pin for the request_erased retry loop
     * (actor_client.cpp): the admission-retry delay used to
     * std::this_thread::sleep_for on the coroutine's own thread. It now
     * co_awaits detail::delay, which must (a) return control to the caller
     * without blocking anywhere near the requested duration, since the
     * suspend happens inside the coroutine machinery rather than the
     * executing thread, and (b) still resume the coroutine, with the
     * correct value, once the duration has actually elapsed. */
    const auto delay_call_start = std::chrono::steady_clock::now ();
    auto delayed_retry = await_async_delay (std::chrono::milliseconds (150), 77);
    const auto delay_call_returned = std::chrono::steady_clock::now ();
    if (delay_call_returned - delay_call_start >= std::chrono::milliseconds (100)) {
        return 19;
    }
    if (delayed_retry.result ().value () != 77) {
        return 20;
    }
    if (std::chrono::steady_clock::now () - delay_call_start
        < std::chrono::milliseconds (140)) {
        return 21;
    }

    auto abandoned_scheduler =
      zlink::framework::detail::capture_runtime_native_continuation_scheduler ();
    std::atomic<bool> abandoned_shutdown_done{false};
    std::thread abandoned_shutdown ([&] {
        zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        abandoned_shutdown_done.store (true, std::memory_order_release);
    });
    abandoned_shutdown.join ();
    if (!abandoned_shutdown_done.load (std::memory_order_acquire)) {
        return 22;
    }
    bool rejected_after_shutdown = false;
    bool rejected_work_ran = false;
    try {
        abandoned_scheduler ([&] { rejected_work_ran = true; });
    }
    catch (const std::runtime_error &) {
        rejected_after_shutdown = true;
    }
    abandoned_scheduler = {};
    if (!rejected_after_shutdown || rejected_work_ran) {
        return 23;
    }

    zlink::framework::runtime::configure_handler_coroutine_executor (1);
    auto queued_scheduler =
      zlink::framework::detail::capture_runtime_native_continuation_scheduler ();
    std::mutex queued_mutex;
    std::condition_variable queued_changed;
    bool queued_started = false;
    bool release_queued = false;
    queued_scheduler ([&] {
        std::unique_lock lock (queued_mutex);
        queued_started = true;
        queued_changed.notify_all ();
        queued_changed.wait (lock, [&] { return release_queued; });
    });
    queued_scheduler = {};
    {
        std::unique_lock lock (queued_mutex);
        if (!queued_changed.wait_for (
              lock, std::chrono::seconds (1), [&] { return queued_started; })) {
            release_queued = true;
            queued_changed.notify_all ();
            zlink::framework::runtime::shutdown_handler_coroutine_executor ();
            return 24;
        }
    }
    std::atomic<bool> queued_shutdown_done{false};
    std::thread queued_shutdown ([&] {
        zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        queued_shutdown_done.store (true, std::memory_order_release);
    });
    std::this_thread::sleep_for (std::chrono::milliseconds (25));
    if (queued_shutdown_done.load (std::memory_order_acquire)) {
        {
            std::lock_guard lock (queued_mutex);
            release_queued = true;
        }
        queued_changed.notify_all ();
        queued_shutdown.join ();
        return 25;
    }
    {
        std::lock_guard lock (queued_mutex);
        release_queued = true;
    }
    queued_changed.notify_all ();
    queued_shutdown.join ();
    if (!queued_shutdown_done.load (std::memory_order_acquire)) {
        return 26;
    }

    return 0;
}
