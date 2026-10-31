/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include "../support/task_allocation_counter.hpp"

#include <condition_variable>
#include <array>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <thread>

using namespace zlink::framework;

namespace
{
thread_local bool count_sync = false;
thread_local unsigned mutex_locks = 0;
thread_local unsigned notifications = 0;

void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}

void begin_count ()
{
    mutex_locks = 0;
    notifications = 0;
    count_sync = true;
}

void end_count ()
{
    count_sync = false;
}

template <typename T> task_t<T> make_ready (result_t<T> result, const char *name)
{
    begin_count ();
    std::optional<task_t<T>> task;
    const auto allocations = tests::count_allocations ([&] { task.emplace (std::move (result)); });
    end_count ();
    std::cout << name << ": allocations=" << allocations << " locks=" << mutex_locks
              << " notifications=" << notifications << '\n';
    require (allocations == 0, "ready task allocated a shared result state");
    require (mutex_locks == 0, "ready task construction locked an unpublished result");
    require (notifications == 0, "ready task construction notified without a waiter");
    require (task->await_ready (), "ready task was not ready");
    return std::move (*task);
}

void check_ready_conversion_chain ()
{
    const std::array shared_result_sites{
      tests::shared_state_allocation_site<detail::task_shared_state_t<int>> (),
      tests::shared_state_allocation_site<detail::application_task_shared_state_t<long>> (),
      tests::shared_state_allocation_site<detail::application_task_shared_state_t<void>> ()};
    if constexpr (tests::shared_state_stack_metrics_available)
        std::cout << "shared-state counter calibration: known-state=1 ordinary-same-size=0\n";
    std::array<tests::allocation_event_t, 32> events;
    tests::allocation_event_log_t log{events.data (), events.size ()};
    const tests::allocation_event_scope_t recording (log);
    unsigned conversions = 0;
    begin_count ();
    const auto allocations = tests::count_allocations ([&] {
        task_t<int> source (result_t<int>::success (42));
        auto intermediate =
          detail::project_task_terminal<long> (source, [&] (const result_t<int> &result) {
              ++conversions;
              return result_t<long>::success (result.value ());
          });
        auto terminal =
          detail::project_task_terminal<void> (intermediate, [&] (const result_t<long> &result) {
              ++conversions;
              require (result.value () == 42, "ready conversion changed its value");
              return result_t<void>::success ();
          });
        require (terminal.await_ready (), "ready conversion became pending");
        terminal.await_resume ();
    });
    end_count ();
    unsigned shared_results = 0;
    for (std::size_t index = 0; index != log.count; ++index) {
        for (const auto &site : shared_result_sites) {
            if (site.matches (events[index])) {
                ++shared_results;
                break;
            }
        }
    }
    std::cout << "ready conversion chain: shared-results=";
    if constexpr (tests::shared_state_stack_metrics_available)
        std::cout << shared_results;
    else
        std::cout << "not-measured";
    std::cout << " allocations=" << allocations << " locks=" << mutex_locks
              << " notifications=" << notifications << '\n';
    require (!log.overflow, "ready conversion allocation log overflowed");
    require (conversions == 2, "ready result conversion did not run exactly once per layer");
    if constexpr (tests::shared_state_stack_metrics_available)
        require (shared_results == 0, "ready conversions constructed shared result states");
    require (allocations == 0, "ready conversions allocated shared result states");
    require (mutex_locks == 0 && notifications == 0, "ready conversions synchronized");
}

void check_projection_semantics ()
{
    completion_owner_t owner;
    const auto failure = std::make_exception_ptr (std::runtime_error ("projection failure"));
    for (const bool ready : {false, true}) {
        task_completion_source_t<int> source;
        if (ready)
            source.complete (detail::result_access_t::failure<int> (failure));
        auto input = source.task ();
        unsigned conversions = 0;
        auto projected = detail::project_task_terminal<void> (input, [&] (const auto &result) {
            ++conversions;
            (void) result.value ();
            return result_t<void>::success ();
        });
        require (projected.await_ready () == ready, "projection changed input readiness");
        if (!ready) {
            require (conversions == 0, "pending projection ran before its terminal");
            source.complete (detail::result_access_t::failure<int> (failure));
        }
        require (projected.result ().exception () == failure, "projection replaced its exception");
        require (conversions == 1, "projection did not convert exactly once");
    }
    task_completion_source_t<int> source;
    auto input = source.task ();
    auto projected = detail::project_task_terminal<int> (
      input, [] (const auto &result) { return result_t<int>::success (result.value () + 1); });
    std::promise<std::thread::id> delivered;
    auto delivery = delivered.get_future ();
    detail::observe_task_completion (projected, [&] (const auto &result) {
        require (result.value () == 42, "pending projection changed its value");
        delivered.set_value (std::this_thread::get_id ());
    });
    const auto completing_thread = std::this_thread::get_id ();
    source.complete (result_t<int>::success (41));
    require (delivery.get () != completing_thread,
             "pending projection bypassed completion dispatch");
}

void check_scheduled_ready_lifetime ()
{
    std::function<void ()> scheduled;
    bool invoked = false;
    {
        auto source = detail::with_task_resume_scheduler (
          task_t<int> (result_t<int>::success (42)),
          [&] (std::function<void ()> callback) { scheduled = std::move (callback); });
        detail::observe_task_completion (
          source, [&] (const auto &result) { invoked = result.value () == 42; });
        require (!invoked && static_cast<bool> (scheduled), "ready observer ignored its scheduler");
    }
    scheduled ();
    require (invoked, "scheduled observer lost its ready result after task destruction");
}

void check_ready_request_alias ()
{
    request_task_t<int> ready (result_t<int>::success (42));
    auto physical = detail::request_task_access_t::physical_reply (ready);
    auto moved = std::move (ready);
    require (physical.result ().value () == 42 && moved.result ().value () == 42,
             "ready request lost its physical reply alias");
    auto admitted = moved.admitted ();
    require (admitted.await_ready () && admitted.result (), "ready request admission changed");
}

struct throwing_move_t
{
    static inline bool fail_moves = false;
    int value;
    explicit throwing_move_t (int value) : value (value) {}
    throwing_move_t (const throwing_move_t &) = default;
    throwing_move_t (throwing_move_t &&other) : value (other.value)
    {
        if (fail_moves)
            throw std::runtime_error ("payload move must not run during a task move");
    }
};

void check_task_move_contract ()
{
    static_assert (std::is_nothrow_move_constructible_v<task_t<throwing_move_t>>);
    static_assert (std::is_nothrow_move_assignable_v<task_t<throwing_move_t>>);
    task_t<throwing_move_t> task (result_t<throwing_move_t>::success (throwing_move_t (42)));
    task_t<throwing_move_t> assigned (result_t<throwing_move_t>::success (throwing_move_t (0)));
    throwing_move_t::fail_moves = true;
    auto moved = std::move (task);
    assigned = std::move (moved);
    throwing_move_t::fail_moves = false;
    require (assigned.result ().value ().value == 42, "task move lost a throwing-move payload");
    struct immutable_value_t
    {
        const int value;
    };
    static_assert (std::is_nothrow_move_assignable_v<task_t<immutable_value_t>>);
    task_t<immutable_value_t> immutable (result_t<immutable_value_t>::success ({42}));
    task_t<immutable_value_t> target (result_t<immutable_value_t>::success ({0}));
    target = std::move (immutable);
    require (target.result ().value ().value == 42,
             "task move assignment required payload assignment");
}

void check_completion_delivery_lock ()
{
    completion_owner_t owner;
    for (const bool ready : {false, true}) {
        bool invoked = false;
        // Scheduler entry marks the end of state preparation; invocation must
        // also be able to read the result without retaining the state mutex.
        auto state = std::make_shared<detail::task_shared_state_t<int>> ();
        auto registration = std::make_shared<detail::task_wait_registration_t> (
          [&] (bool, const detail::ambient_context_snapshot_t &) {
              invoked = state->result ().value () == 42;
          },
          [] (std::function<void ()> work) {
              const auto locks = mutex_locks;
              end_count ();
              require (locks == 1, "completion reacquired its result mutex before delivery");
              work ();
          });
        if (ready)
            state->complete (result_t<int>::success (42));
        else
            state->set_continuation (registration);
        begin_count ();
        if (ready)
            state->set_continuation (registration);
        else
            require (state->complete (result_t<int>::success (42)), "completion rejected");
        end_count ();
        require (invoked, "completion delivery did not run");
        std::cout << "continuation ready=" << ready << " locks=1\n";
    }
}

void check_failure (const result_t<int> &result)
{
    require (!result.has_value (), "failure became success");
    require (result.error_kind () == framework_error_kind_t::rejected, "failure kind changed");
    try {
        result.value ();
        require (false, "failure did not throw");
    }
    catch (const framework_exception_t &error) {
        require (std::string (error.what ()) == "ready failure", "failure message changed");
    }
}
} // namespace

extern "C" int __real_pthread_mutex_lock (pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock (pthread_mutex_t *mutex)
{
    if (count_sync)
        ++mutex_locks;
    return __real_pthread_mutex_lock (mutex);
}

extern "C" void __real__ZNSt18condition_variable10notify_allEv (std::condition_variable *condition);
extern "C" void __wrap__ZNSt18condition_variable10notify_allEv (std::condition_variable *condition)
{
    if (count_sync)
        ++notifications;
    __real__ZNSt18condition_variable10notify_allEv (condition);
}

extern "C" void __real__ZNSt18condition_variable10notify_oneEv (std::condition_variable *condition);
extern "C" void __wrap__ZNSt18condition_variable10notify_oneEv (std::condition_variable *condition)
{
    if (count_sync)
        ++notifications;
    __real__ZNSt18condition_variable10notify_oneEv (condition);
}

int main ()
{
    try {
        std::mutex mutex;
        std::condition_variable condition;
        begin_count ();
        mutex.lock ();
        mutex.unlock ();
        condition.notify_one ();
        condition.notify_all ();
        end_count ();
        require (mutex_locks == 1 && notifications == 2, "synchronization counter calibration");
        std::cout << "counter calibration: locks=1 notifications=2\n";

        check_completion_delivery_lock ();
        check_projection_semantics ();
        check_scheduled_ready_lifetime ();
        check_ready_request_alias ();
        check_task_move_contract ();
        check_ready_conversion_chain ();

        auto value = make_ready (result_t<int>::success (42), "value success");
        const auto *borrowed = &value.result ();
        require (value.await_ready (), "ready reference became pending");
        require (value.result_for (std::chrono::milliseconds (0))->value () == 42,
                 "ready timed observation changed the result");
        require (value.await_resume () == 42 && &value.result () == borrowed,
                 "ordinary ready consumption invalidated its result reference");
        require (value.result ().value () == 42, "ready value changed");
        auto empty = make_ready (result_t<void>::success (), "void success");
        empty.await_resume ();
        auto failed =
          make_ready (result_t<int>::failure (framework_error_kind_t::rejected, "ready failure"),
                      "value failure");
        check_failure (failed.result ());
        auto void_failed =
          make_ready (result_t<void>::failure (framework_error_kind_t::rejected, "ready failure"),
                      "void failure");
        require (!void_failed.result ().has_value (), "void failure became success");
        try {
            void_failed.await_resume ();
            require (false, "void failure did not throw");
        }
        catch (const framework_exception_t &error) {
            require (error.kind () == framework_error_kind_t::rejected,
                     "void failure kind changed");
            require (std::string (error.what ()) == "ready failure",
                     "void failure message changed");
        }
        auto owned = make_ready (
          result_t<std::unique_ptr<int>>::success (std::make_unique<int> (7)), "move-only success");
        require (*owned.result ().value () == 7, "move-only value changed");
        auto moved = std::move (owned);
        require (*moved.result ().value () == 7, "moved task lost result");

        task_completion_source_t<int> pending;
        auto pending_task = pending.task ();
        require (!pending_task.await_ready (), "pending task started ready");
        require (pending.complete (result_t<int>::success (9)), "pending completion rejected");
        require (pending_task.await_ready () && pending_task.result ().value () == 9,
                 "pending result not published");
        require (!pending.complete (result_t<int>::success (10)), "duplicate completion accepted");
        require (pending_task.result ().value () == 9, "duplicate completion replaced result");
        task_completion_source_t<void> pending_void;
        auto pending_void_task = pending_void.task ();
        require (!pending_void_task.await_ready (), "void pending task started ready");
        require (pending_void.complete (result_t<void>::success ()),
                 "void pending completion rejected");
        require (pending_void_task.await_ready (), "void pending result not published");
        pending_void_task.await_resume ();
        std::cout << "ready task and pending completion semantics passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        end_count ();
        std::cerr << error.what () << '\n';
        return 1;
    }
}
