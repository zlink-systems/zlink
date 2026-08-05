/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-C2: the client owns this scenario's public requests and assertions. */
inline void run_sf_c2_scenario (const options_t &options)
{
    const auto propagation_bound =
      options.polling + std::chrono::seconds (5) + std::chrono::milliseconds (100);
    auto drain = std::async (std::launch::async, [&] {
        return sf_client::post_json<sf::operation_status_t, sf::operation_status_t> (
          options.provider_b_url, "/drain", {}, std::chrono::seconds (35));
    });
    sf_client::wait_peers (
      options.consumer_url,
      [] (const auto &peers) {
          return std::any_of (peers.begin (), peers.end (), [] (const auto &peer) {
              return peer.rid == "api-b" && peer.draining;
          });
      },
      options.polling + options.heartbeat, "SF-C2 api-b did not publish draining=true");
    const auto draining_status = sf_client::get_status (options.provider_b_url);
    sf_client::ensure (draining_status.owner_lease_healthy,
                       "SF-C2 api-b owner lease was unhealthy during drain");

    int consecutive_survivor_replies = 0;
    const auto propagation_deadline = std::chrono::steady_clock::now () + propagation_bound;
    for (int probe = 0; std::chrono::steady_clock::now () < propagation_deadline
                        && consecutive_survivor_replies < 20;
         ++probe) {
        const auto reply = sf_client::request_profile (
          options.consumer_url, "sf-c2-propagation-" + std::to_string (probe));
        consecutive_survivor_replies =
          reply.provider_rid == "api-a" ? consecutive_survivor_replies + 1 : 0;
    }
    sf_client::ensure (consecutive_survivor_replies == 20,
                       "SF-C2 draining provider remained eligible for new requests");

    const auto drain_result = drain.get ();
    sf_client::ensure (
      drain_result.status == "stopped",
      "SF-C2 Shutdown did not complete as Stopped");
    sf_client::wait_down (options.provider_b_url);
    const auto removal_started = std::chrono::steady_clock::now ();
    sf_client::wait_peers (
      options.consumer_url, [] (const auto &peers) { return !sf_client::has_rid (peers, "api-b"); },
      options.lease_ttl, "SF-C2 api-b row did not disappear on shutdown");
    sf_client::ensure (std::chrono::steady_clock::now () - removal_started < options.lease_ttl,
                       "SF-C2 row removal did not beat the lease TTL");
    for (int i = 0; i < 6; ++i) {
        auto reply =
          sf_client::request_profile (options.consumer_url, "sf-c2-" + std::to_string (i));
        sf_client::ensure (reply.provider_rid == "api-a", "SF-C2 routed to stopped provider");
    }
    std::cout << "scenario SF-C2 passed\n";
}

} // namespace
