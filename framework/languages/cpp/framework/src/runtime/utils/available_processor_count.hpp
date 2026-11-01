/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <algorithm>
#include <bit>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#endif

namespace zlink::framework::runtime
{

inline unsigned available_processor_count () noexcept
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO (&set);
    // pid 0 would read the calling thread's mask; pools must follow the process.
    if (sched_getaffinity (getpid (), sizeof (set), &set) == 0)
        return (std::max) (1u, static_cast<unsigned> (CPU_COUNT (&set)));
#elif defined(_WIN32)
    DWORD_PTR process_mask = 0;
    DWORD_PTR system_mask = 0;
    if (GetProcessAffinityMask (GetCurrentProcess (), &process_mask, &system_mask))
        return (std::max) (1u, static_cast<unsigned> (std::popcount (process_mask)));
#endif
    return (std::max) (1u, std::thread::hardware_concurrency ());
}

} // namespace zlink::framework::runtime
