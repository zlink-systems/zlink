/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/contracts/dispatch/task.hpp>

#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <future>
#include <functional>
#include <iostream>
#include <new>
#include <optional>
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

std::size_t measure_envelope_parts (std::size_t iterations)
{
    using zlink::framework::runtime::messaging::message_parts_t;
    constexpr std::size_t header_size = 128;
    constexpr std::size_t body_size = 160;
    std::size_t allocations = 0;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        auto header = zlink::message_t::from (std::string (header_size, 'h'));
        auto body = zlink::message_t::from (std::string (body_size, 'b'));
        const auto *header_storage = header.data ();
        const auto *body_storage = body.data ();
        std::optional<message_parts_t> parts;
        allocations +=
          count_allocations ([&] { parts.emplace (std::move (header), std::move (body)); });
        if (parts->size () != 2 || (*parts)[0].data () != header_storage
            || (*parts)[1].data () != body_storage || (*parts)[0].size () != header_size
            || (*parts)[1].size () != body_size || header.valid () || body.valid ()) {
            throw std::runtime_error ("envelope parts changed storage, order, or ownership");
        }
        auto items = std::move (*parts).take_items ();
        const auto *items_storage = items.data ();
        std::optional<message_parts_t> transferred;
        const auto transfer_allocations =
          count_allocations ([&] { transferred.emplace (std::move (items)); });
        if (transfer_allocations != 0 || transferred->items ().data () != items_storage
            || (*transferred)[0].data () != header_storage
            || (*transferred)[1].data () != body_storage) {
            throw std::runtime_error ("envelope vector transfer allocated or changed storage");
        }
    }
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
    constexpr std::size_t envelope_iterations = 1000;
    const std::size_t envelope_parts = measure_envelope_parts (envelope_iterations);
    if (envelope_parts != envelope_iterations) {
        std::cerr << "envelope-parts expected=" << envelope_iterations
                  << " observed=" << envelope_parts << '\n';
        return 1;
    }

    zlink::framework::runtime::configure_handler_coroutine_executor (1);
    zlink::framework::runtime::install_host_context_hooks ();

    (void) measure_native_completion ();
    (void) measure_task_wait_resumption ();
    const std::size_t native = measure_native_completion ();
    const std::size_t task_wait = measure_task_wait_resumption ();

    zlink::framework::runtime::shutdown_handler_coroutine_executor ();
    zlink::framework::detail::set_runtime_execution_hooks (nullptr);
    zlink::framework::detail::set_ambient_context_hooks (nullptr);

    std::cout << "allocation probe=" << probe << " function-copy-probe=" << function_copy_probe
              << " envelope-parts=" << envelope_parts << " native-completion=" << native
              << " task-wait-resumption=" << task_wait << '\n';
    const bool native_regressed =
      exceeds_main_baseline ("ZLINK_EXPECT_MAIN_NATIVE_COMPLETION_ALLOCATIONS", native);
    const bool task_wait_regressed =
      exceeds_main_baseline ("ZLINK_EXPECT_MAIN_TASK_WAIT_ALLOCATIONS", task_wait);
    return native_regressed || task_wait_regressed ? 2 : 0;
}
