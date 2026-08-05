/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-D3: the client owns this scenario's public requests and assertions. */
inline void run_sf_d3_scenario (const options_t &options)
{
    const auto initial = sf_client::wait_status (
      options.consumer_url,
      [] (const auto &status) {
          return status.store_healthy && status.owner_lease_healthy
                 && status.owner_lease_renewed_at_unix_ms > 0 && status.last_refresh_at_unix_ms > 0;
      },
      options.heartbeat * 8, "SF-D3 initial status was not healthy");
    sf_client::stop_store (options.redis_container);
    sf::runtime_status_res_t outage;
    try {
        outage = sf_client::wait_status (
          options.consumer_url,
          [] (const auto &status) {
              return !status.store_healthy && !status.owner_lease_healthy
                     && !status.last_error.empty ();
          },
          options.heartbeat * 10, "SF-D3 outage status was not visible");
    }
    catch (...) {
        sf_client::restart_store (options.redis_container);
        throw;
    }
    sf_client::ensure (outage.owner_lease_renewed_at_unix_ms
                           >= initial.owner_lease_renewed_at_unix_ms
                         && outage.last_refresh_at_unix_ms >= initial.last_refresh_at_unix_ms,
                       "SF-D3 outage discarded the last successful runtime timestamps");
    sf_client::restart_store (options.redis_container);
    const auto recovered = sf_client::wait_status (
      options.consumer_url,
      [&outage] (const auto &status) {
          return status.store_healthy && status.owner_lease_healthy && status.last_error.empty ()
                 && status.last_refresh_at_unix_ms > outage.last_refresh_at_unix_ms
                 && status.owner_lease_renewed_at_unix_ms > outage.owner_lease_renewed_at_unix_ms;
      },
      options.heartbeat * 10, "SF-D3 status did not recover");
    sf_client::ensure (recovered.last_refresh_at_unix_ms > outage.last_refresh_at_unix_ms,
                       "SF-D3 recovery did not advance last refresh time");
    sf_client::ensure (recovered.owner_lease_renewed_at_unix_ms
                         > outage.owner_lease_renewed_at_unix_ms,
                       "SF-D3 recovery did not advance owner lease renewal time");
    std::cout << "scenario SF-D3 passed\n";
}

} // namespace
