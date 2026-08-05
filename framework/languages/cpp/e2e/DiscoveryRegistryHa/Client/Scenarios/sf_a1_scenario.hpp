/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-A1: the client owns this scenario's public requests and assertions. */
inline void run_sf_a1_scenario (const options_t &options)
{
    sf_client::wait_peers (
      options.consumer_url,
      [] (const auto &peers) {
          return sf_client::has_rid (peers, "api-a") && sf_client::has_rid (peers, "api-b");
      },
      options.lease_ttl + options.polling * 8, "SF-A1 providers did not appear");

    std::set<std::string> served;
    for (int i = 0; i < 8; ++i) {
        auto reply =
          sf_client::request_profile (options.consumer_url, "sf-a1-" + std::to_string (i));
        sf_client::ensure (reply.value == "profile:fast", "SF-A1 unexpected reply");
        served.insert (reply.provider_rid);
    }
    sf_client::ensure (!served.empty (), "SF-A1 no provider served traffic");

    for (const auto &url : {options.consumer_url, options.provider_a_url, options.provider_b_url}) {
        sf_client::wait_status (
          url,
          [] (const auto &status) {
              return status.store_healthy && status.owner_lease_healthy
                     && status.owner_lease_renewed_at_unix_ms > 0
                     && status.last_refresh_at_unix_ms > 0;
          },
          options.heartbeat * 8, "SF-A1 status was not healthy");
    }
    std::cout << "scenario SF-A1 passed\n";
}

} // namespace
