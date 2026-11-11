/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

// Measurement-only instrumentation. This file is excluded from the product
// commit; ZLINK_PERF_RECEIVE_ROLE_LOG enables the counters and per-record tags.
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <utility>

namespace zlink::framework::runtime::receive_role_measurement
{
struct rotation_t
{
};
struct counters_t
{
    const char *path = std::getenv ("ZLINK_PERF_RECEIVE_ROLE_LOG");
    std::atomic_size_t rotations{0}, eligible{0}, attempts{0}, successes{0};
    std::atomic_size_t records_started{0}, records_started_locally{0};
    bool enabled () const noexcept { return path && *path; }
    ~counters_t ()
    {
        if (!enabled ())
            return;
        std::ofstream output (path);
        output << "R\tE\tA\tH\trecords_started\trecords_started_locally\n"
               << rotations.load () << '\t' << eligible.load () << '\t' << attempts.load () << '\t'
               << successes.load () << '\t' << records_started.load () << '\t'
               << records_started_locally.load () << '\n';
    }
};
inline counters_t &counters ()
{
    static counters_t result;
    return result;
}
inline thread_local std::shared_ptr<const rotation_t> receive_rotation;
inline thread_local std::shared_ptr<const rotation_t> local_turn;
inline void begin_rotation ()
{
    if (counters ().enabled ())
        receive_rotation = std::make_shared<rotation_t> ();
}
inline void end_rotation ()
{
    if (receive_rotation)
        counters ().rotations.fetch_add (1, std::memory_order_relaxed);
}
inline void enter_application (bool handed_off)
{
    if (receive_rotation) {
        if (handed_off)
            local_turn = std::move (receive_rotation);
        else
            receive_rotation.reset ();
    }
}
inline void record_started (const std::shared_ptr<const rotation_t> &rotation)
{
    counters ().records_started.fetch_add (1, std::memory_order_relaxed);
    if (rotation == local_turn)
        counters ().records_started_locally.fetch_add (1, std::memory_order_relaxed);
}
} // namespace zlink::framework::runtime::receive_role_measurement
