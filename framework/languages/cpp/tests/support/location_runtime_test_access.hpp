/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/locations/location_runtime.hpp"
#include <thread>

namespace zlink::framework::runtime
{
struct location_runtime_test_access_t
{
    using outage_t = location_runtime_t::store_outage_t;
    using time_point = std::chrono::steady_clock::time_point;

    static void clock (location_runtime_t &runtime, std::function<time_point ()> now)
    {
        *runtime._lease_now = std::move (now);
    }

    static void communication_failure (location_runtime_t &runtime)
    {
        runtime._lane
          ->run_checked ([&] { runtime.observe_store_outage_on_lane ((*runtime._lease_now) ()); })
          .get ();
    }

    static bool outage_discarded (const location_runtime_t &runtime)
    {
        return runtime.observation_status_task ().result ().value ().store_outage_discarded;
    }

    static void accept (location_runtime_t &runtime,
                        location_owner_token_t token,
                        std::chrono::system_clock::time_point expires_at,
                        std::chrono::system_clock::time_point store_now,
                        time_point started_at,
                        bool renewal,
                        std::shared_ptr<outage_t> requested_outage = {})
    {
        auto attempt = std::make_shared<location_runtime_t::heartbeat_attempt_t> (
          &runtime, started_at, time_point::max (),
          std::weak_ptr<location_runtime_t::heartbeat_owner_t>{});
        attempt->requested_outage = std::move (requested_outage);
        location_runtime_t::heartbeat_accept (attempt, std::move (token), expires_at, store_now,
                                              started_at, renewal)
          .result ()
          .value ();
    }

    template <class Runtime, class Publisher>
    static void publisher (Runtime &runtime, Publisher publish)
    {
        if constexpr (requires { runtime._descriptor_publisher = std::move (publish); })
            runtime._descriptor_publisher = std::move (publish);
    }

    static void heartbeat (location_runtime_t &runtime,
                           time_point started_at,
                           std::function<void ()> submitted = {})
    {
        auto owner = std::make_shared<location_runtime_t::heartbeat_owner_t> ();
        auto attempt = std::make_shared<location_runtime_t::heartbeat_attempt_t> (
          &runtime, started_at, started_at + runtime._options.owner_lease_renew_timeout, owner);
        auto pending = location_runtime_t::heartbeat_renew_once_async (attempt);
        if (submitted)
            submitted ();
        pending.result ().value ();
    }

    static task_t<void> heartbeat_async (location_runtime_t &runtime, time_point started_at)
    {
        auto owner = std::make_shared<location_runtime_t::heartbeat_owner_t> ();
        auto attempt = std::make_shared<location_runtime_t::heartbeat_attempt_t> (
          &runtime, started_at, started_at + runtime._options.owner_lease_renew_timeout, owner);
        co_await location_runtime_t::heartbeat_renew_once_async (attempt);
    }

    template <class Predicate>
    static bool wait_on_lane (location_runtime_t &runtime, Predicate observed, time_point deadline)
    {
        for (;;) {
            if (runtime._lane->run_sync (observed))
                return true;
            if (std::chrono::steady_clock::now () >= deadline)
                return false;
            std::this_thread::yield ();
        }
    }

    template <class Episode>
    static bool recovery_terminal (location_runtime_t &runtime,
                                   const std::shared_ptr<Episode> &episode,
                                   std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        const auto pending = runtime._lane->run_checked ([&] { return episode->recovery; }).get ();
        if (!pending)
            return true;
        const auto result = pending->result_for (timeout);
        if (!result)
            return false;
        // Failed passes retain the episode for the next renew or its original T.
        if (!*result && !episode->discarded.load (std::memory_order_acquire))
            return true;
        // Aggregate completion precedes finish_execution_recovery's lane post.
        return wait_on_lane (
          runtime,
          [&] {
              return snapshot (runtime)->outage != episode
                     || episode->owner->stop.load (std::memory_order_acquire);
          },
          deadline);
    }

    static void confirm (location_runtime_t &runtime,
                         time_point started_at,
                         std::function<void ()> submitted = {})
    {
        auto owner = std::make_shared<location_runtime_t::heartbeat_owner_t> ();
        auto attempt = std::make_shared<location_runtime_t::heartbeat_attempt_t> (
          &runtime, started_at, started_at + runtime._options.owner_lease_renew_timeout, owner);
        std::string failure;
        auto pending = location_runtime_t::heartbeat_confirm (attempt, failure);
        if (submitted)
            submitted ();
        pending.result ().value ();
    }

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
    static std::size_t pending_store_operations (location_runtime_t &runtime)
    {
        return runtime._lane
          ->run_checked ([&] {
              return std::count_if (
                runtime._descriptor_publications.begin (), runtime._descriptor_publications.end (),
                [] (const auto &pending) { return !pending->drained.task ().await_ready (); });
          })
          .get ();
    }

    static state_lane_t &lane (location_runtime_t &runtime) { return *runtime._lane; }
    static auto snapshot (const location_runtime_t &runtime)
    {
        return std::atomic_load_explicit (runtime._owner_lease.get (), std::memory_order_acquire);
    }
    static void set_deadline (location_runtime_t &runtime,
                              std::chrono::steady_clock::time_point deadline)
    {
        runtime._lane
          ->run_checked ([&] {
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
