/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <cstddef>
namespace zlink::framework::tests
{
inline std::array<std::atomic_size_t, 6> native_calls{};
inline std::atomic_bool count_native_calls{false};
inline std::atomic_size_t native_completion_events{0};
inline std::mutex native_close_mutex;
inline std::condition_variable native_close_changed;
struct native_call_scope_t
{
    native_call_scope_t ()
    {
        for (auto &count : native_calls)
            count.store (0);
        count_native_calls.store (true);
    }
    ~native_call_scope_t () { count_native_calls.store (false); }
    std::size_t operator[] (std::size_t index) const { return native_calls[index].load (); }
    bool wait_for_closes (std::size_t expected,
                          std::chrono::steady_clock::time_point deadline) const
    {
        std::unique_lock lock (native_close_mutex);
        return native_close_changed.wait_until (
          lock, deadline, [&] { return native_calls[4].load () >= expected; });
    }
};
}
