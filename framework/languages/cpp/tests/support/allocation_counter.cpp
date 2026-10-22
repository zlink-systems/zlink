/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include <cstdlib>
#include <new>
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace zlink::framework::tests
{
thread_local bool tracking_allocations = false;
thread_local std::size_t tracked_allocation_count = 0;
thread_local std::size_t tracked_allocation_size = 0;
thread_local std::size_t tracked_allocation_bytes = 0;
thread_local std::size_t largest_allocation = 0;
thread_local allocation_event_log_t *allocation_event_log = nullptr;

void record_allocation (const void *memory, std::size_t size) noexcept
{
    if (tracking_allocations && (tracked_allocation_size == 0 || tracked_allocation_size == size)) {
        if (allocation_event_log != nullptr) {
            if (allocation_event_log->count < allocation_event_log->capacity)
                allocation_event_log->events[allocation_event_log->count++] = {memory, size};
            else
                allocation_event_log->overflow = true;
        }
        ++tracked_allocation_count;
        tracked_allocation_bytes += size;
        if (size > largest_allocation)
            largest_allocation = size;
    }
}

void *allocate_bytes (std::size_t size)
{
    if (void *memory = std::malloc (size == 0 ? 1 : size)) {
        record_allocation (memory, size);
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
        record_allocation (memory, size);
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


} // namespace zlink::framework::tests

using namespace zlink::framework::tests;

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
