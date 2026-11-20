/* SPDX-License-Identifier: Elastic-2.0 */

#include <zlink/framework/contracts/dispatch/task.hpp>
#include "runtime/dispatch/completion_turn.hpp"

#include <atomic>
#include <cassert>
#include <latch>
#include <iostream>
#include <thread>
#include <future>
#include <chrono>

namespace detail = zlink::framework::detail;

namespace
{
std::function<void (bool)> deferred_completion;
bool defer_completion (std::function<void (bool)> &work)
{
    deferred_completion = std::move (work);
    return true;
}
}

int main ()
{
    {
        zlink::framework::completion_owner_t owner;
        auto dispatcher = detail::borrow_completion_dispatcher ();
        const auto observed = detail::observe_completion_dispatcher ();
        std::latch entered (1), release (1);
        std::atomic_bool completed{false};
        detail::dispatch_completion (
          [&] {
              entered.count_down ();
              release.wait ();
              completed.store (true, std::memory_order_release);
          },
          false, nullptr, dispatcher);
        entered.wait ();
        assert (observed.use_count () == 1);
        std::jthread unblock ([&] {
            while (!observed.expired ())
                std::this_thread::yield ();
            release.count_down ();
        });
        owner.close ();
        assert (completed.load (std::memory_order_acquire));
        assert (observed.expired ());
    }
    {
        zlink::framework::completion_owner_t owner;
        auto dispatcher = detail::borrow_completion_dispatcher ();
        std::latch delivered (1);
        std::atomic_uint transition_count{0};
        std::thread notification ([&] {
            const auto notification_resource = std::this_thread::get_id ();
            detail::dispatch_completion (
              [&, notification_resource] {
                  if (notification_resource != std::this_thread::get_id ())
                      transition_count.fetch_add (1, std::memory_order_relaxed);
                  delivered.count_down ();
              },
              false, nullptr, dispatcher);
        });
        notification.join ();
        delivered.wait ();
        assert (transition_count.load (std::memory_order_relaxed) == 1);
    }

    {
        zlink::framework::completion_owner_t first;
        const auto previous = detail::observe_completion_dispatcher ();
        {
            zlink::framework::completion_owner_t second;
            first.close ();
            assert (!previous.expired ());
            std::atomic_bool delivered{false};
            detail::dispatch_completion ([&] { delivered = true; });
            second.close ();
            assert (delivered);
        }
        assert (previous.expired ());
        zlink::framework::completion_owner_t restarted;
        const auto next = detail::observe_completion_dispatcher ();
        assert (previous.owner_before (next) || next.owner_before (previous));
    }
    {
        bool rejected = false;
        try {
            (void) detail::capture_native_continuation_scheduler ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            rejected = error.kind () == zlink::framework::framework_error_kind_t::invalid_operation;
        }
        assert (rejected);
        assert (detail::completion_dispatcher_worker_count () == 0);
    }

    {
        std::atomic_uint delivered{0};
        std::weak_ptr<detail::completion_dispatcher_t> observed;
        {
            zlink::framework::completion_owner_t owner;
            observed = detail::observe_completion_dispatcher ();
            detail::dispatch_completion ([&, observed] {
                while (!observed.expired ())
                    std::this_thread::yield ();
                detail::dispatch_completion ([&] { ++delivered; }, false, &delivered);
            });
        }
        assert (observed.expired ());
        assert (delivered == 1);
    }

    constexpr unsigned rounds = 64;
    constexpr unsigned callbacks_per_round = 64;
    for (unsigned round = 0; round < rounds; ++round) {
        std::atomic_uint delivered{0};
        zlink::framework::completion_owner_t owner;
        auto dispatcher = detail::borrow_completion_dispatcher ();
        for (unsigned index = 0; index < callbacks_per_round; ++index)
            detail::dispatch_completion (
              [&] { delivered.fetch_add (1, std::memory_order_relaxed); }, false, nullptr,
              dispatcher);
        owner.close ();
        assert (delivered.load (std::memory_order_relaxed) == callbacks_per_round);
    }
    // This process has no test-harness owner: close below really releases the
    // last owner. The runtime hook controls deferred placement independently of
    // the executor tests, while exercising the provider's actual work credit.
    for (const bool handed_off : {true, false}) {
        using namespace std::chrono_literals;
        zlink::framework::completion_owner_t owner;
        auto dispatcher = detail::borrow_completion_dispatcher ();
        const auto observed = detail::observe_completion_dispatcher ();
        const detail::runtime_execution_hooks_t hooks{
          [] () -> detail::completion_dispatcher_borrow_t { return {}; },
          [] (detail::completion_dispatcher_borrow_t *) { return detail::task_scheduler_t{}; },
          [] { return detail::task_scheduler_t{}; }, [] {}, &defer_completion};
        detail::set_runtime_execution_hooks (&hooks);
        std::promise<void> callback, release;
        auto entered = callback.get_future ();
        auto released = release.get_future ().share ();
        detail::dispatch_request_completion (
          [&] {
              callback.set_value ();
              released.wait ();
          },
          &callback, dispatcher);
        assert (deferred_completion);
        assert (observed.use_count () == 1);
        auto closed = std::async (std::launch::async, [&] { owner.close (); });
        while (!observed.expired ())
            std::this_thread::yield ();
        assert (closed.wait_for (100ms) != std::future_status::ready);
        std::jthread delivery ([&] {
            auto placement = std::move (deferred_completion);
            placement (handed_off);
        });
        assert (entered.wait_for (5s) == std::future_status::ready);
        assert (closed.wait_for (100ms) != std::future_status::ready);
        release.set_value ();
        assert (closed.wait_for (5s) == std::future_status::ready);
        closed.get ();
        delivery.join ();
        detail::set_runtime_execution_hooks (nullptr);
        assert (!deferred_completion);
    }
    std::cout << "deferred_and_local_owner_release_drains=pass\n";
    std::cout << "owner_release_drains=pass rounds=" << rounds
              << " callbacks_per_round=" << callbacks_per_round << '\n';
}
