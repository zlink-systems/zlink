/* SPDX-License-Identifier: MPL-2.0 */
#include "support.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <unistd.h>

namespace
{
std::atomic_bool count_allocations{false};
std::atomic_size_t allocation_count{0};
}

void *operator new (std::size_t size_)
{
    if (count_allocations.load (std::memory_order_relaxed))
        allocation_count.fetch_add (1, std::memory_order_relaxed);
    if (void *memory = std::malloc (size_ ? size_ : 1))
        return memory;
    throw std::bad_alloc ();
}

void *operator new[] (std::size_t size_)
{
    return ::operator new (size_);
}

void operator delete (void *memory_) noexcept
{
    std::free (memory_);
}
void operator delete[] (void *memory_) noexcept
{
    std::free (memory_);
}
void operator delete (void *memory_, std::size_t) noexcept
{
    std::free (memory_);
}
void operator delete[] (void *memory_, std::size_t) noexcept
{
    std::free (memory_);
}

int main ()
{
    int fds[2];
    if (pipe (fds) != 0)
        return 1;
    zlink::poller_t poller;
    poller.add_fd (fds[0], zlink::poll_event_flag_t::pollin, 42);
    const char ready = 1;
    if (write (fds[1], &ready, 1) != 1)
        return 1;

    zlink::poll_event_t event;
    if (poller.wait (&event, 1, std::chrono::milliseconds (0)) != 1)
        return 1;
    allocation_count.store (0, std::memory_order_relaxed);
    count_allocations.store (true, std::memory_order_relaxed);
    for (int i = 0; i < 64; ++i) {
        if (poller.wait (&event, 1, std::chrono::milliseconds (0)) != 1 || event.slot != 42
            || event.fd != fds[0]) {
            count_allocations.store (false, std::memory_order_relaxed);
            return 1;
        }
    }
    count_allocations.store (false, std::memory_order_relaxed);
    const size_t measured = allocation_count.load (std::memory_order_relaxed);
    std::printf ("repeated_wait_cpp_allocations=%zu\n", measured);
    poller.remove_fd (fds[0]);
    int replacement[2];
    if (pipe (replacement) != 0)
        return 1;
    poller.add_fd (replacement[0], zlink::poll_event_flag_t::pollin, 77);
    if (write (replacement[1], &ready, 1) != 1
        || poller.wait (&event, 1, std::chrono::milliseconds (0)) != 1
        || event.slot != 77 || event.fd != replacement[0])
        return 1;
    poller.remove_fd (replacement[0]);
    close (replacement[0]);
    close (replacement[1]);
    close (fds[0]);
    close (fds[1]);
    return measured == 0 ? 0 : 1;
}
