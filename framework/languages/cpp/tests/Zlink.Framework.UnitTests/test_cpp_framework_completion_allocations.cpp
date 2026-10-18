/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework.hpp>

#include "runtime/dispatch/coroutine_executor.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace
{
// This target forces std::function copies onto the heap for allocation measurements.
struct native_completion_work_t
{
    std::array<std::byte, 128> payload{};
    std::promise<void> *completed;

    void operator() () const { completed->set_value (); }
};

thread_local bool tracking_allocations = false;
thread_local std::size_t tracked_allocation_count = 0;

void record_allocation () noexcept
{
    if (tracking_allocations)
        ++tracked_allocation_count;
}

void *allocate_bytes (std::size_t size)
{
    if (void *memory = std::malloc (size == 0 ? 1 : size)) {
        record_allocation ();
        return memory;
    }
    throw std::bad_alloc ();
}

void *allocate_aligned_bytes (std::size_t size, std::size_t alignment)
{
#if defined(_MSC_VER)
    void *memory = _aligned_malloc (size == 0 ? 1 : size, alignment);
#else
    void *memory = nullptr;
    const int status = posix_memalign (&memory, alignment, size == 0 ? 1 : size);
    if (status != 0)
        memory = nullptr;
#endif
    if (memory != nullptr) {
        record_allocation ();
        return memory;
    }
    throw std::bad_alloc ();
}

void free_aligned_bytes (void *memory) noexcept
{
#if defined(_MSC_VER)
    _aligned_free (memory);
#else
    std::free (memory);
#endif
}

template <typename TAction> std::size_t count_allocations (TAction &&action)
{
    const bool previous_tracking = tracking_allocations;
    const std::size_t previous_count = tracked_allocation_count;
    tracked_allocation_count = 0;
    tracking_allocations = true;
    try {
        std::forward<TAction> (action) ();
    }
    catch (...) {
        tracking_allocations = previous_tracking;
        tracked_allocation_count = previous_count;
        throw;
    }
    tracking_allocations = previous_tracking;
    const std::size_t measured = tracked_allocation_count;
    tracked_allocation_count = previous_count;
    return measured;
}

std::size_t measure_native_completion ()
{
    auto scheduler = zlink::framework::detail::capture_native_continuation_scheduler ();
    if (!scheduler)
        throw std::runtime_error ("host continuation scheduler was not captured");

    std::promise<void> completed;
    auto ready = completed.get_future ();
    std::function<void ()> work = native_completion_work_t{{}, &completed};
    const auto allocations = count_allocations ([&] { scheduler (std::move (work)); });
    if (ready.wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        throw std::runtime_error ("native completion did not run");
    ready.get ();
    return allocations;
}

zlink::framework::task_t<void> resume_waiter (zlink::framework::task_t<int> pending,
                                              std::promise<void> *resumed)
{
    (void) co_await pending;
    resumed->set_value ();
    co_return;
}

std::size_t measure_task_wait_resumption ()
{
    using namespace zlink::framework;
    task_completion_source_t<int> completion;
    auto pending = completion.task ();
    std::promise<void> resumed;
    auto ready = resumed.get_future ();
    auto waiter = resume_waiter (std::move (pending), &resumed);
    if (waiter.await_ready ())
        throw std::runtime_error ("task waiter did not suspend");

    const auto allocations = count_allocations ([&] {
        if (!completion.complete (result_t<int>::success (42)))
            throw std::runtime_error ("task completion was already settled");
    });
    if (ready.wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        throw std::runtime_error ("task waiter did not resume");
    ready.get ();
    // Submit and completion spec §11: the waiter resumes in its own turn, so its
    // co_return may finish after the promise above is observed on this thread.
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!waiter.await_ready ()) {
        if (std::chrono::steady_clock::now () >= deadline)
            throw std::runtime_error ("resumed task did not complete");
        std::this_thread::yield ();
    }
    return allocations;
}

struct allocation_spot_t
{
    std::size_t calls = 0;
    void on_packet (const std::string &) { ++calls; }
};

std::size_t measure_spot_dispatch ()
{
    using namespace zlink::framework;
    service_collection_t services;
    auto provider = services.build_provider ();
    serializer_registry_t serializers;
    spot_handler_registry_t registry;
    registry.add_handler<&allocation_spot_t::on_packet> ("allocation-packet");
    allocation_spot_t spot;
    const auto payload = zlink::message_t::from (std::string ("payload"));
    const auto invoke = [&] {
        auto result =
          registry.invoke_packet ("allocation-packet", spot, provider, serializers, payload);
        if (!result)
            throw std::runtime_error (result.error ()->what ());
    };
    invoke ();
    constexpr std::size_t dispatches = 100;
    const auto allocations = count_allocations ([&] {
        for (std::size_t index = 0; index < dispatches; ++index)
            invoke ();
    });
    if (spot.calls != dispatches + 1)
        throw std::runtime_error ("ordinary Spot dispatch did not invoke each handler once");
    return allocations;
}

bool exceeds_main_baseline (const char *variable, std::size_t observed)
{
    const char *const text = std::getenv (variable);
    if (text == nullptr)
        return false;
    char *end = nullptr;
    const auto expected = std::strtoull (text, &end, 10);
    if (end == text || *end != '\0') {
        std::cerr << variable << " is not an allocation count: " << text << '\n';
        return true;
    }
    if (observed <= expected)
        return false;
    std::cerr << variable << " baseline=" << expected << " observed=" << observed << '\n';
    return true;
}

} // namespace

void *operator new (std::size_t size)
{
    return allocate_bytes (size);
}
void *operator new[] (std::size_t size)
{
    return allocate_bytes (size);
}
void *operator new (std::size_t size, std::align_val_t alignment)
{
    return allocate_aligned_bytes (size, static_cast<std::size_t> (alignment));
}
void *operator new[] (std::size_t size, std::align_val_t alignment)
{
    return allocate_aligned_bytes (size, static_cast<std::size_t> (alignment));
}
void operator delete (void *memory) noexcept
{
    std::free (memory);
}
void operator delete[] (void *memory) noexcept
{
    std::free (memory);
}
void operator delete (void *memory, std::size_t) noexcept
{
    std::free (memory);
}
void operator delete[] (void *memory, std::size_t) noexcept
{
    std::free (memory);
}
void operator delete (void *memory, std::align_val_t) noexcept
{
    free_aligned_bytes (memory);
}
void operator delete[] (void *memory, std::align_val_t) noexcept
{
    free_aligned_bytes (memory);
}
void operator delete (void *memory, std::size_t, std::align_val_t) noexcept
{
    free_aligned_bytes (memory);
}
void operator delete[] (void *memory, std::size_t, std::align_val_t) noexcept
{
    free_aligned_bytes (memory);
}

int main ()
{
    void *probe_allocation = nullptr;
    const std::size_t probe = count_allocations ([&] { probe_allocation = ::operator new (64); });
    ::operator delete (probe_allocation);
    if (probe != 1) {
        std::cerr << "allocation positive probe expected=1 observed=" << probe << '\n';
        return 1;
    }
    std::promise<void> probe_completion;
    const std::function<void ()> copy_probe_work = native_completion_work_t{{}, &probe_completion};
    const std::size_t function_copy_probe =
      count_allocations ([&] { const std::function<void ()> copy = copy_probe_work; });
    if (function_copy_probe == 0) {
        std::cerr << "std::function copy probe expected>0 observed=" << function_copy_probe << '\n';
        return 1;
    }

    zlink::framework::runtime::configure_handler_coroutine_executor (1);
    zlink::framework::runtime::install_host_context_hooks ();

    (void) measure_native_completion ();
    (void) measure_task_wait_resumption ();
    const std::size_t native = measure_native_completion ();
    const std::size_t task_wait = measure_task_wait_resumption ();
    const std::size_t spot_dispatch = measure_spot_dispatch ();

    zlink::framework::runtime::shutdown_handler_coroutine_executor ();
    zlink::framework::detail::set_runtime_execution_hooks (nullptr);
    zlink::framework::detail::set_ambient_context_hooks (nullptr);

    std::cout << "allocation probe=" << probe << " function-copy-probe=" << function_copy_probe
              << " native-completion=" << native << " task-wait-resumption=" << task_wait
              << " spot-dispatches=100 spot-dispatch-allocations=" << spot_dispatch << '\n';
    const bool native_regressed =
      exceeds_main_baseline ("ZLINK_EXPECT_MAIN_NATIVE_COMPLETION_ALLOCATIONS", native);
    const bool task_wait_regressed =
      exceeds_main_baseline ("ZLINK_EXPECT_MAIN_TASK_WAIT_ALLOCATIONS", task_wait);
    const bool spot_dispatch_regressed =
      exceeds_main_baseline ("ZLINK_EXPECT_MAIN_SPOT_DISPATCH_ALLOCATIONS", spot_dispatch);
    return native_regressed || task_wait_regressed || spot_dispatch_regressed ? 2 : 0;
}
