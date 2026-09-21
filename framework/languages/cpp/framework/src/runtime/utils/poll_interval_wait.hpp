/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
// Needs the Windows 10 1803 SDK; older headers omit the flag but the runtime
// call still accepts it, and a system that rejects it falls back below.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace zlink::framework::runtime
{

// Single owner of "wait one poll tick" for every runtime loop that re-reads a
// state it cannot be woken from: peer topology, a location-store creation
// terminal, a relocation unit's readiness, an authority row, an active-request
// count, a route-mesh pump.
//
// std::this_thread::sleep_for cannot express such a tick on Windows. The
// scheduler's default timer period is ~15.6 ms and a shorter sleep still waits a
// whole period, so a loop written as a 1 ms tick runs at 15.3 ms and a 100 us
// backoff costs 15.2 ms - measured on Windows 11 against 1.07 ms and 170 us for
// the same source on Linux. A loop needing N observations then takes 15x longer
// on Windows for a reason its call site never stated. That is a defect in the
// wait, not a budget that wants raising.
//
// A high-resolution waitable timer honours the requested duration without
// raising the process-wide timer period, so it costs no extra power - unlike
// timeBeginPeriod, which would also still leave a 100 us request at ~1.5 ms.
// Measured here: 100 us request -> 538 us, 1 ms request -> 1.40 ms.
//
// Each caller still states its own interval; this owns only how the wait is
// taken. Durations finer than microseconds convert implicitly.
inline void wait_poll_interval (std::chrono::microseconds interval) noexcept
{
    if (interval <= std::chrono::microseconds::zero ())
        return;

#if defined(_WIN32)
    // One timer per thread: creating a handle per wait would cost more than the
    // wait itself in a poll loop, and threads that never poll never create one.
    static thread_local struct timer_handle_t
    {
        HANDLE handle = NULL;

        timer_handle_t () noexcept
        {
            handle = ::CreateWaitableTimerExW (NULL, NULL,
                                               CREATE_WAITABLE_TIMER_MANUAL_RESET
                                                 | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                               TIMER_ALL_ACCESS);
            if (handle == NULL) {
                // Before Windows 10 1803 the high-resolution flag is rejected. A
                // plain timer behaves exactly like the sleep this replaces, so
                // the fallback is never worse than the previous behaviour.
                handle = ::CreateWaitableTimerExW (NULL, NULL, CREATE_WAITABLE_TIMER_MANUAL_RESET,
                                                   TIMER_ALL_ACCESS);
            }
        }

        ~timer_handle_t ()
        {
            if (handle != NULL)
                ::CloseHandle (handle);
        }

        timer_handle_t (const timer_handle_t &) = delete;
        timer_handle_t &operator= (const timer_handle_t &) = delete;
    } timer;

    if (timer.handle != NULL) {
        LARGE_INTEGER due_time;
        // Negative means relative, in 100 ns units.
        due_time.QuadPart = -(static_cast<LONGLONG> (interval.count ()) * 10);
        if (::SetWaitableTimer (timer.handle, &due_time, 0, NULL, NULL, FALSE) != 0) {
            ::WaitForSingleObject (timer.handle, INFINITE);
            return;
        }
    }
#endif

    std::this_thread::sleep_for (interval);
}

} // namespace zlink::framework::runtime
