/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/dispatch/task.hpp>
#include "runtime/dispatch/completion_turn.hpp"

#include <atomic>
#include <cassert>
#include <latch>
#include <iostream>
#include <thread>

namespace detail = zlink::framework::detail;

int main ()
{
    {
        zlink::framework::completion_owner_t owner;
        auto *dispatcher = detail::borrow_completion_dispatcher ();
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
        auto *dispatcher = detail::borrow_completion_dispatcher ();
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
