/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-D1: the client owns this scenario's public requests and assertions. */
inline void run_sf_d1_scenario (const options_t &options)
{
    warm_provider_connections (options, "SF-D1");
    sf_client::wait_connected (options.consumer_url, options.provider_a_endpoint);
    sf_client::wait_connected (options.consumer_url, options.provider_b_endpoint);
    const auto before = sf_client::connection_evidence (options.consumer_url);
    auto traffic = std::async (std::launch::async, [&options] {
        return sf_client::drive_requests (options.consumer_url, "sf-d1", options.lease_ttl * 2,
                                          "SF-D1");
    });
    sf_client::stop_store (options.redis_container);
    std::this_thread::sleep_for (options.lease_ttl / 2);
    sf_client::restart_store (options.redis_container);
    (void) traffic.get ();
    sf_client::wait_status (
      options.consumer_url,
      [] (const auto &status) { return status.store_healthy && status.owner_lease_healthy; },
      options.heartbeat * 10, "SF-D1 status did not recover");
    const auto after = sf_client::connection_evidence (options.consumer_url);
    for (const auto &endpoint : {options.provider_a_endpoint, options.provider_b_endpoint}) {
        std::cerr << "SF-D1 connection endpoint=" << endpoint << " connected-before="
                  << sf_client::connection_event_count (before, "Connected", endpoint)
                  << " connected-after="
                  << sf_client::connection_event_count (after, "Connected", endpoint)
                  << " disconnected-before="
                  << sf_client::connection_event_count (before, "Disconnected", endpoint)
                  << " disconnected-after="
                  << sf_client::connection_event_count (after, "Disconnected", endpoint) << '\n';
        sf_client::ensure (
          sf_client::connection_event_count (after, "Disconnected", endpoint)
              == sf_client::connection_event_count (before, "Disconnected", endpoint)
            && sf_client::connection_event_count (after, "Connected", endpoint)
                 == sf_client::connection_event_count (before, "Connected", endpoint),
          "SF-D1 survivor connection changed");
    }
    std::cout << "scenario SF-D1 passed\n";
}

} // namespace
