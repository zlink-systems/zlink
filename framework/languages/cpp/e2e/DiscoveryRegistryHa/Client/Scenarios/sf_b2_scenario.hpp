/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-B2: the client owns this scenario's public requests and assertions. */
inline void run_sf_b2_scenario (const options_t &options)
{
    sf_client::stop_store (options.redis_container);
    try {
        sf_client::wait_ready (options.replacement_provider_url);
        const auto replies = sf_client::drive_requests (
          options.consumer_url, "sf-b2", options.grace + options.heartbeat * 2, "SF-B2");
        sf_client::ensure (
          std::none_of (replies.begin (), replies.end (),
                        [] (const auto &reply) { return reply.provider_rid == "api-b"; }),
          "SF-B2 replacement provider served before recovery");
        const auto status = sf_client::get_status (options.consumer_url);
        sf_client::ensure (!status.store_healthy, "SF-B2 outage was not visible after grace");
    }
    catch (...) {
        sf_client::restart_store (options.redis_container);
        throw;
    }
    sf_client::restart_store (options.redis_container);
    sf_client::wait_status (
      options.consumer_url,
      [] (const auto &status) { return status.store_healthy && status.owner_lease_healthy; },
      options.heartbeat * 10, "SF-B2 status did not recover");
    sf_client::wait_peers (
      options.consumer_url,
      [&options] (const auto &peers) {
          return std::any_of (peers.begin (), peers.end (), [&options] (const auto &peer) {
              return peer.rid == "api-b" && peer.endpoint == options.replacement_provider_endpoint;
          });
      },
      options.lease_ttl + options.polling * 12,
      "SF-B2 replacement provider row did not appear after recovery");
    bool replacement_used = false;
    const auto deadline = std::chrono::steady_clock::now () + options.heartbeat * 8;
    for (int index = 0; std::chrono::steady_clock::now () < deadline; ++index) {
        const auto reply = sf_client::request_profile (
          options.consumer_url,
          sf_client::unique_marker ("sf-b2-recovered-" + std::to_string (index)));
        if (reply.provider_rid == "api-b") {
            replacement_used = true;
            break;
        }
    }
    sf_client::ensure (replacement_used, "SF-B2 replacement provider was not used after recovery");
    std::cout << "scenario SF-B2 passed\n";
}

} // namespace
