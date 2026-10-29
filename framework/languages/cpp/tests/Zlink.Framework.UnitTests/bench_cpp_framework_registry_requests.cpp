/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace fw = zlink::framework;
namespace foundation = fw::runtime::foundation;

// Exercise the request registry and the real completion dispatcher, without
// socket/codec noise. Thread creation, warmup and shutdown are outside timing.
void measure (std::size_t threads)
{
    constexpr std::size_t requests = 1000;
    foundation::operation_registry_t registry;
    std::barrier ready (static_cast<std::ptrdiff_t> (threads + 1));
    std::barrier start (static_cast<std::ptrdiff_t> (threads + 1));
    std::barrier finish (static_cast<std::ptrdiff_t> (threads + 1));
    std::vector<std::thread> callers;
    for (std::size_t thread = 0; thread < threads; ++thread) {
        callers.emplace_back ([&, thread] {
            std::atomic_bool delivered{false};
            const auto call = [&] (std::uint64_t sequence) {
                const foundation::call_id_t id{thread + 1, sequence};
                delivered.store (false, std::memory_order_relaxed);
                if (!registry.register_operation (
                      id,
                      foundation::operation_registry_t::clock_t::now () + std::chrono::seconds (10),
                      [&] (auto terminal, auto) {
                          if (terminal != foundation::operation_terminal_t::completed)
                              std::abort ();
                          delivered.store (true, std::memory_order_release);
                          delivered.notify_one ();
                      })
                    || !registry.complete (id, {}))
                    std::abort ();
                delivered.wait (false, std::memory_order_acquire);
            };
            for (std::size_t i = 1; i <= 32; ++i)
                call (i);
            ready.arrive_and_wait ();
            start.arrive_and_wait ();
            for (std::size_t i = 1; i <= requests; ++i)
                call (1000 + i);
            finish.arrive_and_wait ();
        });
    }
    ready.arrive_and_wait ();
    fw::tests::process_allocation_count.store (0);
    fw::tests::tracking_process_allocations.store (true);
    const auto began = std::chrono::steady_clock::now ();
    start.arrive_and_wait ();
    finish.arrive_and_wait ();
    const auto elapsed =
      std::chrono::duration<double> (std::chrono::steady_clock::now () - began).count ();
    fw::tests::tracking_process_allocations.store (false);
    const auto allocations = fw::tests::process_allocation_count.load ();
    for (auto &caller : callers)
        caller.join ();
    registry.shutdown ();
    const auto count = threads * requests;
    std::printf ("threads=%zu requests=%zu seconds=%.9f requests_per_second=%.3f allocations=%zu "
                 "allocations_per_request=%.6f\n",
                 threads, count, elapsed, count / elapsed, allocations,
                 double (allocations) / count);
}

int main ()
{
    fw::completion_owner_t owner;
    measure (1);
    measure (8);
}
