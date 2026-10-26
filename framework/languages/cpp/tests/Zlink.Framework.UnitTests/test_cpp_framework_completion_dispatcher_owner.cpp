/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/dispatch/task.hpp>
#include "runtime/dispatch/completion_turn.hpp"

#include <atomic>
#include <cassert>
#include <latch>
#include <iostream>
#include <thread>
#include <string_view>

namespace detail = zlink::framework::detail;

zlink::framework::task_t<int> await_value (zlink::framework::task_t<int> &pending)
{
    co_return co_await pending;
}

void verify_wait_after_owner_close (bool closed)
{
    using namespace zlink::framework;
    completion_owner_t owner;
    task_completion_source_t<int> source;
    auto pending = source.task ();
    auto waiting = await_value (pending);
    if (closed)
        owner.close ();
    assert (source.complete (result_t<int>::success (1639)));
    if (closed) {
        bool shutdown = false;
        try {
            (void) waiting.result ().value ();
        }
        catch (const framework_exception_t &error) {
            shutdown = error.kind () == framework_error_kind_t::shutting_down;
        }
        assert (shutdown);
    } else {
        assert (waiting.result ().value () == 1639);
        owner.close ();
    }
    assert (pending.result ().value () == 1639);
    std::cout << "wait_owner_closed=" << closed << " terminal="
              << (closed ? "shutting_down" : "success") << " source=1639\n";
}

int main (int argc, char **argv)
{
    if (argc == 2) {
        const std::string_view mode (argv[1]);
        assert (mode == "--closed-wait" || mode == "--live-wait");
        verify_wait_after_owner_close (mode == "--closed-wait");
        return 0;
    }
    verify_wait_after_owner_close (false);
    verify_wait_after_owner_close (true);
    {
        zlink::framework::completion_owner_t owner;
        auto dispatcher = detail::borrow_completion_dispatcher ();
        const std::weak_ptr storage = dispatcher;
        owner.close ();
        // A new owner cannot revive a dispatcher retained by an older wait.
        zlink::framework::completion_owner_t restarted;
        bool ran = false;
        std::function<void ()> callback = [&] { ran = true; };
        bool rejected = false;
        try {
            detail::dispatch_completion (std::move (callback), false, &ran, dispatcher);
        }
        catch (const zlink::framework::framework_exception_t &error) {
            rejected = error.kind () == zlink::framework::framework_error_kind_t::shutting_down;
        }
        assert (rejected && callback && !ran && !storage.expired ());
        callback ();
        assert (ran);
        dispatcher.reset ();
        assert (storage.expired ());
    }
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
        std::weak_ptr<void> observed;
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
    std::cout << "owner_release_drains=pass rounds=" << rounds
              << " callbacks_per_round=" << callbacks_per_round << '\n';
}
