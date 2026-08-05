/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-E1: the client owns this scenario's public requests and assertions. */
inline void run_sf_e1_scenario (const options_t &options)
{
    constexpr int delay_ms = 300;
    sf_client::ensure (!options.redis_proxy_admin_url.empty (),
                       "SF-E1 Redis latency proxy admin URL is required");
    sf_client::wait_peers (
      options.consumer_url,
      [] (const auto &peers) {
          return sf_client::has_rid (peers, "api-a") && sf_client::has_rid (peers, "api-b");
      },
      options.lease_ttl + options.heartbeat * 4, "SF-E1 baseline peers were not ready");

    const auto baseline = measure_requests (options.consumer_url, "SF-E1-baseline", 10);
    const auto baseline_p99 = percentile_ms (baseline, 0.99);

    sf_client::set_store_delay (options.redis_proxy_admin_url, delay_ms);
    try {
        auto delayed_store_read =
          std::async (std::launch::async, [&options] { return measure_peer_query_ms (options); });
        std::this_thread::sleep_for (std::chrono::milliseconds (150));
        const auto status_started = std::chrono::steady_clock::now ();
        const auto status = sf_client::get_status (options.consumer_url);
        const auto status_query_ms = std::chrono::duration<double, std::milli> (
                                       std::chrono::steady_clock::now () - status_started)
                                       .count ();
        const auto concurrent = measure_requests (options.consumer_url, "SF-E1-concurrent", 12);
        const auto delayed_store_read_ms = delayed_store_read.get ();
        const auto concurrent_p99 = percentile_ms (concurrent, 0.99);
        const auto budget = std::max (baseline_p99 * 8.0, 100.0);

        sf_client::ensure (delayed_store_read_ms >= static_cast<double> (delay_ms) * 0.75,
                           "SF-E1 delayed store read finished too quickly");
        sf_client::ensure (status.store_healthy && status_query_ms <= budget,
                           "SF-E1 runtime status query blocked during store delay");
        sf_client::ensure (concurrent_p99 <= budget,
                           "SF-E1 unrelated request p99 grew too much during store delay");
        std::cout << "SF-E1 latency baseline-p99-ms=" << baseline_p99
                  << " delayed-store-read-ms=" << delayed_store_read_ms
                  << " status-query-ms=" << status_query_ms
                  << " concurrent-p99-ms=" << concurrent_p99 << " budget-ms=" << budget << '\n';

        auto recovery = sf_client::request_profile (options.consumer_url, "SF-E1-recovery");
        sf_client::ensure (recovery.value == "profile:fast",
                           "SF-E1 request path did not recover after delayed store read");
        sf_client::set_store_delay (options.redis_proxy_admin_url, 0);
    }
    catch (...) {
        sf_client::set_store_delay (options.redis_proxy_admin_url, 0);
        throw;
    }

    std::cout << "scenario SF-E1 passed\n";
}

} // namespace
