/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-B1: the client owns this scenario's public requests and assertions. */
inline void run_sf_b1_scenario (const options_t &options)
{
    sf_client::stop_store (options.redis_container);
    try {
        sf_client::drive_requests (options.consumer_url, "sf-b1", options.lease_ttl * 7 / 10,
                                   "SF-B1");
        sf_client::wait_status (
          options.consumer_url,
          [] (const auto &status) { return !status.store_healthy && !status.last_error.empty (); },
          options.heartbeat * 8, "SF-B1 outage status was not visible");
    }
    catch (...) {
        sf_client::restart_store (options.redis_container);
        throw;
    }
    sf_client::restart_store (options.redis_container);
    sf_client::wait_status (
      options.consumer_url,
      [] (const auto &status) { return status.store_healthy && status.owner_lease_healthy; },
      options.heartbeat * 10, "SF-B1 status did not recover");
    std::cout << "scenario SF-B1 passed\n";
}

} // namespace
