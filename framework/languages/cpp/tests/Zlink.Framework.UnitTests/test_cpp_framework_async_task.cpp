/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/channels/call.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include "runtime/timers/async_delay.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"

#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdlib>
#include <string>
#include <deque>
#include <functional>
#include <iostream>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
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
    return std::make_shared<ambient_guard_t> (*std::static_pointer_cast<int> (snapshot));
}

class test_serial_turn_t final : public zlink::framework::detail::serial_turn_t
{
  public:
    explicit test_serial_turn_t (std::stop_token cancellation = {}) : cancellation (cancellation) {}
    std::stop_token wait_cancellation () const override { return cancellation; }
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
    bool belongs_to (const void *owner) const noexcept override { return owner == this; }
    bool is_after_active_phase () const noexcept override { return false; }
    bool allows_yield () const noexcept override { return true; }
    zlink::framework::result_t<void> defer (std::function<void ()> work,
                                            std::function<void ()>) override
    {
        work ();
        return zlink::framework::result_t<void>::success ();
    }
    void cancel_deferred () noexcept override {}

  private:
    bool released_value = false;
    std::stop_token cancellation;
};

class native_async_state_t final : public zlink::detail::async_result_state_t<int>
{
  public:
    bool ready () const noexcept override
    {
        std::lock_guard<std::mutex> lock (mutex);
        return terminal;
    }

    bool suspend (std::coroutine_handle<> continuation_,
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
    void detach () noexcept override {}
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

zlink::framework::task_t<int>
await_native_binding_result (const std::shared_ptr<native_async_state_t> &state,
                             const std::shared_ptr<test_serial_turn_t> &turn,
                             std::atomic<int> &observed_ambient,
                             std::atomic<bool> &observed_serial_turn,
                             std::atomic<bool> &resumed_inside_completion)
{
    const int value = co_await zlink::detail::async_result_access_t::make<int> (state);
    resumed_inside_completion.store (inside_native_completion, std::memory_order_release);
    observed_ambient.store (native_await_ambient_value, std::memory_order_release);
    observed_serial_turn.store (zlink::framework::detail::capture_current_serial_turn () == turn,
                                std::memory_order_release);
    co_return value;
}

zlink::framework::task_t<int>
await_native_binding_outside_completion (const std::shared_ptr<native_async_state_t> &state,
                                         std::atomic<bool> &resumed_inside_completion)
{
    const int value = co_await zlink::detail::async_result_access_t::make<int> (state);
    resumed_inside_completion.store (inside_native_completion, std::memory_order_release);
    co_return value;
}

zlink::framework::task_t<int> await_shared (zlink::framework::task_t<int> &task, int offset)
{
    const auto value = co_await task;
    co_return value + offset;
}

zlink::framework::task_t<int> timeout_task ()
{
    co_return zlink::framework::detail::boundary_failure<int> (
      zlink::framework::detail::boundary_error_t::timed_out, "timeout preserved");
}

zlink::framework::task_t<int> await_timeout ()
{
    auto task = timeout_task ();
    co_return co_await task;
}

zlink::framework::task_t<int> await_async_delay (std::chrono::milliseconds duration, int value)
{
    co_await zlink::framework::detail::delay (duration);
    co_return value;
}

zlink::framework::task_t<void> observe_void_task (zlink::framework::task_t<void> task)
{
    co_await task;
}

zlink::framework::task_t<void> observe_void_with_lifetime (zlink::framework::task_t<void> task,
                                                           std::shared_ptr<int> lifetime)
{
    co_await task;
    (void) lifetime;
}

struct before_registration_t
{
    zlink::framework::task_t<void> task;
    std::function<void ()> before_registration;

    bool await_ready ()
    {
        const bool ready = task.await_ready ();
        before_registration ();
        return ready;
    }
    void await_suspend (std::coroutine_handle<> continuation) { task.await_suspend (continuation); }
    void await_resume () { task.await_resume (); }
};

zlink::framework::task_t<void>
observe_registration_boundary (zlink::framework::task_t<void> task,
                               std::function<void ()> before_registration)
{
    co_await before_registration_t{std::move (task), std::move (before_registration)};
}

bool verify_rejected_delivery_lifetime ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    auto source = std::make_unique<task_completion_source_t<void>> ();
    std::optional<task_t<void>> original (source->task ());
    auto outside = observe_void_task (source->task ());
    auto marker = std::make_shared<int> (1);
    std::weak_ptr<int> lifetime = marker;
    auto view = detail::with_task_resume_scheduler (source->task (), [] (std::function<void ()>) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "executor rejected before owner cancellation");
    });
    auto owned = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return observe_void_with_lifetime (std::move (view), std::move (marker));
    }();
    int terminal_calls = 0;
    detail::observe_task_completion (owned, [&] (const result_t<void> &) { ++terminal_calls; });
    bool first = false, duplicate = true;
    std::exception_ptr failure;
    std::thread completion ([&] {
        try {
            first = source->complete (result_t<void>::success ());
            duplicate = source->complete (result_t<void>::success ());
        }
        catch (...) {
            failure = std::current_exception ();
        }
    });
    completion.join ();
    if (failure || !first || duplicate || !original->result () || !outside.result ()
        || owned.result_for (std::chrono::milliseconds (0)) || terminal_calls != 0)
        return false;
    original.reset ();
    source.reset ();
    if (lifetime.expired ())
        return false;
    owner.request_stop ();
    const auto cancelled = owned.result ();
    owner.request_stop ();
    return !cancelled && cancelled.error_kind () == framework_error_kind_t::shutting_down
           && terminal_calls == 1 && lifetime.expired ();
}

bool verify_rejected_ready_registration ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    task_completion_source_t<void> source;
    auto view = detail::with_task_resume_scheduler (source.task (), [] (std::function<void ()>) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "ready delivery rejected before owner cancellation");
    });
    bool first = false;
    auto owned = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return observe_registration_boundary (
          std::move (view), [&] { first = source.complete (result_t<void>::success ()); });
    }();
    int terminal_calls = 0;
    detail::observe_task_completion (owned, [&] (const result_t<void> &) { ++terminal_calls; });
    if (!first || !source.task ().result () || source.complete (result_t<void>::success ())
        || owned.result_for (std::chrono::milliseconds (0)) || terminal_calls != 0)
        return false;
    owner.request_stop ();
    const auto cancelled = owned.result ();
    return !cancelled && cancelled.error_kind () == framework_error_kind_t::shutting_down
           && terminal_calls == 1;
}

bool verify_stopped_registration_state_lifetime ()
{
    using namespace zlink::framework;
    for (const bool complete : {false, true}) {
        std::stop_source owner;
        auto source = std::make_unique<task_completion_source_t<void>> ();
        auto task = source->task ();
        auto observed = [&] {
            const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
            return observe_registration_boundary (std::move (task), [&] {
                if (complete)
                    source->complete (result_t<void>::success ());
                source.reset ();
                owner.request_stop ();
            });
        }();
        const auto cancelled = observed.result ();
        if (source || cancelled || cancelled.error_kind () != framework_error_kind_t::shutting_down)
            return false;
    }
    return true;
}

// A wait registered outside any execution turn has no owner that can cancel
// it. When the context that captured its scheduler has stopped, the completing
// thread delivers the result instead of terminating the process.
bool verify_ownerless_wait_after_context_stop ()
{
    using namespace zlink::framework;
    task_completion_source_t<int> source;
    auto view = detail::with_task_resume_scheduler (source.task (), [] (std::function<void ()>) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "registered context stopped");
    });
    auto waiter = await_shared (view, 1);
    if (!source.complete (result_t<int>::success (41)))
        return false;
    const auto delivered = waiter.result_for (std::chrono::seconds (1));
    return delivered && delivered->value () == 42;
}

bool verify_cancelled_registration_lease ()
{
    using namespace zlink::framework;
    for (const bool complete : {false, true}) {
        std::stop_source owner;
        task_completion_source_t<void> source;
        auto marker = std::make_shared<int> (1);
        std::weak_ptr<int> scheduler_lease = marker;
        auto turn = std::make_shared<test_serial_turn_t> (owner.get_token ());
        std::weak_ptr<test_serial_turn_t> turn_lease = turn;
        auto view = detail::with_task_resume_scheduler (
          source.task (), [marker = std::move (marker)] (std::function<void ()>) {
              (void) marker;
              throw framework_exception_t (framework_error_kind_t::shutting_down,
                                           "scheduler rejects while owner is alive");
          });
        auto owned = [&] {
            const detail::serial_turn_scope_t scope (turn);
            return observe_void_task (std::move (view));
        }();
        turn.reset ();
        if (complete && !source.complete (result_t<void>::success ()))
            return false;
        if (turn_lease.expired () || scheduler_lease.expired ())
            return false;
        owner.request_stop ();
        const auto cancelled = owned.result ();
        if (cancelled || cancelled.error_kind () != framework_error_kind_t::shutting_down
            || !turn_lease.expired () || !scheduler_lease.expired ())
            return false;
        if (!complete && !source.complete (result_t<void>::success ()))
            return false;
        if (source.complete (result_t<void>::success ()) || !source.task ().result ())
            return false;
    }
    return true;
}

bool verify_observer_lifetime ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    std::weak_ptr<int> pending_marker;
    int pending_calls = 0;
    {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        task_completion_source_t<int> source;
        auto task = source.task ();
        auto marker = std::make_shared<int> (1);
        pending_marker = marker;
        detail::observe_task_completion (task, [marker, &pending_calls] (const result_t<int> &) {
            (void) marker;
            ++pending_calls;
        });
    }
    if (!pending_marker.expired () || pending_calls != 0)
        return false;
    owner.request_stop ();
    if (pending_calls != 0)
        return false;

    std::stop_source queued_owner;
    std::deque<std::function<void ()>> queue;
    std::weak_ptr<int> queued_marker;
    int queued_calls = 0;
    {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, queued_owner.get_token ());
        task_completion_source_t<int> source;
        auto task =
          detail::with_task_resume_scheduler (source.task (), [&] (std::function<void ()> work) {
              queue.push_back (std::move (work));
          });
        auto marker = std::make_shared<int> (18);
        queued_marker = marker;
        detail::observe_task_completion (
          task, [marker, &queued_calls] (const result_t<int> &result) {
              if (result.value () != *marker)
                  throw std::logic_error ("queued observer result was lost");
              ++queued_calls;
          });
        if (!source.complete (result_t<int>::success (18)))
            return false;
    }
    if (queued_marker.expired () || queue.size () != 1 || queued_calls != 0)
        return false;
    auto work = std::move (queue.front ());
    queue.pop_front ();
    work ();
    queued_owner.request_stop ();
    return queued_marker.expired () && queued_calls == 1;
}

bool verify_released_observer_owner ()
{
    using namespace zlink::framework;
    task_completion_source_t<int> pending_source, ready_source;
    auto pending = pending_source.task ();
    auto ready = ready_source.task ();
    ready_source.complete (result_t<int>::success (1));
    auto turn = std::make_shared<test_serial_turn_t> ();
    std::weak_ptr<test_serial_turn_t> released_turn = turn;
    turn->release ();
    int calls = 0;
    bool restored_released_turn = false;
    {
        const detail::serial_turn_scope_t scope (turn);
        detail::observe_task_completion (ready, [&] (const result_t<int> &) { ++calls; });
        detail::observe_task_completion (pending, [&] (const result_t<int> &) {
            restored_released_turn = static_cast<bool> (detail::capture_current_serial_turn ());
            ++calls;
        });
    }
    turn.reset ();
    if (!released_turn.expired ())
        return false;
    pending_source.complete (result_t<int>::success (1));
    return calls == 2 && !restored_released_turn;
}

bool verify_registration_completion_cancellation_race ()
{
    using namespace zlink::framework;
    constexpr int race_cases = 32;
    for (int i = 0; i != race_cases; ++i) {
        std::stop_source owner;
        task_completion_source_t<void> source;
        std::barrier start (4);
        std::optional<task_t<void>> observed;
        std::atomic_int observer_calls{0};
        bool first = false, duplicate = true;
        std::exception_ptr completion_failure;
        std::thread registration ([&] {
            const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
            start.arrive_and_wait ();
            auto task = source.task ();
            detail::observe_task_completion (task,
                                             [&] (const result_t<void> &) { ++observer_calls; });
            observed.emplace (observe_void_task (std::move (task)));
        });
        std::thread completion ([&] {
            start.arrive_and_wait ();
            try {
                first = source.complete (result_t<void>::success ());
                duplicate = source.complete (result_t<void>::success ());
            }
            catch (...) {
                completion_failure = std::current_exception ();
            }
        });
        std::thread cancellation ([&] {
            start.arrive_and_wait ();
            owner.request_stop ();
        });
        start.arrive_and_wait ();
        registration.join ();
        completion.join ();
        cancellation.join ();
        const auto result = observed->result ();
        if (completion_failure || !first || duplicate || observer_calls != 1
            || (!result && result.error_kind () != framework_error_kind_t::shutting_down)
            || !source.task ().result ())
            return false;
    }
    return true;
}

struct nested_cleanup_probe_t
{
    bool child_finished = false;
    bool child_before_parent = false;
    int parent_observations = 0;
};

// The child's destructor writes parent-owned memory, so the parent must
// resume only after the child's scope is gone.
struct nested_child_cleanup_t
{
    int &parent_value;
    nested_cleanup_probe_t &probe;
    ~nested_child_cleanup_t ()
    {
        parent_value = 42;
        probe.child_finished = true;
    }
};

zlink::framework::task_t<void>
cleanup_nested_child (zlink::framework::task_completion_source_t<void> &source,
                      int &parent_value,
                      nested_cleanup_probe_t &probe)
{
    nested_child_cleanup_t cleanup{parent_value, probe};
    auto pending = source.task ();
    try {
        co_await pending;
    }
    catch (const zlink::framework::framework_exception_t &error) {
        if (error.kind () != zlink::framework::framework_error_kind_t::shutting_down)
            throw;
    }
    co_return;
}

zlink::framework::task_t<int>
return_nested_child (zlink::framework::task_completion_source_t<void> &source,
                     int &parent_value,
                     nested_cleanup_probe_t &probe)
{
    nested_child_cleanup_t cleanup{parent_value, probe};
    auto pending = source.task ();
    co_await pending;
    co_return 7;
}

zlink::framework::task_t<void>
cleanup_nested_parent (zlink::framework::task_completion_source_t<void> &source,
                       nested_cleanup_probe_t &probe)
{
    auto parent_value = std::make_unique<int> (0);
    auto child = cleanup_nested_child (source, *parent_value, probe);
    zlink::framework::detail::observe_task_completion (
      child, [&probe] (const zlink::framework::result_t<void> &result) {
          if (result && probe.child_finished)
              ++probe.parent_observations;
      });
    co_await child;
    probe.child_before_parent = probe.child_finished && *parent_value == 42;
}

bool verify_nested_cancellation_lifetime ()
{
    using namespace zlink::framework;
    std::stop_source owner;
    task_completion_source_t<void> source;
    nested_cleanup_probe_t probe;
    auto parent = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return cleanup_nested_parent (source, probe);
    }();
    owner.request_stop ();
    const auto result = parent.result_for (std::chrono::milliseconds::zero ());
    const bool original_pending = !source.task ().result_for (std::chrono::milliseconds::zero ());
    const bool first = source.complete (result_t<void>::success ());
    const bool duplicate = source.complete (result_t<void>::success ());
    return result && *result && probe.child_before_parent && probe.parent_observations == 1
           && original_pending && first && !duplicate;
}

// An explicit co_return publishes only after the child's locals are destroyed.
zlink::framework::task_t<void>
return_nested_parent (zlink::framework::task_completion_source_t<void> &void_source,
                      zlink::framework::task_completion_source_t<void> &typed_source,
                      nested_cleanup_probe_t &probe)
{
    auto void_value = std::make_unique<int> (0);
    auto void_child = cleanup_nested_child (void_source, *void_value, probe);
    co_await void_child;
    const bool void_ordered = probe.child_finished && *void_value == 42;
    probe.child_finished = false;
    auto typed_value = std::make_unique<int> (0);
    auto typed_child = return_nested_child (typed_source, *typed_value, probe);
    const auto returned = co_await typed_child;
    probe.child_before_parent =
      void_ordered && probe.child_finished && *typed_value == 42 && returned == 7;
}

bool verify_nested_return_lifetime ()
{
    using namespace zlink::framework;
    task_completion_source_t<void> void_source;
    task_completion_source_t<void> typed_source;
    nested_cleanup_probe_t probe;
    auto parent = return_nested_parent (void_source, typed_source, probe);
    const bool void_completed = void_source.complete (result_t<void>::success ());
    const bool typed_completed = typed_source.complete (result_t<void>::success ());
    const auto result = parent.result_for (std::chrono::milliseconds::zero ());
    return void_completed && typed_completed && result && *result && probe.child_before_parent;
}

bool verify_void_completion_rejection_race (bool cancel_before_rejection)
{
    using namespace zlink::framework;
    std::stop_source owner;
    task_completion_source_t<void> trigger;
    // cancellation-and-shutdown §5.1: only the application-source waiter may
    // finish independently when its delivery owner shuts down.
    auto producer = trigger.task ();
    std::promise<void> submit_entered, allow_rejection;
    auto entered = submit_entered.get_future ();
    auto reject = allow_rejection.get_future ();
    bool producer_succeeded = false;
    detail::observe_task_completion (producer, [&] (const result_t<void> &result) {
        producer_succeeded = static_cast<bool> (result);
    });
    auto rejected_view =
      detail::with_task_resume_scheduler (std::move (producer), [&] (std::function<void ()>) {
          submit_entered.set_value ();
          reject.wait ();
          throw framework_exception_t (framework_error_kind_t::shutting_down,
                                       "executor stopped after owner cancellation");
      });
    auto owned = [&] {
        const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
        return observe_void_task (std::move (rejected_view));
    }();
    auto outside = observe_void_task (trigger.task ());
    bool first = false, duplicate = true;
    std::exception_ptr failure;
    std::thread completing ([&] {
        try {
            first = trigger.complete (result_t<void>::success ());
            duplicate = trigger.complete (result_t<void>::success ());
        }
        catch (...) {
            failure = std::current_exception ();
        }
    });
    entered.wait ();
    if (cancel_before_rejection)
        owner.request_stop ();
    allow_rejection.set_value ();
    completing.join ();
    if (!cancel_before_rejection) {
        if (owned.result_for (std::chrono::milliseconds (0)))
            return false;
        owner.request_stop ();
    }
    const auto cancelled = owned.result ();
    return !failure && first && !duplicate && !cancelled
           && cancelled.error_kind () == framework_error_kind_t::shutting_down && producer_succeeded
           && outside.result ();
}

bool verify_rejection_retain_cancellation_race ()
{
    using namespace zlink::framework;
    for (int i = 0; i != 32; ++i) {
        std::stop_source owner;
        task_completion_source_t<void> source;
        std::barrier rejection (2);
        auto view =
          detail::with_task_resume_scheduler (source.task (), [&] (std::function<void ()>) {
              rejection.arrive_and_wait ();
              throw framework_exception_t (framework_error_kind_t::shutting_down,
                                           "executor rejection races owner cancellation");
          });
        auto owned = [&] {
            const detail::ambient_context_scope_t scope (nullptr, nullptr, owner.get_token ());
            return observe_void_task (std::move (view));
        }();
        auto outside = observe_void_task (source.task ());
        std::atomic_int terminal_calls{0};
        detail::observe_task_completion (owned, [&] (const result_t<void> &) { ++terminal_calls; });
        std::thread cancellation ([&] {
            rejection.arrive_and_wait ();
            owner.request_stop ();
        });
        bool first = false;
        std::exception_ptr failure;
        try {
            first = source.complete (result_t<void>::success ());
        }
        catch (...) {
            failure = std::current_exception ();
        }
        cancellation.join ();
        const auto cancelled = owned.result ();
        if (failure || !first || source.complete (result_t<void>::success ()) || cancelled
            || cancelled.error_kind () != framework_error_kind_t::shutting_down
            || terminal_calls != 1 || !outside.result () || !source.task ().result ())
            return false;
    }
    return true;
}

enum class delivery_defect_t
{
    pending_observer,
    ready_observer,
    terminal_observer,
    scheduled_observer,
    scheduled_after_delivery,
    owned_observer,
    cancellation_observer,
    queue_cancellation,
    reservation_transfer,
    native_continuation,
    count
};
constexpr int delivery_defect_exit = 86;

void run_delivery_defect (delivery_defect_t defect)
{
    using namespace zlink::framework;
    std::set_terminate ([] { std::_Exit (delivery_defect_exit); });
    if (defect == delivery_defect_t::native_continuation) {
        runtime::coroutine_executor_t executor (1);
        executor.post_native_continuation (
          [] { throw std::logic_error ("native continuation delivery defect"); });
        executor.drain ();
        return;
    }
    if (defect == delivery_defect_t::queue_cancellation
        || defect == delivery_defect_t::reservation_transfer) {
        runtime::offload_executor_t executor;
        runtime::serial_execution_queue_t queue (executor);
        if (defect == delivery_defect_t::reservation_transfer) {
            runtime::serial_work_options_t options;
            options.transfer_owner_reservation = [] {
                throw std::logic_error ("reservation transfer defect");
            };
            queue.try_post_async (
              "reservation-defect", [] (auto complete) { complete ([] {}); }, options);
        } else {
            std::promise<void> started;
            auto running = started.get_future ();
            runtime::serial_execution_queue_t::async_completion_t finish;
            const auto submission = queue.try_post_cancellable_async (
              "cancellation-defect",
              [&] (auto complete) {
                  finish = std::move (complete);
                  started.set_value ();
              },
              [] { throw std::logic_error ("cancellation delivery defect"); });
            if (!submission)
                return;
            running.get ();
            queue.cancel_submission (submission.value ());
            finish ([] {});
        }
        return;
    }
    task_completion_source_t<void> source;
    auto task = source.task ();
    std::stop_source owner;
    const detail::ambient_context_scope_t scope (
      nullptr, nullptr,
      defect == delivery_defect_t::owned_observer
          || defect == delivery_defect_t::cancellation_observer
          || defect == delivery_defect_t::scheduled_after_delivery
        ? owner.get_token ()
        : std::stop_token{});
    const auto callback = [] (const result_t<void> &) {
        throw framework_exception_t (framework_error_kind_t::shutting_down,
                                     "internal observer defect");
    };
    switch (defect) {
        case delivery_defect_t::ready_observer:
            source.complete (result_t<void>::success ());
            detail::observe_task_completion (task, callback);
            break;
        case delivery_defect_t::terminal_observer:
            detail::observe_task_terminal (task, callback);
            break;
        case delivery_defect_t::scheduled_observer:
            task = detail::reschedule_task (std::move (task), [] (std::function<void ()>) {
                throw std::logic_error ("internal scheduler defect");
            });
            detail::observe_task_completion (task, [] (const result_t<void> &) {});
            break;
        case delivery_defect_t::scheduled_after_delivery:
            task = detail::with_task_resume_scheduler (
              std::move (task), [] (std::function<void ()> work) {
                  work ();
                  throw framework_exception_t (framework_error_kind_t::shutting_down,
                                               "scheduler failed after delivery");
              });
            {
                auto observed = observe_void_task (std::move (task));
                source.complete (result_t<void>::success ());
                (void) observed;
                return;
            }
        default:
            detail::observe_task_completion (task, callback);
            break;
    }
    if (defect == delivery_defect_t::cancellation_observer)
        owner.request_stop ();
    else
        source.complete (result_t<void>::success ());
}

bool verify_delivery_defects (const char *executable)
{
    for (int defect = 0; defect < static_cast<int> (delivery_defect_t::count); ++defect) {
        const auto command =
          std::string ("\"") + executable + "\" --delivery-defect " + std::to_string (defect);
        const auto status = std::system (command.c_str ());
#ifdef _WIN32
        if (status != delivery_defect_exit)
#else
        if (status != delivery_defect_exit * 256)
#endif
            return false;
    }
    return true;
}

} // namespace

namespace
{

template <typename T> bool has_standard_cancellation (const zlink::framework::result_t<T> &result)
{
    using namespace zlink::framework;
    if (result || result.error () != nullptr || !result.exception ())
        return false;
    try {
        (void) result.error_kind ();
        return false;
    }
    catch (const framework_exception_t &error) {
        if (error.kind () != framework_error_kind_t::invalid_operation)
            return false;
    }
    try {
        result.value ();
        return false;
    }
    catch (const std::system_error &error) {
        return error.code () == std::errc::operation_canceled;
    }
}

zlink::framework::task_t<int> throw_standard_cancellation ()
{
    throw std::system_error (std::make_error_code (std::errc::operation_canceled));
    co_return 0;
}

bool verify_cancellation_contract ()
{
    using namespace zlink::framework;
    const auto original = detail::make_cancellation_exception ("request cancelled");
    try {
        std::rethrow_exception (original);
        return false;
    }
    catch (const std::system_error &error) {
        if (!detail::is_cancellation_exception (error))
            return false;
    }
    const auto cancelled = detail::result_access_t::failure<int> (original);
    if (cancelled.exception () != original)
        return false;
    if (detail::is_cancellation_exception (
          std::system_error (std::make_error_code (std::errc::timed_out))))
        return false;
    for (const auto state :
         {detail::boundary_error_t::timed_out, detail::boundary_error_t::shutdown,
          detail::boundary_error_t::disconnected, detail::boundary_error_t::closed,
          detail::boundary_error_t::stale_generation}) {
        auto error = detail::make_boundary_exception (state, "boundary failure");
        if (detail::is_cancellation_exception (error))
            return false;
        error = detail::with_error_origin (std::move (error), detail::error_origin_t::framework);
        const auto pointer = std::make_exception_ptr (error);
        const auto boundary = detail::result_access_t::failure<int> (pointer);
        if (boundary.exception () != pointer || !boundary.error ()
            || boundary.error ()->kind () != error.kind ()
            || boundary.error ()->code () != error.code ()
            || detail::boundary_state (*boundary.error ()) != state
            || detail::error_origin (*boundary.error ()) != detail::error_origin_t::framework)
            return false;
    }
    if (!has_standard_cancellation (cancelled))
        return false;
    auto mutable_cancelled = cancelled;
    try {
        mutable_cancelled.value ();
        return false;
    }
    catch (const std::system_error &error) {
        if (error.code () != std::errc::operation_canceled)
            return false;
    }
    const auto propagated = detail::propagate_failure<void> (cancelled, "must retain cancellation");
    if (!has_standard_cancellation (propagated)
        || propagated.exception () != cancelled.exception ())
        return false;
    request_call_t<int> request (cancelled);
    send_call_t send (propagated);
    send_call_t submit ("test.cancelled", [] (const auto &, const auto &) {
        return detail::result_access_t::failure<void> (
          detail::make_cancellation_exception ("submit cancelled"));
    });
    if (!has_standard_cancellation (request.async ().result ())
        || !has_standard_cancellation (send.async ().result ())
        || !has_standard_cancellation (submit.async ().result ())
        || !has_standard_cancellation (throw_standard_cancellation ().result ()))
        return false;
    auto awaiting = request.async ();
    if (!has_standard_cancellation (await_shared (awaiting, 0).result ()))
        return false;
    const auto success = result_t<int>::success (1);
    if (success.error () || success.exception ())
        return false;
    try {
        (void) success.error_kind ();
        return false;
    }
    catch (const framework_exception_t &error) {
        if (error.kind () != framework_error_kind_t::invalid_operation)
            return false;
    }
    const auto failure = result_t<int>::failure (framework_error_kind_t::invalid_operation,
                                                 "invalid invocation is not cancellation");
    return failure.error () && failure.exception ()
           && failure.error_kind () == framework_error_kind_t::invalid_operation;
}

struct yield_resume_probe_t
{
    std::atomic_bool running{false};
    std::atomic_bool overlapped{false};
    std::atomic_bool ran_after_await{false};
    std::atomic_int failure_kind{-1};
    std::promise<void> done;
};

zlink::framework::task_t<void>
yielding_lifecycle_handler (zlink::framework::request_call_t<int> call,
                            zlink::framework::runtime::serial_execution_queue_t &queue,
                            yield_resume_probe_t &probe)
{
    using namespace zlink::framework;
    try {
        (void) co_await call.yield ();
    }
    catch (const framework_exception_t &error) {
        probe.failure_kind.store (static_cast<int> (error.kind ()));
        probe.done.set_value ();
        co_return;
    }
    probe.ran_after_await.store (true);
    probe.running.store (true);
    (void) queue.try_post ("yield-probe-application",
                           [&probe] { probe.overlapped.store (probe.running.load ()); });
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    probe.running.store (false);
    probe.done.set_value ();
}

/* Handler turn and execution gate §3·§4: a Yield continuation resumes in a new
 * turn of the same Spot queue, and a Spot that closes during the wait ends the
 * handler with a failure instead of resuming it. */
bool verify_yield_continuation_owned_by_spot_queue (bool close_before_reply)
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;
    runtime::offload_executor_t executor (2);
    runtime::serial_execution_queue_t queue (executor, runtime::serial_execution_queue_options_t{},
                                             runtime::serial_execution_queue_t::error_handler_t{},
                                             runtime::serial_lane_policy_t::spot_wide ());
    task_completion_source_t<int> reply;
    std::promise<void> submitted;
    yield_resume_probe_t probe;
    std::optional<task_t<void>> handler;
    request_call_t<int> call ("test.yield.leave",
                              [&] (const std::string &, std::chrono::milliseconds,
                                   const request_call_t<int>::metadata_map_t &) {
                                  submitted.set_value ();
                                  return reply.task ();
                              });
    queue.post_async (
      "yielding-leave",
      [&] (auto complete) {
          handler.emplace (yielding_lifecycle_handler (call, queue, probe));
          complete ([] {});
      },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
    if (submitted.get_future ().wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        return false;
    if (close_before_reply)
        queue.close ();
    reply.complete (result_t<int>::success (1));
    if (probe.done.get_future ().wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        return false;
    queue.drain ();
    if (close_before_reply) {
        if (probe.ran_after_await.load ()
            || probe.failure_kind.load ()
                 != static_cast<int> (framework_error_kind_t::shutting_down))
            std::cerr << "closed Spot resumed its yielded handler after_await="
                      << probe.ran_after_await.load () << " kind=" << probe.failure_kind.load ()
                      << '\n';
        return !probe.ran_after_await.load ()
               && probe.failure_kind.load ()
                    == static_cast<int> (framework_error_kind_t::shutting_down);
    }
    if (!probe.ran_after_await.load () || probe.overlapped.load ())
        std::cerr << "yield continuation left its Spot turn after_await="
                  << probe.ran_after_await.load () << " overlapped=" << probe.overlapped.load ()
                  << '\n';
    return probe.ran_after_await.load () && !probe.overlapped.load ();
}

} // namespace

int main (int argc, char **argv)
{
    if (!verify_cancellation_contract ())
        return 90;

    // This test drives runtime parts without a host.
    zlink::framework::runtime::install_host_context_hooks ();
    if (argc == 3 && std::string (argv[1]) == "--delivery-defect") {
        run_delivery_defect (static_cast<delivery_defect_t> (std::atoi (argv[2])));
        return 1;
    }
    if (!verify_delivery_defects (argv[0]))
        return 30;
    zlink::framework::task_completion_source_t<int> shared_source;
    auto shared = shared_source.task ();
    auto first_waiter = await_shared (shared, 1);
    auto second_waiter = await_shared (shared, 2);
    std::thread shared_completion (
      [&] { shared_source.complete (zlink::framework::result_t<int>::success (40)); });
    shared_completion.join ();
    if (first_waiter.result ().value () != 41 || second_waiter.result ().value () != 42) {
        return 1;
    }

    zlink::framework::task_completion_source_t<int> completion;
    auto first_complete_wins = completion.task ();
    int callback_count = 0;
    int callback_value = 0;
    zlink::framework::detail::observe_task_completion (
      first_complete_wins,
      [&callback_count, &callback_value] (const zlink::framework::result_t<int> &result) {
          ++callback_count;
          callback_value = result.value ();
      });
    const bool first_completion =
      completion.complete (zlink::framework::result_t<int>::success (100));
    const bool duplicate_completion =
      completion.complete (zlink::framework::result_t<int>::success (200));
    if (!first_completion || duplicate_completion || first_complete_wins.result ().value () != 100
        || callback_count != 1 || callback_value != 100) {
        return 2;
    }

    zlink::framework::request_call_t<int> call (zlink::framework::detail::boundary_failure<int> (
      zlink::framework::detail::boundary_error_t::timed_out, "timeout"));
    auto coroutine_result = call.async ().result ();
    if ((coroutine_result.error () != nullptr
         && zlink::framework::detail::boundary_state (*coroutine_result.error ())
              != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 3;
    }

    const auto preserved_failure = await_timeout ().result ();
    if (preserved_failure
        || (preserved_failure.error () != nullptr
            && zlink::framework::detail::boundary_state (*preserved_failure.error ())
                 != zlink::framework::detail::boundary_error_t::timed_out)) {
        return 4;
    }

    zlink::framework::request_call_t<int> shutdown_call (
      zlink::framework::detail::boundary_failure<int> (
        zlink::framework::detail::boundary_error_t::shutdown, "shutdown"));
    const auto shutdown_result = shutdown_call.async ().result ();
    if (shutdown_result || shutdown_result.error () == nullptr
        || zlink::framework::detail::boundary_state (*shutdown_result.error ())
             != zlink::framework::detail::boundary_error_t::shutdown) {
        return 5;
    }

    std::deque<std::function<void ()>> rescheduled_work;
    auto immediate_task =
      zlink::framework::task_t<int> (zlink::framework::result_t<int>::success (500));
    auto rescheduled_task = zlink::framework::detail::reschedule_task (
      std::move (immediate_task), [&rescheduled_work] (std::function<void ()> work) {
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
      "test.command", [&one_way_invoked] (const std::string &,
                                          const zlink::framework::send_call_t::metadata_map_t &) {
          one_way_invoked = true;
          return zlink::framework::result_t<void>::success ();
      });
    const auto accepted_result = accepted.async ().result ();
    if (!accepted_result || !one_way_invoked) {
        return 10;
    }

    zlink::framework::send_call_t timed_out (zlink::framework::detail::boundary_failure<void> (
      zlink::framework::detail::boundary_error_t::timed_out, "send timed out"));
    const auto timed_out_result = timed_out.async ().result ();
    if (timed_out_result
        || timed_out_result.error_kind ()
             != zlink::framework::framework_error_kind_t::deadline_exceeded) {
        return 11;
    }

    zlink::framework::send_call_t disconnected (zlink::framework::detail::boundary_failure<void> (
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
        if (!publish_changed.wait_for (lock, std::chrono::seconds (1),
                                       [&] { return publish_started; })) {
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
        if (!failed_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return failed_target_ran; })) {
            return 16;
        }
    }

    const zlink::framework::detail::ambient_context_hooks_t test_ambient_hooks{
      &capture_test_ambient, &enter_test_ambient};
    zlink::framework::runtime::configure_handler_coroutine_executor (1);
    zlink::framework::detail::set_ambient_context_hooks (&test_ambient_hooks);
    native_await_ambient_value = 42;
    auto native_state = std::make_shared<native_async_state_t> ();
    auto serial_turn = std::make_shared<test_serial_turn_t> ();
    std::atomic<int> observed_ambient{0};
    std::atomic<bool> observed_serial_turn{false};
    std::atomic<bool> serial_resumed_inside_completion{true};
    auto native_task = [&] {
        zlink::framework::detail::serial_turn_scope_t scope (serial_turn);
        return await_native_binding_result (native_state, serial_turn, observed_ambient,
                                            observed_serial_turn, serial_resumed_inside_completion);
    }();
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
    zlink::framework::detail::set_ambient_context_hooks (nullptr);

    auto unowned_native_state = std::make_shared<native_async_state_t> ();
    std::atomic<bool> resumed_inside_completion{true};
    auto unowned_native_task =
      await_native_binding_outside_completion (unowned_native_state, resumed_inside_completion);
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
    if (std::chrono::steady_clock::now () - delay_call_start < std::chrono::milliseconds (140)) {
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
    catch (const zlink::framework::framework_exception_t &error) {
        rejected_after_shutdown =
          error.kind () == zlink::framework::framework_error_kind_t::shutting_down;
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
        if (!queued_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return queued_started; })) {
            release_queued = true;
            queued_changed.notify_all ();
            zlink::framework::runtime::shutdown_handler_coroutine_executor ();
            return 24;
        }
    }
    std::atomic<bool> queued_shutdown_done{false};
    std::promise<void> queued_shutdown_started;
    auto shutdown_entered = queued_shutdown_started.get_future ();
    std::thread queued_shutdown ([&] {
        queued_shutdown_started.set_value ();
        zlink::framework::runtime::shutdown_handler_coroutine_executor ();
        queued_shutdown_done.store (true, std::memory_order_release);
    });
    shutdown_entered.wait ();
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

    zlink::framework::task_completion_source_t<int> observed_completion;
    auto observed_task = observed_completion.task ();
    if (observed_task.result_for (std::chrono::milliseconds (0)))
        return 27;
    if (!observed_completion.complete (zlink::framework::result_t<int>::success (123))
        || observed_completion.complete (zlink::framework::result_t<int>::success (456)))
        return 28;
    const auto observed_result = observed_task.result_for (std::chrono::milliseconds (0));
    if (!observed_result || observed_result->value () != 123)
        return 29;

    if (!verify_observer_lifetime () || !verify_released_observer_owner ()
        || !verify_registration_completion_cancellation_race ()
        || !verify_nested_cancellation_lifetime () || !verify_nested_return_lifetime ()
        || !verify_void_completion_rejection_race (true)
        || !verify_void_completion_rejection_race (false) || !verify_rejected_delivery_lifetime ()
        || !verify_rejected_ready_registration () || !verify_rejection_retain_cancellation_race ()
        || !verify_cancelled_registration_lease () || !verify_stopped_registration_state_lifetime ()
        || !verify_ownerless_wait_after_context_stop ())
        return 30;

    zlink::framework::runtime::configure_handler_coroutine_executor (2);
    const bool yield_kept_turn = verify_yield_continuation_owned_by_spot_queue (false);
    const bool closed_yield_failed = verify_yield_continuation_owned_by_spot_queue (true);
    zlink::framework::runtime::shutdown_handler_coroutine_executor ();
    if (!yield_kept_turn)
        return 31;
    if (!closed_yield_failed)
        return 32;
    return 0;
}
