/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/locations/location_runtime.hpp"

namespace zlink::framework::runtime
{
struct location_runtime_test_access_t
{
    template <typename Now>
    static std::chrono::milliseconds first_heartbeat_delay (location_runtime_t &runtime, Now now)
    {
        const auto claim_started_at = now ();
        runtime.renew_owner_lease_once ();
        auto heartbeat = std::make_shared<location_runtime_t::heartbeat_owner_t> ();
        auto delay = std::chrono::milliseconds::zero ();
        runtime.heartbeat_loop (heartbeat, claim_started_at, now,
                                [&] (auto &, auto &, auto remaining, auto, auto...) {
                                    delay = remaining;
                                    heartbeat->stop.store (true, std::memory_order_release);
                                });
        return delay;
    }
    static state_lane_t &lane (location_runtime_t &runtime) { return runtime._lane; }
    static auto snapshot (const location_runtime_t &runtime)
    {
        return std::atomic_load_explicit (&runtime._owner_lease, std::memory_order_acquire);
    }
    static void set_deadline (location_runtime_t &runtime,
                              std::chrono::steady_clock::time_point deadline)
    {
        runtime._lane
          .run_checked ([&] {
              runtime.update_owner_lease_on_lane (
                [&] (auto &lease) { lease.admission_deadline = deadline; });
          })
          .get ();
    }
    static bool usable_at (const location_runtime_t &runtime,
                           std::chrono::steady_clock::time_point now)
    {
        return location_runtime_t::owner_lease_usable (*snapshot (runtime), now);
    }
};
} // namespace zlink::framework::runtime
