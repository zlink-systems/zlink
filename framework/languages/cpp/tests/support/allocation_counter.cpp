/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include <cstdlib>
#include <new>
#include <sstream>
#include <cstdint>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#endif
#if defined(_MSC_VER)
#include <malloc.h>
#endif
namespace zlink::framework::tests
{
thread_local bool tracking_allocations = false;
thread_local std::optional<std::size_t> allocations_before_failure;
thread_local std::size_t tracked_allocation_count = 0;
thread_local std::size_t tracked_allocation_size = 0;
thread_local std::size_t tracked_allocation_bytes = 0;
thread_local std::size_t largest_allocation = 0;
thread_local allocation_event_log_t *allocation_event_log = nullptr;

void record_allocation (const void *memory, std::size_t size) noexcept
{
    if (tracking_allocations && (tracked_allocation_size == 0 || tracked_allocation_size == size)) {
        if (allocation_event_log != nullptr) {
            if (allocation_event_log->count < allocation_event_log->capacity) {
                auto &event = allocation_event_log->events[allocation_event_log->count++];
                event = {memory, size};
#if defined(_WIN32)
                event.stack_depth = CaptureStackBackTrace (
                  0, static_cast<DWORD> (event.stack.size ()), event.stack.data (), nullptr);
#elif defined(__linux__) || defined(__APPLE__)
                event.stack_depth = static_cast<std::size_t> (
                  backtrace (event.stack.data (), static_cast<int> (event.stack.size ())));
#endif
            } else
                allocation_event_log->overflow = true;
        }
        ++tracked_allocation_count;
        tracked_allocation_bytes += size;
        if (size > largest_allocation)
            largest_allocation = size;
    }
}

std::string describe_allocations (const allocation_event_log_t &log)
{
    std::ostringstream output;
    output << "allocation events=" << log.count << " overflow=" << log.overflow << '\n';
    for (std::size_t index = 0; index != log.count; ++index) {
        const auto &event = log.events[index];
        output << '#' << index << " size=" << event.size << " memory=" << event.memory << '\n';
#if defined(__linux__) || defined(__APPLE__)
        const auto symbols =
          backtrace_symbols (event.stack.data (), static_cast<int> (event.stack_depth));
#endif
        for (std::size_t frame = 0; frame != event.stack_depth; ++frame) {
            const auto address = event.stack[frame];
            output << "  " << address;
#if defined(_WIN32)
            HMODULE module = nullptr;
            if (GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                      | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR> (address), &module)) {
                char name[MAX_PATH]{};
                GetModuleFileNameA (module, name, MAX_PATH);
                output << ' ' << name << "+0x" << std::hex
                       << reinterpret_cast<std::uintptr_t> (address)
                            - reinterpret_cast<std::uintptr_t> (module)
                       << std::dec;
            }
#elif defined(__linux__) || defined(__APPLE__)
            if (symbols)
                output << ' ' << symbols[frame];
#endif
            output << '\n';
        }
#if defined(__linux__) || defined(__APPLE__)
        std::free (symbols);
#endif
    }
    return output.str ();
}

void check_allocation_failure ()
{
    if (!allocations_before_failure)
        return;
    if ((*allocations_before_failure)-- == 0) {
        allocations_before_failure.reset ();
        throw std::bad_alloc ();
    }
}

void *allocate_bytes (std::size_t size)
{
    check_allocation_failure ();
    if (void *memory = std::malloc (size == 0 ? 1 : size)) {
        record_allocation (memory, size);
        return memory;
    }
    throw std::bad_alloc ();
}

void *allocate_aligned_bytes (std::size_t size, std::size_t alignment)
{
    check_allocation_failure ();
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
