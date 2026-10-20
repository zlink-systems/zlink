/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/dispatch/task.hpp>

#include <atomic>
#include <cassert>
#include <latch>
#include <iostream>
#include <thread>

namespace detail = zlink::framework::detail;

int main ()
{
    {
        auto owner = detail::acquire_completion_dispatcher ();
        const std::weak_ptr<detail::completion_dispatcher_t> observed (owner);
        std::latch entered (1), release (1);
        std::atomic_bool completed{false};
        detail::dispatch_completion (
          [&] {
              entered.count_down ();
              release.wait ();
              completed.store (true, std::memory_order_release);
          },
          false, nullptr, owner);
        entered.wait ();
        assert (observed.use_count () == 1);
        std::jthread unblock ([&] {
            while (!observed.expired ())
                std::this_thread::yield ();
            release.count_down ();
        });
        owner.reset ();
        assert (completed.load (std::memory_order_acquire));
        assert (observed.expired ());
    }

    constexpr unsigned rounds = 64;
    constexpr unsigned callbacks_per_round = 64;
    for (unsigned round = 0; round < rounds; ++round) {
        std::atomic_uint delivered{0};
        auto owner = detail::acquire_completion_dispatcher ();
        for (unsigned index = 0; index < callbacks_per_round; ++index)
            detail::dispatch_completion (
              [&] { delivered.fetch_add (1, std::memory_order_relaxed); }, false, nullptr, owner);
        owner.reset ();
        assert (delivered.load (std::memory_order_relaxed) == callbacks_per_round);
    }
    std::cout << "owner_release_drains=pass rounds=" << rounds
              << " callbacks_per_round=" << callbacks_per_round << '\n';
}
