/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../common/bench_async.hpp"

#include <barrier>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

namespace
{
thread_local bool counting = false;
thread_local size_t allocations = 0;
thread_local size_t locks = 0;
int failures = 0;
void expect (bool condition, const char *message)
{
    if (!condition) {
        std::fprintf (stderr, "FAIL: %s\n", message);
        ++failures;
    }
}
}

void *operator new (size_t size)
{
    if (counting)
        ++allocations;
    if (void *value = std::malloc (size ? size : 1))
        return value;
    throw std::bad_alloc ();
}
void operator delete (void *value) noexcept
{
    std::free (value);
}
void operator delete (void *value, size_t) noexcept
{
    std::free (value);
}

#ifdef BENCH_TEST_WRAP_MUTEX
extern "C" int __real_pthread_mutex_lock (pthread_mutex_t *mutex);
extern "C" int __wrap_pthread_mutex_lock (pthread_mutex_t *mutex)
{
    if (counting)
        ++locks;
    return __real_pthread_mutex_lock (mutex);
}
#endif

int main ()
{
    namespace fw = zlink::framework;
    fw::completion_owner_t completion_owner;
    counting = true;
    void *calibration = ::operator new (1);
    counting = false;
    ::operator delete (calibration);
    expect (allocations == 1, "allocation counter detects an explicit allocation");
    allocations = 0;
    auto signal = std::make_shared<zlink_cpp_bench::completion_signal_t> ();
    std::vector<size_t> completed;
    completed.reserve (8);
    std::vector<size_t> remote;
    remote.reserve (8);
    auto ready = fw::task_t<void> (fw::result_t<void>::success ());
    counting = true;
    expect (ready.await_ready (), "ready-task baseline is complete");
    counting = false;
    const size_t task_locks = locks;
    allocations = locks = 0;
    counting = true;
    signal->notify (ready, 3, completed, signal);
    counting = false;
    const auto ready_allocations = allocations;
    const auto ready_locks = locks;
    std::printf ("ready completion: allocations=%zu fixture_locks=%zu direct=%zu\n",
                 ready_allocations, ready_locks - task_locks, completed.size ());
    std::fflush (stdout);
    expect (ready_allocations == 0, "ready completion adds no fixture allocation");
#ifdef BENCH_TEST_WRAP_MUTEX
    expect (ready_locks == task_locks, "ready completion adds no fixture lock");
#endif
    expect (completed == std::vector<size_t>{3}, "ready completion is consumed directly");
    signal->take_completed (remote);
    expect (remote.empty (), "ready completion does not enqueue a notification");
    expect (static_cast<bool> (ready.result ()), "ready success is preserved");

    auto failed = fw::task_t<void> (fw::result_t<void>::failure (
      fw::framework_error_kind_t::invalid_operation, "fixture failure"));
    completed.clear ();
    signal->notify (failed, 4, completed, signal);
    expect (completed == std::vector<size_t>{4}, "ready failure is consumed directly");
    expect (!failed.result (), "ready error remains an error for fixture counters");
    signal->take_completed (remote);
    expect (remote.empty (), "ready failure does not enqueue a notification");

    fw::task_completion_source_t<void> source;
    auto pending = source.task ();
    completed.clear ();
    allocations = locks = 0;
    counting = true;
    signal->notify (pending, 5, completed, signal);
    counting = false;
    std::printf ("pending observer: allocations=%zu locks=%zu\n", allocations, locks);
    expect (allocations > 0, "allocation counter detects a real pending registration");
    expect (completed.empty (), "pending task is not prematurely consumed");
    signal->take_completed (remote);
    expect (remote.empty (), "pending task is not prematurely notified");
    std::barrier rendezvous (2);
    std::thread producer ([&] {
        rendezvous.arrive_and_wait ();
        source.complete (fw::result_t<void>::success ());
    });
    rendezvous.arrive_and_wait ();
    producer.join ();
    signal->wait_until (std::chrono::steady_clock::time_point::max ());
    signal->take_completed (remote);
    expect (remote == std::vector<size_t>{5}, "remote completion is notified once");
    expect (pending.await_ready () && pending.result (), "remote success is preserved");
    signal->take_completed (remote);
    expect (remote.empty (), "remote completion is removed after consumption");
    std::printf ("fixture completion failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
