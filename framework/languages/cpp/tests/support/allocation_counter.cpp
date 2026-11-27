/* SPDX-License-Identifier: Elastic-2.0 */
#include "allocation_counter.hpp"
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <sstream>
#include <cstdint>
#if defined(_WIN32)
#include <bit>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <execinfo.h>
#endif
#if defined(_MSC_VER)
#include <intrin.h>
#include <malloc.h>
#endif
namespace zlink::framework::tests
{
std::atomic_bool tracking_process_allocations{false};
std::atomic<std::size_t> process_allocation_count{0};
std::atomic<std::size_t> process_allocation_failure_size{0};
std::atomic<std::size_t> process_allocation_failure_count{0};
thread_local bool tracking_allocations = false;
thread_local std::optional<std::size_t> allocations_before_failure;
thread_local std::size_t tracked_allocation_count = 0;
thread_local std::size_t tracked_allocation_size = 0;
thread_local std::size_t tracked_allocation_bytes = 0;
thread_local std::size_t largest_allocation = 0;
thread_local allocation_event_log_t *allocation_event_log = nullptr;

namespace
{
thread_local bool tracing_allocation_site = false;

bool allocation_sites_enabled () noexcept
{
    static const bool enabled = [] {
        const auto *value = std::getenv ("ZLINK_TEST_ALLOCATION_SITES");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

void capture_allocation_site (allocation_event_t &event) noexcept
{
#if defined(_WIN32)
    event.stack_depth = CaptureStackBackTrace (0, static_cast<DWORD> (event.stack.size ()),
                                               event.stack.data (), nullptr);
#elif defined(__linux__) || defined(__APPLE__)
    event.stack_depth = static_cast<std::size_t> (
      backtrace (event.stack.data (), static_cast<int> (event.stack.size ())));
#else
    (void) event;
#endif
}

#if defined(_WIN32)
HMODULE allocation_site_module (void *address) noexcept
{
    HMODULE module = nullptr;
    GetModuleHandleExA (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                          | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR> (address), &module);
    return module;
}

auto load_symbol_lookup () noexcept -> decltype (&SymFromAddr)
{
    const auto library = LoadLibraryExW (L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
        return nullptr;
    const auto initialize =
      std::bit_cast<decltype (&SymInitialize)> (GetProcAddress (library, "SymInitialize"));
    const auto lookup =
      std::bit_cast<decltype (&SymFromAddr)> (GetProcAddress (library, "SymFromAddr"));
    if (initialize && lookup && initialize (GetCurrentProcess (), ".", TRUE))
        return lookup;
    FreeLibrary (library);
    return nullptr;
}
#endif

void print_allocation_site (const allocation_event_t &event, std::size_t index) noexcept
{
#if defined(_WIN32)
    // DbgHelp requires serialized calls, including initialization.
    static SRWLOCK symbol_lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive (&symbol_lock);
    static const auto lookup = load_symbol_lookup ();
    struct
    {
        SYMBOL_INFO info{};
        char name[MAX_SYM_NAME - 1];
    } symbol;
    symbol.info.SizeOfStruct = sizeof (SYMBOL_INFO);
    symbol.info.MaxNameLen = MAX_SYM_NAME;
#elif defined(__linux__) || defined(__APPLE__)
    auto **symbols = backtrace_symbols (event.stack.data (), static_cast<int> (event.stack_depth));
    flockfile (stderr);
#endif
    std::fprintf (stderr, "ZLINK_TEST_ALLOCATION_SITE index=%zu size=%zu frames=%zu\n", index,
                  event.size, event.stack_depth);
    for (std::size_t frame = 0; frame != event.stack_depth; ++frame) {
        const auto address = event.stack[frame];
        std::fprintf (stderr, "  frame=%zu address=0x%" PRIxPTR, frame,
                      reinterpret_cast<std::uintptr_t> (address));
#if defined(_WIN32)
        if (const auto module = allocation_site_module (address)) {
            char name[MAX_PATH + 1]{};
            GetModuleFileNameA (module, name, MAX_PATH);
            std::fprintf (stderr, " module=%s+0x%zx", name,
                          reinterpret_cast<std::size_t> (address)
                            - reinterpret_cast<std::size_t> (module));
        }
        DWORD64 displacement = 0;
        if (lookup
            && lookup (GetCurrentProcess (), reinterpret_cast<DWORD64> (address), &displacement,
                       &symbol.info)) {
            std::fprintf (stderr, " symbol=%s+0x%llx", symbol.info.Name,
                          static_cast<unsigned long long> (displacement));
        }
#elif defined(__linux__) || defined(__APPLE__)
        if (symbols)
            std::fprintf (stderr, " symbol=%s", symbols[frame]);
#endif
        std::fputc ('\n', stderr);
    }
#if defined(_WIN32)
    ReleaseSRWLockExclusive (&symbol_lock);
#elif defined(__linux__) || defined(__APPLE__)
    funlockfile (stderr);
    std::free (symbols);
#endif
}
} // namespace

void record_allocation (const void *memory, std::size_t size) noexcept
{
    if (tracing_allocation_site)
        return;
    if (tracking_process_allocations.load (std::memory_order_relaxed))
        process_allocation_count.fetch_add (1, std::memory_order_relaxed);
    if (tracking_allocations && (tracked_allocation_size == 0 || tracked_allocation_size == size)) {
        const bool trace = allocation_sites_enabled ();
        allocation_event_t *stored_event = nullptr;
        if (allocation_event_log != nullptr) {
            if (allocation_event_log->count < allocation_event_log->capacity) {
                stored_event = &allocation_event_log->events[allocation_event_log->count++];
            } else
                allocation_event_log->overflow = true;
        }
        if (trace) {
            const int saved_errno = errno;
#if defined(_WIN32)
            const auto saved_error = GetLastError ();
#endif
            tracing_allocation_site = true;
            allocation_event_t event{memory, size};
            capture_allocation_site (event);
#if defined(_MSC_VER)
            if (event.stack_depth == 0) {
                event.stack[0] = _ReturnAddress ();
                event.stack_depth = 1;
            }
#endif
            print_allocation_site (event, tracked_allocation_count);
            tracing_allocation_site = false;
            if (stored_event)
                *stored_event = event;
            errno = saved_errno;
#if defined(_WIN32)
            SetLastError (saved_error);
#endif
        } else if (stored_event) {
            *stored_event = {memory, size};
            capture_allocation_site (*stored_event);
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
            if (const auto module = allocation_site_module (address)) {
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

bool should_fail_allocation () noexcept
{
    if (!allocations_before_failure)
        return false;
    if ((*allocations_before_failure)-- == 0) {
        allocations_before_failure.reset ();
        return true;
    }
    return false;
}

bool should_fail_process_allocation (std::size_t size) noexcept
{
    auto expected = process_allocation_failure_size.load (std::memory_order_relaxed);
    if (expected == 0 || size < expected
        || !process_allocation_failure_size.compare_exchange_strong (expected, 0,
                                                                     std::memory_order_relaxed))
        return false;
    process_allocation_failure_count.fetch_add (1, std::memory_order_relaxed);
    return true;
}

bool should_fail_injected_allocation (std::size_t size) noexcept
{
    return !tracing_allocation_site
           && (should_fail_process_allocation (size) || should_fail_allocation ());
}

void *allocate_bytes (std::size_t size) noexcept
{
    if (should_fail_injected_allocation (size))
        return nullptr;
    if (void *memory = std::malloc (size == 0 ? 1 : size)) {
        record_allocation (memory, size);
        return memory;
    }
    return nullptr;
}

void *allocate_aligned_bytes (std::size_t size, std::size_t alignment) noexcept
{
    if (should_fail_injected_allocation (size))
        return nullptr;
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
    return nullptr;
}

void *require_allocation (void *memory)
{
    if (memory == nullptr)
        throw std::bad_alloc ();
    return memory;
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
    return require_allocation (allocate_bytes (size));
}
void *operator new[] (std::size_t size)
{
    return require_allocation (allocate_bytes (size));
}
void *operator new (std::size_t size, std::align_val_t alignment)
{
    return require_allocation (allocate_aligned_bytes (size, static_cast<std::size_t> (alignment)));
}
void *operator new[] (std::size_t size, std::align_val_t alignment)
{
    return require_allocation (allocate_aligned_bytes (size, static_cast<std::size_t> (alignment)));
}
void *operator new (std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate_bytes (size);
}
void *operator new[] (std::size_t size, const std::nothrow_t &) noexcept
{
    return allocate_bytes (size);
}
void *operator new (std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return allocate_aligned_bytes (size, static_cast<std::size_t> (alignment));
}
void *operator new[] (std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
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
void operator delete (void *memory, const std::nothrow_t &) noexcept
{
    std::free (memory);
}
void operator delete[] (void *memory, const std::nothrow_t &) noexcept
{
    std::free (memory);
}
void operator delete (void *memory, std::align_val_t, const std::nothrow_t &) noexcept
{
    free_aligned_bytes (memory);
}
void operator delete[] (void *memory, std::align_val_t, const std::nothrow_t &) noexcept
{
    free_aligned_bytes (memory);
}
